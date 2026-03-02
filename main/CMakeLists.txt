idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_netif
        esp_event
        esp_eth
        ethernet_init
        esp_http_client
        esp-tls
        mbedtls
)