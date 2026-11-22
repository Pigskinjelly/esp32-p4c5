rows = []
for line in open(r"<用户目录>\Desktop\ESP32-P4C5\_tools\dxf_all.txt", encoding="utf-8"):
    line = line.rstrip("\n")
    if len(line) < 19:
        continue
    try:
        x = float(line[0:8]); y = float(line[8:17])
    except Exception:
        continue
    rows.append((x, y, line[19:]))

def dump(name, x0, x1, y0, y1):
    sel = [r for r in rows if x0 <= r[0] <= x1 and y0 <= r[1] <= y1]
    sel.sort(key=lambda r: (-r[1], r[0]))
    buf = ["=== %s  n=%d" % (name, len(sel))]
    for x, y, t in sel:
        buf.append("%7.1f %7.1f  %s" % (x, y, t))
    return "\n".join(buf)

out = [
    "total parsed %d" % len(rows),
    dump("J2 area", 290, 430, 490, 578),
    dump("J4 area", 290, 430, 350, 438),
]
open(r"<用户目录>\Desktop\ESP32-P4C5\_tools\jarea.txt", "w", encoding="utf-8").write("\n\n".join(out))
