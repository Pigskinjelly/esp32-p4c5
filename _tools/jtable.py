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

def table(name, ys_ref, x_left=(290, 320), x_right=(365, 420), tol=1.6):
    buf = ["=== %s" % name]
    for pin_y in ys_ref:
        left = [t for x, y, t in rows if x_left[0] <= x <= x_left[1] and abs(y - pin_y) <= tol and t]
        right = [t for x, y, t in rows if x_right[0] <= x <= x_right[1] and abs(y - pin_y) <= tol and t]
        buf.append("y=%.1f  L=%s  R=%s" % (pin_y, "|".join(left), "|".join(right)))
    return "\n".join(buf)

# J2 rows: PIJ2xx label ys
j2 = sorted({y for x, y, t in rows if t.startswith("PIJ2") and 300 < x < 330}, reverse=True)
j4 = sorted({y for x, y, t in rows if t.startswith("PIJ4") and 300 < x < 330 and y < 430}, reverse=True)
out = [table("J2 (34 pins)", j2), table("J4 (34 pins)", j4)]
open(r"<用户目录>\Desktop\ESP32-P4C5\_tools\jtable.txt", "w", encoding="utf-8").write("\n\n".join(out))
