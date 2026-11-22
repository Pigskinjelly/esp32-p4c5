from PIL import Image
import os

src = r"<用户目录>\AppData\Local\Temp\sch600\page-0001.png"
out = r"<用户目录>\Desktop\ESP32-P4C5\_tools\crops600"
os.makedirs(out, exist_ok=True)
im = Image.open(src)
W, H = im.size
sx = W / 822.9
sy = H / 583.5

def dxf_crop(name, x0, y0, x1, y1, scale=1.0):
    box = (int(x0 * sx), int((583.5 - y1) * sy), int(x1 * sx), int((583.5 - y0) * sy))
    c = im.crop(box)
    if scale != 1.0:
        c = c.resize((int(c.width * scale), int(c.height * scale)), Image.LANCZOS)
    p = os.path.join(out, name + ".png")
    c.save(p)
    return "%s box=%s size=%s" % (name, box, c.size)

log = []
log.append(dxf_crop("J4_p1_p10", 265, 378, 410, 420, 1.6))
log.append(dxf_crop("J2_p1_p8", 265, 520, 410, 562, 1.6))
open(r"<用户目录>\Desktop\ESP32-P4C5\_tools\crop_log2.txt", "w", encoding="utf-8").write("\n".join(log))
