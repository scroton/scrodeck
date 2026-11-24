# windows/res/make-fonts-rc.py - (re)writes windows/res/fonts.rc: every font file under assets/fonts as RCDATA 2001..,
# so the exe carries the bundled console fonts (gfx.hpp loadFontResources). Run from the repo root after assets/fonts
# changes:  python windows/res/make-fonts-rc.py
import io, os
root = os.getcwd()
fonts = []
for d, _, fs in os.walk(os.path.join(root, 'assets', 'fonts')):
    for f in sorted(fs):
        if f.lower().endswith(('.ttf', '.otf', '.ttc')):
            fonts.append(os.path.relpath(os.path.join(d, f), os.path.join(root, 'windows', 'res')).replace(os.sep, '/'))
fonts.sort()
assert len(fonts) < 99, 'gfx.hpp reads RCDATA 2001..2099'
lines = ['// windows/res/fonts.rc - the bundled console fonts (assets/fonts), inside the exe as RCDATA 2001.. (the user\'s list,',
         '// 2026-11-19: "fonts in the exe"). gfx.hpp loadFontResources reads them; the folder beside the exe is the fallback.',
         '// Re-made by windows/res/make-fonts-rc.py when assets/fonts changes.']
lines += ['%d RCDATA "%s"' % (2001 + i, f) for i, f in enumerate(fonts)]
io.open(os.path.join(root, 'windows', 'res', 'fonts.rc'), 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
print(len(fonts), 'fonts')
