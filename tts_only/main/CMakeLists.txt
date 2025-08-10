idf_component_register(SRCS "app_main.c"
                    EMBED_FILES "assets/err_reg.p3" "assets/err_pin.p3" "assets/err_wificonfig.p3"
                    INCLUDE_DIRS "."
                    REQUIRES driver 78__esp-opus)