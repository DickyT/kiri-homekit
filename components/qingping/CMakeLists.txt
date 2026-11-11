idf_component_register(SRCS "qingping_parser.cpp" "qingping.cpp" INCLUDE_DIRS "include"
    REQUIRES bt esp_timer nvs_flash platform_lock cn105_transport device_settings)
