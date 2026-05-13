idf_component_register(
    SRCS
        "main.c"
        "ble_mouse.c"
        "wifi_ota.c"
        "esp_hidd_prf_api.c"
        "hid_dev.c"
        "hid_device_le_prf.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        bt
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        app_update
        esp_driver_gpio
        esp_adc
        esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-const-variable)
