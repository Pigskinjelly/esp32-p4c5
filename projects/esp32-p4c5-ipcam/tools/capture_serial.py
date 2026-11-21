import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM19"
secs = int(sys.argv[2]) if len(sys.argv) > 2 else 120
out = sys.argv[3] if len(sys.argv) > 3 else "_log.txt"

s = serial.Serial(port, 115200, timeout=0.2)
s.setDTR(False)
s.setRTS(False)
t0 = time.time()
with open(out, "wb") as f:
    while time.time() - t0 < secs:
        try:
            d = s.read(4096)
        except Exception:
            break
        if d:
            f.write(d)
            f.flush()
s.close()
print("captured %s (%ds)" % (out, secs))
