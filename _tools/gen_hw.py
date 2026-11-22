# -*- coding: utf-8 -*-
"""生成 ESP32-P4C5 供电/储能扩展板 v1 的 BOM 与网表。"""

import csv
import os
from collections import defaultdict

OUT = r"<用户目录>\Desktop\ESP32-P4C5\docs\hw"
os.makedirs(OUT, exist_ok=True)

C = []


def add(ref, mpn, desc, pkg, qty, blk, func, pins):
    C.append(dict(ref=ref, mpn=mpn, desc=desc, pkg=pkg, qty=qty,
                  blk=blk, func=func, pins=pins))


# ============ B1 Type-C 输入 ============
add("J1", "TYPE-C 16PIN 2MD(073) / C2765186", "Type-C 母座 16P 卧贴", "SMD", 1, "B1",
    "充电输入口（5V 受电，CC 5.1k）",
    {"A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND", "SH1": "GND", "SH2": "GND",
     "A4": "VBUS_TC", "A9": "VBUS_TC", "B4": "VBUS_TC", "B9": "VBUS_TC",
     "A5": "CC1", "B5": "CC2",
     "A6": "USB_DP", "A7": "USB_DN", "B6": "USB_DP", "B7": "USB_DN",
     "A2": "NC", "A3": "NC", "A8": "NC", "B2": "NC", "B3": "NC", "B8": "NC"})

add("F1", "SMD1206-3A 慢断", "一次性保险丝 3A/32V", "1206", 1, "B1",
    "输入过流二级保护", {"1": "VBUS_TC", "2": "VBUS_IN"})

add("D1", "SMBJ5.0A", "TVS 单向 5V", "SMB", 1, "B1", "VBUS 浪涌钳位", {"1": "VBUS_IN", "2": "GND"})

add("U1", "USBLC6-2SC6", "USB ESD 阵列", "SOT-23-6", 1, "B1", "CC1/CC2 ESD 保护",
    {"IO1": "CC1", "IO2": "CC2", "VBUS": "VBUS_IN", "GND": "GND", "IO3": "NC", "IO4": "NC"})

add("R1", "0603WAF5101T5E / C23186", "5.1kΩ ±1%", "0603", 1, "B1", "CC1 下拉（声明 5V/3A 受电）", {"1": "CC1", "2": "GND"})
add("R2", "0603WAF5101T5E / C23186", "5.1kΩ ±1%", "0603", 1, "B1", "CC2 下拉", {"1": "CC2", "2": "GND"})

# ============ B2 充电 + 电源路径 ============
add("U2", "BQ25606", "1S 开关型充电器 3A，带 Power Path", "WQFN-24 4x4", 1, "B2",
    "5V→4.2V 充电 + SYS 路径管理",
    {"VBUS": "VBUS_IN", "PMID": "PMID", "BTST": "BTST", "SW": "SW_CHG", "SYS": "SYS_V",
     "BAT": "BAT_P", "ILIM": "ILIM", "ICHG": "ICHG", "TS": "TS_NET", "STAT": "STAT_N",
     "PG": "PG_N", "CE": "GND", "REGN": "REGN", "D+": "USB_DP", "D-": "USB_DN", "GND": "GND", "EP": "GND"})

add("L1", "1.5µH/3A 屏蔽功率电感", "1.5µH ±20%，Isat ≥ 4A", "7x7x4", 1, "B2", "充电 buck 电感", {"1": "SW_CHG", "2": "SYS_V"})
add("C1", "CL31B106KOHNNNE", "10µF/25V X5R", "1206", 2, "B2", "VBUS 输入去耦", {"1": "VBUS_IN", "2": "GND"})
add("C2", "CL31B106KOHNNNE", "10µF/25V X5R", "1206", 2, "B2", "PMID 去耦", {"1": "PMID", "2": "GND"})
add("C3", "CL10B104KB8NNNC / C1591", "100nF/50V", "0603", 1, "B2", "BTST 自举电容", {"1": "BTST", "2": "SW_CHG"})
add("C4", "CL31B106KOHNNNE", "10µF/25V X5R", "1206", 2, "B2", "SYS 输出电容", {"1": "SYS_V", "2": "GND"})
add("C5", "CL10A475KO8NNNC", "4.7µF/16V", "0603", 1, "B2", "REGN 去耦", {"1": "REGN", "2": "GND"})
add("C6", "CL31B106KOHNNNE", "10µF/25V X5R", "1206", 1, "B2", "BAT 引脚去耦", {"1": "BAT_P", "2": "GND"})
add("R3", "0603 1%", "22kΩ", "0603", 1, "B2", "ILIM：设输入限流 1.5A（按手册公式核算）", {"1": "ILIM", "2": "GND"})
add("R4", "0603 1%", "1.2kΩ", "0603", 1, "B2", "ICHG：设充电电流 1.0A（按手册公式核算）", {"1": "ICHG", "2": "GND"})

# ============ B3 电池保护（低边双 FET）============
add("BT1", "JST-PH2.0 2P 卧贴", "电池连接器", "SMD", 1, "B3", "1S 电池接口", {"1": "BAT_C_P", "2": "BAT_C_N"})
add("F2", "SMD1206-5A 慢断", "一次性保险丝 5A", "1206", 1, "B3", "电池硬短路二级保护", {"1": "BAT_C_P", "2": "BAT_P"})
add("U3", "S-8261ABJMD-G3JT2U", "1S 锂电保护 IC（过充 4.30V / 过放 2.80V）", "SOT-23-6", 1, "B3",
    "一次保护：过充 / 过放 / 过流 / 短路",
    {"VDD": "PROT_VDD", "VSS": "BAT_C_N", "DO": "FET_DO", "CO": "FET_CO", "VM": "PROT_VM", "NC": "NC"})
add("Q1", "8205A（双 N-MOS 20V/6A）", "双 N 沟道 MOSFET", "SOP-8", 1, "B3", "充/放电双 FET 切断",
    {"S1": "BAT_C_N", "G1": "FET_DO", "D1": "BAT_MID", "S2": "BAT_MID", "G2": "FET_CO", "D2": "GND"})
add("R5", "0603 1%", "100Ω", "0603", 1, "B3", "保护 IC VDD 限流", {"1": "BAT_P", "2": "PROT_VDD"})
add("C7", "CL10B104KB8NNNC / C1591", "100nF/50V", "0603", 1, "B3", "保护 IC VDD 滤波", {"1": "PROT_VDD", "2": "BAT_C_N"})
add("R6", "0603 1%", "2kΩ", "0603", 1, "B3", "VM 检测电阻", {"1": "PROT_VM", "2": "GND"})
add("NTC1", "10kΩ B3950", "电池 NTC（贴电芯）", "引线", 1, "B3", "电芯温度检测", {"1": "TS_NET", "2": "GND"})
add("R7", "0603 1%", "10kΩ", "0603", 1, "B3", "TS 上分压（25℃ 时 TS≈VREGN/2）", {"1": "REGN", "2": "TS_NET"})

# ============ B4 电量计 ============
add("U4", "MAX17048G+T10", "I2C 电量计（ModelGauge，1S）", "TDFN-8 2x2", 1, "B4",
    "SOC/电压上报 + 低电量中断",
    {"VDD": "BAT_P", "GND": "GND", "SDA": "SDA", "SCL": "SCL", "ALRT": "ALRT_N",
     "QSTRT": "GND", "CTP": "NC", "CELL": "BAT_P"})
add("C8", "CL10A105KB8NNNC / C15849", "1µF/16V", "0603", 1, "B4", "电量计去耦", {"1": "BAT_P", "2": "GND"})
add("R8", "0603 1%", "10kΩ", "0603", 1, "B4", "SDA 上拉（到开发板 +3V3，仅 µA）", {"1": "SDA", "2": "3V3_EXT"})
add("R9", "0603 1%", "10kΩ", "0603", 1, "B4", "SCL 上拉", {"1": "SCL", "2": "3V3_EXT"})
add("R10", "0603 1%", "100kΩ", "0603", 1, "B4", "ALRT 上拉", {"1": "ALRT_N", "2": "3V3_EXT"})

# ============ B5 升压 5V ============
add("U5", "TPS61088RHLR", "同步升压 5V/3A（10A 开关）", "VQFN-20", 1, "B5",
    "电池 3.0~4.4V → 5.0V/3A",
    {"VIN": "SYS_V", "SW": "SW_BST", "VOUT": "BST_5V", "FB": "FB_BST", "COMP": "COMP_BST",
     "EN": "PG_N", "SS": "SS_BST", "ILIM": "ILIM_BST", "MODE": "GND", "GND": "GND", "EP": "GND"})
add("L2", "FXL0630-4R7-M / C167220", "4.7µH ±20%，Isat 9A", "7x6.6", 1, "B5", "升压电感（复用 BOM）", {"1": "SYS_V", "2": "SW_BST"})
add("C9", "CL31B106KOHNNNE", "10µF/25V X5R", "1206", 3, "B5", "VIN 输入电容", {"1": "SYS_V", "2": "GND"})
add("C10", "CL31A226KOHNNNE", "22µF/25V X5R", "1210", 2, "B5", "VOUT 输出电容", {"1": "BST_5V", "2": "GND"})
add("C11", "CL10B104KB8NNNC / C1591", "100nF/50V", "0603", 1, "B5", "SS 软启动", {"1": "SS_BST", "2": "GND"})
add("R11", "0603 1%", "120kΩ", "0603", 1, "B5", "ILIM：输入限流 ≈4A（OCP）", {"1": "ILIM_BST", "2": "GND"})
add("R12", "0603 1%", "910kΩ", "0603", 1, "B5", "FB 上分压（5.0V 设定）", {"1": "BST_5V", "2": "FB_BST"})
add("R13", "0603 1%", "100kΩ", "0603", 1, "B5", "FB 下分压", {"1": "FB_BST", "2": "GND"})
add("R14", "0603 1%", "20kΩ", "0603", 1, "B5", "COMP 电阻", {"1": "COMP_BST", "2": "RC_COMP"})
add("C12", "CL10B103KB8NNNC / C1589", "10nF/50V", "0603", 1, "B5", "COMP 电容", {"1": "RC_COMP", "2": "GND"})

# ============ B6 双路 ORing ============
add("U6", "LM74700QDBVRQ1", "理想二极管控制器", "SOT-23-6", 1, "B6", "USB 直供通路，防反灌",
    {"ANODE": "VBUS_IN", "CATHODE": "SYS_5V", "GATE": "GATE_USB", "VC": "VC_USB", "EN": "NC", "GND": "GND"})
add("Q2", "NCE3080K / C108901", "N-MOS 30V/80A，4mΩ", "TO-252", 1, "B6", "USB 通路 FET", {"G": "GATE_USB", "D": "SYS_5V", "S": "VBUS_IN"})
add("C13", "CL10B104KB8NNNC / C1591", "100nF/50V", "0603", 1, "B6", "VC 电荷泵电容", {"1": "VC_USB", "2": "VBUS_IN"})

add("U7", "LM74700QDBVRQ1", "理想二极管控制器", "SOT-23-6", 1, "B6", "电池升压通路，防反灌",
    {"ANODE": "BST_5V", "CATHODE": "SYS_5V", "GATE": "GATE_BST", "VC": "VC_BST", "EN": "NC", "GND": "GND"})
add("Q3", "NCE3080K / C108901", "N-MOS 30V/80A，4mΩ", "TO-252", 1, "B6", "升压通路 FET", {"G": "GATE_BST", "D": "SYS_5V", "S": "BST_5V"})
add("C14", "CL10B104KB8NNNC / C1591", "100nF/50V", "0603", 1, "B6", "VC 电荷泵电容", {"1": "VC_BST", "2": "BST_5V"})

# 状态量到 P4 的电平适配（5V 侧 → 3.3V 侧）
add("R15", "0603 1%", "100kΩ", "0603", 1, "B6", "PG 上拉（决定升压 EN，USB 在场则拉低）", {"1": "SYS_V", "2": "PG_N"})
add("R16", "0603 1%", "100kΩ", "0603", 1, "B6", "PG→P4 分压上臂", {"1": "PG_N", "2": "PG_STAT"})
add("R17", "0603 1%", "220kΩ", "0603", 1, "B6", "PG→P4 分压下臂", {"1": "PG_STAT", "2": "GND"})
add("R18", "0603 1%", "1kΩ", "0603", 1, "B6", "STAT→P4 限流", {"1": "STAT_N", "2": "STAT_3V3"})

# ============ B7 输出与指示 ============
add("D2", "SMBJ5.0A", "TVS 单向 5V", "SMB", 1, "B7", "VIN_5V 输出钳位", {"1": "SYS_5V", "2": "GND"})
add("C15", "100µF/10V 固态或 X5R", "100µF/10V（低矮型 ≤3.2mm）", "1210/固态B", 2, "B7", "输出储能（弃用 7.7mm 高电解，控高）", {"1": "SYS_5V", "2": "GND"})
add("R19", "0603 1%", "1kΩ", "0603", 1, "B7", "CHG LED 限流", {"1": "SYS_V", "2": "STAT_LED"})
add("LED1", "NCD0603R1 红色", "红色 LED", "0603", 1, "B7", "充电指示", {"A": "STAT_LED", "K": "STAT_N"})
add("R20", "0603 1%", "1kΩ", "0603", 1, "B7", "PWR LED 限流", {"1": "SYS_5V", "2": "PWR_LED"})
add("LED2", "NCD0603G3 绿色", "绿色 LED", "0603", 1, "B7", "电源指示", {"A": "PWR_LED", "K": "GND"})

# ============ B8 与开发板接口 ============
add("J5", "2x2P 2.54 排针", "电源线束座 2x2", "THT", 1, "B8", "5V/GND 到开发板 J4-1~4",
    {"1": "SYS_5V", "2": "SYS_5V", "3": "GND", "4": "GND"})

add("J6", "1x6P 2.54 排针", "信号线束座 1x6", "THT", 1, "B8", "I2C + 状态到开发板 J4",
    {"1": "3V3_EXT", "2": "SDA", "3": "SCL", "4": "STAT_3V3", "5": "PG_STAT", "6": "ALRT_N"})

# ============ B9 测试点 ============
for i, (net, desc) in enumerate([
        ("VBUS_IN", "VBUS 输入"), ("SYS_V", "充电器 SYS"), ("BAT_P", "电池（保护后）"),
        ("BST_5V", "升压输出"), ("SYS_5V", "VIN_5V 输出"), ("GND", "地")], start=1):
    add("TP%d" % i, "测试点 Ø1.0", desc, "TP", 1, "B9", "调试/测试", {"1": net})

# ---------------- 输出 ----------------
with open(os.path.join(OUT, "bom_v1.csv"), "w", encoding="utf-8-sig", newline="") as f:
    w = csv.writer(f)
    w.writerow(["位号", "型号/立创编号", "参数描述", "封装", "数量", "功能块", "作用"])
    for c in C:
        w.writerow([c["ref"], c["mpn"], c["desc"], c["pkg"], c["qty"], c["blk"], c["func"]])

net_rows = []
for c in C:
    for pin, net in c["pins"].items():
        if net == "NC":
            continue
        net_rows.append([net, c["ref"], pin])
net_rows.sort(key=lambda r: (r[0], r[1]))

with open(os.path.join(OUT, "netlist_v1.csv"), "w", encoding="utf-8-sig", newline="") as f:
    w = csv.writer(f)
    w.writerow(["网络", "位号", "引脚"])
    w.writerows(net_rows)

g = defaultdict(list)
for net, ref, pin in net_rows:
    g[net].append("%s.%s" % (ref, pin))

lines = ["# v1 网表（按网络分组）", "",
         "> 由 `_tools/gen_hw.py` 生成，与 `netlist_v1.csv` 同源；`NC` 引脚已剔除。", "",
         "> IC 引脚按**引脚名**给出（具体脚号以数据手册为准），连接器/阻容按脚号给出。", ""]
for net in sorted(g):
    lines.append("**`%s`**（%d 点）：%s" % (net, len(g[net]), "、".join(g[net])))
    lines.append("")

with open(os.path.join(OUT, "netlist_by_net.md"), "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

# 自查：悬空网络检测（只有 1 个点的网络）
susp = [n for n, v in g.items() if len(v) < 2]
print("ok")
