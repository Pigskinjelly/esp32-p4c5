import re, io, sys

path = r"<用户目录>\AppData\Local\Temp\sch.dxf"
lines = open(path, encoding="utf-8", errors="ignore").read().splitlines()

# parse pairs (code, value)
items = []
for i in range(0, len(lines) - 1, 2):
    try:
        code = int(lines[i].strip())
    except Exception:
        continue
    items.append((code, lines[i + 1]))

texts = []  # (x, y, text)
cur = {}
i = 0
while i < len(items):
    code, val = items[i]
    if code == 0:
        # flush entity
        if cur.get("type") in ("TEXT", "MTEXT") and cur.get("text"):
            texts.append((cur.get("x", 0.0), cur.get("y", 0.0), cur["text"], cur.get("h", 0)))
        cur = {}
        if val.strip() in ("TEXT", "MTEXT"):
            cur["type"] = val.strip()
            cur["text"] = ""
    else:
        if code == 1:
            cur["text"] = cur.get("text", "") + val
        elif code == 10:
            try:
                cur["x"] = float(val)
            except Exception:
                pass
        elif code == 20:
            try:
                cur["y"] = float(val)
            except Exception:
                pass
        elif code == 40:
            try:
                cur["h"] = float(val)
            except Exception:
                pass
        elif code == 3:
            cur["text"] = val + cur.get("text", "")
    i += 1

out = io.StringIO()
out.write("total texts: %d\n" % len(texts))
xs = [t[0] for t in texts]; ys = [t[1] for t in texts]
out.write("x range %.1f..%.1f   y range %.1f..%.1f\n" % (min(xs), max(xs), min(ys), max(ys)))
# find J2 / J4 reference designators
for idx, (x, y, t, h) in enumerate(texts):
    if t.strip() in ("J2", "J4", "J1", "J3"):
        out.write("REFD %s at (%.1f, %.1f) h=%.1f\n" % (t.strip(), x, y, h))

open(r"<用户目录>\Desktop\ESP32-P4C5\_tools\dxf_summary.txt", "w", encoding="utf-8").write(out.getvalue())

# dump all texts sorted by y desc then x asc
allt = sorted(texts, key=lambda p: (-p[1], p[0]))
with open(r"<用户目录>\Desktop\ESP32-P4C5\_tools\dxf_all.txt", "w", encoding="utf-8") as f:
    for x, y, t, h in allt:
        f.write("%8.1f %8.1f  %s\n" % (x, y, t))
