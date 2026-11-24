# windows/test/montage.py — paste crops of several shots side by side, so one picture can be read instead of five.
#   python montage.py out.png  a.png[:x,y,w,h]  b.png[:x,y,w,h] ...
import sys
from PIL import Image
out = sys.argv[1]; parts = []
for a in sys.argv[2:]:
    path, _, box = a.rpartition(':') if (':' in a[2:]) else (a, '', '')
    if not path: path, box = a, ''
    im = Image.open(path).convert('RGB')
    if box:
        x, y, w, h = [int(v) for v in box.split(',')]
        im = im.crop((x, y, x + w, y + h))
    parts.append(im)
W = sum(p.width for p in parts) + 6 * (len(parts) - 1); H = max(p.height for p in parts)
m = Image.new('RGB', (W, H), (255, 0, 255)); x = 0
for p in parts: m.paste(p, (x, 0)); x += p.width + 6
m.save(out)
