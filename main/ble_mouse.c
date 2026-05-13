/*
 * BLE HID Mouse wrapper for DAWKnob IDF
 * Based on ESP-IDF ble_hid_device_demo example
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "ble_mouse.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_defs.h"
#include "esp_hidd_prf_api.h"
#include "hid_dev.h"

#define TAG "BLE_MOUSE"
#define DEVICE_NAME "DAWKnob"

static uint16_t s_conn_id = 0;
static bool s_connected = false;
static bool s_sec_conn = false;
static uint8_t s_buttons = 0;

static uint8_t s_hidd_service_uuid128[] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

static esp_ble_adv_data_t s_hidd_adv_data = {
    .set_scan_rsp        = false,
    .include_name        = true,
    .include_txpower     = true,
    .min_interval        = 0x0006,
    .max_interval        = 0x0010,
    .appearance          = 0x03c0,  // HID Generic
    .manufacturer_len    = 0,
    .p_manufacturer_data = NULL,
    .service_data_len    = 0,
    .p_service_data      = NULL,
    .service_uuid_len    = sizeof(s_hidd_service_uuid128),
    .p_service_uuid      = s_hidd_service_uuid128,
    .flag                = 0x6,
};

static esp_ble_adv_params_t s_hidd_adv_params = {
    .adv_int_min       = 0x20,
    .adv_int_max       = 0x30,
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void hidd_event_callback(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    switch (event) {
    case ESP_HIDD_EVENT_REG_FINISH:
        if (param->init_finish.state == ESP_HIDD_INIT_OK) {
            esp_ble_gap_set_device_name(DEVICE_NAME);
            esp_ble_gap_config_adv_data(&s_hidd_adv_data);
        }
        break;
    case ESP_BAT_EVENT_REG:
        break;
    case ESP_HIDD_EVENT_DEINIT_FINISH:
        break;
    case ESP_HIDD_EVENT_BLE_CONNECT:
        ESP_LOGI(TAG, "BLE connected, conn_id=%d", param->connect.conn_id);
        s_conn_id = param->connect.conn_id;
        s_connected = true;
        break;
    case ESP_HIDD_EVENT_BLE_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected");
        s_connected = false;
        s_sec_conn = false;
        s_buttons = 0;
        esp_ble_gap_start_advertising(&s_hidd_adv_params);
        break;
    case ESP_HIDD_EVENT_BLE_VENDOR_REPORT_WRITE_EVT:
        break;
    case ESP_HIDD_EVENT_BLE_LED_REPORT_WRITE_EVT:
        break;
    default:
        break;
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&s_hidd_adv_params);
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            s_sec_conn = true;
            ESP_LOGI(TAG, "BLE pairing successful");
        } else {
            ESP_LOGE(TAG, "BLE pairing failed, reason=0x%x",
                     param->ble_security.auth_cmpl.fail_reason);
        }
        break;
    default:
        break;
    }
}

void ble_mouse_init(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_hidd_profile_init());

    esp_ble_gap_register_callback(gap_event_handler);
    esp_hidd_register_callbacks(hidd_event_callback);

    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key  = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE,      &iocap,    sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE,    &key_size, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY,    &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY,     &rsp_key,  sizeof(uint8_t));

    ESP_LOGI(TAG, "BLE Mouse initialized, advertising as \"%s\"", DEVICE_NAME);
}

void ble_mouse_deinit(void)
{
    if (s_connected) {
        ble_mouse_release();
    }
    esp_hidd_profile_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    s_connected = false;
    s_sec_conn = false;
    ESP_LOGI(TAG, "BLE Mouse deinitialized");
}

bool ble_mouse_is_connected(void)
{
    return s_connected && s_sec_conn;
}

void ble_mouse_press(uint8_t buttons)
{
    if (!ble_mouse_is_connected()) return;
    s_buttons = buttons;
    esp_hidd_send_mouse_value(s_conn_id, s_buttons, 0, 0);
}

void ble_mouse_release(void)
{
    if (!s_connected) return;
    s_buttons = 0;
    esp_hidd_send_mouse_value(s_conn_id, 0, 0, 0);
}

void ble_mouse_move(int8_t x, int8_t y)
{
    if (!ble_mouse_is_connected()) return;
    esp_hidd_send_mouse_value(s_conn_id, s_buttons, x, y);
}
