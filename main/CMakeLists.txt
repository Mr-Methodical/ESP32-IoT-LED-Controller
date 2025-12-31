# The main component CMakeLists.txt
idf_component_register(SRCS "led_controller_main.c" "led_strip_encoder.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_driver_rmt esp_http_server)