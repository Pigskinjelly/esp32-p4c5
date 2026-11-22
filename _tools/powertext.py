import re
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

kw = ["BAT", "CHRG", "TP405", "IP53", "IP2312", "ETA", "LM662", "MT97", "SY72", "BOOST",
      "BUCK", "VIN", "5V", "3V3", "VBUS", "CC1", "CC2", "PD", "NTC", "LDO", "PMIC",
      "AXP", "BQ", "SGM", "MP2", "SY8", "TYPEC", "TYPE-C", "USB"]
seen = set()
hits = []
for x, y, t in sorted(rows, key=lambda r: (-r[1], r[0])):
    up = t.upper()
    if any(k in up for k in kw) and 2 < len(t) < 40:
        key = (t, round(x / 5), round(y / 5))
        if key in seen:
            continue
        seen.add(key)
        hits.append("%7.1f %7.1f  %s" % (x, y, t))
open(r"<用户目录>\Desktop\ESP32-P4C5\_tools\power_texts.txt", "w", encoding="utf-8").write(
    "n=%d\n" % len(hits) + "\n".join(hits))
