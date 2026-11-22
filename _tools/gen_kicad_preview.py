# -*- coding: utf-8 -*-
"""从生成的 KiCad .sch 画出布局预览 SVG/HTML，用于导入后对照。"""
import os, re

ROOT = r"<用户目录>\Desktop\ESP32-P4C5"
SDIR = os.path.join(ROOT, "hardware", "power-board-v1", "kicad-import")
sch = open(os.path.join(SDIR, "power-board-v1.sch"), encoding="utf-8").read().splitlines()

comps = []
i = 0
while i < len(sch):
    if sch[i] == "$Comp":
        blk = []
        i += 1
        while i < len(sch) and sch[i] != "$EndComp":
            blk.append(sch[i]); i += 1
        l = next((x for x in blk if x.startswith("L ")), "")
        p = next((x for x in blk if x.startswith("P ")), "")
        f1 = next((x for x in blk if x.startswith('F 1 "')), "")
        ref = l.split()[-1] if l else "?"
        xy = p.split()[1:3] if p else ["0", "0"]
        val = re.search(r'F 1 "([^"]*)"', f1)
        comps.append((ref, int(xy[0]), int(xy[1]), val.group(1) if val else ""))
    i += 1

labels = []
for j, line in enumerate(sch):
    if line.startswith("Text Label "):
        m = re.match(r"Text Label (\d+) (\d+)", line)
        if m and j + 1 < len(sch):
            labels.append((int(m.group(1)), int(m.group(2)), sch[j + 1]))

w = 33102; h = 23387
sc = 0.055
W = int(w * sc); H = int(h * sc)
out = ['<svg viewBox="0 0 %d %d" width="%d" height="%d" xmlns="http://www.w3.org/2000/svg" font-family="Arial,Microsoft YaHei,sans-serif">' % (W, H, W, H)]
out.append('<rect x="0" y="0" width="%d" height="%d" fill="#ffffff"/>' % (W, H))
out.append('<rect x="10" y="10" width="%d" height="%d" fill="none" stroke="#888" stroke-width="2"/>' % (W - 20, H - 20))
out.append('<text x="30" y="45" font-size="20" fill="#333">KiCad 导入包布局预览 · 64 元件 / 45 网络 · A1</text>')
for ref, x, y, val in comps:
    cx, cy = x * sc, y * sc
    out.append('<rect x="%.1f" y="%.1f" width="22" height="16" fill="#EAF2FB" stroke="#185FA5" stroke-width="1"/>' % (cx - 11, cy - 8))
    out.append('<text x="%.1f" y="%.1f" font-size="9" fill="#185FA5" text-anchor="middle" font-weight="600">%s</text>' % (cx, cy + 3, ref))
    out.append('<text x="%.1f" y="%.1f" font-size="7" fill="#777" text-anchor="middle">%s</text>' % (cx, cy + 15, val[:22]))
for x, y, net in labels:
    out.append('<circle cx="%.1f" cy="%.1f" r="1.4" fill="#993C1D"/>' % (x * sc, y * sc))
svg = "".join(out) + "</svg>"
p = os.path.join(ROOT, "hardware", "power-board-v1", "kicad-import", "layout-preview.svg")
open(p, "w", encoding="utf-8").write(svg)
html = '<!DOCTYPE html><html><head><meta charset="utf-8"><style>body{margin:16px;background:#fff}</style></head><body>%s</body></html>' % svg
open(os.path.join(ROOT, "hardware", "power-board-v1", "kicad-import", "layout-preview.html"), "w", encoding="utf-8").write(html)
open(os.path.join(ROOT, "_preview_kicad.txt"), "w", encoding="utf-8").write("comps=%d labels=%d" % (len(comps), len(labels)))
