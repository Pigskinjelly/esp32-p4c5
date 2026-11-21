set(srcs "main.c" "app_video.c" "app_lcd.c" "app_audio.c")

idf_component_register(SRCS "${srcs}"
                       PRIV_INCLUDE_DIRS .)
