import re, glob, sys, os
os.chdir(sys.argv[1])
for f in sorted(glob.glob('3D/Objects/*.model')):
    s = open(f, encoding='utf-8').read()
    vs = re.findall(r'<vertex x="([-\d.eE+]+)" y="([-\d.eE+]+)" z="([-\d.eE+]+)"', s)
    if not vs:
        print(f, 'no vertices')
        continue
    xs = [float(a) for a, b, c in vs]
    ys = [float(b) for a, b, c in vs]
    zs = [float(c) for a, b, c in vs]
    print(f, 'nverts', len(vs))
    print('   X %.2f..%.2f  size %.2f' % (min(xs), max(xs), max(xs) - min(xs)))
    print('   Y %.2f..%.2f  size %.2f' % (min(ys), max(ys), max(ys) - min(ys)))
    print('   Z %.2f..%.2f  size %.2f' % (min(zs), max(zs), max(zs) - min(zs)))
