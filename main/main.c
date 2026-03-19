/***********************************************************************
 *  Photodiode power-meter + SSD1306 OLED (big centred read-out)
 *  ESP32 DevKit-C V4 – ESP-IDF v5.4.x
 *  Legacy ADC driver enabled (activate both ticks in menuconfig)
 **********************************************************************/
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "driver/i2c.h"
#include "esp_log.h"

/* ------------ GPIOs: SSD1306 I²C + buttons ------------------------ */
#define I2C_PORT I2C_NUM_0
#define OLED_SDA_GPIO GPIO_NUM_21
#define OLED_SCL_GPIO GPIO_NUM_22
#define OLED_ADDR 0x3C /* SSD1306 I2C addr */

/* Four momentary buttons, active-low, with 10 kΩ pull-ups to 3.3 V
 * (change the GPIOs to suit your PCB) */
#define BTN_K1 GPIO_NUM_25 // UP
#define BTN_K2 GPIO_NUM_26 // DOWN
#define BTN_K3 GPIO_NUM_14 // MENU
#define BTN_K4 GPIO_NUM_27 // OK/ESC

#include "font6x8.h" /* 6×8 ASCII font */

/* ------------ Logging tag ----------------------------------------- */
static const char *TAG = "PD_OLED";

/* ------------ OLED helpers ---------------------------------------- */
static esp_err_t i2c_master_init(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .sda_pullup_en = true,
        .scl_pullup_en = true,
        .master.clk_speed = 400000};
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &cfg));
    return i2c_driver_install(I2C_PORT, cfg.mode, 0, 0, 0);
}
static esp_err_t ssd1306_write(uint8_t ctrl, const uint8_t *data, size_t len)
{
    if (!len)
        return ESP_OK;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (OLED_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, ctrl, true);
    i2c_master_write(cmd, (uint8_t *)data, len, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}
static inline esp_err_t ssd1306_cmd(uint8_t c) { return ssd1306_write(0x00, &c, 1); }

static void ssd1306_init(void)
{
    const uint8_t init[] = {0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14,
                            0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0x7F, 0xD9, 0x22,
                            0xDB, 0x20, 0xA4, 0xA6, 0xAF};
    for (size_t i = 0; i < sizeof(init); ++i)
    {
        ssd1306_cmd(init[i]);
    }
}
static void ssd1306_clear(void)
{
    uint8_t zero[128] = {0};
    ssd1306_cmd(0x22);
    ssd1306_cmd(0);
    ssd1306_cmd(7);
    ssd1306_cmd(0x21);
    ssd1306_cmd(0);
    ssd1306_cmd(127);
    for (int p = 0; p < 8; ++p)
    {
        ssd1306_write(0x40, zero, 128);
    }
}

/* ------------ Text drawing ---------------------------------------- */
static void ssd1306_draw_text(uint8_t page, uint8_t col, const char *s)
{
    ssd1306_cmd(0xB0 | page);
    ssd1306_cmd(0x00 | (col & 0x0F));
    ssd1306_cmd(0x10 | (col >> 4));
    while (*s)
    {
        char c = *s++;
        uint8_t idx = (c >= 32 && c <= 127) ? c - 32 : '?' - 32;
        ssd1306_write(0x40, &ssd1306xled_font6x8[idx * 6], 6);
    }
}

void ssd1306_draw_text2x_center(const char *s)
{
    int len = strlen(s);
    int tot_w = len * 6 * 2;
    int start_c = (128 - tot_w) / 2;
    int start_p = 3;

    uint8_t page_buf[2][128] = {{0}};
    int col_off = start_c;
    for (int i = 0; i < len;)
    {
        uint8_t idx;
        if ((uint8_t)s[i] == 0xC2 && (uint8_t)s[i + 1] == 0xB5)
        {
            idx = 'u' - 32;
            i += 2;
        }
        else
        {
            idx = (s[i] >= 32 && s[i] <= 127) ? s[i] - 32 : '?' - 32;
            i++;
        }
        const uint8_t *glyph = &ssd1306xled_font6x8[idx * 6];
        for (int cx = 0; cx < 6; ++cx)
        {
            uint8_t b = glyph[cx];
            uint16_t mask = 0;
            for (int bit = 0; bit < 8; ++bit)
            {
                if (b & (1 << bit))
                    mask |= (3 << (2 * bit));
            }
            if (col_off < 128)
            {
                page_buf[0][col_off] = mask & 0xFF;
                page_buf[1][col_off] = mask >> 8;
            }
            if (++col_off < 128)
            {
                page_buf[0][col_off] = mask & 0xFF;
                page_buf[1][col_off] = mask >> 8;
            }
            ++col_off;
        }
    }
    for (int p = 0; p < 2; ++p)
    {
        ssd1306_cmd(0xB0 | (start_p + p));
        ssd1306_cmd(0x00);
        ssd1306_cmd(0x10);
        ssd1306_write(0x40, page_buf[p], 128);
    }
}

/* ------------ Wavelength menu data ---------------------------------- */
typedef struct
{
    const char *label;
    uint16_t nm;
    float a_per_w;
} wl_item_t;
static const wl_item_t wl_menu[] = {
    {"Red   650 nm", 650, 0.32f},
    {"Blue  450 nm", 450, 0.18f},
    {"Green 520 nm", 520, 0.25f},
};
#define WL_ITEMS (sizeof(wl_menu) / sizeof(wl_menu[0]))

/* ------------ Photodiode / ADC -------------------------------------- */
#define ADC_CH ADC_CHANNEL_4
#define GPIO_MOSFET GPIO_NUM_18
#define VREF_MV 3300
#define R_HIGH 43000.0f
#define R_PARALLEL 8200.0f
#define R_LOW (R_HIGH * R_PARALLEL / (R_HIGH + R_PARALLEL))
#define RAW_UPPER 3300
#define RAW_LOWER 500
#define SAMPLES 32
static esp_adc_cal_characteristics_t adc_chars;
static float g_responsivity = 0.32f;

/* ------------ Button handling -------------------------------------- */
typedef struct
{
    gpio_num_t pin;
    uint8_t last;
} btn_t;
static btn_t buttons[] = {{BTN_K1, 1}, {BTN_K2, 1}, {BTN_K3, 1}, {BTN_K4, 1}};
static void gpio_init_buttons(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BTN_K1) | (1ULL << BTN_K2) | (1ULL << BTN_K3) | (1ULL << BTN_K4),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&io);
}
static bool btn_clicked(int i)
{
    uint8_t lvl = gpio_get_level(buttons[i].pin);
    bool clicked = (buttons[i].last == 1 && lvl == 0);
    buttons[i].last = lvl;
    return clicked;
}

/* ------------ Range switching -------------------------------------- */
typedef enum
{
    RANGE_HIGH,
    RANGE_LOW
} range_t;
static range_t cur_range;
static float cur_shunt;
static void apply_range(range_t r)
{
    cur_range = r;
    if (r == RANGE_HIGH)
    {
        gpio_set_level(GPIO_MOSFET, 0);
        cur_shunt = R_HIGH;
    }
    else
    {
        gpio_set_level(GPIO_MOSFET, 1);
        cur_shunt = R_LOW;
    }
}
static int adc_read(void)
{
    int32_t acc = 0;
    for (int i = 0; i < SAMPLES; ++i)
        acc += adc1_get_raw(ADC_CH);
    return acc / SAMPLES;
}

/* ------------ UI state --------------------------------------------- */
typedef enum
{
    UI_MEAS,
    UI_MENU
} ui_t;
static ui_t ui_state = UI_MEAS;
static int menu_idx = 0;
static void draw_menu(int sel)
{
    ssd1306_clear();
    uint8_t p = 2;
    for (int i = 0; i < WL_ITEMS; ++i)
    {
        char line[20];
        snprintf(line, sizeof(line), "%c %s", i == sel ? '>' : ' ', wl_menu[i].label);
        ssd1306_draw_text(p + i, 0, line);
    }
}

/* ------------ Main task -------------------------------------------- */
static void pd_task(void *arg)
{
    apply_range(RANGE_HIGH);
    ssd1306_clear();

    // enable INFO logging for our tag
    esp_log_level_set(TAG, ESP_LOG_INFO);

    while (1)
    {
        if (ui_state == UI_MENU)
        {
            if (btn_clicked(0))
            {
                menu_idx = (menu_idx + WL_ITEMS - 1) % WL_ITEMS;
                draw_menu(menu_idx);
            }
            if (btn_clicked(1))
            {
                menu_idx = (menu_idx + 1) % WL_ITEMS;
                draw_menu(menu_idx);
            }
            if (btn_clicked(3))
            {
                g_responsivity = wl_menu[menu_idx].a_per_w;
                ui_state = UI_MEAS;
                ssd1306_clear();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (btn_clicked(2))
        {
            ui_state = UI_MENU;
            menu_idx = 0;
            draw_menu(menu_idx);
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        int raw = adc_read();
        uint32_t mv = esp_adc_cal_raw_to_voltage(raw, &adc_chars);
        float volts = mv / 1000.0f;

        // auto-range
        if (cur_range == RANGE_HIGH && raw > RAW_UPPER)
        {
            apply_range(RANGE_LOW);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (cur_range == RANGE_LOW && raw < RAW_LOWER)
        {
            apply_range(RANGE_HIGH);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        float iA = volts / cur_shunt;
        float p_uW = (iA / g_responsivity) * 1e6f;

        // log RAW, voltage, and power
        ESP_LOGI(TAG,
                 "[%s] RAW=%d  V=%.3f V  P=%.2f µW  (R=%.2f A/W)",
                 cur_range == RANGE_HIGH ? "43kΩ" : "6.9kΩ",
                 raw, volts, p_uW, g_responsivity);

        char buf[16];
        snprintf(buf, sizeof(buf), "%.2fµW", p_uW);
        ssd1306_draw_text2x_center(buf);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ------------ Init / main ------------------------------------------ */
static void adc_init(void)
{
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC_CH, ADC_ATTEN_DB_11);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, VREF_MV, &adc_chars);
}
static void gpio_init_mosfet(void)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << GPIO_MOSFET, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
}

void app_main(void)
{
    ESP_ERROR_CHECK(i2c_master_init());
    ssd1306_init();
    gpio_init_buttons();
    gpio_init_mosfet();
    adc_init();
    xTaskCreatePinnedToCore(pd_task, "pd_task", 4096, NULL, 5, NULL, 1);
}
