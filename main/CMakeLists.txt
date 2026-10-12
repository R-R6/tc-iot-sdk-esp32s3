set(_MAIN_REQUIRES console esp_driver_uart esp_driver_usb_serial_jtag
                    esp_usb_cdc_rom_console fatfs nvs_flash
                    cmd_system cmd_wifi cmd_nvs
                    esp_psram)

# PRODUCT=trtc：只链 TRTC SDK + cmd_trtc_demo；默认 IoT。
if(DEFINED ENV{PRODUCT} AND "$ENV{PRODUCT}" STREQUAL "trtc")
  list(APPEND _MAIN_REQUIRES tc_trtc_sdk cmd_trtc_demo)
  set(_TRTC_PRODUCT 1)
else()
  list(APPEND _MAIN_REQUIRES tc_iot_sdk cmd_iot_demo)
  set(_TRTC_PRODUCT 0)
endif()

idf_component_register(SRCS "app_main.c" "console_settings.c"
                    PRIV_REQUIRES ${_MAIN_REQUIRES}
                    INCLUDE_DIRS ".")

target_compile_definitions(${COMPONENT_LIB} PRIVATE TC_PRODUCT_TRTC=${_TRTC_PRODUCT})
