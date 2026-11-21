# IDF 工具链补丁存档

原开发机在 ESP-IDF 工具链上打了若干补丁，**只存在于本机磁盘、不受 git 管理**。换机器或重装 IDF 后必须重打，否则构建会以各种方式失败（详见 [`../REPLICATION.md`](../REPLICATION.md) §3）。

本目录收录可从本机提取的补丁文件：

| 文件 | 对应原位置（ESP-IDF v5.5.5） | 说明 |
|---|---|---|
| `ldgen.py.patched` | `tools/ldgen/ldgen.py` | 已打补丁的完整文件：`_objdump_sections()` 用 cmd 重定向/管道/直调三种方式轮询 objdump，校验 `In archive`/`Sections:` 且连续两次一致才接受，12 次重试、每次 `timeout=20`。**复刻时用它替换 IDF 中的同名文件**（IDF 路径如 `$IDF_PATH/tools/ldgen/ldgen.py`）。 |

## 未在仓库中留档的补丁

以下补丁的原文件未随仓库留存，改动内容已在 `../REPLICATION.md` §3 的表格中逐条描述，请按描述手工修改：

- `tools/partition_table/gen_esp32part.py`：`main()` 开头 `sys.stdout is None` 兜底 devnull。
- `Modules/Internal/CheckSourceCompiles.cmake`、`CMakeDetermineCompilerABI.cmake`、`CMakeTestCCompiler.cmake`、`CMakeTestCXXCompiler.cmake`：有 `IDF_SKIP_TRY_COMPILE` 时判定检查通过。
- `esp_lvgl_port_touch.c`：`ESP_ERROR_CHECK(read_data)` 改为容错。

## 已随仓库提供的组件补丁

`managed_components/` 内的以下修改已入库，无需重打（但注意 `idf.py reconfigure` 可能覆盖，改动前备份）：

- `espressif__esp_video/.../esp_video_jpeg_device.c`：`timeout_ms = 300`。
- `espressif__esp_wifi_remote/idf_v5.5/include/injected/esp_wifi_types_generic.h`：补 `gtk_rekey_interval`。
- `espressif__esp_lcd_touch_st7123/.../esp_lcd_touch_st7123.c`：触点按面积排序 + 清零 + max_touches 保护。
- `espressif__esp_hosted/Kconfig`（audio-monitor 工程）：`$(ESP_IDF_VERSION) >= "6.1"` → `depends on n`。

## 提示

没有安装文档透明加密驱动（DLP）的机器通常只需要 `ESP_IDF_VERSION=5.5` 和 bash 构建链即可；`ldgen.py` 补丁属于"保留更稳"的加固。若构建正常，可不打 DLP 相关补丁。
