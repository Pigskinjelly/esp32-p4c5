# -*- coding: utf-8 -*-
"""从 bom_v1.csv + netlist_v1.csv 生成 KiCad 5.1 工程，供嘉立创EDA专业版导入。"""
import csv, os, uuid, zipfile, collections

ROOT = r"<用户目录>\Desktop\ESP32-P4C5"
OUT = os.path.join(ROOT, "hardware", "power-board-v1", "kicad-import")
os.makedirs(OUT, exist_ok=True)

bom = {}
order = []
with open(os.path.join(ROOT, "docs/hw/bom_v1.csv"), encoding="utf-8-sig") as f:
    for r in csv.DictReader(f):
        ref = r["位号"].strip()
        if not ref or ref.startswith("#"):
            continue
        bom[ref] = r
        order.append(ref)

pins = collections.defaultdict(set)
net_of = {}
with open(os.path.join(ROOT, "docs/hw/netlist_v1.csv"), encoding="utf-8-sig") as f:
    for r in csv.DictReader(f):
        ref, pin, net = r["位号"].strip(), r["引脚"].strip(), r["网络"].strip()
        pins[ref].add(pin)
        net_of[(ref, pin)] = net

# 未在网表中出现的位号（理论不会有）也给两个脚
for ref in order:
    pins.setdefault(ref, {"1", "2"})

# ---------- 库 ----------
lib = ["EESchema-LIBRARY Version 2.4", "#encoding utf-8", "#"]

for ref in order:
    plist = sorted(pins[ref])
    n = len(plist)
    nleft = (n + 1) // 2
    nright = n - nleft
    h = max(500, max(nleft, nright) * 170 + 240)
    name = "SYM_" + ref
    val = bom[ref].get("型号/立创编号", ref).replace('"', "'")[:40]
    lib += ["#", "# " + name, "#",
            "DEF %s %s 0 40 Y Y 1 F N" % (name, name),
            'F0 "%s" 0 %d 50 H V C CNN' % (ref, h // 2 + 130),
            'F1 "%s" 0 %d 50 H V C CNN' % (val, -h // 2 - 130),
            'F2 "" 0 0 50 H I C CNN', 'F3 "" 0 0 50 H I C CNN',
            "DRAW",
            "S -200 -%d 200 %d 0 1 10 N" % (h // 2, h // 2)]
    for i, p in enumerate(plist):
        if i < nleft:
            y = h // 2 - 85 - i * 170
            lib.append("X %s %s -400 %d 200 R 1 1 P" % (p, p, y))
        else:
            y = h // 2 - 85 - (i - nleft) * 170
            lib.append("X %s %s 400 %d 200 L 1 1 P" % (p, p, y))
    lib += ["ENDDRAW", "ENDDEF"]
lib += ["#", "#End Library"]
open(os.path.join(OUT, "powerboard.lib"), "w", encoding="utf-8").write("\n".join(lib) + "\n")

# ---------- 原理图 ----------
COLS, CELLW, CELLH = 8, 3900, 2700
OX, OY = 900, 700
sch = ["EESchema Schematic File Version 4", "EELAYER 30 0", "EELAYER END",
       "$Descr A1 33102 23387", "encoding utf-8", "Sheet 1 1",
       'Title "ESP32-P4C5 Power & Storage Board"',
       'Date "2026-11-21"', 'Rev "v1.1"', 'Company ""',
       'Comment1 "52x72mm / 1S LiPo / BQ25606 + TPS61088"',
       'Comment2 "Generated from netlist_v1.csv"',
       'Comment3 ""', 'Comment4 ""', "$EndDescr"]

for idx, ref in enumerate(order):
    c, r = idx % COLS, idx // COLS
    cx = OX + c * CELLW + 1500
    cy = OY + r * CELLH + 1500
    plist = sorted(pins[ref])
    n = len(plist)
    nleft = (n + 1) // 2
    h = max(500, max(nleft, n - nleft) * 170 + 240)
    uid = uuid.uuid5(uuid.NAMESPACE_DNS, "p4c5-" + ref).hex
    val = bom[ref].get("型号/立创编号", ref).replace('"', "'")[:40]
    fp = bom[ref].get("封装", "") or ""
    sch += ["$Comp", "L powerboard:SYM_%s %s" % (ref, ref),
            "U 1 1 %s" % uid, "P %d %d" % (cx, cy),
            'F 0 "%s" H %d %d 50  0000 C CNN' % (ref, cx, cy - h // 2 - 130),
            'F 1 "%s" H %d %d 50  0000 C CNN' % (val, cx, cy + h // 2 + 130),
            'F 2 "%s" H %d %d 50  0001 C CNN' % (fp, cx, cy),
            'F 3 "" H %d %d 50  0001 C CNN' % (cx, cy),
            "\t1    %d %d" % (cx, cy), "\t1    0    0    -1  ", "$EndComp"]
    for i, p in enumerate(plist):
        net = net_of.get((ref, p), "NC_" + ref + "_" + p)
        if i < nleft:
            py = cy + h // 2 - 85 - i * 170
            px, ex, ang = cx - 400, cx - 640, 2
        else:
            py = cy + h // 2 - 85 - (i - nleft) * 170
            px, ex, ang = cx + 400, cx + 640, 0
        sch += ["Wire Wire Line", "\t%d %d %d %d" % (px, py, ex, py),
                "Text Label %d %d %d    50   ~ 0" % (ex, py, ang), net]

sch += ["$EndSCHEMATC"]
open(os.path.join(OUT, "power-board-v1.sch"), "w", encoding="utf-8").write("\n".join(sch) + "\n")

# ---------- 工程与库表 ----------
pro = """[general]
version=1
last_client=eeschema

[eeschema]
version=1
libsautostart=

[eeschema/libraries]
LibName1=powerboard
"""
open(os.path.join(OUT, "power-board-v1.pro"), "w", encoding="utf-8").write(pro)

slt = """(sym_lib_table
  (lib (name "powerboard")(type "Legacy")(uri "${KIPRJMOD}/powerboard.lib")(options "")(descr "P4C5 power board symbols"))
)
"""
open(os.path.join(OUT, "sym-lib-table"), "w", encoding="utf-8").write(slt)

zp = os.path.join(ROOT, "hardware", "power-board-v1", "power-board-v1_kicad.zip")
with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED) as z:
    for fn in ("power-board-v1.pro", "power-board-v1.sch", "powerboard.lib", "sym-lib-table"):
        z.write(os.path.join(OUT, fn), fn)
open(os.path.join(ROOT, "_gen_kicad.txt"), "w", encoding="utf-8").write(
    "refs=%d nets=%d zip=%s bytes=%d\n" % (len(order), len(net_of), zp, os.path.getsize(zp)))
