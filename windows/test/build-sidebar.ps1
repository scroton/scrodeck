# windows/test/build-sidebar.ps1 — builds sidebar_shot.exe and renders every fixture into windows/test/out/*.png.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows\test\build-sidebar.ps1 [-NoBuild] [-Only <name>]
#
# Nothing here opens a window: g++ is a console tool run inside this (already hidden or already visible) shell,
# and the test exe is built -mwindows and only draws to an off-screen bitmap.
#   -Truth       also render the Electron renderer's own picture of each case (headless Edge) and compare
#   -ClaimRoot   the checkout whose renderer draws the driver CLAIM (branch native-remote-server, not on main at
#                the time of writing): the claim-* truth cases read ITS index.html / style.css / js, read-only.
#                Default: the sibling worktree _wt-deck-remote-rule; when it is not there those cases are skipped.
param([switch]$NoBuild, [switch]$Truth, [string]$Only = '', [string]$ClaimRoot = '')
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)
$out = Join-Path $here 'out'
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Force $out | Out-Null }
$ignore = Join-Path $out '.gitignore'
if (-not (Test-Path $ignore)) { Set-Content -Path $ignore -Value '*' -Encoding ascii }
$exe = Join-Path $out 'sidebar_shot.exe'

if (-not $NoBuild) {
  $bin = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages') -Directory -Filter 'BrechtSanders.WinLibs*' |
    ForEach-Object { Join-Path $_.FullName 'mingw64\bin' } | Where-Object { Test-Path (Join-Path $_ 'g++.exe') } | Select-Object -First 1
  if (-not $bin) { throw 'g++ not found under %LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs*\mingw64\bin' }
  $env:PATH = "$bin;$env:PATH"
  $gxx = Join-Path $bin 'g++.exe'
  # static is mandatory: a dynamically linked exe exits silently without the MinGW runtime DLLs beside it
  $args = @('-std=c++17', '-O1', '-municode', '-mwindows', '-static', '-static-libgcc', '-static-libstdc++',
    (Join-Path $here 'sidebar_shot.cpp'), (Join-Path $root 'windows\src\sidebar.cpp'), '-o', $exe,
    '-ld2d1', '-ldwrite', '-lwindowscodecs', '-lole32', '-luuid', '-lshell32')
  $sw = [Diagnostics.Stopwatch]::StartNew()
  & $gxx @args
  if ($LASTEXITCODE -ne 0) { throw "g++ failed ($LASTEXITCODE)" }
  Write-Output ("built {0} in {1:n0} s" -f $exe, $sw.Elapsed.TotalSeconds)
}

# name, fixture, width, height, scroll, flags…
$fx = Join-Path $here 'fixtures'
$shots = @(
  @('mixed',            'meta-mixed.json',  340, 1180, 0, '--dump'),
  @('mixed-selected',   'meta-mixed.json',  340, 1180, 0, '--sel=t2', '--hover=t4', '--part=tile'),
  @('mixed-grouped',    'meta-mixed.json',  340, 1180, 0, '--grouped', '--collapse=bug queues', '--sel=t1'),
  @('mixed-nobars',     'meta-mixed.json',  340, 900,  0, '--nobars', '--sel=t7'),
  @('mixed-nomsgs',     'meta-mixed.json',  340, 900,  0, '--nomsgs', '--hover=t1', '--part=tile-pin'),
  @('mixed-search',     'meta-mixed.json',  340, 900,  0, '--search=job'),
  @('mixed-wide',       'meta-mixed.json',  460, 1300, 0, '--sel=t3'),
  @('mixed-narrow',     'meta-mixed.json',  220, 1300, 0, '--sel=t2'),
  @('mixed-window1',    'meta-mixed.json',  340, 1180, 0, '--window=1', '--sel=t3'),
  @('mixed-2x',         'meta-mixed.json',  340, 1180, 0, '--scale=2', '--sel=t2'),
  @('layout',           'meta-layout.json', 340, 1180, 0, '--sel=t1'),
  @('color',            'meta-color.json',  340, 1180, 0, '--sel=t8'),
  @('color-2x',         'meta-color.json',  340, 1180, 0, '--sel=t8', '--scale=2'),
  @('color-sel-max',    'meta-color.json',  340, 1180, 0, '--sel=t4'),
  @('right',            'meta-right.json',  340, 1180, 0, '--sel=t1', '--dump'),
  @('empty',            'meta-empty.json',  340, 600,  0, '--dump'),
  @('empty-rail',       'meta-empty.json',  14,  600,  0, '--closed'),
  @('sixty',            'meta-sixty.json',  340, 900,  0, '--dump'),
  @('sixty-scrolled',   'meta-sixty.json',  340, 900,  1500, '--sel=s20'),
  @('sixty-end',        'meta-sixty.json',  340, 900,  99999, '--grouped'),
  # the selected tile asking, renaming, dragging (30-tiles.js state.confirm / renameFns / the drag classes)
  @('confirm-close',    'meta-mixed.json',  340, 700,  0, '--sel=t2', '--confirm=close'),
  @('confirm-refresh',  'meta-mixed.json',  340, 700,  0, '--sel=t2', '--confirm=refresh::1'),
  @('confirm-move',     'meta-mixed.json',  340, 700,  0, '--sel=t2', '--confirm=move:work'),
  @('rename',           'meta-mixed.json',  340, 700,  0, '--sel=t1', '--rename=bug queue rework'),
  @('drag',             'meta-mixed.json',  340, 700,  0, '--sel=t2', '--drag=t3', '--drop=t1:after'),
  @('restart-confirm',  'meta-mixed.json',  340, 1180, 0, '--sel=t2', '--restartconfirm'),
  @('menu',             'meta-mixed.json',  340, 700,  0, '--sel=t2', '--menu=60,150,Rename,Pin,Special prompt…,-,Fill the window,*Top half,Bottom half,Left half,Right half,Remove from layout,Open in a new window,-,*bug queues,build bot,~No group,New group…'),
  @('prompt',           'meta-mixed.json',  340, 700,  0, '--sel=t2', '--prompt=60,150,group name'),
  @('hold-next',        'meta-mixed.json',  340, 1180,  0, '--sel=t2', '--hold=next:check the build and report'),
  @('hold-timed',       'meta-mixed.json',  340, 1180,  0, '--sel=t2', '--hold=timed'),
  @('hold-recurring',   'meta-mixed.json',  340, 1180,  0, '--sel=t2', '--hold=recurring'),
  @('hold-triggered',   'meta-mixed.json',  340, 1180,  0, '--sel=t2', '--hold=triggered'),
  @('settings',         'meta-mixed.json',  600, 1180, 0, '--sel=t2', ('--settings=' + (Join-Path $fx 'settings.json')), '--toolsw=332'),
  @('settings-2',       'meta-mixed.json',  600, 1180, 0, '--sel=t2', ('--settings=' + (Join-Path $fx 'settings.json')), '--toolsw=332', '--settingsscroll=1040'),
  @('settings-3',       'meta-mixed.json',  600, 1180, 0, '--sel=t2', ('--settings=' + (Join-Path $fx 'settings.json')), '--toolsw=332', '--settingsscroll=2080'),
  # a NEW deck (the driver is a claim: fixtures/make-claim.py). The 35 above are a deck from before it and must not move.
  @('claim-auto',       'meta-claim-auto.json',   340, 1180, 0, '--sel=t2', '--dump'),
  @('claim-office',     'meta-claim-office.json', 340, 1180, 0, '--sel=t2', '--dump'),
  @('claim-phone',      'meta-claim-phone.json',  340, 1180, 0, '--sel=t2', '--dump'),
  @('claim-remote',     'meta-claim-remote.json', 340, 1180, 0, '--sel=t2', '--dump'),
  @('claim-noname',     'meta-claim-noname.json', 340, 1180, 0, '--sel=t2', '--dump'),
  @('claim-remote-2x',  'meta-claim-remote.json', 340, 1180, 0, '--sel=t2', '--scale=2'),
  @('claim-auto-2x',    'meta-claim-auto.json',   340, 1180, 0, '--sel=t2', '--scale=2'),
  @('claim-office-2x',  'meta-claim-office.json', 340, 1180, 0, '--sel=t2', '--scale=2'),
  @('claim-auto-hover',   'meta-claim-auto.json',   340, 1180, 0, '--sel=t2', '--hover=drive-btn', '--part=tool'),
  @('claim-remote-hover', 'meta-claim-remote.json', 340, 1180, 0, '--sel=t2', '--hover=drive-btn', '--part=tool'),
  @('claim-settings',   'meta-claim-remote.json', 600, 1180, 0, '--sel=t2', ('--settings=' + (Join-Path $fx 'settings-claim.json')), '--toolsw=332', '--settingsscroll=300')
)
$failed = 0
foreach ($s in $shots) {
  if ($Only -and $s[0] -notlike $Only) { continue }
  $meta = Join-Path $fx $s[1]
  if (-not (Test-Path $meta)) { Write-Output "skip $($s[0]): no $($s[1])"; continue }
  $png = Join-Path $out ($s[0] + '.png')
  $a = @($meta, $png, [string]$s[2], [string]$s[3], [string]$s[4]) + @($s[5..($s.Count - 1)] | Where-Object { $_ })
  $p = Start-Process -FilePath $exe -ArgumentList ($a | ForEach-Object { '"' + $_ + '"' }) -Wait -PassThru -WindowStyle Hidden
  if ($p.ExitCode -ne 0) { Write-Output "FAIL $($s[0]): exit $($p.ExitCode)"; $failed++ } else { Write-Output "ok   $png" }
}

# ---------------------------------------------------------------- -Truth: the Electron renderer's own picture
# The REAL renderer/style.css and renderer/js/*.js (tiles, groups, new tile, search, asks tile, restart strip),
# fed a fixture through stubs for the parts that need Electron (`deck`, the terminals, the layout), and
# screenshotted with headless Edge. Static only: no Electron, no deck, no window (headless, started hidden,
# its own scratch profile). Output: out/truth-<name>.png, 520 px wide (the sidebar is the left 340; the right 340 when
# the fixture puts it on the right; 700 for the wide shot, because #sidebar is capped at 70vw).
if ($Truth) {
  $edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
  if (-not $edge) { Write-Output 'truth: msedge.exe not found, skipped'; if ($failed) { exit 1 } else { exit 0 } }
  $utf8 = New-Object System.Text.UTF8Encoding($false)
  Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class DeckCmp {
  static byte[] Px(Bitmap b, int x0, int w, int h) {
    var d = b.LockBits(new Rectangle(x0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
    var buf = new byte[w * h * 4];
    for (int y = 0; y < h; y++) Marshal.Copy(new IntPtr(d.Scan0.ToInt64() + (long)y * d.Stride), buf, y * w * 4, w * 4);
    b.UnlockBits(d); return buf;
  }
  // the sidebar is the left `native.Width` px of the truth shot (the right ones when rightSide)
  public static int Compare(string nativePath, string truthPath, string outPath, bool rightSide) {
    using (var a = new Bitmap(nativePath)) using (var t = new Bitmap(truthPath)) {
      int w = Math.Min(a.Width, t.Width), h = Math.Min(a.Height, t.Height), off = rightSide ? t.Width - w : 0;
      byte[] pa = Px(a, 0, w, h), pt = Px(t, off, w, h); var o = new byte[(w * 3 + 8) * h * 4]; int W = w * 3 + 8, n = 0;
      for (int y = 0; y < h; y++) for (int x = 0; x < W; x++) {
        int i = (y * W + x) * 4; o[i + 3] = 255;
        if (x < w) { int j = (y * w + x) * 4; o[i] = pa[j]; o[i + 1] = pa[j + 1]; o[i + 2] = pa[j + 2]; }
        else if (x >= w + 4 && x < 2 * w + 4) { int j = (y * w + x - w - 4) * 4; o[i] = pt[j]; o[i + 1] = pt[j + 1]; o[i + 2] = pt[j + 2]; }
        else if (x >= 2 * w + 8) {
          int j = (y * w + x - 2 * w - 8) * 4; int[] dv = new int[3];
          for (int c = 0; c < 3; c++) { dv[c] = Math.Abs(pa[j + c] - pt[j + c]); o[i + c] = (byte)Math.Min(255, dv[c] * 3); }
          if ((dv[2] * 299 + dv[1] * 587 + dv[0] * 114) / 1000 > 70) n++;   // BGRA: luma of the difference
        } else { o[i] = 255; o[i + 2] = 255; }
      }
      using (var r = new Bitmap(W, h, PixelFormat.Format32bppArgb)) {
        var d = r.LockBits(new Rectangle(0, 0, W, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        for (int y = 0; y < h; y++) Marshal.Copy(o, y * W * 4, new IntPtr(d.Scan0.ToInt64() + (long)y * d.Stride), W * 4);
        r.UnlockBits(d); r.Save(outPath, ImageFormat.Png);
      }
      return n;
    }
  }
}
'@
  # the page of one checkout's renderer: $base is what its files are reached by from windows/test/out
  function Truth-Html([string]$from, [string]$base) {
    $h = [IO.File]::ReadAllText((Join-Path $from 'renderer\index.html'), $utf8)
    $h = [regex]::Replace($h, '<meta http-equiv="Content-Security-Policy"[^>]*>', '')
    $h = [regex]::Replace($h, '<link rel="stylesheet" href="\.\./[^"]*">', '')
    $h = $h.Replace('href="style.css"', "href=`"${base}renderer/style.css`"")
    [regex]::Replace($h, '\s*<script src="[^"]*"></script>', '')
  }
  $html = Truth-Html $root '../../../'
  # the claim's renderer lives in another checkout (read-only): reached by an absolute file: URL
  if (-not $ClaimRoot) { $ClaimRoot = Join-Path (Split-Path -Parent $root) '_wt-deck-remote-rule' }
  $claimOk = (Test-Path (Join-Path $ClaimRoot 'lib\driverrule.js')) -and (Test-Path (Join-Path $ClaimRoot 'renderer\index.html'))
  $claimBase = 'file:///' + ((Resolve-Path $ClaimRoot -ErrorAction SilentlyContinue).Path -replace '\\', '/') + '/'
  $claimHtml = if ($claimOk) { Truth-Html $ClaimRoot $claimBase } else { '' }
  if (-not $claimOk) { Write-Output "truth: no claim renderer at $ClaimRoot - the claim-* cases are skipped" }
  $stub = @'
'use strict';
// everything the sidebar scripts expect from Electron and from the scripts not loaded here
const deck = new Proxy({}, { get: (t, k) => (k in t ? t[k] : k === 'then' ? undefined : () => Promise.resolve(null)) });
const CLOUD_SVG = '<svg class="cloud-ico" viewBox="0 0 16 16" aria-hidden="true"><path d="M4.4 13.2h7.6a3.2 3.2 0 0 0 .55-6.35A4.4 4.4 0 0 0 4.06 6.3 3.45 3.45 0 0 0 4.4 13.2z"/></svg>';
state.layout = (window.OPTS && OPTS.layout) || {};
state.grid = { rows: 2, cols: 2 };
const GRID = () => state.grid || { rows: 2, cols: 2 };
const isPlaced = id => !!(id && state.layout[id]);
const placedIds = () => Object.keys(state.layout);
const moveMenu = { hidden: true };
function activate(id) { if (window.OPTS && OPTS.noActivate) return; state.active = id; }
function createTerm(id) { terms.set(id, { wrap: { classList: { contains: () => true } }, term: { focus() {}, write() {} } }); }
function disposeTerm(id) { terms.delete(id); }
function applyLayout() {} function pruneLayout() {} function fitActive() {} function refreshMoveBars() {}
function applyFontSize() {} function applyFontFamily() {} function paintPaneNames() {} function paintWindowTitle() {}
function zoomBlocks() { return false; } function hidePreview() {} function placeNew() {} function openInNewWindow() {}
function stepPreviewBegin() {} function stepPreviewMark() {} const stepSnap = { active: null };
let dragSrcId = null, dragRescued = false;
'@
  $run = @'
'use strict';
state.platform = { shellName: 'PowerShell' };
state.windowIndex = OPTS.window || 0;
newTile.title = `New ${shellName()} terminal`;
Date.now = () => META.now;
META.settings.animations = false;          // the static state: no pulse caught mid-frame
state.active = OPTS.sel || null;
applyMeta(META);
clockEl.textContent = fmtClock(new Date(META.now));
if (OPTS.search != null) { openSearch(); searchInput.value = OPTS.search; state.search = OPTS.search; applySearchFilter(); }
if (OPTS.run) (0, eval)(OPTS.run);        // a state the fixture cannot say: a question, the rename box, a menu
'@
  [IO.File]::WriteAllText((Join-Path $out 'truth-stub.js'), $stub, $utf8)
  [IO.File]::WriteAllText((Join-Path $out 'truth-run.js'), $run, $utf8)
  $js = @('00-state', '10-format', '20-terminals') | ForEach-Object { "<script src=`"../../../renderer/js/$_.js`"></script>" }
  $js2 = @('30-tiles', '35-groups', '40-new-tile', '46-search', '49-asks', '50-settings', '70-meta', '80-sidebar', '82-restart') | ForEach-Object { "<script src=`"../../../renderer/js/$_.js`"></script>" }
  # name, fixture, height, scale, OPTS (a JS object literal: sel, noActivate, grouped, collapsed, nobars, nomsgs, search, window, width, layout)
  $truths = @(
    @('mixed-selected', 'meta-mixed.json', 1180, 1, "{ sel: 't2' }"),
    @('mixed-2x',       'meta-mixed.json', 1180, 2, "{ sel: 't2' }"),
    @('mixed',          'meta-mixed.json', 1180, 1, "{ noActivate: true }"),
    @('mixed-grouped',  'meta-mixed.json', 1180, 1, "{ sel: 't1', grouped: true, collapsed: ['bug queues'] }"),
    @('mixed-nobars',   'meta-mixed.json', 900,  1, "{ sel: 't7', nobars: true }"),
    @('mixed-nomsgs',   'meta-mixed.json', 900,  1, "{ noActivate: true, nomsgs: true }"),
    @('mixed-search',   'meta-mixed.json', 900,  1, "{ noActivate: true, search: 'job' }"),
    @('mixed-wide',     'meta-mixed.json', 1300, 1, "{ sel: 't3', width: 460 }", 700),
    @('mixed-narrow',   'meta-mixed.json', 1300, 1, "{ sel: 't2', width: 220 }"),
    @('mixed-window1',  'meta-mixed.json', 1180, 1, "{ sel: 't3', window: 1 }"),
    @('layout',         'meta-layout.json', 1180, 1, "{ sel: 't1', layout: { t1: [1, 3], t4: [2], t8: [4] } }"),
    @('color',          'meta-color.json', 1180, 1, "{ sel: 't8' }"),
    @('color-2x',       'meta-color.json', 1180, 2, "{ sel: 't8' }"),
    @('color-sel-max',  'meta-color.json', 1180, 1, "{ sel: 't4' }"),
    @('right',          'meta-right.json', 1180, 1, "{ sel: 't1' }"),
    @('empty',          'meta-empty.json', 600,  1, "{ noActivate: true }"),
    @('empty-rail',     'meta-empty.json', 600,  1, "{ noActivate: true, rail: true }"),
    @('sixty',          'meta-sixty.json', 900,  1, "{ noActivate: true }"),
    @('confirm-close',   'meta-mixed.json', 700, 1, "{ sel: 't2', run: 'state.confirm.t2 = { kind: \'close\' }; actionSig.clear(); renderTiles();' }"),
    @('confirm-refresh', 'meta-mixed.json', 700, 1, "{ sel: 't2', run: 'state.confirm.t2 = { kind: \'refresh\', browser: true }; actionSig.clear(); renderTiles();' }"),
    @('confirm-move',    'meta-mixed.json', 700, 1, "{ sel: 't2', run: 'state.confirm.t2 = { kind: \'move\', account: \'work\' }; actionSig.clear(); renderTiles();' }"),
    @('rename',          'meta-mixed.json', 700, 1, "{ sel: 't1', run: 'startRename(\'t1\'); document.querySelector(\'input.rename\').value = \'bug queue rework\'; document.querySelector(\'input.rename\').select();' }"),
    @('drag',            'meta-mixed.json', 700, 1, "{ sel: 't2', run: 'tileEls.get(\'t3\').classList.add(\'dragging\'); tileEls.get(\'t1\').classList.add(\'drop-after\');' }"),
    @('restart-confirm', 'meta-mixed.json', 1180, 1, "{ sel: 't2', run: 'nowConfirmUntil = Date.now() + 4000; renderRestartStrip();' }"),
    @('menu',            'meta-mixed.json', 700, 1, "{ sel: 't2', run: 'state.groups = [\'bug queues\', \'build bot\']; state.tiles.find(t => t.id === \'t2\').group = \'bug queues\'; state.layout = { t2: [1, 2] }; openTileMenu({ clientX: 60, clientY: 150, screenX: 0, screenY: 0, preventDefault() {}, stopPropagation() {} }, \'t2\');' }"),
    @('prompt',          'meta-mixed.json', 700, 1, "{ sel: 't2', run: 'promptInline({ clientX: 60, clientY: 150 }, \'group name\', () => {});' }"),
    @('hold-next',       'meta-mixed.json', 1180, 1, "{ sel: 't2', run: 'state.confirm.t2 = { kind: \'hold\' }; actionSig.clear(); renderTiles(); document.querySelector(\'.hold-text\').value = \'check the build and report\';' }"),
    @('hold-timed',      'meta-mixed.json', 1180, 1, "{ sel: 't2', run: 'state.confirm.t2 = { kind: \'hold\' }; actionSig.clear(); renderTiles(); document.querySelectorAll(\'.sp-kind\')[1].click();' }"),
    @('hold-recurring',  'meta-mixed.json', 1180, 1, "{ sel: 't2', run: 'state.confirm.t2 = { kind: \'hold\' }; actionSig.clear(); renderTiles(); document.querySelectorAll(\'.sp-kind\')[2].click();' }"),
    @('settings',        'meta-mixed.json', 1180, 1, "{ sel: 't2', run: 'deck.getSettings = () => Promise.resolve(SETTINGS); openSettings();' }", 900),
    @('settings-2',      'meta-mixed.json', 1180, 1, "{ sel: 't2', run: 'deck.getSettings = () => Promise.resolve(SETTINGS); openSettings().then(() => { document.querySelector(\'.panel-body\').scrollTop = 1040; });' }", 900),
    @('settings-3',      'meta-mixed.json', 1180, 1, "{ sel: 't2', run: 'deck.getSettings = () => Promise.resolve(SETTINGS); openSettings().then(() => { document.querySelector(\'.panel-body\').scrollTop = 2080; });' }", 900),
    @('hold-triggered',  'meta-mixed.json', 1180, 1, "{ sel: 't2', run: 'state.confirm.t2 = { kind: \'hold\' }; actionSig.clear(); renderTiles(); document.querySelectorAll(\'.sp-kind\')[3].click();' }"),
    # the driver as a claim: the 7th field says the page is the CLAIM checkout's renderer, as a remote desk (deck._desk)
    @('claim-auto',      'meta-claim-auto.json',   1180, 1, "{ sel: 't2' }", 520, 'claim'),
    @('claim-office',    'meta-claim-office.json', 1180, 1, "{ sel: 't2' }", 520, 'claim'),
    @('claim-phone',     'meta-claim-phone.json',  1180, 1, "{ sel: 't2' }", 520, 'claim'),
    @('claim-remote',    'meta-claim-remote.json', 1180, 1, "{ sel: 't2' }", 520, 'claim'),
    @('claim-noname',    'meta-claim-noname.json', 1180, 1, "{ sel: 't2' }", 520, 'claim'),
    @('claim-remote-2x', 'meta-claim-remote.json', 1180, 2, "{ sel: 't2' }", 520, 'claim'),
    @('claim-auto-2x',   'meta-claim-auto.json',   1180, 2, "{ sel: 't2' }", 520, 'claim'),
    @('claim-office-2x', 'meta-claim-office.json', 1180, 2, "{ sel: 't2' }", 520, 'claim'),
    @('claim-settings',  'meta-claim-remote.json', 1180, 1, "{ sel: 't2', run: 'deck.getSettings = () => Promise.resolve(SETTINGS); openSettings().then(() => { document.querySelector(\'.panel-body\').scrollTop = 300; });' }", 900, 'claim')
  )
  $edgeProfile = Join-Path $env:TEMP 'deck-native-truth-profile'
  foreach ($t in $truths) {
    if ($Only -and $t[0] -notlike $Only) { continue }
    $claim = $t.Count -gt 6 -and $t[6] -eq 'claim'
    if ($claim -and -not $claimOk) { continue }
    $base = if ($claim) { $claimBase } else { '../../../' }
    $metaText = [IO.File]::ReadAllText((Join-Path $fx $t[1]), $utf8)
    $settingsText = [IO.File]::ReadAllText((Join-Path $fx $(if ($claim) { 'settings-claim.json' } else { 'settings.json' })), $utf8)
    $pre = "window.OPTS = $($t[4]);`nwindow.META = $metaText;`nwindow.SETTINGS = $settingsText;`n" +
      "localStorage.clear();`nif (OPTS.grouped) localStorage.setItem('deck.grouped', '1');`n" +
      "if (OPTS.collapsed) localStorage.setItem('deck.groups.collapsed', JSON.stringify(OPTS.collapsed));`n" +
      "if (OPTS.nobars) localStorage.setItem('deck.hidebars', '1');`nif (OPTS.nomsgs) localStorage.setItem('deck.hidemsgs', '1');`n" +
      "if (OPTS.width) localStorage.setItem('deck.sbwidth', String(OPTS.width));`nif (OPTS.rail) localStorage.setItem('deck.collapsed', '1');`n"
    [IO.File]::WriteAllText((Join-Path $out "truth-$($t[0]).pre.js"), $pre, $utf8)
    # a remote desk's page: bridge.js marks `deck._desk` (80-sidebar.js DRIVE_ME reads it); after the run, the
    # button's own title goes into the page's <title> so claim-words.js can read the sentence out of the html
    $jsA = if ($claim) { $js | ForEach-Object { $_.Replace('../../../', $base) } } else { $js }
    $jsB = if ($claim) { $js2 | ForEach-Object { $_.Replace('../../../', $base) } } else { $js2 }
    $desk = if ($claim) { "<script>deck._desk = true;</script>`n" } else { '' }
    $scripts = "<script src=`"truth-$($t[0]).pre.js`"></script>`n" + ($jsA -join "`n") + "`n<script src=`"truth-stub.js`"></script>`n$desk<script src=`"${base}lib/layout.js`"></script>`n" + ($jsB -join "`n") + "`n<script src=`"truth-run.js`"></script>`n"
    $page = $(if ($claim) { $claimHtml } else { $html }).Replace('</body>', $scripts + '</body>')
    $pageFile = Join-Path $out "truth-$($t[0]).html"
    [IO.File]::WriteAllText($pageFile, $page, $utf8)
    $png = Join-Path $out "truth-$($t[0]).png"
    if (Test-Path $png) { Remove-Item $png -Force }
    $url = 'file:///' + ($pageFile -replace '\\', '/')
    $ea = @('--headless=new', '--disable-gpu', '--hide-scrollbars', '--no-first-run', '--no-default-browser-check', '--disable-extensions',
      "--force-device-scale-factor=$($t[3])", "--user-data-dir=`"$edgeProfile`"", "--window-size=$(if ($t.Count -gt 5) { $t[5] } else { 520 }),$($t[2])", '--virtual-time-budget=4000',
      "--screenshot=`"$png`"", "`"$url`"")
    $p = Start-Process -FilePath $edge -ArgumentList $ea -Wait -PassThru -WindowStyle Hidden
    if (-not (Test-Path $png)) { Write-Output "truth $($t[0]): no screenshot (exit $($p.ExitCode))"; continue }
    # native | truth | difference ×3, and the count of pixels whose difference has a luma over 70 (ClearType
    # fringes stay under that; a box or a line of text one pixel off does not)
    $native = Join-Path $out ($t[0] + '.png')
    if (Test-Path $native) {
      $n = [DeckCmp]::Compare($native, $png, (Join-Path $out "cmp-$($t[0]).png"), ($t[1] -eq 'meta-right.json'))
      Write-Output ("ok   {0}   differing px: {1}" -f $png, $n)
    } else { Write-Output "ok   $png" }
  }
}
if ($failed) { exit 1 }
