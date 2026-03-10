idf_component_register(
    SRCS "main.c" "console.c" "hub_pairing.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        console
        vfs
        driver
        esp_netif
        esp_event
        esp_eth
        ethernet_init
        esp_http_client
        esp-tls
        mbedtls
        nvs_flash
        led_init
        sub_init
)
