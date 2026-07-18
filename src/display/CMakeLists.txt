idf_component_register(
    SRCS "display_ui.c"
    INCLUDE_DIRS "."
    REQUIRES pico-keys-sdk fido esp_lcd esp_driver_spi esp_driver_i2c esp_driver_gpio driver esp_timer
)
idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE ON)
