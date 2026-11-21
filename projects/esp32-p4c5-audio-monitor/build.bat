@echo off
REM ESP32-P4C5 audio-monitor build script (run via cmd.exe from the project directory)
REM Adjust IDF paths below to match your own ESP-IDF installation.
set IDF_PATH=D:/Espressif/frameworks/esp-idf-v5.5.5
set IDF_PYTHON_ENV_PATH=D:/Espressif/python_env/idf5.5_py3.11_env
set IDF_TOOLS_PATH=D:/Espressif
set ESP_ROM_ELF_DIR=D:/Espressif/tools/esp-rom-elfs/20241011
set MSYSTEM=
set PATH=%IDF_PYTHON_ENV_PATH%\Scripts;D:/Espressif/tools/cmake/3.30.2/bin;D:/Espressif/tools/ninja/1.12.1;D:/Espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin;D:/Espressif/tools/ccache/4.12.1;D:/Espressif/tools/idf-git/2.44.0/cmd;D:/Espressif/tools/dfu-util/0.11;%PATH%
cd /d "%~dp0"
python "%IDF_PATH%\tools\idf.py" build
