idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES camera_source soft_awb video_tx wifi_ap
                                     nvs_flash esp_event esp_system log)
