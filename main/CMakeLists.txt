idf_component_register(
    SRCS "memory.cpp" "brain.cpp" "main.cpp" "board.cpp" "wifi.cpp" "azure.cpp" "audio.cpp" "config.cpp" "webconfig.cpp" "va.cpp" "thinking.cpp" "mic.cpp" "brain.cpp" "memory.cpp" "touch.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_http_client esp-tls mbedtls esp_wifi esp_event esp_netif nvs_flash driver esp_timer lwip esp_http_server json mdns
)
