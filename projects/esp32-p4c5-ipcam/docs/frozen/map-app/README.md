# 冻结存档：导航地图 App（ui_app_map.c）

- 冻结日期：2026-11-20
- 冻结原因：用户决定**停止跟进与优化**该功能（真实导航依赖外部服务，离线示意版差距大）
- 冻结时固件版本：`2.34-audit`（在 `6a2002f` 之后）
- 文件大小：38087 字节
- SHA-256：`407ad798626572cc4546fa2ae6899c337d1075c3687c7b4c8e86bcf657faf91d`

## 已做的处置

1. 源码从 `main/ui_app_map.c` 移到本目录，改名 `ui_app_map.c.frozen`（文件头加了冻结说明）
2. `main/CMakeLists.txt` 的源文件列表里删掉 `"ui_app_map.c"` —— **不再参与编译**
3. `main/ui_apps.c` 注销 `ui_app_map` —— **桌面不再出现「导航」图标**
4. `main/main.c` 删掉临时的 MAP_UI_TEST 自测任务

## 怎么恢复（如果哪天又想要）

```bash
cp docs/frozen/map-app/ui_app_map.c.frozen main/ui_app_map.c
# main/CMakeLists.txt 的 srcs 里加回 "ui_app_map.c"
# main/ui_apps.c 里恢复 extern const ui_app_t ui_app_map; 和 ui_shell_register_app(&ui_app_map);
```

依赖：`wifi_mgr`（IP 定位）、`ui_theme`、LVGL 画布（用 PSRAM 画 380×196 RGB565）。

## 说明

这份存档只保留源码，不再维护。若要把同样的包放到设备的 TF 卡（冷备），
把 `map-app-freeze.zip` 拷到卡根目录即可（设备不读它，纯存档）。
