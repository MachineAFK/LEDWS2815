idf_component_register(SRCS "WLED.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_lcd lvgl esp_timer)