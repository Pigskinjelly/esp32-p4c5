#!/bin/bash
# ESP32-P4C5 IP Camera —— 构建脚本（必须由 bash 作为进程树根运行）
#
# 背景：本机装有 TEC Ocular（TFsdframe/TsdEncrypt）文档透明加密驱动。
#   由 powershell.exe 启动的 cmake/gcc/ar 写出的产物会被加密成 "%TSD-Header-###%"，
#   导致 gcc 汇编报 "junk at end of line"。
#   由 bash.exe 启动的同一批命令，产物是明文 —— 这是本机可用的构建方式。
#
# 用法：
#   bash build.sh              # 增量构建
#   bash build.sh clean        # 删除构建目录后全新构建
#   bash build.sh cfg          # 只重新配置
#
# 环境变量：
#   BUILD_DIR  构建目录（默认 /d/p4c5b2）

set -o pipefail

PROJ="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR=${BUILD_DIR:-/d/p4c5b2}
IDF_PATH=${IDF_PATH:-/d/Espressif/frameworks/esp-idf-v5.5.5}
IDF_PY_ENV=${IDF_PY_ENV:-/d/Espressif/python_env/idf5.5_py3.11_env}
TMPDIR_IDF=${TMPDIR_IDF:-/d/idftmp}

# 交叉编译器必须在 PATH 里，否则 CMake 找不到 CMAKE_ASM_COMPILER
export PATH="$IDF_PATH/../tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin:$PATH"
export PATH="/d/Espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin:/d/Espressif/tools/cmake/3.30.2/bin:/d/Espressif/tools/ninja/1.12.1:$IDF_PY_ENV/Scripts:$PATH"

export IDF_PATH
export TMP="$TMPDIR_IDF" TEMP="$TMPDIR_IDF"

# 1) 让 CMake 跳过 try_compile（DLP 会锁住临时目录，导致 "could not be removed"）
export IDF_SKIP_TRY_COMPILE=1
# 2) -pipe 让 gcc 不落临时 .s 文件（临时汇编曾被加密）
export CFLAGS=-pipe CXXFLAGS=-pipe ASMFLAGS=-pipe
# 3) 彻底禁用 ccache（删除 depfile 会被 DLP 拒绝）
export IDF_CCACHE_ENABLE=0 CCACHE_DISABLE=1
# 4) esp_wifi_remote / esp_hosted 的 Kconfig 用 ./idf_v$ESP_IDF_VERSION/ 拼路径，
#    必须传 "5.5"（不是 5.5.5），否则 Wi-Fi 相关选项会被 orsource 静默跳过
export IDF_PATH=${IDF_PATH:-/d/Espressif/frameworks/esp-idf-v5.5.5}
export ESP_IDF_VERSION=5.5

# 与交互式 shell 共用同一份环境（含 ESP_IDF_VERSION=5.5 等必须项）
[ -f "$PROJ/tools/idfenv.sh" ] && . "$PROJ/tools/idfenv.sh" >/dev/null 2>&1

# 用 pip 安装的 cmake（不同签名，DLP 行为正常）
CMAKE_BIN="$IDF_PY_ENV/Lib/site-packages/cmake/data/bin/cmake.exe"
PYTHON_BIN="$IDF_PY_ENV/Scripts/python.exe"

mkdir -p "$TMPDIR_IDF"

do_config() {
    echo ">> 配置：$BUILD_DIR"
    "$CMAKE_BIN" -S "$PROJ" -B "$BUILD_DIR" -GNinja \
        -DPYTHON_DEPS_CHECKED=1 \
        -DPYTHON="$PYTHON_BIN" \
        -DSDKCONFIG="$PROJ/sdkconfig" \
        -DIDF_TARGET=esp32p4
}

case "${1:-}" in
    clean)
        rm -rf "$BUILD_DIR"
        do_config || exit 1
        ;;
    cfg)
        do_config || exit 1
        ;;
    "")
        ;;
    *)
        echo "未知参数：$1（可用：clean | cfg）"; exit 2
        ;;
esac

echo ">> 编译"
(cd "$BUILD_DIR" && "$CMAKE_BIN" --build . ) || exit 1

echo
echo ">> 完成"
ls -la "$BUILD_DIR"/esp32-p4c5-demo.bin "$BUILD_DIR"/bootloader/bootloader.bin 2>/dev/null
