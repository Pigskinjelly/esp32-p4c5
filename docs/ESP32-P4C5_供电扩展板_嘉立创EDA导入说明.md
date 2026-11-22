# ESP32-P4C5 供电/储能扩展板 —— 嘉立创 EDA 专业版导入说明

> 生成日期：2026-11-21 · 对应电气方案 v1.1

## 1. 这是什么

`hardware/power-board-v1/power-board-v1_kicad.zip` 是从本项目的 BOM 与网表**自动生成的 KiCad 5.1 原理图工程**，包含：

| 文件 | 内容 |
|---|---|
| `power-board-v1.pro` | 工程配置 |
| `power-board-v1.sch` | **A1 单页原理图**，64 个位号、188 个引脚连接、45 个网络 |
| `powerboard.lib` | 与本工程配套的 64 个符号定义 |
| `sym-lib-table` | 符号库映射 |

**所有连接关系来自 `docs/hw/netlist_v1.csv`，不是手画的示意图。** 每个元件引脚都引出一小段短线并挂本地网络标签，导入后网络名与 v1.1 方案完全一致。

## 2. 为什么要走"导入"而不是手画

64 个位号、188 根引脚连接，手动放置 + 连线至少几百次点击，且极易漏脚。嘉立创 EDA 专业版官方支持导入 KiCad 5.1 工程（见 `prodocs.lceda.cn/cn/import-export/import-kicad`），所以自动生成工程再导入是最可靠的方式。

## 3. 导入步骤（约 1 分钟）

1. 启动桌面上的 **嘉立创EDA(专业版)**，进入**开始页**。
2. 在开始页找到 **导入**（Import）入口，选择 **KiCad**。
3. 选择压缩包：
   `<用户目录>\Desktop\ESP32-P4C5\hardware\power-board-v1\power-board-v1_kicad.zip`
4. 等待解析完成，会自动创建一个新工程并打开原理图。
5. 先对照 `hardware/power-board-v1/kicad-import/layout-preview.html` 检查：应该有 **64 个元件框**，位号从 J1 到 TP6。
6. 在**工程 → 其他工具**里执行**标注 / Annotate**，检查位号有无冲突；再导出网表，与 `docs/hw/netlist_v1.csv` 对照。

> 如果导入器提示版本不兼容：本包刻意使用 KiCad 5.1 的**旧式 `.sch` + `.lib`** 格式（不是 KiCad 6+ 的 `.kicad_sch`），就是为了让专业版能直接吃下。若仍失败，用官方「嘉立创EDA文件迁移助手」选 KiCad 再转一次。

## 4. 导入后必须做的事（重要）

这个包解决的是**电气连接**，不是"可直接下单的成品"：

| 项 | 状态 | 你需要做 |
|---|---|---|
| 网络连接 | ✅ 完整（45 网 / 188 脚） | 导出网表核对一遍 |
| 符号外观 | ⚠️ 通用矩形符号 | 可保留；要好看就换成标准库符号，**引脚号不能改** |
| 封装 | ⚠️ 只写了文字说明 | 按 `docs/hw/bom_v1.csv` 逐项指定封装 |
| PCB / 布线 / 铺铜 | ❌ 未做 | 按 `design-review.html` 里的 52×72 布局示意布线 |
| 排针坐标 | ❌ 未实测 | 已改线束方案，不依赖排针坐标 |

## 5. 对照资料

- 逐脚网表：`docs/hw/netlist_v1.csv`
- 按网络速查：`docs/hw/netlist_by_net.md`
- BOM：`docs/hw/bom_v1.csv`
- 可视化原理图：`hardware/power-board-v1/schematic-full.png` / `.pdf`
- 设计评审包（8 页原理图 + PCB 示意 + 堆叠剖面）：`hardware/power-board-v1/design-review.html`
- 方案说明：`docs/ESP32-P4C5_供电扩展板_阶段1_方案定稿v1.md`

## 6. 重新生成

如果改了网表，重跑：

```
python _tools/gen_hw.py          # 由内建数据表重生成 BOM + 网表
python _tools/gen_kicad.py       # 重生成 KiCad 工程与 zip
python _tools/gen_kicad_preview.py
```

不要手改 `.sch` / `.lib`，会被下次生成覆盖。
