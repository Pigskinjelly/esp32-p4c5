# ESP32-P4C5 整机 Demo（相机 + 屏 + 音频）
cmake_minimum_required(VERSION 3.16)

add_compile_options(-Wno-maybe-uninitialized)

# 固件版本（R-19）：烧录后串口/boot 日志会打印，现场可据此确认设备在跑哪一版
set(PROJECT_VER "2.39")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-p4c5-demo)
