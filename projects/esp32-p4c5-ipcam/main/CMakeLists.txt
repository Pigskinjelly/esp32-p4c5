set(srcs "main.c"
         "app_video.c" "app_lcd.c" "app_audio.c"
         "app_encoder.c" "rtsp_server.c" "rtsp_selftest.c"
         "wifi_mgr.c" "app_storage.c" "app_jpeg.c" "gbk2utf8.c" "bt_mgr.c"
         "app_touch.c" "runlog.c"
         "ui_theme.c" "ui_shell.c" "ui_apps.c"
         "ui_app_monitor.c" "ui_app_camera.c" "ui_app_gallery.c"
         "ui_app_stock.c" "trade_day.c" "stock_cfg.c" "ui_app_music.c" "ui_pinyin.c" "ui_app_weather.c" "ui_app_settings.c" "ui_boot.c"
         "ui_font_cjk_16.c" "ui_font_cjk_24.c" "ui_font_time_96.c" "ui_font_time_96_light.c"
         "ui_font_cjk_16_extra.c" "ui_font_cjk_24_extra.c")

# 不显式写 REQUIRES：main 组件默认链接所有组件，
# 避免遗漏 esp_timer / esp_lcd / lwip / fatfs 等间接依赖
idf_component_register(SRCS "${srcs}"
                       PRIV_INCLUDE_DIRS .)
