idf_component_register(
    SRCS "main.cpp" "board.cpp" "touch.cpp" "leds.cpp" "audio.cpp" "mic.cpp" "thinking.cpp" "wifi.cpp" "setupnet.cpp" "webconfig.cpp" "config.cpp" "azure.cpp" "va.cpp" "memory.cpp" "brain.cpp" "ota.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_http_client esp-tls mbedtls esp_wifi esp_event esp_netif nvs_flash driver esp_timer lwip esp_http_server json mdns esp_https_ota app_update esp_app_format
)
