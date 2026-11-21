#!/bin/bash
# 烧录固件到 ESP32-P4（拨码开关须拨到 P4）
#
# 用法：bash flash.sh [串口]     例如 bash flash.sh COM19
#
# 注意：分区表偏移为 0x9000（rev3 bootloader 约 24.7KB，0x8000 放不下）
PORT=${1:-COM19}
BUILD_DIR=${2:-/d/p4c5b2}
PY=/d/Espressif/python_env/idf5.5_py3.11_env/Scripts/python.exe

cd "$BUILD_DIR" || exit 1

$PY -m esptool --chip esp32p4 -p "$PORT" -b 460800 write_flash \
    0x2000  bootloader/bootloader.bin \
    0x9000  partition_table/partition-table.bin \
    0x10000 esp32-p4c5-demo.bin
