idf_component_register(
  SRCS "main.c"
  INCLUDE_DIRS "."
  REQUIRES
    cjson
    esp_http_client
    esp_netif
    esp_wifi
    esp_websocket_client
    nvs_flash
    esp_peer
)
