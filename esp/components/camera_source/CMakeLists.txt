idf_component_register(SRCS "camera_source.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES espressif__esp32-camera protocol esp_timer freertos log)
