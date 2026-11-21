/*
 * 内置 App 注册
 */
#include "ui_shell.h"

extern const ui_app_t ui_app_monitor;
extern const ui_app_t ui_app_camera;
extern const ui_app_t ui_app_gallery;
extern const ui_app_t ui_app_stock;
extern const ui_app_t ui_app_weather;
extern const ui_app_t ui_app_music;
extern const ui_app_t ui_app_settings;

/* 注意：导航地图 App 已于 2026-11-20 冻结，源码见
 * docs/frozen/map-app/ui_app_map.c.frozen，不再注册、不参与编译。 */

void ui_apps_register_all(void)
{
    ui_shell_register_app(&ui_app_monitor);
    ui_shell_register_app(&ui_app_camera);
    ui_shell_register_app(&ui_app_gallery);
    ui_shell_register_app(&ui_app_stock);
    ui_shell_register_app(&ui_app_music);
    ui_shell_register_app(&ui_app_weather);
    ui_shell_register_app(&ui_app_settings);
}
