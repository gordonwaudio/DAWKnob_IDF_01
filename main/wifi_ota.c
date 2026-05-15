/*
 * WiFi Station + OTA update HTTP server for DAWKnob IDF
 * Replaces Arduino AsyncElegantOTA + AsyncWebServer
 */

#include "wifi_ota.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#define TAG "WIFI_OTA"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRIES   5

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

// Landing page HTML — uses fetch() to POST raw binary, not multipart/form-data
static const char *INDEX_HTML =
    "<!DOCTYPE html><html><head><title>DAWKnob</title>"
    "<style>body{background-color:#003080;color:white;font-family:Arial,sans-serif;padding:20px;}"
    "h1{color:#ffffff;}input[type=file]{margin:10px 0;display:block;}"
    "button{background:#00a0ff;color:white;border:none;padding:10px 20px;"
    "border-radius:4px;cursor:pointer;font-size:16px;}"
    "</style></head><body>"
    "<h1>DAWKnob</h1>"
    "<p>ESP32 DAWKnob IDF - OTA Update</p>"
    "<p>Select firmware binary (.bin):</p>"
    "<input type='file' id='file' accept='.bin'>"
    "<button onclick='upload()'>Upload Firmware</button>"
    "<p id='status'></p>"
    "<script>"
    "function upload(){"
    "var f=document.getElementById('file').files[0];"
    "if(!f){alert('Select a .bin file first');return;}"
    "document.getElementById('status').innerText='Uploading...';"
    "fetch('/update',{method:'POST',body:f,"
    "headers:{'Content-Type':'application/octet-stream'}})"
    ".then(r=>r.text()).then(t=>{document.body.innerHTML=t;})"
    ".catch(e=>{document.getElementById('status').innerText='Upload error: '+e;});}"
    "</script>"
    "</body></html>";

// OTA state
static esp_ota_handle_t s_ota_handle = 0;
static const esp_partition_t *s_ota_partition = NULL;
static bool s_ota_started = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying WiFi connection (%d/%d)", s_retry_num, WIFI_MAX_RETRIES);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
    return ESP_OK;
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    char buf[1024];
    int remaining = req->content_len;
    esp_err_t err;

    ESP_LOGI(TAG, "OTA upload started, size=%d bytes", remaining);

    s_ota_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_ota_partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    err = esp_ota_begin(s_ota_partition, OTA_WITH_SEQUENTIAL_WRITES, &s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }
    s_ota_started = true;

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "OTA receive error");
            esp_ota_abort(s_ota_handle);
            s_ota_started = false;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        err = esp_ota_write(s_ota_handle, buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(s_ota_handle);
            s_ota_started = false;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }
        remaining -= recv_len;
    }

    err = esp_ota_end(s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        s_ota_started = false;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(s_ota_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA boot set failed");
        return ESP_FAIL;
    }

    s_ota_started = false;
    ESP_LOGI(TAG, "OTA successful, rebooting in 2 seconds...");

    const char *success_html =
        "<!DOCTYPE html><html><head><title>DAWKnob OTA</title></head><body>"
        "<h2 style='color:green'>Update successful! Rebooting...</h2>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, success_html, strlen(success_html));

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &index_uri);

    httpd_uri_t ota_uri = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = ota_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &ota_uri);

    ESP_LOGI(TAG, "HTTP server started on port 80");
}

void wifi_ota_start(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    // Set static IP
    esp_netif_dhcpc_stop(sta_netif);
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    ip_info.ip.addr      = esp_ip4addr_aton(CONFIG_DAWKNOB_STATIC_IP);
    ip_info.gw.addr      = esp_ip4addr_aton(CONFIG_DAWKNOB_GATEWAY_IP);
    ip_info.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    esp_netif_set_ip_info(sta_netif, &ip_info);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL,
                                                        &instance_got_ip));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = CONFIG_DAWKNOB_WIFI_SSID,
            .password = CONFIG_DAWKNOB_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi: %s ...", CONFIG_DAWKNOB_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected. Static IP: %s", CONFIG_DAWKNOB_STATIC_IP);
        start_http_server();
    } else {
        ESP_LOGE(TAG, "WiFi connection failed");
    }
}
