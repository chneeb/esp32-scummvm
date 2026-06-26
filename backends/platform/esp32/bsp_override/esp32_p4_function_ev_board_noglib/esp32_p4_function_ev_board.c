/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_spiffs.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "esp_vfs_fat.h"
#include "usb/usb_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"


// Panel driver selection for the 5" 800x480 DSI display.
// BSP_LCD_PANEL_TC358762 = TC358762 DSI-to-DPI bridge (RPi 7" V1 type, confirmed ATTINY88 0xC3)
// BSP_LCD_PANEL_HX8399   = HX8399C vendor init
// BSP_LCD_PANEL_MINIMAL  = just SLPOUT+DISPON (generic test)
// (none)                 = ILI9881C vendor init
#define BSP_LCD_PANEL_TC358762
// #define BSP_LCD_PANEL_HX8399
// #define BSP_LCD_PANEL_MINIMAL

#if CONFIG_BSP_LCD_TYPE_1024_600
#include "esp_lcd_ek79007.h"
#elif CONFIG_BSP_LCD_TYPE_HDMI
#include "esp_lcd_lt8912b.h"
#else
#include "esp_lcd_ili9881c.h"
#include "esp_lcd_hx8399.h"
#endif

#ifdef BSP_LCD_PANEL_TC358762
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/mipi_dsi_hal.h"
#include "hal/mipi_dsi_types.h"
#include "hal/mipi_dsi_ll.h"
#include "driver/i2c_master.h"

// ATTINY88 at I2C 0x45 (controls power rails, TC358762 reset, backlight PWM)
#define ATTINY_ADDR             (0x45)
#define ATTINY_SCL_HZ           (400000)
#define ATTINY_TIMEOUT_MS       (50)
#define ATTINY_REG_PORTA        (0x81)
#define ATTINY_REG_PORTB        (0x82)
#define ATTINY_REG_PORTC        (0x83)
#define ATTINY_REG_PWM          (0x86)
#define ATTINY_REG_ADDR_H       (0x8d)
#define ATTINY_REG_ADDR_L       (0x8c)
#define ATTINY_REG_DATA_H       (0x90)
#define ATTINY_REG_DATA_L       (0x91)
#define ATTINY_PA_LCD_LR        BIT(2)
#define ATTINY_PB_LCD_MAIN      BIT(7)
#define ATTINY_PC_LED_EN        BIT(0)
#define ATTINY_PC_RST_TP_N      BIT(1)
#define ATTINY_PC_RST_LCD_N     BIT(2)
#define ATTINY_PC_RST_BRIDGE_N  BIT(3)

// TC358762 DSI-to-DPI bridge registers
#define TC_DSI_LANEENABLE       (0x0210)
#define TC_PPI_D0S_CLRSIPOCOUNT (0x0164)
#define TC_PPI_D1S_CLRSIPOCOUNT (0x0168)
#define TC_PPI_D0S_ATMR         (0x0144)
#define TC_PPI_D1S_ATMR         (0x0148)
#define TC_PPI_LPTXTIMECNT      (0x0114)
#define TC_SPICMR               (0x0450)
#define TC_LCDCTRL              (0x0420)
#define TC_SYSCTRL              (0x0464)
#define TC_LCD_HS_HBP           (0x0424)
#define TC_LCD_HDISP_HFP        (0x0428)
#define TC_LCD_VS_VBP           (0x042c)
#define TC_LCD_VDISP_VFP        (0x0430)
#define TC_PPI_STARTPPI         (0x0104)
#define TC_DSI_STARTDSI         (0x0204)

// Observed-stable DPI timing for this RPi-style 800x480 panel (from embenix ESP32-P4 testing)
// ~46 Hz refresh, but panel syncs stably at this rate
#define RPI_HFP  210
#define RPI_HSW    2
#define RPI_HBP   46
#define RPI_VFP   22
#define RPI_VSW   20
#define RPI_VBP    4

// TC358762 VTGEN timing register values — must match the plain mode timing.
// The earlier "+127 pixel pipeline delay" compensation was an unfounded guess that put VTGEN's
// HBP out of range and made the bridge lose sync (dark screen). The embenix reference driver
// (rpi-7inch-touch-display-v1.c) writes the plain mode HBP/HFP directly, so we do the same.
#define TC_VTGEN_HBP  RPI_HBP   // 46
#define TC_VTGEN_HFP  RPI_HFP   // 210

// Replicates esp_lcd_dsi_bus_t private layout to reach the HAL context.
// Matches esp-idf/components/esp_lcd/dsi/mipi_dsi_priv.h — update if IDF changes this.
typedef struct {
    int bus_id;
    mipi_dsi_hal_context_t hal;
} rpi_dsi_bus_priv_t;
#endif // BSP_LCD_PANEL_TC358762

#include "bsp/esp32_p4_function_ev_board.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_lcd_touch_gt911.h"
#include "bsp_err_check.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "ESP32_P4_EV";

#ifdef BSP_LCD_PANEL_TC358762
// i2c_handle is declared further down; forward reference is safe since these functions
// are only called after bsp_i2c_init() has initialized it.
static esp_err_t attiny_write(uint8_t reg, uint8_t val)
{
    i2c_device_config_t dev_cfg = {
        .device_address = ATTINY_ADDR,
        .scl_speed_hz   = ATTINY_SCL_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t ret = i2c_master_bus_add_device(bsp_i2c_get_handle(), &dev_cfg, &dev);
    if (ret != ESP_OK) return ret;
    uint8_t buf[2] = {reg, val};
    ret = i2c_master_transmit(dev, buf, 2, ATTINY_TIMEOUT_MS);
    i2c_master_bus_rm_device(dev);
    return ret;
}

static void tc358762_reg_write(esp_lcd_dsi_bus_handle_t bus, uint16_t reg, uint32_t val)
{
    rpi_dsi_bus_priv_t *priv = (rpi_dsi_bus_priv_t *)bus;
    uint8_t payload[6] = {
        (reg >> 0) & 0xFF, (reg >> 8) & 0xFF,
        (val >> 0) & 0xFF, (val >> 8) & 0xFF,
        (val >>16) & 0xFF, (val >>24) & 0xFF,
    };
    mipi_dsi_hal_host_gen_write_long_packet(&priv->hal, 0,
        MIPI_DSI_DT_GENERIC_LONG_WRITE, payload, sizeof(payload));
}

static esp_err_t tc358762_power_on(void)
{
    ESP_RETURN_ON_ERROR(attiny_write(ATTINY_REG_PORTC, 0x00), TAG, "ATTINY PORTC=0 failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(attiny_write(ATTINY_REG_PORTA, ATTINY_PA_LCD_LR), TAG, "ATTINY PORTA failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(attiny_write(ATTINY_REG_PORTB, ATTINY_PB_LCD_MAIN), TAG, "ATTINY PORTB failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(attiny_write(ATTINY_REG_PORTC, ATTINY_PC_LED_EN), TAG, "ATTINY PORTC=LED failed");
    vTaskDelay(pdMS_TO_TICKS(80));
    ESP_LOGI(TAG, "TC358762: ATTINY88 power-on complete (bridge still in reset)");
    return ESP_OK;
}

static esp_err_t tc358762_release_and_wake(void)
{
    ESP_RETURN_ON_ERROR(attiny_write(ATTINY_REG_PORTC,
        ATTINY_PC_LED_EN | ATTINY_PC_RST_LCD_N | ATTINY_PC_RST_BRIDGE_N),
        TAG, "ATTINY bridge release failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    // Wake TC358762: write SYSPMCTRL(0x047C)=0 via ATTINY SPI proxy
    ESP_RETURN_ON_ERROR(attiny_write(ATTINY_REG_ADDR_H,  0x04), TAG, "ATTINY ADDR_H failed");
    vTaskDelay(pdMS_TO_TICKS(8));
    ESP_RETURN_ON_ERROR(attiny_write(ATTINY_REG_ADDR_L,  0x7c), TAG, "ATTINY ADDR_L failed");
    vTaskDelay(pdMS_TO_TICKS(8));
    ESP_RETURN_ON_ERROR(attiny_write(ATTINY_REG_DATA_H, 0x00), TAG, "ATTINY DATA_H failed");
    vTaskDelay(pdMS_TO_TICKS(8));
    ESP_RETURN_ON_ERROR(attiny_write(ATTINY_REG_DATA_L, 0x00), TAG, "ATTINY DATA_L failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "TC358762: Bridge reset released, SYSPMCTRL wake sent");
    return ESP_OK;
}

static esp_err_t tc358762_bridge_init(esp_lcd_dsi_bus_handle_t bus)
{
    ESP_LOGI(TAG, "TC358762: Configuring bridge (mirrors Linux tc358762.c)");
    tc358762_reg_write(bus, TC_DSI_LANEENABLE,       BIT(0) | BIT(1)); // CLEN + D0EN (1 lane)
    tc358762_reg_write(bus, TC_PPI_D0S_CLRSIPOCOUNT, 0x05);
    tc358762_reg_write(bus, TC_PPI_D1S_CLRSIPOCOUNT, 0x05);
    tc358762_reg_write(bus, TC_PPI_D0S_ATMR,         0x00);
    tc358762_reg_write(bus, TC_PPI_D1S_ATMR,         0x00);
    tc358762_reg_write(bus, TC_PPI_LPTXTIMECNT,      0x03);
    tc358762_reg_write(bus, TC_SPICMR,               0x00);
    // LCDCTRL: VSDELAY(1)|RGB888(bit8)|UNK6|VTGEN — matches embenix rpi-7inch-v1 reference (0x00100150).
    // RGB888 end-to-end fixes the constant horizontal FIFO-phase offset (the ~200px wrap).
    tc358762_reg_write(bus, TC_LCDCTRL,              0x00100150);
    tc358762_reg_write(bus, TC_SYSCTRL,              0x040f);
    // Embenix encoding: bits[31:16]=porch, bits[15:0]=sync_pulse_width
    // TC_VTGEN_HBP/HFP compensate for TC358762 FIFO pipeline delay (see defines above)
    tc358762_reg_write(bus, TC_LCD_HS_HBP,    ((uint32_t)TC_VTGEN_HBP << 16) | RPI_HSW);
    tc358762_reg_write(bus, TC_LCD_HDISP_HFP, ((uint32_t)TC_VTGEN_HFP << 16) | 800);
    tc358762_reg_write(bus, TC_LCD_VS_VBP,    ((uint32_t)RPI_VBP << 16) | RPI_VSW);
    tc358762_reg_write(bus, TC_LCD_VDISP_VFP, ((uint32_t)RPI_VFP << 16) | 480);
    vTaskDelay(pdMS_TO_TICKS(100));
    tc358762_reg_write(bus, TC_PPI_STARTPPI,  0x01);
    tc358762_reg_write(bus, TC_DSI_STARTDSI,  0x01);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "TC358762: Bridge configured, PPI+DSI streams started");
    return ESP_OK;
}
#endif // BSP_LCD_PANEL_TC358762

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
static lv_indev_t *disp_indev = NULL;
#endif // (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

sdmmc_card_t *bsp_sdcard = NULL;    // Global uSD card handler
static bool i2c_initialized = false;
static TaskHandle_t usb_host_task;  // USB Host Library task
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0))
static i2c_master_bus_handle_t i2c_handle = NULL;  // I2C Handle
#endif
static i2s_chan_handle_t i2s_tx_chan = NULL;
static i2s_chan_handle_t i2s_rx_chan = NULL;
static const audio_codec_data_if_t *i2s_data_if = NULL;  /* Codec data interface */
static bsp_lcd_handles_t disp_handles;
static esp_ldo_channel_handle_t disp_phy_pwr_chan = NULL;
static esp_lcd_touch_handle_t tp = NULL;
static esp_lcd_panel_io_handle_t tp_io_handle = NULL;

/* Can be used for `i2s_std_gpio_config_t` and/or `i2s_std_config_t` initialization */
#define BSP_I2S_GPIO_CFG       \
    {                          \
        .mclk = BSP_I2S_MCLK,  \
        .bclk = BSP_I2S_SCLK,  \
        .ws = BSP_I2S_LCLK,    \
        .dout = BSP_I2S_DOUT,  \
        .din = BSP_I2S_DSIN,   \
        .invert_flags = {      \
            .mclk_inv = false, \
            .bclk_inv = false, \
            .ws_inv = false,   \
        },                     \
    }

/* This configuration is used by default in `bsp_extra_audio_init()` */
#define BSP_I2S_DUPLEX_MONO_CFG(_sample_rate)                                                         \
    {                                                                                                 \
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(_sample_rate),                                          \
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO), \
        .gpio_cfg = BSP_I2S_GPIO_CFG,                                                                 \
    }

esp_err_t bsp_i2c_init(void)
{
    /* I2C was initialized before */
    if (i2c_initialized) {
        return ESP_OK;
    }

    i2c_master_bus_config_t i2c_bus_conf = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .i2c_port = BSP_I2C_NUM,
    };
    BSP_ERROR_CHECK_RETURN_ERR(i2c_new_master_bus(&i2c_bus_conf, &i2c_handle));

    i2c_initialized = true;

    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
    if (i2c_initialized && i2c_handle) {
        BSP_ERROR_CHECK_RETURN_ERR(i2c_del_master_bus(i2c_handle));
        i2c_initialized = false;
    }
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    return i2c_handle;
}

esp_err_t bsp_sdcard_mount(void)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files = 5,
        .allocation_unit_size = 64 * 1024
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create a new on-chip LDO power control driver");
        return ret;
    }
    host.pwr_ctrl_handle = pwr_ctrl_handle;

    const sdmmc_slot_config_t slot_config = {
        /* SD card is connected to Slot 0 pins. Slot 0 uses IO MUX, so not specifying the pins here */
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };

    return esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &bsp_sdcard);
}

esp_err_t bsp_sdcard_unmount(void)
{
    return esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, bsp_sdcard);
}

esp_err_t bsp_spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_BSP_SPIFFS_MOUNT_POINT,
        .partition_label = CONFIG_BSP_SPIFFS_PARTITION_LABEL,
        .max_files = CONFIG_BSP_SPIFFS_MAX_FILES,
#ifdef CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
    };

    esp_err_t ret_val = esp_vfs_spiffs_register(&conf);

    BSP_ERROR_CHECK_RETURN_ERR(ret_val);

    size_t total = 0, used = 0;
    ret_val = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret_val != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret_val));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    return ret_val;
}

esp_err_t bsp_spiffs_unmount(void)
{
    return esp_vfs_spiffs_unregister(CONFIG_BSP_SPIFFS_PARTITION_LABEL);
}

esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config)
{
    if (i2s_tx_chan && i2s_rx_chan) {
        /* Audio was initialized before */
        return ESP_OK;
    }

    /* Setup I2S peripheral */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx_chan, &i2s_rx_chan));

    /* Setup I2S channels */
    const i2s_std_config_t std_cfg_default = BSP_I2S_DUPLEX_MONO_CFG(22050);
    const i2s_std_config_t *p_i2s_cfg = &std_cfg_default;
    if (i2s_config != NULL) {
        p_i2s_cfg = i2s_config;
    }

    if (i2s_tx_chan != NULL) {
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_chan, p_i2s_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_chan));
    }

    if (i2s_rx_chan != NULL) {
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx_chan, p_i2s_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx_chan));
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .tx_handle = i2s_tx_chan,
        .rx_handle = i2s_rx_chan,
    };
    i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);

    return ESP_OK;
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    if (i2s_data_if == NULL) {
        /* Initilize I2C */
        ESP_ERROR_CHECK(bsp_i2c_init());
        /* Configure I2S peripheral and Power Amplifier */
        ESP_ERROR_CHECK(bsp_audio_init(NULL));
    }
    assert(i2s_data_if);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_handle,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(i2c_ctrl_if);

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_TYPE_OUT,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    assert(es8311_dev);

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = es8311_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_dev_cfg);
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    if (i2s_data_if == NULL) {
        /* Initilize I2C */
        ESP_ERROR_CHECK(bsp_i2c_init());
        /* Configure I2S peripheral and Power Amplifier */
        ESP_ERROR_CHECK(bsp_audio_init(NULL));
    }
    assert(i2s_data_if);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_handle,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(i2c_ctrl_if);

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };

    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    assert(es8311_dev);

    esp_codec_dev_cfg_t codec_es8311_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es8311_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_es8311_dev_cfg);
}

// Bit number used to represent command and parameter
#define LCD_LEDC_CH            CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH

esp_err_t bsp_display_brightness_init(void)
{
    if (BSP_LCD_BACKLIGHT == GPIO_NUM_NC) {
        return ESP_OK; // No backlight GPIO — skip LEDC init
    }
    // Setup LEDC peripheral for PWM backlight control
    const ledc_channel_config_t LCD_backlight_channel = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = 1,
        .duty = 0,
        .hpoint = 0
    };
    const ledc_timer_config_t LCD_backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = 1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };

    BSP_ERROR_CHECK_RETURN_ERR(ledc_timer_config(&LCD_backlight_timer));
    BSP_ERROR_CHECK_RETURN_ERR(ledc_channel_config(&LCD_backlight_channel));
    return ESP_OK;
}

esp_err_t bsp_display_brightness_deinit(void)
{
    const ledc_timer_config_t LCD_backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = 1,
        .deconfigure = 1
    };
    BSP_ERROR_CHECK_RETURN_ERR(ledc_timer_pause(LEDC_LOW_SPEED_MODE, 1));
    BSP_ERROR_CHECK_RETURN_ERR(ledc_timer_config(&LCD_backlight_timer));
    return ESP_OK;
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    if (brightness_percent < 0) {
        brightness_percent = 0;
    }

    ESP_LOGI(TAG, "Setting LCD backlight: %d%%", brightness_percent);
    uint32_t duty_cycle = (1023 * brightness_percent) / 100; // LEDC resolution set to 10bits, thus: 100% = 1023
    BSP_ERROR_CHECK_RETURN_ERR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH, duty_cycle));
    BSP_ERROR_CHECK_RETURN_ERR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH));
    return ESP_OK;
}

esp_err_t bsp_display_backlight_off(void)
{
    return bsp_display_brightness_set(0);
}

esp_err_t bsp_display_backlight_on(void)
{
    return bsp_display_brightness_set(100);
}

static esp_err_t bsp_enable_dsi_phy_power(void)
{
#if BSP_MIPI_DSI_PHY_PWR_LDO_CHAN > 0
    // Turn on the power for MIPI DSI PHY, so it can go from "No Power" state to "Shutdown" state
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = BSP_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &disp_phy_pwr_chan), TAG, "Acquire LDO channel for DPHY failed");
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
#endif // BSP_MIPI_DSI_PHY_PWR_LDO_CHAN > 0

    return ESP_OK;
}

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io)
{
    esp_err_t ret = ESP_OK;
    bsp_lcd_handles_t handles;
    ret = bsp_display_new_with_handles(config, &handles);

    *ret_panel = handles.panel;
    *ret_io = handles.io;

    return ret;
}

esp_err_t bsp_display_new_with_handles(const bsp_display_config_t *config, bsp_lcd_handles_t *ret_handles)
{
    esp_err_t ret = ESP_OK;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t disp_panel = NULL;

    ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "Brightness init failed");

#ifdef BSP_LCD_PANEL_TC358762
    // For TC358762: init I2C and run ATTINY88 power-on BEFORE enabling DSI PHY
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C init for ATTINY88 failed");
    ESP_RETURN_ON_ERROR(tc358762_power_on(), TAG, "TC358762 power-on failed");
#endif

    ESP_RETURN_ON_ERROR(bsp_enable_dsi_phy_power(), TAG, "DSI PHY power failed");

    /* create MIPI DSI bus first, it will initialize the DSI PHY as well */
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = BSP_LCD_MIPI_DSI_LANE_NUM,
        .phy_clk_src = config->dsi_bus.phy_clk_src,
        .lane_bit_rate_mbps = config->dsi_bus.lane_bit_rate_mbps,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus), TAG, "New DSI bus init failed");

#if !CONFIG_BSP_LCD_TYPE_HDMI && !defined(BSP_LCD_PANEL_TC358762)
    if (config->hdmi_resolution != BSP_HDMI_RES_NONE) {
        ESP_LOGW(TAG, "Please select HDMI in menuconfig, if you want to use it.");
    }

    ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
    // we use DBI interface to send LCD commands and parameters
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,   // according to the LCD spec
        .lcd_param_bits = 8, // according to the LCD spec
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io), err, TAG, "New panel IO failed");
#endif

#if CONFIG_BSP_LCD_TYPE_1024_600
    // create EK79007 control panel
    ESP_LOGI(TAG, "Install EK79007 LCD control panel");

#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
    esp_lcd_dpi_panel_config_t dpi_config = EK79007_1024_600_PANEL_60HZ_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB888);
#else
    esp_lcd_dpi_panel_config_t dpi_config = EK79007_1024_600_PANEL_60HZ_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
#endif
    dpi_config.num_fbs = CONFIG_BSP_LCD_DPI_BUFFER_NUMS;

    ek79007_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    esp_lcd_panel_dev_config_t lcd_dev_config = {
        .bits_per_pixel = 16,
        .rgb_ele_order = BSP_LCD_COLOR_SPACE,
        .reset_gpio_num = BSP_LCD_RST,
        .vendor_config = &vendor_config,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_ek79007(io, &lcd_dev_config, &disp_panel), err, TAG, "New LCD panel EK79007 failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(disp_panel), err, TAG, "LCD panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(disp_panel), err, TAG, "LCD panel init failed");
#elif CONFIG_BSP_LCD_TYPE_1280_800

#ifdef BSP_LCD_PANEL_TC358762
    // TC358762 DSI-to-DPI bridge path.
    // Power-on and I2C already done above. Now create DPI panel directly (no panel IC driver).
    ESP_LOGI(TAG, "TC358762: Creating raw DPI panel (800x480, 26MHz, RPi stable timing)");

    // Create and immediately delete a DBI IO to latch LP speed settings in the DSI controller.
    // The LP speed configuration persists after IO deletion.
    {
        esp_lcd_panel_io_handle_t lp_io = NULL;
        esp_lcd_dbi_io_config_t lp_cfg = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
        ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &lp_cfg, &lp_io), err, TAG, "LP DBI IO failed");
        esp_lcd_panel_io_del(lp_io);
    }

    esp_lcd_dpi_panel_config_t dpi_config = {
        .dpi_clk_src          = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz   = 26,
        .virtual_channel      = 0,
        // RGB888 end-to-end (matches embenix rpi-7inch-v1 reference) → fixes the horizontal wrap.
        .pixel_format         = LCD_COLOR_PIXEL_FORMAT_RGB888,
        .num_fbs              = CONFIG_BSP_LCD_DPI_BUFFER_NUMS,
        .video_timing = {
            .h_size            = 800,
            .v_size            = 480,
            .hsync_back_porch  = RPI_HBP,
            .hsync_pulse_width = RPI_HSW,
            .hsync_front_porch = RPI_HFP,
            .vsync_back_porch  = RPI_VBP,
            .vsync_pulse_width = RPI_VSW,
            .vsync_front_porch = RPI_VFP,
        },
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_dpi(mipi_dsi_bus, &dpi_config, &disp_panel), err, TAG, "DPI panel failed");

    // Override to NON-BURST with sync pulses — required by TC358762 (mirrors Linux MIPI_DSI_MODE_VIDEO_SYNC_PULSE).
    // esp_lcd_new_panel_dpi() hardcodes BURST; we override before panel_init() so it takes effect.
    {
        rpi_dsi_bus_priv_t *priv = (rpi_dsi_bus_priv_t *)mipi_dsi_bus;
        mipi_dsi_host_ll_dpi_set_video_burst_type(priv->hal.host, MIPI_DSI_LL_VIDEO_NON_BURST_WITH_SYNC_PULSES);
        mipi_dsi_host_ll_dpi_enable_frame_ack(priv->hal.host, false);
        ESP_LOGI(TAG, "TC358762: DSI set to NON-BURST with sync pulses");
    }

    // Start DPI video stream.
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(disp_panel), err, TAG, "DPI panel init failed");

    // Force continuous HS clock. TC358762 FLL needs a stable HS clock reference.
    // Data lanes still go LP during blanking so LP Generic Long Write packets can be sent
    // to TC358762 during the vertical blanking window.
    {
        rpi_dsi_bus_priv_t *priv = (rpi_dsi_bus_priv_t *)mipi_dsi_bus;
        mipi_dsi_host_ll_set_clock_lane_state(priv->hal.host, MIPI_DSI_LL_CLOCK_LANE_STATE_HS);
        mipi_dsi_host_ll_enable_cmd_ack(priv->hal.host, false);
        ESP_LOGI(TAG, "TC358762: DSI clock forced to continuous HS, CMD ACK disabled");
    }

    // Release TC358762 from reset and configure bridge registers via LP during blanking.
    // Must happen AFTER panel_init() so the HS clock reference is running.
    ESP_GOTO_ON_ERROR(tc358762_release_and_wake(), err, TAG, "TC358762 release/wake failed");
    ESP_GOTO_ON_ERROR(tc358762_bridge_init(mipi_dsi_bus), err, TAG, "TC358762 bridge init failed");

    // Release touch controller from reset, then set full brightness via ATTINY88 PWM.
    attiny_write(ATTINY_REG_PORTC,
        ATTINY_PC_LED_EN | ATTINY_PC_RST_TP_N | ATTINY_PC_RST_LCD_N | ATTINY_PC_RST_BRIDGE_N);
    attiny_write(ATTINY_REG_PWM, 255);
    ESP_LOGI(TAG, "TC358762: Backlight set to full brightness via ATTINY88");

    io = NULL; // No persistent DBI IO for raw DPI panel

#else // not BSP_LCD_PANEL_TC358762
    // DPI timing — embenix observed stable values for RPi-style 1-lane 800x480 panel on ESP32-P4.
    // Refresh = 25.98MHz / ((800+210+2+46) * (480+22+20+4)) = 25.98M / (1058 * 526) ≈ 46.7 Hz
    // (Lower refresh is fine; the panel is stable at this rate per embenix testing)
    esp_lcd_dpi_panel_config_t dpi_config = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 26,
        .virtual_channel = 0,
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888,
#else
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
#endif
        .num_fbs = CONFIG_BSP_LCD_DPI_BUFFER_NUMS,
        .video_timing = {
            .h_size = 800,
            .v_size = 480,
            .hsync_back_porch  = RPI_HBP,
            .hsync_pulse_width = RPI_HSW,
            .hsync_front_porch = RPI_HFP,
            .vsync_back_porch  = RPI_VBP,
            .vsync_pulse_width = RPI_VSW,
            .vsync_front_porch = RPI_VFP,
        },
    };

#ifdef BSP_LCD_PANEL_MINIMAL
    ESP_LOGI(TAG, "Install HX8399 LCD control panel (minimal init: SLPOUT + DISPON only)");
    static const hx8399_lcd_init_cmd_t minimal_init_cmds[] = {
        {0x11, NULL, 0, 120}, // SLPOUT - exit sleep
        {0x29, NULL, 0, 20},  // DISPON - display on
    };
    hx8399_vendor_config_t vendor_config = {
        .init_cmds = minimal_init_cmds,
        .init_cmds_size = sizeof(minimal_init_cmds) / sizeof(minimal_init_cmds[0]),
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num = BSP_LCD_MIPI_DSI_LANE_NUM,
        },
    };
    const esp_lcd_panel_dev_config_t lcd_dev_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = BSP_LCD_COLOR_SPACE,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_hx8399(io, &lcd_dev_config, &disp_panel), err, TAG, "New LCD panel failed");
#elif defined(BSP_LCD_PANEL_HX8399)
    ESP_LOGI(TAG, "Install HX8399 LCD control panel");
    hx8399_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num = BSP_LCD_MIPI_DSI_LANE_NUM,
        },
    };
    const esp_lcd_panel_dev_config_t lcd_dev_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = BSP_LCD_COLOR_SPACE,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_hx8399(io, &lcd_dev_config, &disp_panel), err, TAG, "New LCD panel HX8399 failed");
#else
    ESP_LOGI(TAG, "Install ILI9881C LCD control panel");
    ili9881c_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num = BSP_LCD_MIPI_DSI_LANE_NUM,
        },
    };
    const esp_lcd_panel_dev_config_t lcd_dev_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = BSP_LCD_COLOR_SPACE,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_ili9881c(io, &lcd_dev_config, &disp_panel), err, TAG, "New LCD panel ILI9881C failed");
#endif
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(disp_panel), err, TAG, "LCD panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(disp_panel), err, TAG, "LCD panel init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(disp_panel, true), err, TAG, "LCD panel ON failed");
#endif // BSP_LCD_PANEL_TC358762

#elif CONFIG_BSP_LCD_TYPE_HDMI

#if !CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
#error The color format must be RGB888 in HDMI display type!
#endif
    ESP_LOGI(TAG, "Install MIPI DSI HDMI control panel");
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C init failed");

    /* Main IO */
    esp_lcd_panel_io_i2c_config_t io_config = LT8912B_IO_CFG(CONFIG_BSP_I2C_CLK_SPEED_HZ, LT8912B_IO_I2C_MAIN_ADDRESS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_handle, &io_config, &io));

    /* CEC DSI IO */
    esp_lcd_panel_io_handle_t io_cec_dsi = NULL;
    esp_lcd_panel_io_i2c_config_t io_config_cec = LT8912B_IO_CFG(CONFIG_BSP_I2C_CLK_SPEED_HZ, LT8912B_IO_I2C_CEC_ADDRESS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_handle, &io_config_cec, &io_cec_dsi));

    /* AVI IO */
    esp_lcd_panel_io_handle_t io_avi = NULL;
    esp_lcd_panel_io_i2c_config_t io_config_avi = LT8912B_IO_CFG(CONFIG_BSP_I2C_CLK_SPEED_HZ, LT8912B_IO_I2C_AVI_ADDRESS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_handle, &io_config_avi, &io_avi));

    const esp_lcd_dpi_panel_config_t dpi_configs[] = {
        LT8912B_800x600_PANEL_60HZ_DPI_CONFIG_WITH_FBS(CONFIG_BSP_LCD_DPI_BUFFER_NUMS),
        LT8912B_1024x768_PANEL_60HZ_DPI_CONFIG_WITH_FBS(CONFIG_BSP_LCD_DPI_BUFFER_NUMS),
        LT8912B_1280x720_PANEL_60HZ_DPI_CONFIG_WITH_FBS(CONFIG_BSP_LCD_DPI_BUFFER_NUMS),
        LT8912B_1280x800_PANEL_60HZ_DPI_CONFIG_WITH_FBS(CONFIG_BSP_LCD_DPI_BUFFER_NUMS),
        LT8912B_1920x1080_PANEL_30HZ_DPI_CONFIG_WITH_FBS(CONFIG_BSP_LCD_DPI_BUFFER_NUMS)
    };

    const esp_lcd_panel_lt8912b_video_timing_t video_timings[] = {
        ESP_LCD_LT8912B_VIDEO_TIMING_800x600_60Hz(),
        ESP_LCD_LT8912B_VIDEO_TIMING_1024x768_60Hz(),
        ESP_LCD_LT8912B_VIDEO_TIMING_1280x720_60Hz(),
        ESP_LCD_LT8912B_VIDEO_TIMING_1280x800_60Hz(),
        ESP_LCD_LT8912B_VIDEO_TIMING_1920x1080_30Hz()
    };
    lt8912b_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .lane_num = BSP_LCD_MIPI_DSI_LANE_NUM,
        },
    };

    /* DPI config */
    switch (config->hdmi_resolution) {
    case BSP_HDMI_RES_800x600:
        ESP_LOGI(TAG, "HDMI configuration for 800x600@60HZ");
        vendor_config.mipi_config.dpi_config = &dpi_configs[0];
        memcpy(&vendor_config.video_timing, &video_timings[0], sizeof(esp_lcd_panel_lt8912b_video_timing_t));
        break;
    case BSP_HDMI_RES_1024x768:
        ESP_LOGI(TAG, "HDMI configuration for 1024x768@60HZ");
        vendor_config.mipi_config.dpi_config = &dpi_configs[1];
        memcpy(&vendor_config.video_timing, &video_timings[1], sizeof(esp_lcd_panel_lt8912b_video_timing_t));
        break;
    case BSP_HDMI_RES_1280x720:
        ESP_LOGI(TAG, "HDMI configuration for 1280x720@60HZ");
        vendor_config.mipi_config.dpi_config = &dpi_configs[2];
        memcpy(&vendor_config.video_timing, &video_timings[2], sizeof(esp_lcd_panel_lt8912b_video_timing_t));
        break;
    case BSP_HDMI_RES_1280x800:
        ESP_LOGI(TAG, "HDMI configuration for 1280x800@60HZ");
        vendor_config.mipi_config.dpi_config = &dpi_configs[3];
        memcpy(&vendor_config.video_timing, &video_timings[3], sizeof(esp_lcd_panel_lt8912b_video_timing_t));
        break;
    case BSP_HDMI_RES_1920x1080:
        ESP_LOGI(TAG, "HDMI configuration for 1920x1080@30HZ");
        vendor_config.mipi_config.dpi_config = &dpi_configs[4];
        memcpy(&vendor_config.video_timing, &video_timings[4], sizeof(esp_lcd_panel_lt8912b_video_timing_t));
        break;
    default:
        ESP_LOGE(TAG, "Unsupported display type (%d)", config->hdmi_resolution);
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 24,
        .rgb_ele_order = BSP_LCD_COLOR_SPACE,
        .reset_gpio_num = BSP_LCD_RST,
        .vendor_config = &vendor_config,
    };
    const esp_lcd_panel_lt8912b_io_t io_all = {
        .main = io,
        .cec_dsi = io_cec_dsi,
        .avi = io_avi,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_lt8912b(&io_all, &panel_config, &disp_panel));
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(disp_panel), err, TAG, "LCD panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(disp_panel), err, TAG, "LCD panel init failed");

#endif //CONFIG_BSP_LCD_TYPE_

    /* Return all handles */
    ret_handles->io = io;
    disp_handles.io = io;
#if CONFIG_BSP_LCD_TYPE_HDMI
    ret_handles->io_cec = io_cec_dsi;
    disp_handles.io_cec = io_cec_dsi;
    ret_handles->io_avi = io_avi;
    disp_handles.io_avi = io_avi;
#endif
    ret_handles->mipi_dsi_bus = mipi_dsi_bus;
    disp_handles.mipi_dsi_bus = mipi_dsi_bus;
    ret_handles->panel = disp_panel;
    disp_handles.panel = disp_panel;
    ret_handles->control = NULL;
    disp_handles.control = NULL;

    ESP_LOGI(TAG, "Display initialized");

    return ret;

err:
    bsp_display_delete();
    return ret;
}

void bsp_display_delete(void)
{
    if (disp_handles.panel) {
        esp_lcd_panel_del(disp_handles.panel);
        disp_handles.panel = NULL;
    }
    if (disp_handles.io) {
        esp_lcd_panel_io_del(disp_handles.io);
        disp_handles.io = NULL;
    }
#if CONFIG_BSP_LCD_TYPE_HDMI
    if (disp_handles.io_cec) {
        esp_lcd_panel_io_del(disp_handles.io_cec);
        disp_handles.io_cec = NULL;
    }
    if (disp_handles.io_avi) {
        esp_lcd_panel_io_del(disp_handles.io_avi);
        disp_handles.io_avi = NULL;
    }
#endif
    if (disp_handles.mipi_dsi_bus) {
        esp_lcd_del_dsi_bus(disp_handles.mipi_dsi_bus);
        disp_handles.mipi_dsi_bus = NULL;
    }

    if (disp_phy_pwr_chan) {
        esp_ldo_release_channel(disp_phy_pwr_chan);
        disp_phy_pwr_chan = NULL;
    }

    bsp_display_brightness_deinit();
}

#if !CONFIG_BSP_LCD_TYPE_HDMI
esp_err_t bsp_touch_new(const bsp_touch_config_t *config, esp_lcd_touch_handle_t *ret_touch)
{
    /* Initilize I2C */
    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());

    /* Initialize touch */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST, // Shared with LCD reset
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
#if CONFIG_BSP_LCD_TYPE_1024_600
            .mirror_x = 1,
            .mirror_y = 1,
#else
            .mirror_x = 0,
            .mirror_y = 0,
#endif
        },
    };
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle), TAG, "");
    return esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, ret_touch);
}

void bsp_touch_delete(void)
{
    if (tp) {
        esp_lcd_touch_del(tp);
    }
    if (tp_io_handle) {
        esp_lcd_panel_io_del(tp_io_handle);
        tp_io_handle = NULL;
    }
}
#endif //!CONFIG_BSP_LCD_TYPE_HDMI

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg)
{
    assert(cfg != NULL);
    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_new_with_handles(&cfg->hw_cfg, &disp_handles));

    uint32_t display_hres = 0;
    uint32_t display_vres = 0;
#if CONFIG_BSP_LCD_TYPE_HDMI
    switch (cfg->hw_cfg.hdmi_resolution) {
    case BSP_HDMI_RES_800x600:
        display_hres = 800;
        display_vres = 600;
        break;
    case BSP_HDMI_RES_1024x768:
        display_hres = 1024;
        display_vres = 768;
        break;
    case BSP_HDMI_RES_1280x720:
        display_hres = 1280;
        display_vres = 720;
        break;
    case BSP_HDMI_RES_1280x800:
        display_hres = 1280;
        display_vres = 800;
        break;
    case BSP_HDMI_RES_1920x1080:
        display_hres = 1920;
        display_vres = 1080;
        break;
    default:
        ESP_LOGE(TAG, "Unsupported HDMI resolution");
    }
#else
    display_hres = BSP_LCD_H_RES;
    display_vres = BSP_LCD_V_RES;
#endif

    ESP_LOGI(TAG, "Display resolution %ldx%ld", display_hres, display_vres);

    /* Add LCD screen */
    ESP_LOGD(TAG, "Add LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = disp_handles.io,
        .panel_handle = disp_handles.panel,
        .control_handle = disp_handles.control,
        .buffer_size = cfg->buffer_size,
        .double_buffer = cfg->double_buffer,
        .hres = display_hres,
        .vres = display_vres,
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = true,
        },
#if LVGL_VERSION_MAJOR >= 9
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
        .color_format = LV_COLOR_FORMAT_RGB888,
#else
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
#endif
        .flags = {
            .buff_dma = cfg->flags.buff_dma,
            .buff_spiram = cfg->flags.buff_spiram,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = (BSP_LCD_BIGENDIAN ? true : false),
#endif
#if CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR
            .sw_rotate = false,                /* Avoid tearing is not supported for SW rotation */
#else
            .sw_rotate = cfg->flags.sw_rotate, /* Only SW rotation is supported for 90° and 270° */
#endif
#if CONFIG_BSP_DISPLAY_LVGL_FULL_REFRESH
            .full_refresh = true,
#elif CONFIG_BSP_DISPLAY_LVGL_DIRECT_MODE
            .direct_mode = true,
#endif
        }
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
#if CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR
            .avoid_tearing = true,
#else
            .avoid_tearing = false,
#endif
        }
    };

    return lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
}

#if !CONFIG_BSP_LCD_TYPE_HDMI
static lv_indev_t *bsp_display_indev_init(lv_display_t *disp)
{
    BSP_ERROR_CHECK_RETURN_NULL(bsp_touch_new(NULL, &tp));
    assert(tp);

    /* Add touch input (for selected screen) */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = tp,
    };

    return lvgl_port_add_touch(&touch_cfg);
}
#endif //!CONFIG_BSP_LCD_TYPE_HDMI

lv_display_t *bsp_display_start(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .hw_cfg = {
#if CONFIG_BSP_LCD_TYPE_HDMI
#if CONFIG_BSP_LCD_HDMI_800x600_60HZ
            .hdmi_resolution = BSP_HDMI_RES_800x600,
#elif CONFIG_BSP_LCD_HDMI_1280x720_60HZ
            .hdmi_resolution = BSP_HDMI_RES_1280x720,
#elif CONFIG_BSP_LCD_HDMI_1280x800_60HZ
            .hdmi_resolution = BSP_HDMI_RES_1280x800,
#elif CONFIG_BSP_LCD_HDMI_1920x1080_30HZ
            .hdmi_resolution = BSP_HDMI_RES_1920x1080,
#endif
#else
            .hdmi_resolution = BSP_HDMI_RES_NONE,
#endif
            .dsi_bus = {
                .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
                .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
            }
        },
        .flags = {
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
            .buff_dma = false,
#else
            .buff_dma = true,
#endif
            .buff_spiram = false,
            .sw_rotate = true,
        }
    };
    return bsp_display_start_with_config(&cfg);
}

lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg)
{
    lv_display_t *disp;

    assert(cfg != NULL);
    BSP_ERROR_CHECK_RETURN_NULL(lvgl_port_init(&cfg->lvgl_port_cfg));

    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_brightness_init());

    BSP_NULL_CHECK(disp = bsp_display_lcd_init(cfg), NULL);
#if !CONFIG_BSP_LCD_TYPE_HDMI
    BSP_NULL_CHECK(disp_indev = bsp_display_indev_init(disp), NULL);
#endif
    return disp;
}

void bsp_display_stop(lv_display_t *display)
{
    /* Deinit LVGL */
#if !CONFIG_BSP_LCD_TYPE_HDMI
    lvgl_port_remove_touch(disp_indev);
#endif
    lvgl_port_remove_disp(display);
    lvgl_port_deinit();

#if !CONFIG_BSP_LCD_TYPE_HDMI
    /* Deinit touch */
    bsp_touch_delete();
#endif

    /* Deinit display */
    bsp_display_delete();

    /* Deinit I2C if initialized */
    bsp_i2c_deinit();
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return disp_indev;
}

void bsp_display_rotate(lv_display_t *disp, lv_disp_rotation_t rotation)
{
    lv_disp_set_rotation(disp, rotation);
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    lvgl_port_unlock();
}

#endif // (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

static void usb_lib_task(void *arg)
{
    while (1) {
        // Start handling system events
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB: All devices freed");
            // Continue handling USB events to allow device reconnection
            // The only way this task can be stopped is by calling bsp_usb_host_stop()
        }
    }
}

esp_err_t bsp_usb_host_start(bsp_usb_host_power_mode_t mode, bool limit_500mA)
{
    //Install USB Host driver. Should only be called once in entire application
    ESP_LOGI(TAG, "Installing USB Host");
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    BSP_ERROR_CHECK_RETURN_ERR(usb_host_install(&host_config));

    // Create a task that will handle USB library events
    if (xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, &usb_host_task) != pdTRUE) {
        ESP_LOGE(TAG, "Creating USB host lib task failed");
        abort();
    }

    return ESP_OK;
}

esp_err_t bsp_usb_host_stop(void)
{
    usb_host_uninstall();
    if (usb_host_task) {
        vTaskSuspend(usb_host_task);
        vTaskDelete(usb_host_task);
    }
    return ESP_OK;
}
