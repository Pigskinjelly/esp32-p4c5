from PIL import Image
import os

src = r"<用户目录>\AppData\Local\Temp\schpng\page-0001.png"
out = r"<用户目录>\Desktop\ESP32-P4C5\_tools\crops"
os.makedirs(out, exist_ok=True)
im = Image.open(src)
W, H = im.size
log = ["size %dx%d" % (W, H)]

# regions defined in the 1071x755 preview coordinate space
regions = {
    "j4": (400, 30, 500, 210),
    "j2": (400, 200, 500, 350),
    "top_left_power": (20, 20, 300, 200),
    "power_mid": (560, 20, 800, 200),
    "usbc_blk": (300, 380, 460, 560),
    "typec_sw": (540, 560, 700, 740),
    "buck": (560, 380, 800, 470),
}
sx, sy = W / 1071.0, H / 755.0
for name, (x0, y0, x1, y1) in regions.items():
    box = (int(x0 * sx), int(y0 * sy), int(x1 * sx), int(y1 * sy))
    c = im.crop(box)
    # upscale 2x for legibility
    c = c.resize((c.width * 2, c.height * 2), Image.LANCZOS)
    p = os.path.join(out, name + ".png")
    c.save(p)
    log.append("%s box=%s -> %s %s" % (name, box, p, c.size))
open(r"<用户目录>\Desktop\ESP32-P4C5\_tools\crop_log.txt", "w", encoding="utf-8").write("\n".join(log))
