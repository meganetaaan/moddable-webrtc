idf_component_register(
  SRCS "main.c" "atomic_compat.c"
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

target_link_options(${COMPONENT_LIB} INTERFACE
  "-Wl,-u,__atomic_fetch_add_1"
  "-Wl,-u,__atomic_fetch_sub_1"
)
