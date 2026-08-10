idf_component_register(
    SRCS
        "modbus/pzem.cpp"
        "wifi/mqtt.cpp"
        "wifi/wifi_handler.cpp"
        "wifi/wifi_provisioning.cpp"
        "button/button.cpp"
        "app_main.cpp"

    PRIV_REQUIRES
        esp-tls
        esp_wifi
        esp_event
        nvs_flash

    INCLUDE_DIRS 
        "."

    EMBED_TXTFILES
        "broker.emqx.io-ca.crt"

    REQUIRES
        esp_driver_uart
        esp_driver_gpio
        esp_http_server
        mqtt
)
