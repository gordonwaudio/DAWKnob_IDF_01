/*
 * DAWKnob IDF - Main Application
 * Converted from DawKnob_03 Arduino sketch to ESP-IDF framework.
 *
 * Hardware:
 *  - Rotary encoder: PIN_A (GPIO33, clock), PIN_B (GPIO32, DT)
 *  - RE push button:  GPIO25 (pull-up)
 *  - WiFi/OTA button: GPIO27
 *  - Right-mode btn:  GPIO26
 *  - Sensitivity pot: GPIO35 (ADC1_CH7)
 *  - LEDs: GPIO2 (internal), GPIO21 (BT), GPIO18 (WiFi), GPIO19 (active mode)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "ble_mouse.h"
#include "wifi_ota.h"

#define TAG "DawKnob"

// GPIO Pin Definitions
#define PIN_A           GPIO_NUM_33
#define PIN_B           GPIO_NUM_32
#define BUTTON_RE       GPIO_NUM_25
#define LED_INTERNAL    GPIO_NUM_2
#define LED_BLUETOOTH   GPIO_NUM_21
#define LED_WIFI        GPIO_NUM_18
#define LED_ACTIVE      GPIO_NUM_19
#define BUTTON_WIFI     GPIO_NUM_27
#define BUTTON_RM       GPIO_NUM_26
#define SENS_ADC_CH     ADC_CHANNEL_7   // GPIO35

// Timing constants (ms)
#define PULSE_GAP_MS    20
#define DEBOUNCE_MS     200
#define KNOB_SENS_MS    1000

// Filter period (matches original samplePeriod = 1000.0 using micros())
#define SAMPLE_PERIOD   1000.0f

// Rotary encoder (modified by ISR)
static volatile int  s_rotation_counter = 0;
static volatile bool s_rotary_flag = false;

// Application state
static int   s_prev_rotation = 0;
static int   s_movement_scale = 10;
static bool  s_mouse_right_mode = true;   // true = right button, false = left
static bool  s_mouse_pressed = false;
static int   s_mouse_move_accum = 0;
static int64_t s_last_knob_ms = 0;
static bool  s_wifi_active = false;

// Sensitivity filter state
static int   s_sensitivity = 0;
static int   s_old_sensitivity = 0;
static int   s_movement_sensitivity = 100;
static float s_xn1 = 0.0f;
static float s_yn1 = 0.0f;
static float s_now_filter = 0.0f;

// Debounce
static int64_t s_last_press_ms = 0;

// ADC handle
static adc_oneshot_unit_handle_t s_adc1;

// ---- ISR ----------------------------------------------------------------

static void IRAM_ATTR rotary_isr(void *arg)
{
    static int64_t isr_last_ms = 0;
    int64_t now_ms = esp_timer_get_time() / 1000LL;

    if (now_ms - isr_last_ms >= PULSE_GAP_MS) {
        s_rotary_flag = true;
        int a = gpio_get_level(PIN_A);
        int b = gpio_get_level(PIN_B);
        if (a) {
            s_rotation_counter += (b == 0) ? 1 : -1;
        } else {
            s_rotation_counter += (b == 1) ? 1 : -1;
        }
    }
    isr_last_ms = now_ms;
}

// ---- Helpers ------------------------------------------------------------

static void knob_rotated(void)
{
    s_rotary_flag = false;
    int movement = (s_rotation_counter < s_prev_rotation) ? 1 : -1;
    ESP_LOGI(TAG, s_rotation_counter < s_prev_rotation ? "L%d" : "R%d", s_rotation_counter);
    s_prev_rotation = s_rotation_counter;

    uint8_t btn = s_mouse_right_mode ? BLE_MOUSE_RIGHT : BLE_MOUSE_LEFT;
    if (!s_mouse_pressed) {
        ble_mouse_press(btn);
        s_mouse_pressed = true;
        s_mouse_move_accum = 0;
    }

    int delta = movement * s_movement_scale;
    // Clamp to int8_t range
    if (delta >  127) delta =  127;
    if (delta < -127) delta = -127;

    s_mouse_move_accum += delta;
    ble_mouse_move(0, (int8_t)delta);
    s_last_knob_ms = esp_timer_get_time() / 1000LL;
}

static void change_sensitivity(void)
{
    ESP_LOGI(TAG, "Sensitivity raw: %d", s_sensitivity);
    s_movement_scale = 1 + (int)(0.005f * (float)s_sensitivity);
    ESP_LOGI(TAG, "Movement scale: %d", s_movement_scale);
}

static void re_button_pressed(void)
{
    s_mouse_right_mode = !s_mouse_right_mode;
    gpio_set_level(LED_ACTIVE, s_mouse_right_mode ? 1 : 0);
    ESP_LOGI(TAG, "Mode: %s button", s_mouse_right_mode ? "RIGHT" : "LEFT");
}

// ---- Init ---------------------------------------------------------------

static void gpio_init_all(void)
{
    // Output LEDs
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED_INTERNAL) | (1ULL << LED_BLUETOOTH) |
                        (1ULL << LED_WIFI)      | (1ULL << LED_ACTIVE),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    // PIN_B, WiFi button, RM button — plain inputs
    cfg.pin_bit_mask  = (1ULL << PIN_B) | (1ULL << BUTTON_WIFI) | (1ULL << BUTTON_RM);
    cfg.mode          = GPIO_MODE_INPUT;
    gpio_config(&cfg);

    // RE button — input with pull-up
    cfg.pin_bit_mask  = (1ULL << BUTTON_RE);
    cfg.pull_up_en    = GPIO_PULLUP_ENABLE;
    gpio_config(&cfg);
    cfg.pull_up_en    = GPIO_PULLUP_DISABLE;  // reset for next cfg

    // PIN_A — input with CHANGE interrupt (rising and falling)
    cfg.pin_bit_mask  = (1ULL << PIN_A);
    cfg.intr_type     = GPIO_INTR_ANYEDGE;
    gpio_config(&cfg);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_A, rotary_isr, NULL);
}

static void adc_init_all(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc1));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc1, SENS_ADC_CH, &chan_cfg));
}

static void disable_bluetooth_start_wifi(void)
{
    ble_mouse_deinit();
    gpio_set_level(LED_BLUETOOTH, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));

    wifi_ota_start();
    gpio_set_level(LED_WIFI, 1);
    s_wifi_active = true;
}

// ---- Main task ----------------------------------------------------------

static void main_task(void *pvParameters)
{
    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000LL;

        // Release mouse button after inactivity (mirrors original knobSens logic)
        if (s_mouse_pressed && (now_ms - s_last_knob_ms > KNOB_SENS_MS)) {
            ble_mouse_release();
            s_mouse_pressed = false;
            if (s_mouse_move_accum != 0) {
                int back = -s_mouse_move_accum;
                if (back >  127) back =  127;
                if (back < -127) back = -127;
                ble_mouse_move(0, (int8_t)back);
                s_mouse_move_accum = 0;
            }
        }

        if (s_rotary_flag) {
            knob_rotated();
        }

        // WiFi OTA button — high = pressed
        if (gpio_get_level(BUTTON_WIFI) == 1) {
            gpio_set_level(LED_INTERNAL, 1);
            if (!s_wifi_active) {
                disable_bluetooth_start_wifi();
            }
        } else {
            gpio_set_level(LED_INTERNAL, 0);
        }

        // BLE-only actions
        if (ble_mouse_is_connected() && !s_wifi_active) {
            gpio_set_level(LED_BLUETOOTH, 1);

            // Rotary encoder button (debounced)
            if (gpio_get_level(BUTTON_RE) == 1) {
                if (now_ms - s_last_press_ms > DEBOUNCE_MS) {
                    re_button_pressed();
                }
                s_last_press_ms = now_ms;
            }

            // Sensitivity potentiometer — IIR low-pass filter
            // Matches original: nowFilter += micros(); if >= samplePeriod ...
            s_now_filter += (float)esp_timer_get_time();
            if (s_now_filter >= SAMPLE_PERIOD) {
                s_now_filter -= SAMPLE_PERIOD;
                int raw = 0;
                adc_oneshot_read(s_adc1, SENS_ADC_CH, &raw);
                float xn = (float)raw;
                float yn = 0.969f * s_yn1 + 0.0155f * xn + 0.0155f * s_xn1;
                s_xn1 = xn;
                s_yn1 = yn;
                s_sensitivity = (int)yn;
                if (abs(s_sensitivity - s_old_sensitivity) > s_movement_sensitivity) {
                    change_sensitivity();
                    s_old_sensitivity = s_sensitivity;
                }
            }

            // RM button (low = pressed, pull-up implied by hardware)
            if (gpio_get_level(BUTTON_RM) == 0) {
                ESP_LOGI(TAG, "RM button pressed");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));  // yield / 1ms tick
    }
}

// ---- Entry point --------------------------------------------------------

void app_main(void)
{
    // NVS is required by BT stack
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gpio_init_all();
    adc_init_all();
    ble_mouse_init();

    xTaskCreate(main_task, "main_task", 4096, NULL, 5, NULL);
}
