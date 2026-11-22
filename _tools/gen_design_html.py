# -*- coding: utf-8 -*-
"""生成 ESP32-P4C5 供电扩展板 v1 的可视化设计包（单文件 HTML）。"""
import csv
import os

OUT_DIR = r"<用户目录>\Desktop\ESP32-P4C5\hardware\power-board-v1"
os.makedirs(OUT_DIR, exist_ok=True)

C_WIRE = "#3a3a3a"
C_PIN = "#2c2c2a"
C_NET = "#185FA5"
C_NC = "#a8a8a0"
C_BODY = "#ffffff"
C_EDGE = "#2c2c2a"
C_ACC = "#993C1D"


def esc(s):
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def T(x, y, s, size=11, fill=C_PIN, anchor="start", weight="400"):
    return ('<text x="%.1f" y="%.1f" font-size="%s" fill="%s" text-anchor="%s" '
            'font-weight="%s" font-family="Inter,Helvetica,Arial,sans-serif">%s</text>'
            % (x, y, size, fill, anchor, weight, esc(s)))


def L(x1, y1, x2, y2, color=C_WIRE, w=1.1):
    return ('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="%s" '
            'stroke-linecap="round"/>' % (x1, y1, x2, y2, color, w))


def REC(x, y, w, h, fill=C_BODY, stroke=C_EDGE, sw=1.1, rx=2, dash=""):
    d = ' stroke-dasharray="%s"' % dash if dash else ""
    return ('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%s" fill="%s" '
            'stroke="%s" stroke-width="%s"%s/>' % (x, y, w, h, rx, fill, stroke, sw, d))


def GND(x, y):
    """接地符号：竖线末端三条横杠。"""
    s = [L(x, y - 10, x, y - 2, C_WIRE, 1.1),
         L(x - 7, y - 2, x + 7, y - 2, C_WIRE, 1.4),
         L(x - 4.5, y + 1.5, x + 4.5, y + 1.5, C_WIRE, 1.4),
         L(x - 2, y + 5, x + 2, y + 5, C_WIRE, 1.4)]
    return "".join(s)


class Sheet(object):
    def __init__(self, num, title, sub):
        self.num = num
        self.title = title
        self.sub = sub
        self.p = []

    def add(self, s):
        self.p.append(s)

    def ic(self, x, y, w, ref, name, left, right, pitch=28):
        n = max(len(left), len(right), 1)
        h = n * pitch + 64
        self.add(REC(x, y, w, h, C_BODY, C_EDGE, 1.3, 3))
        self.add(T(x + 12, y + 22, ref, 12, C_ACC, "start", "500"))
        self.add(T(x + w - 12, y + 22, name, 11, "#5F5E5A", "end"))
        self.add(L(x + 8, y + 32, x + w - 8, y + 32, "#d8d6ce", 0.9))
        for i, (pn, net) in enumerate(left):
            py = y + 50 + i * pitch
            self.add(L(x - 22, py, x, py))
            self.add(T(x + 10, py + 4, pn, 11, C_PIN))
            if net == "NC":
                self.add(T(x - 26, py + 4, "NC", 10, C_NC, "end"))
            else:
                self.add(T(x - 26, py + 4, net, 11, C_NET, "end"))
        for i, (pn, net) in enumerate(right):
            py = y + 50 + i * pitch
            self.add(L(x + w, py, x + w + 22, py))
            self.add(T(x + w - 10, py + 4, pn, 11, C_PIN, "end"))
            if net == "NC":
                self.add(T(x + w + 26, py + 4, "NC", 10, C_NC, "start"))
            else:
                self.add(T(x + w + 26, py + 4, net, 11, C_NET, "start"))
        return h

    def part2(self, x, y, kind, ref, val, top, bot, h=38):
        t = y - h / 2.0
        b = y + h / 2.0
        self.add(L(x, t - 20, x, t))
        self.add(L(x, b, x, b + 20))
        if kind == "R":
            self.add(REC(x - 8, t + 3, 16, h - 6, C_BODY, C_EDGE, 1.1, 0))
        elif kind == "C":
            self.add(L(x - 10, y - 3, x + 10, y - 3, C_EDGE, 1.8))
            self.add(L(x - 10, y + 3, x + 10, y + 3, C_EDGE, 1.8))
            self.add(L(x, t - 3, x, y - 3))
            self.add(L(x, y + 3, x, b + 3))
        elif kind == "L":
            self.add(REC(x - 8, t + 3, 16, h - 6, "#F1EFE8", C_EDGE, 1.1, 0))
            for k in range(1, 4):
                self.add(L(x - 8, t + 3 + k * (h - 6) / 4.0, x + 8, t + 3 + k * (h - 6) / 4.0, C_EDGE, 0.8))
        elif kind == "F":
            self.add(REC(x - 8, t + 3, 16, h - 6, C_BODY, C_EDGE, 1.1, 0))
            self.add('<path d="M%.1f %.1f Q%.1f %.1f %.1f %.1f T%.1f %.1f" fill="none" stroke="%s" stroke-width="1"/>'
                     % (x - 6, y, x - 3, y - 6, x, y, x + 6, y, C_EDGE))
        elif kind in ("D", "LED"):
            self.add('<path d="M%.1f %.1f L%.1f %.1f L%.1f %.1f Z" fill="%s" stroke="%s" stroke-width="1"/>'
                     % (x - 7, t + 6, x + 7, t + 6, x, t + 20, C_BODY, C_EDGE))
            self.add(L(x - 7, t + 20, x + 7, t + 20, C_EDGE, 1.6))
            self.add(L(x, t, x, t + 6))
            self.add(L(x, t + 20, x, b + 3))
            if kind == "LED":
                self.add('<path d="M%.1f %.1f l6 -5 M%.1f %.1f l6 -5" stroke="%s" stroke-width="0.9" fill="none"/>'
                         % (x + 8, y - 2, x + 8, y + 4, C_EDGE))
        elif kind == "N":
            self.add(REC(x - 8, t + 3, 16, h - 6, C_BODY, C_EDGE, 1.1, 0))
            self.add(L(x - 10, b + 2, x - 4, t - 2, C_EDGE, 0.9))
        self.add(T(x, t - 26, top, 11, C_NET, "middle"))
        if bot == "GND":
            self.add(GND(x, b + 22))
            self.add(T(x, b + 44, "GND", 10, "#5F5E5A", "middle"))
        else:
            self.add(T(x, b + 34, bot, 11, C_NET, "middle"))
        self.add(T(x + 16, y - 3, ref, 10.5, C_ACC))
        self.add(T(x + 16, y + 10, val, 10, "#5F5E5A"))
        return h

    def note(self, x, y, lines, w=460):
        h = 18 * len(lines) + 14
        self.add(REC(x, y, w, h, "#FAEEDA", "#EF9F27", 0.9, 3))
        for i, s in enumerate(lines):
            self.add(T(x + 12, y + 22 + i * 18, s, 10.5, "#633806"))
        return h

    def render(self):
        head = ('<svg viewBox="0 0 1200 792" width="100%" xmlns="http://www.w3.org/2000/svg" '
                'font-family="Inter,Helvetica,Arial,sans-serif">')
        tb = [L(40, 730, 1160, 730, "#2c2c2a", 1.4),
              T(40, 752, "ESP32-P4C5 供电/储能扩展板  v1.1", 12, C_PIN, "start", "500"),
              T(40, 770, "原理图 · 第 %d 页 · %s" % (self.num, self.sub), 10.5, "#5F5E5A"),
              T(1160, 752, self.title, 11, C_PIN, "end"),
              T(1160, 770, "A3 · 2026-11-21 · Sheet %d/8" % self.num, 10, "#5F5E5A", "end")]
        return head + "".join(self.p) + "".join(tb) + "</svg>"


sheets = []

# ================= P1 =================
s = Sheet(1, "Type-C 输入与保护", "5V 受电 · CC 5.1k · ESD · 3A 保险丝")
s.ic(80, 120, 250, "J1", "TYPE-C-16P", [],
     [("VBUS A4/A9", "VBUS_TC"), ("B4/B9", "VBUS_TC"), ("GND", "GND"),
      ("CC1", "CC1"), ("CC2", "CC2"), ("D+ A6/B6", "USB_DP"), ("D- A7/B7", "USB_DN")])
s.part2(500, 340, "F", "F1", "3A 慢断", "VBUS_TC", "VBUS_IN")
s.part2(630, 340, "D", "D1", "SMBJ5.0A", "VBUS_IN", "GND")
s.ic(720, 120, 250, "U1", "USBLC6-2SC6",
     [("IO1", "CC1"), ("IO2", "CC2"), ("VBUS", "VBUS_IN"), ("GND", "GND")],
     [("IO3", "NC"), ("IO4", "NC")])
s.part2(1080, 200, "R", "R1", "5.1k", "CC1", "GND")
s.part2(1080, 360, "R", "R2", "5.1k", "CC2", "GND")
s.note(80, 470, [
    "CC1/CC2 各 5.1k 下拉 = 纯受电端(UFP)，向充电器声明 5V/3A；不做 PD 诱骗。",
    "D+/D- 同时接到 U2 的 D+/D-，让 BQ25606 做 BC1.2 识别，否则会按 USB100（500mA）限流。",
    "F1 为一次性熔断（3A），D1 为浪涌钳位，U1 保护 CC 与数据脚。"], 700)
s.note(80, 590, ["禁止：本口不对外输出 5V。板载 Type-C 只做充电输入。"], 460)
sheets.append(s)

# ================= P2 =================
s = Sheet(2, "充电与电源路径", "BQ25606 · 1A · Power Path")
s.ic(340, 100, 360, "U2", "BQ25606",
     [("VBUS", "VBUS_IN"), ("PMID", "PMID"), ("BTST", "BTST"), ("SW", "SW_CHG"),
      ("SYS", "SYS_V"), ("BAT", "BAT_P"), ("GND", "GND")],
     [("ILIM", "ILIM"), ("ICHG", "ICHG"), ("TS", "TS_NET"), ("STAT", "STAT_N"),
      ("PG", "PG_N"), ("CE", "GND"), ("REGN", "REGN"), ("D+", "USB_DP"), ("D-", "USB_DN")])
s.part2(100, 170, "C", "C1", "10uF x2", "VBUS_IN", "GND")
s.part2(100, 300, "C", "C2", "10uF x2", "PMID", "GND")
s.part2(100, 430, "C", "C3", "100nF", "BTST", "SW_CHG")
s.part2(920, 170, "C", "C4", "10uF x2", "SYS_V", "GND")
s.part2(920, 300, "C", "C5", "4.7uF", "REGN", "GND")
s.part2(920, 430, "C", "C6", "10uF", "BAT_P", "GND")
s.part2(200, 610, "L", "L1", "1.5uH", "SW_CHG", "SYS_V")
s.part2(340, 610, "R", "R3", "22k", "ILIM", "GND")
s.part2(480, 610, "R", "R4", "1.2k", "ICHG", "GND")
s.note(620, 500, [
    "R3 设输入限流 1.5A、R4 设充电电流 1.0A，实际取值按手册公式复核后定稿。",
    "SYS 是路径管理输出：插电时由 USB 供电，同时给电池充电；拔电时由电池经 BATFET 接管。",
    "TS 接 NTC 分压（见 P3），<0C 或 >45C 自动停充。"], 480)
sheets.append(s)

# ================= P3 =================
s = Sheet(3, "电池一次保护", "S-8261 + 8205A 低边双 FET")
s.ic(140, 110, 170, "BT1", "JST-PH2.0", [("1 +", "BAT_C_P"), ("2 -", "BAT_C_N")], [])
s.part2(330, 340, "F", "F2", "5A 慢断", "BAT_C_P", "BAT_P")
s.ic(440, 110, 190, "U3", "S-8261",
     [("VDD", "PROT_VDD"), ("VSS", "BAT_C_N"), ("VM", "PROT_VM")],
     [("DO", "FET_DO"), ("CO", "FET_CO"), ("NC", "NC")])
s.ic(800, 110, 230, "Q1", "8205A 双N-MOS",
     [("S1", "BAT_C_N"), ("G1", "FET_DO"), ("D1", "BAT_MID")],
     [("S2", "BAT_MID"), ("G2", "FET_CO"), ("D2", "GND")])
s.part2(120, 470, "R", "R5", "100R", "BAT_P", "PROT_VDD")
s.part2(240, 470, "C", "C7", "100nF", "PROT_VDD", "BAT_C_N")
s.part2(360, 470, "R", "R6", "2k", "PROT_VM", "GND")
s.part2(480, 470, "R", "R7", "10k", "REGN", "TS_NET")
s.part2(600, 470, "N", "NTC1", "10k B3950", "TS_NET", "GND")
s.note(700, 420, [
    "FET 在低边：BAT_C_N -> Q1 -> GND，U3.VSS 接 BAT_C_N，VM 经 2k 到 GND。",
    "过充 4.30V / 过放 2.80V / 过流 8~12A / 短路 <1ms。",
    "F2 为第二层保护，防止 FET 击穿后电池直接短路。"], 400)
sheets.append(s)

# ================= P4 =================
s = Sheet(4, "电量计", "MAX17048 · I2C")
s.ic(340, 120, 320, "U4", "MAX17048",
     [("VDD", "BAT_P"), ("CELL", "BAT_P"), ("GND", "GND")],
     [("SDA", "SDA"), ("SCL", "SCL"), ("ALRT", "ALRT_N"), ("QSTRT", "GND")])
s.part2(140, 300, "C", "C8", "1uF", "BAT_P", "GND")
s.part2(880, 220, "R", "R8", "10k", "SDA", "3V3_EXT")
s.part2(880, 360, "R", "R9", "10k", "SCL", "3V3_EXT")
s.part2(1000, 290, "R", "R10", "100k", "ALRT_N", "3V3_EXT")
s.note(340, 430, [
    "SDA/SCL/ALRT 的上拉统一接到 J3-5/6 的 +3V3（只取 uA 级电流，绝不从此脚取电）。",
    "MAX17048 的 VIH = 0.7 x VDD：满电 4.2V 时阈值 2.94V，上拉到 3.3V 余量 0.36V，可用但需实测波形。",
    "QSTRT 接地 = 上电即快速预测 SOC。ALRT 用于通知 P4 低电量关机。"], 620)
sheets.append(s)

# ================= P5 =================
s = Sheet(5, "升压 5V", "TPS61088 · 5.0V/2.5A")
s.ic(340, 110, 380, "U5", "TPS61088",
     [("VIN", "SYS_V"), ("EN", "PG_N"), ("SS", "SS_BST"), ("ILIM", "ILIM_BST"), ("MODE", "GND")],
     [("SW", "SW_BST"), ("VOUT", "BST_5V"), ("FB", "FB_BST"), ("COMP", "COMP_BST"), ("GND", "GND")])
s.part2(120, 250, "L", "L2", "4.7uH/9A", "SYS_V", "SW_BST")
s.part2(900, 250, "C", "C9", "10uF x3", "SYS_V", "GND")
s.part2(1010, 250, "C", "C10", "22uF x2", "BST_5V", "GND")
s.part2(1120, 250, "C", "C11", "100nF", "SS_BST", "GND")
s.part2(120, 470, "R", "R11", "120k", "ILIM_BST", "GND")
s.part2(250, 470, "R", "R12", "910k", "BST_5V", "FB_BST")
s.part2(380, 470, "R", "R13", "100k", "FB_BST", "GND")
s.part2(510, 470, "R", "R14", "20k", "COMP_BST", "RC_COMP")
s.part2(640, 470, "C", "C12", "10nF", "RC_COMP", "GND")
s.note(800, 420, [
    "EN 直接接 U2 的 /PG：USB 在场 -> /PG 拉低 -> 升压停；拔掉 -> 上拉 -> 升压启动。",
    "R11 设输入限流约 4A，兼作输出短路的 OCP；打嗝式恢复。",
    "R12/R13 分压把输出定在 5.0V；C9 必须靠近 VIN 脚，C10 靠近 VOUT 脚。"], 360)
sheets.append(s)

# ================= P6 =================
s = Sheet(6, "双路理想二极管", "LM74700-Q1 防反灌")
s.ic(160, 100, 300, "U6", "LM74700 A",
     [("ANODE", "VBUS_IN"), ("CATHODE", "SYS_5V"), ("EN", "NC")],
     [("GATE", "GATE_USB"), ("VC", "VC_USB"), ("GND", "GND")])
s.ic(660, 100, 240, "Q2", "NCE3080K",
     [("G", "GATE_USB")],
     [("D", "SYS_5V"), ("S", "VBUS_IN")])
s.part2(1020, 220, "C", "C13", "100nF", "VC_USB", "VBUS_IN")
s.ic(160, 420, 300, "U7", "LM74700 B",
     [("ANODE", "BST_5V"), ("CATHODE", "SYS_5V"), ("EN", "NC")],
     [("GATE", "GATE_BST"), ("VC", "VC_BST"), ("GND", "GND")])
s.ic(660, 420, 240, "Q3", "NCE3080K",
     [("G", "GATE_BST")],
     [("D", "SYS_5V"), ("S", "BST_5V")])
s.part2(1020, 540, "C", "C14", "100nF", "VC_BST", "BST_5V")
s.note(430, 300, [
    "两路输出到同一节点 SYS_5V，谁高谁导通，另一边反向截止。",
    "Q2/Q3 用 NCE3080K（30V/80A/4mR），对 2.5A 严重富余，但压降只有 10mV 级且不需散热。"], 480)
sheets.append(s)

# ================= P7 =================
s = Sheet(7, "输出与状态", "TVS · 储能 · 电平适配 · 指示灯")
s.part2(100, 200, "D", "D2", "SMBJ5.0A", "SYS_5V", "GND")
s.part2(220, 200, "C", "C15", "100uF x2", "SYS_5V", "GND")
s.part2(400, 200, "R", "R15", "100k", "SYS_V", "PG_N")
s.part2(520, 200, "R", "R16", "100k", "PG_N", "PG_STAT")
s.part2(640, 200, "R", "R17", "220k", "PG_STAT", "GND")
s.part2(760, 200, "R", "R18", "1k", "STAT_N", "STAT_3V3")
s.part2(880, 200, "R", "R19", "1k", "SYS_V", "STAT_LED")
s.part2(1000, 200, "LED", "LED1", "红", "STAT_LED", "STAT_N")
s.part2(880, 430, "R", "R20", "1k", "SYS_5V", "PWR_LED")
s.part2(1000, 430, "LED", "LED2", "绿", "PWR_LED", "GND")
s.note(100, 480, [
    "PG_N 是 4.4V 电平，进 P4 前必须过 R16/R17 分压到 3.0V 左右，绝不能直连。",
    "STAT_N 被 LED 抬到约 3.1V，进 P4 前串 R18（1k）。",
    "C15 用低矮型（<=3.2mm），不用 7.7mm 高电解 —— 堆叠高度不允许。"], 700)
sheets.append(s)

# ================= P8 =================
s = Sheet(8, "对外接线", "J5 电源 / J6 信号 -> 开发板 J4")
s.ic(120, 120, 260, "J5", "2x2P 2.54",
     [], [("1", "SYS_5V"), ("2", "SYS_5V"), ("3", "GND"), ("4", "GND")])
s.ic(560, 110, 280, "J6", "1x6P 2.54",
     [], [("1", "3V3_EXT"), ("2", "SDA"), ("3", "SCL"),
          ("4", "STAT_3V3"), ("5", "PG_STAT"), ("6", "ALRT_N")])
s.note(120, 386, [
    "J5-1/2 -> J4-1/2 (VIN_5V, 双线并联降低压降)      J5-3/4 -> J4-3/4 (GND)",
    "J6-1 -> J4-5/6 (+3V3, 只取 I2C 上拉的 uA 级电流, 丝印标『禁止取电』)",
    "J6-2 -> J4-31 (GPIO20 SDA)      J6-3 -> J4-32 (GPIO21 SCL)",
    "J6-4 -> J4-33 (GPIO22 STAT)     J6-5 -> J4-34 (GPIO23 PG)     J6-6 -> J4-19 (GPIO2 ALRT)"], 760)
s.note(120, 520, [
    "为什么用线束而不用排母直接堆叠: 开发板两排 2x17 排针的横向间距无法从公开资料确证,",
    "堆叠一旦偏 1.27mm 就插不进去。线束方案只依赖 4 个安装孔(46.15 x 66.16), 100% 可装配。",
    "若后续拿到底板 STEP 并确认排针坐标, 可改回排母直插(板框不变)。"], 760)
for i, (nm, lab) in enumerate([("TP1", "VBUS_IN"), ("TP2", "SYS_V"), ("TP3", "BAT_P"),
                               ("TP4", "BST_5V"), ("TP5", "SYS_5V"), ("TP6", "GND")]):
    x = 150 + i * 120
    s.add('<circle cx="%.1f" cy="640" r="7" fill="none" stroke="%s" stroke-width="1.2"/>' % (x, C_EDGE))
    s.add('<circle cx="%.1f" cy="640" r="2" fill="%s"/>' % (x, C_EDGE))
    s.add(T(x, 620, nm, 10.5, C_ACC, "middle"))
    s.add(T(x, 666, lab, 10, C_NET, "middle"))
sheets.append(s)

# ================= PCB 布局 =================
SC = 6.6
BX, BY = 170, 64


def mm(v):
    return v * SC


def fx(x):
    return BX + x * SC


def fy(y):
    return BY + y * SC


def fp(x, y, w, h, ref, fill="#EDEAE2", stroke="#8a8880"):
    return (REC(fx(x), fy(y), mm(w), mm(h), fill, stroke, 0.9, 2) +
            T(fx(x) + mm(w) / 2, fy(y) + mm(h) / 2 + 3.0, ref, 7.5, "#444441", "middle"))


pcb = []
pcb.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%d" fill="#F7F6F1" stroke="%s" stroke-width="1.6"/>'
           % (fx(0), fy(0), mm(52), mm(72), int(mm(3)), "#2c2c2a"))
pcb.append('<path d="M%.1f %.1f h%.1f v%.1f h%.1f z" fill="#E6F1FB" opacity="0.85"/>'
           % (fx(6), fy(13), mm(40), mm(9), -mm(40)))
pcb.append('<path d="M%.1f %.1f h%.1f v%.1f h%.1f z" fill="#FAEEDA" opacity="0.75"/>'
           % (fx(6), fy(24), mm(40), mm(9), -mm(40)))

for hx, hy in ((2.925, 2.925), (49.075, 2.925), (2.925, 69.075), (49.075, 69.075)):
    pcb.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="#ffffff" stroke="#2c2c2a" stroke-width="1.1"/>'
               % (fx(hx), fy(hy), mm(1.6)))
    pcb.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="none" stroke="#a8a8a0" stroke-width="0.7" stroke-dasharray="2 2"/>'
               % (fx(hx), fy(hy), mm(2.8)))

parts = [
    (21.5, 0.8, 9.0, 7.4, "J1"),
    (14.5, 9.6, 3.0, 1.7, "U1"),
    (19.0, 9.6, 3.2, 1.6, "F1"),
    (24.2, 9.4, 4.0, 2.4, "D1"),
    (30.0, 9.6, 3.2, 1.6, "C1"),
    (35.2, 9.6, 3.2, 1.6, "C2"),
    (7.5, 15.0, 7.0, 7.0, "L1"),
    (16.5, 17.0, 4.0, 4.0, "U2"),
    (22.0, 16.6, 3.2, 1.6, "C4"),
    (27.0, 16.6, 3.2, 1.6, "C6"),
    (7.5, 26.0, 7.0, 6.6, "L2"),
    (16.5, 27.4, 3.5, 4.5, "U5"),
    (22.0, 27.0, 3.2, 1.6, "C9"),
    (27.0, 27.0, 3.2, 1.6, "C10"),
    (33.0, 26.4, 3.0, 1.7, "U6"),
    (33.0, 32.4, 3.0, 1.7, "U7"),
    (38.2, 25.0, 6.6, 6.0, "Q2"),
    (38.2, 33.0, 6.6, 6.0, "Q3"),
    (7.5, 37.4, 3.0, 1.7, "U3"),
    (12.8, 36.4, 5.0, 4.0, "Q1"),
    (19.2, 37.4, 3.2, 1.6, "F2"),
    (24.5, 36.9, 4.3, 2.6, "D2"),
    (30.5, 36.8, 5.0, 3.0, "C15"),
    (40.0, 40.5, 7.0, 4.5, "BT1"),
    (7.5, 43.6, 2.0, 2.0, "U4"),
    (11.2, 43.8, 3.0, 1.4, "LED"),
    (8.0, 49.5, 15.2, 2.5, "J6"),
    (30.0, 48.4, 5.1, 5.1, "J5"),
]
for x, y, w, h, ref in parts:
    pcb.append(fp(x, y, w, h, ref))

for i, tx in enumerate((12, 18, 24, 30, 36, 42)):
    pcb.append('<circle cx="%.1f" cy="%.1f" r="3.4" fill="none" stroke="%s" stroke-width="1"/>' % (fx(tx), fy(60.5), C_ACC))

pcb.append(T(fx(26), fy(76.2), "52.00 mm（安装孔 46.15）", 9, "#5F5E5A", "middle"))
pcb.append(T(fx(-2.2), fy(36), "72.00 mm（孔距 66.16）", 9, "#5F5E5A", "middle"))
pcb.append(T(fx(0), fy(-3.2), "布局示意（未布线）· 52 x 72 mm · 4 层 · 与开发板同尺寸、同安装孔", 10, "#993C1D"))
pcb_svg = ('<svg viewBox="0 0 700 600" width="100%" xmlns="http://www.w3.org/2000/svg">' +
           "".join(pcb) + "</svg>")

# ================= 剖面 =================
sec = []
y0 = 60
layers = [("开发板 PCB", 1.6, "#D3D1C7"), ("M3 铜柱", 8.0, "#EF9F27"),
          ("扩展板 PCB", 1.6, "#97C459"), ("元件面（最高 4.5）", 4.5, "#85B7EB")]
yy = y0
for name, h, col in layers:
    hh = h * 9
    sec.append(REC(180, yy, 300, hh, col, "#2c2c2a", 0.9, 1))
    sec.append(T(500, yy + hh / 2 + 4, "%s  %.1f mm" % (name, h), 11, "#2c2c2a"))
    yy += hh
sec.append(L(140, y0, 140, yy, "#993C1D", 1.2))
sec.append(T(132, (y0 + yy) / 2, "合计 15.7", 11, "#993C1D", "end"))
sec.append(L(150, y0, 510, y0, "#a8a8a0", 0.8))
sec.append(L(150, yy, 510, yy, "#a8a8a0", 0.8))
sec.append(GND(120, y0))
sec.append(T(60, yy + 40, "后壳内深 21.5 mm；开发板 1.6 + 铜柱 8 + 扩展板 1.6 + 元件 4.5 = 15.7 mm", 11, "#5F5E5A"))
sec.append(T(60, yy + 62, "若超差：铜柱改 5mm（省 3mm）或后壳加垫圈；线束比排母自由得多", 11, "#5F5E5A"))
sec_svg = ('<svg viewBox="0 0 720 340" width="100%" xmlns="http://www.w3.org/2000/svg">' +
           "".join(sec) + "</svg>")

# ================= BOM 表 =================
rows = []
with open(r"<用户目录>\Desktop\ESP32-P4C5\docs\hw\bom_v1.csv", encoding="utf-8-sig") as f:
    for r in csv.reader(f):
        rows.append(r)
bom_html = ['<table><thead><tr>' + "".join("<th>%s</th>" % esc(h) for h in rows[0]) + "</tr></thead><tbody>"]
for r in rows[1:]:
    bom_html.append("<tr>" + "".join("<td>%s</td>" % esc(c) for c in r) + "</tr>")
bom_html.append("</tbody></table>")
bom_html = "\n".join(bom_html)

# ================= HTML =================
CSS = """
*{box-sizing:border-box}
body{margin:0;background:#F7F6F1;color:#2c2c2a;font-family:Inter,Helvetica,Arial,sans-serif;font-size:13px}
.wrap{max-width:1240px;margin:0 auto;padding:24px 20px 60px}
h1{font-size:19px;font-weight:500;margin:0 0 6px}
.sub{color:#5F5E5A;font-size:12.5px;margin-bottom:18px;line-height:1.7}
.kpi{display:flex;gap:10px;flex-wrap:wrap;margin:0 0 20px}
.kpi div{background:#fff;border:1px solid #E3E0D6;border-radius:10px;padding:10px 14px;min-width:110px}
.kpi b{display:block;font-size:15px;font-weight:500;color:#993C1D}
.kpi span{font-size:11px;color:#5F5E5A}
.tabs{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:14px}
.tabs button{border:1px solid #E3E0D6;background:#fff;border-radius:8px;padding:7px 12px;font-size:12px;
color:#2c2c2a;cursor:pointer;font-family:inherit}
.tabs button.on{background:#2c2c2a;color:#fff;border-color:#2c2c2a}
.panel{display:none;background:#fff;border:1px solid #E3E0D6;border-radius:12px;padding:16px}
.panel.on{display:block}
.panel h2{font-size:13px;font-weight:500;margin:0 0 10px;color:#5F5E5A}
table{border-collapse:collapse;width:100%;font-size:11.5px}
th,td{border-bottom:1px solid #EFEDE6;padding:5px 8px;text-align:left;vertical-align:top}
th{background:#FAF9F5;font-weight:500;color:#5F5E5A;position:sticky;top:0}
td:nth-child(5),td:nth-child(6){text-align:center}
.legend{font-size:11.5px;color:#5F5E5A;line-height:1.9;margin-top:10px}
"""

TABS = [("s%d" % s.num, "P%d %s" % (s.num, s.title.split("·")[-1].strip() if "·" in s.title else s.title)) for s in sheets]
TABS.append(("pcb", "PCB 布局"))
TABS.append(("sec", "堆叠剖面"))
TABS.append(("bom", "BOM 明细"))

tabs_html = "".join('<button data-t="%s"%s>%s</button>' % (k, ' class="on"' if i == 0 else "", esc(lab))
                    for i, (k, lab) in enumerate(TABS))
panels = "".join('<section class="panel%s" id="%s">%s</section>' %
                 (' on' if i == 0 else "", k, s.render()) for i, (s, (k, _)) in enumerate(zip(sheets, TABS)))

panels += '<section class="panel" id="pcb"><h2>PCB 顶视图 · 52 x 72 mm · 4 层 · 与开发板同安装孔</h2>%s' \
          '<div class="legend">深灰圆点 = 2x17P 排母（装在底面，朝开发板）；浅蓝 = 5V 主铜皮，浅橙 = BAT/SYS 主铜皮；' \
          '橙色小圈 = TP1~TP6 测试点；仅画出 22 个主要器件，其余 40 余只阻容按就近原则布置未示意。' \
          '此图为布局示意，正式走线与铺铜在布线阶段生成。</div></section>' % pcb_svg
panels += '<section class="panel" id="sec"><h2>垂直堆叠剖面（后壳内深 21.5 mm）</h2>%s</section>' % sec_svg
panels += '<section class="panel" id="bom"><h2>BOM v1 · 64 个位号</h2>%s</section>' % bom_html

JS = """
const btns=document.querySelectorAll('.tabs button');
btns.forEach(b=>b.addEventListener('click',()=>{
  btns.forEach(x=>x.classList.remove('on'));
  document.querySelectorAll('.panel').forEach(p=>p.classList.remove('on'));
  b.classList.add('on');
  document.getElementById(b.dataset.t).classList.add('on');
}));
"""

html = """<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-P4C5 供电扩展板 v1 · 设计评审</title>
<style>%s</style></head>
<body><div class="wrap">
<h1>ESP32-P4C5 供电/储能扩展板 · v1 设计评审包</h1>
<div class="sub">52x72mm 四层板，与开发板同尺寸同安装孔，用线束接到 J4。板载 Type-C 给 1S 电池充电，电池升压到 5V 经理想二极管注入开发板 VIN_5V。<br>
原理图共 8 页，按实际网表绘制；PCB 为布局示意（未布线）；BOM 64 个位号，其中 12 项复用你现有 BOM。</div>
<div class="kpi">
<div><b>5.0V / 2.5A</b><span>输出能力</span></div>
<div><b>1S 锂电</b><span>3.0-4.2V</span></div>
<div><b>1.0A</b><span>充电电流</span></div>
<div><b>4 层</b><span>1.6mm · 1oz</span></div>
<div><b>15.7mm</b><span>堆叠高度</span></div>
<div><b>52x72</b><span>板框 mm</span></div>
</div>
<div class="tabs">%s</div>
%s
</div>
<script>%s</script>
</body></html>""" % (CSS, tabs_html, panels, JS)

path = os.path.join(OUT_DIR, "design-review.html")
with open(path, "w", encoding="utf-8") as f:
    f.write(html)
print("ok")


# ================= 单页全图（A1 竖版） =================
COLS, ROWS = 2, 4
TOP = 96
BOT = 96
SW, SH = 1200, 792
BIG_W = SW * COLS
BIG_H = TOP + SH * ROWS + BOT

blocks = []
for i, sh in enumerate(sheets):
    c, r = i % COLS, i // COLS
    inner = sh.render()
    body = inner[inner.index(">") + 1: inner.rindex("</svg>")]
    blocks.append('<g transform="translate(%d,%d)">%s</g>' % (c * SW, TOP + r * SH, body))

x = BIG_W / 2.0
big = ['<svg viewBox="0 0 %d %d" width="%d" height="%d" xmlns="http://www.w3.org/2000/svg" '
       'font-family="Inter,Helvetica,Arial,sans-serif">' % (BIG_W, BIG_H, BIG_W, BIG_H)]
big.append('<rect x="0" y="0" width="%d" height="%d" fill="#ffffff"/>' % (BIG_W, BIG_H))
big.append('<rect x="18" y="18" width="%d" height="%d" fill="none" stroke="#2c2c2a" stroke-width="3"/>'
           % (BIG_W - 36, BIG_H - 36))
big.append(T(40, 56, "ESP32-P4C5 供电 / 储能扩展板 · 整机原理图", 26, "#2c2c2a", "start", "500"))
big.append(T(40, 82, "v1.1 · 2026-11-21 · 52.00 x 72.00 mm · 4 层 · 1S 锂离子或锂聚合物电池 · 5V/2.5A 输出",
             14, "#5F5E5A"))
big.append(T(BIG_W - 40, 56, "8 / 8 页合图", 15, "#993C1D", "end", "500"))
big.append(T(BIG_W - 40, 80, "连线按 netlist_v1.csv 逐脚核对", 12, "#5F5E5A", "end"))
for r in range(1, ROWS):
    yy = TOP + r * SH
    big.append(L(30, yy, BIG_W - 30, yy, "#d8d6ce", 1.0))
big.append(L(BIG_W / 2, TOP, BIG_W / 2, TOP + ROWS * SH, "#d8d6ce", 1.0))

ny = TOP + ROWS * SH + 34
big.append(T(40, ny, "电池说明：1S 锂离子与 1S 锂聚合物电气特性相同（标称 3.7V / 满充 4.2V / 放电截止 2.8V），本板两者通用。", 15, "#633806"))
big.append(T(40, ny + 26, "选型要求：4.2V 截止（不要 4.35V/4.4V 高压电芯）· 容量 3000~5000mAh · 持续放电 >=3C · 自带保护板为佳 · NTC 10k 贴电芯表面。", 15, "#633806"))
big.append(T(40, ny + 52, "安全提示：软包电芯怕穿刺与鼓包，安装时不得压在螺丝/焊点/排针上；JST-PH 座有方向，插反无保护。", 15, "#A32D2D"))
big += blocks
big.append("</svg>")
bigsvg = "".join(big)
open(r"C://Users//<用户名>//Desktop//ESP32-P4C5//hardware//power-board-v1//schematic-full.svg", "w", encoding="utf-8").write(bigsvg)

print_html = """<!DOCTYPE html><html><head><meta charset="utf-8"><style>
@page{size:600mm 860mm;margin:0}
html,body{margin:0;padding:0;background:#fff}
svg{display:block}
</style></head><body>%s</body></html>""" % bigsvg
open(r"C://Users//<用户名>//Desktop//ESP32-P4C5//hardware//power-board-v1//_print.html", "w", encoding="utf-8").write(print_html)
print("sheet ok")
