idf_component_register(
    SRCS
        "src/ch9434_spi.c"
        "src/ch9434_drv.c"
        "src/ch9434_uart.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_spi esp_driver_gpio esp_timer
)
