# windows/res/make-remote-icon.py - the remote's icon: the deck's own picture on a blue ground, so a remote window
# is told from the local deck at a glance on the taskbar (2026-11-19: "remote can get own task bar, and can you
# make the icons noticeably different"). Re-run after assets/icon.png changes:
#   python windows/res/make-remote-icon.py      -> windows/res/remote.ico (+ remote-preview.png, not committed)
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'assets', 'icon.png')
BLUE = (37, 99, 235)          # the ground: a strong blue (the local deck's is near-black)
BLUE_EDGE = (29, 78, 216)

img = Image.open(SRC).convert('RGBA')
px = img.load()
w, h = img.size
for y in range(h):
    for x in range(w):
        r, g, b, a = px[x, y]
        # the near-black ground (and its anti-aliased edge toward the glyphs) becomes blue, keeping its weight
        lum = (r + g + b) / 3
        if lum < 40 and a > 0:
            px[x, y] = (*BLUE, a)
        elif r < 90 and g < 90 and b < 90 and abs(r - g) < 12 and abs(g - b) < 12:
            # the dark-grey panes: a darker blue, so the three panes still read
            px[x, y] = (*BLUE_EDGE, a)
img.save(os.path.join(HERE, 'remote.ico'), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
img.save(os.path.join(HERE, 'remote-preview.png'))
print('remote.ico written')
