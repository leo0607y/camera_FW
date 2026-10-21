idf_component_register(SRCS "main.c" "frame_quality.c" "avi_writer.c" "destripe.c" "jpeg_writer.cpp" INCLUDE_DIRS "." PRIV_INCLUDE_DIRS "../managed_components/espressif__esp32-camera/conversions/private_include" REQUIRES esp_driver_gpio esp_driver_usb_serial_jtag esp_adc esp_psram esp_timer esp_system fatfs sdmmc json esp32-camera esp_jpeg mbedtls nvs_flash esp_wifi esp_event esp_netif esp_http_server EMBED_TXTFILES "gallery.html")
# 画像処理は重いので-O2（プロジェクトは-Os）
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
