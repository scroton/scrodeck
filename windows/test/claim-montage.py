# windows/test/claim-montage.py — the driver button, close up: the exe's drawing beside the renderer's own.
#   python windows/test/claim-montage.py            (after build-sidebar.ps1 -Truth)
# One row per state: native | truth (headless Edge on the real css/html) | difference x4, the tool row's right
# half magnified 6x. Rows: the NEW deck's four states (truth = branch native-remote-server's renderer, as a remote
# desk) and the OLD deck's three (truth = this checkout's renderer). Writes out/claim-button-montage.png and prints,
# per row, the pixels of the button's own 26x26 box whose difference has a luma over 70.
import os, sys
from PIL import Image, ImageChops, ImageDraw
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out')
rows = [('claim-auto', 'new deck  auto'), ('claim-office', 'new deck  office (grey)'), ('claim-phone', 'new deck  phone (grey)'),
        ('claim-remote', 'new deck  remote = this screen (green)'), ('mixed-selected', 'old deck  auto'), ('color', 'old deck  phone (green)'), ('right', 'old deck  desk (grey)')]
Z, W, H = 6, 170, 34
tiles = []; bad = 0
for name, label in rows:
    n = Image.open(os.path.join(out, name + '.png')).convert('RGB')
    tp = os.path.join(out, 'truth-' + name + '.png')
    if not os.path.exists(tp): print('%s: no truth shot' % name); continue
    t = Image.open(tp).convert('RGB')
    right = name == 'right'                              # meta-right.json: the sidebar is on the right of the truth page, its tool row reversed
    w, h = n.size
    t = t.crop((t.size[0] - w, 0, t.size[0], h)) if right else t.crop((0, 0, w, h))
    box = (0, h - H, W, h) if right else (w - W, h - H, w, h)
    a, b = n.crop(box), t.crop(box)
    d = ImageChops.difference(a, b)
    # the drive button's 26px column: third from the outer end of the row (drive, restart, settings)
    bx = (26 + 4, 0, 26 + 4 + 34, H) if right else (W - 4 - 26 * 3 - 8, 0, W - 4 - 26 * 2 + 8, H)
    cnt = sum(1 for p in d.crop(bx).getdata() if (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000 > 70)
    bad += cnt
    print('%-16s %-40s button-area px over luma 70: %d' % (name, label, cnt))
    d = d.point(lambda v: min(255, v * 4))
    tiles.append((label, [im.resize((W * Z, H * Z), Image.NEAREST) for im in (a, b, d)]))
if not tiles: sys.exit(1)
M = Image.new('RGB', (3 * W * Z + 40, len(tiles) * (H * Z + 26) + 10), (40, 40, 40))
dr = ImageDraw.Draw(M)
for i, (label, ims) in enumerate(tiles):
    y = 10 + i * (H * Z + 26)
    dr.text((10, y), label + '      native | renderer | difference x4', fill=(230, 230, 230))
    for j, im in enumerate(ims): M.paste(im, (10 + j * (W * Z + 10), y + 16))
p = os.path.join(out, 'claim-button-montage.png'); M.save(p); print(p)
sys.exit(1 if bad else 0)
