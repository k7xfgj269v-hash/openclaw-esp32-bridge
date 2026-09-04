# 组件注册：REQUIRES 用 `driver` 聚合组件，保证在 IDF v5.0（I2S 在 driver 内）
# 与 IDF v5.2+（driver 聚合 esp_driver_*）都能编译。
idf_component_register(
    SRCS
        "app_main.c"
        "wifi_mgr.c"
        "i2s_audio.c"
        "wav_parse.c"
        "voice_client.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_http_client
        mbedtls
)
