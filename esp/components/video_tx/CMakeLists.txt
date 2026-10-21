idf_component_register(SRCS "video_tx.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES protocol camera_source lwip esp_timer heap freertos log esp_wifi esp_netif)
