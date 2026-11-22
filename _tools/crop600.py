from PIL import Image
import os

src = r"<用户目录>\AppData\Local\Temp\sch600\page-0001.png"
out = r"<用户目录>\Desktop\ESP32-P4C5\_tools\crops600"
os.makedirs(out, exist_ok=True)
im = Image.open(src)
W, H = im.size
log = ["size %dx%d" % (W, H)]

regions = {
    "j4_full": (390, 25, 510, 215),
    "j2_full": (390, 195, 510, 355),
    "cell_u4": (10, 20, 300, 220),
    "power_left": (555, 20, 800, 210),
    "typec1": (300, 380, 480, 570),
    "typec2": (280, 560, 470, 740),
    "buck_typec": (555, 375, 800, 470),
    "usbhost": (500, 555, 720, 740),
    "audio_pmic": (700, 460, 1010, 740),
}
sx, sy = W / 1071.0, H / 755.0
for name, (x0, y0, x1, y1) in regions.items():
    box = (int(x0 * sx), int(y0 * sy), int(x1 * sx), int(y1 * sy))
    c = im.crop(box)
    p = os.path.join(out, name + ".png")
    c.save(p)
    log.append("%s box=%s -> %s %s" % (name, box, p, c.size))
open(r"<用户目录>\Desktop\ESP32-P4C5\_tools\crop_log600.txt", "w", encoding="utf-8").write("\n".join(log))
