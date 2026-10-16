idf_component_register(SRCS "wifi_ap.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event log)
