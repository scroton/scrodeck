# windows/test/pngdiff.py — count the pixels that differ between pairs of PNGs.
#   python windows/test/pngdiff.py <dirA> <dirB> [glob=*.png]     every file of dirA that dirB also has
#   python windows/test/pngdiff.py <a.png> <b.png>
# Prints "<name>: identical" or "<name>: N px differ (bbox …)"; exit code 1 when anything differs.
import sys, os, glob
from PIL import Image, ImageChops

def diff(a, b):
    ia, ib = Image.open(a).convert('RGB'), Image.open(b).convert('RGB')
    if ia.size != ib.size:
        return 'sizes differ %s vs %s' % (ia.size, ib.size)
    d = ImageChops.difference(ia, ib)
    box = d.getbbox()
    if not box:
        return None
    n = sum(1 for p in d.getdata() if p != (0, 0, 0))
    return '%d px differ (bbox %s)' % (n, box)

a, b = sys.argv[1], sys.argv[2]
pairs = []
if os.path.isdir(a):
    pat = sys.argv[3] if len(sys.argv) > 3 else '*.png'
    for f in sorted(glob.glob(os.path.join(a, pat))):
        g = os.path.join(b, os.path.basename(f))
        if os.path.exists(g):
            pairs.append((f, g))
else:
    pairs.append((a, b))
bad = 0
for f, g in pairs:
    r = diff(f, g)
    print('%s: %s' % (os.path.basename(f), r or 'identical'))
    bad += 1 if r else 0
print('%d compared, %d differ' % (len(pairs), bad))
sys.exit(1 if bad else 0)
