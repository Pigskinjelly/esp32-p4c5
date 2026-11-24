#!/bin/bash
# 烧录固件到 ESP32-P4（拨码开关须拨到 P4）
#
# 用法：bash flash.sh [串口] [构建目录]
#   bash flash.sh COM19                # 默认烧 /d/p4c5b3
#   bash flash.sh COM19 /d/p4c5b3      # 显式指定
#
# 注意 1：分区表偏移为 0x9000（rev3 bootloader 约 24.7KB，0x8000 放不下）
# 注意 2：默认构建目录曾经指向 /d/p4c5b2，那是 11/20 的旧固件（3.47MB）。
#         不指定目录直接跑，烧进去的是老版本，看起来就像"改了没生效"。
#         现在默认 /d/p4c5b3，并在烧录前打印固件时间供核对。
PORT=${1:-COM19}
BUILD_DIR=${2:-/d/p4c5b3}
PY=/d/Espressif/python_env/idf5.5_py3.11_env/Scripts/python.exe

cd "$BUILD_DIR" || exit 1

BIN=esp32-p4c5-demo.bin
if [ ! -f "$BIN" ]; then
    echo "找不到 $BUILD_DIR/$BIN，先编译：cd $BUILD_DIR && ninja" >&2
    exit 1
fi
echo ">> 烧录 $BUILD_DIR/$BIN ($(stat -c '%y' "$BIN" | cut -c1-16), $(stat -c '%s' "$BIN") bytes)"

$PY -m esptool --chip esp32p4 -p "$PORT" -b 460800 write_flash \
    0x2000  bootloader/bootloader.bin \
    0x9000  partition_table/partition-table.bin \
    0x10000 "$BIN"

