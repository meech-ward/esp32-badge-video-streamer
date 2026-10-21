idf_component_register(SRCS "main.cpp" "console.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES camera_source soft_awb video_tx wifi_ap
                                     nvs_flash esp_event esp_system esp_driver_usb_serial_jtag vfs log)
