idf_component_register(SRCS "soft_awb.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES camera_source video_tx espressif__esp_jpeg esp_timer heap freertos log)
