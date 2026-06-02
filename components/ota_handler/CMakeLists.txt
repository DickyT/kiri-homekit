idf_component_register(SRCS "ota_handler.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES app_update board_profile esp_app_format esp_http_server esp_partition esp_system mbedtls)
