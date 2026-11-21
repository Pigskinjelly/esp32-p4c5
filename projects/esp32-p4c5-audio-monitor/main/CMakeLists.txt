set(srcs "main.c" "wifi_sta.c" "audio_capture.c" "audio_stream.c")

idf_component_register(SRCS "${srcs}"
                       PRIV_INCLUDE_DIRS ".")
