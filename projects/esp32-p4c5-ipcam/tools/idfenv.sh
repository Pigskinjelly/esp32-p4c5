#!/bin/bash
# ESP-IDF 环境（本机专用，落库以便复现构建）
#
# 用法：
#   source tools/idfenv.sh
#   cd /d/p4c5b2 && ninja
#
# 为什么不用 $IDF_PATH/export.sh：
#   本机 python3 指向 workbuddy 的 3.13，而 IDF 的 venv 是 3.11，
#   export.sh 会直接报 "Python virtual environment not found"。
#
# ⚠️ ESP_IDF_VERSION 必须是 "5.5"（major.minor），不能是 5.5.5：
#   esp_wifi_remote / esp_hosted 的 Kconfig 用 orsource "./idf_v$ESP_IDF_VERSION/xxx"，
#   写成 5.5.5 会找不到文件 → CONFIG_WIFI_RMT_* 被静默删除 → 编译报
#   'CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM' undeclared（2026-11-19 就踩过，见 R-03）

export IDF_PATH=${IDF_PATH:-/d/Espressif/frameworks/esp-idf-v5.5.5}
export IDF_TOOLS_PATH=${IDF_TOOLS_PATH:-/d/Espressif}
export IDF_PYTHON_ENV_PATH=${IDF_PYTHON_ENV_PATH:-/d/Espressif/python_env/idf5.5_py3.11_env}

export IDF_VERSION=5.5.5
export ESP_IDF_VERSION=5.5

export PATH="$IDF_PYTHON_ENV_PATH/Scripts:/d/Espressif/tools/ninja/1.12.1:/d/Espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin:/d/Espressif/tools/cmake/3.30.2/bin:$PATH"

# 本机 DLP 文档加密驱动的规避项（见 build.sh 注释）
export IDF_SKIP_TRY_COMPILE=1
export IDF_CCACHE_ENABLE=0
export CCACHE_DISABLE=1
export CFLAGS=-pipe
export CXXFLAGS=-pipe
export ASMFLAGS=-pipe
