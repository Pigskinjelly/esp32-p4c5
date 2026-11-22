import re, glob, os

base = r"<用户目录>\AppData\Local\Temp\m3d"
lines = []
for f in sorted(glob.glob(os.path.join(base, "3D", "Objects", "*.model"))):
    s = open(f, encoding="utf-8").read()
    vs = re.findall(r'<vertex x="([-\d.eE+]+)" y="([-\d.eE+]+)" z="([-\d.eE+]+)"', s)
    if not vs:
        lines.append(os.path.basename(f) + " no vertices")
        continue
    xs = [float(a) for a, b, c in vs]
    ys = [float(b) for a, b, c in vs]
    zs = [float(c) for a, b, c in vs]
    lines.append("%s  nverts=%d" % (os.path.basename(f), len(vs)))
    lines.append("   X %.2f..%.2f  size %.2f" % (min(xs), max(xs), max(xs) - min(xs)))
    lines.append("   Y %.2f..%.2f  size %.2f" % (min(ys), max(ys), max(ys) - min(ys)))
    lines.append("   Z %.2f..%.2f  size %.2f" % (min(zs), max(zs), max(zs) - min(zs)))
open(r"<用户目录>\Desktop\ESP32-P4C5\_tools\bbox.txt", "w", encoding="utf-8").write("\n".join(lines))
