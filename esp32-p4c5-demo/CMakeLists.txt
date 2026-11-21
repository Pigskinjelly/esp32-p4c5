# ESP32-P4C5 整机 Demo（相机 + 屏 + 音频）
cmake_minimum_required(VERSION 3.16)

add_compile_options(-Wno-maybe-uninitialized)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-p4c5-demo)
