rows = []
for line in open(r"<用户目录>\Desktop\ESP32-P4C5\_tools\dxf_all.txt", encoding="utf-8"):
    line = line.rstrip("\n")
    if len(line) < 19:
        continue
    try:
        x = float(line[0:8]); y = float(line[8:17])
    except Exception:
        continue
    rows.append((x, y, line[19:].strip()))

out = []
for label, y0 in [("J4 row10 (p19/p20)", 357.6), ("J4 row14 (p27/p28)", 336.6),
                  ("J4 row3 (p5/p6)", 394.6), ("J4 top p1-p4", 400.0),
                  ("J2 top p1-p4", 545.0)]:
    sel = [r for r in rows if abs(r[1] - y0) <= 3.0 and 200 <= r[0] <= 520 and r[2]]
    sel.sort(key=lambda r: r[0])
    out.append("=== %s" % label)
    for x, y, t in sel:
        out.append("%7.1f %7.1f  %s" % (x, y, t))

# also list every text containing GPIO2 or GPIO10 near J4
out.append("=== search GPIO2/GPIO10/GPIO11 near J4")
for x, y, t in sorted(rows, key=lambda r: (-r[1], r[0])):
    if t in ("GPIO2", "GPIO3", "GPIO10", "GPIO11", "GPIO0") and 300 < y < 400:
        out.append("%7.1f %7.1f  %s" % (x, y, t))
open(r"<用户目录>\Desktop\ESP32-P4C5\_tools\jmiss.txt", "w", encoding="utf-8").write("\n".join(out))
