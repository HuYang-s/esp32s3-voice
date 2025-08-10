idf_component_register(SRCS "app_main.c" "stream_ws_tts.c"
                    EMBED_FILES "assets/err_reg.p3" "assets/err_pin.p3" "assets/err_wificonfig.p3"
                    INCLUDE_DIRS "."
                    REQUIRES driver 78__esp-opus espressif__esp_websocket_client esp_wifi esp_netif nvs_flash esp_event json)

set(KCONFIG_PROJBUILD "Kconfig.projbuild")