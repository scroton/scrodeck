'use strict';
// windows/test/driver-deck.js — a deck for the driver test (driver.ps1): the REAL lib/phone.js PhoneServer of one
// of two checkouts, with a small fake of main.js behind it that keeps the one thing the test is about — who
// drives a session's size.
//   node driver-deck.js <old|new> <port file> <record file> <port> [claim checkout]
//   old   this checkout's lib/phone.js (origin/main's words: auto | phone | desk; deskInit takes no argument)
//   new   the claim checkout's lib/phone.js AND lib/driverrule.js (branch native-remote-server, read-only; default
//         the sibling worktree _wt-deck-remote-rule): auto | office | phone | remote, sizeBy, remoteDesk, deskInit({ name })
// Two shell-like tiles, no process behind them (nothing is started). The pty's size is a number this file keeps:
// a `resize` moves it only when the rule says that client drives. Every frame a client sends is written to the
// record file. Test-only calls: `__office [id, cols, rows]` (the office resized the pty), `__flags [id, mouseOn, sgrMouse]`.
// Loopback only, a made-up token, nothing written but the two files named on the command line.
const fs = require('fs'), path = require('path');
const REPO = path.resolve(__dirname, '..', '..');
const [kind, portFile, recordFile, portArg, claimArg] = process.argv.slice(2);
if (!['old', 'new'].includes(kind) || !portFile || !recordFile) { console.error('usage: node driver-deck.js <old|new> <port file> <record file> <port> [claim checkout]'); process.exit(2); }
const NEW = kind === 'new';
const SRC = NEW ? (claimArg || path.resolve(REPO, '..', '_wt-deck-remote-rule')) : REPO;
const { PhoneServer } = require(path.join(SRC, 'lib', 'phone.js'));
const DriverRule = NEW ? require(path.join(SRC, 'lib', 'driverrule.js')) : null;
const PORT = +(portArg || 8796), TOKEN = 'drivertoken000001';

const settings = { driver: 'auto', msgLines: { normal: 3, wide: 10 }, grid: { rows: 1, cols: 2 }, oneWindow: true };
const tiles = [
  { id: 'd1', kind: 'shell', title: 'driver one', status: 'shell', ptySize: { cols: 100, rows: 30 }, mouseOn: false, sgrMouse: false, driver: null },
  { id: 'd2', kind: 'shell', title: 'driver two', status: 'shell', ptySize: { cols: 100, rows: 30 }, mouseOn: false, sgrMouse: false, driver: null },
];
const record = { kind, frames: [], sizes: [] };
let saveAgain = null;
const save = () => {
  try { fs.writeFileSync(recordFile + '.tmp', JSON.stringify(record)); fs.renameSync(recordFile + '.tmp', recordFile); }
  catch (e) { clearTimeout(saveAgain); saveAgain = setTimeout(save, 40); }
};
let server;
const who = id => ({ phone: server.phoneWatching(id), remote: NEW ? server.remoteShows(id) : server.watching(id) });
function meta() {
  const m = {
    now: Date.now(), restoring: false, accounts: [], settings: { ...settings }, groups: [], usage: {}, triggers: [], asks: [], askLog: [],
    tiles: tiles.map(t => {
      const o = { id: t.id, kind: t.kind, title: t.title, status: t.status, lastAt: Date.now(), held: [], shownBy: [], ptySize: t.ptySize, mouseOn: t.mouseOn, sgrMouse: t.sgrMouse, driver: t.driver, mirror: null };
      if (NEW) o.sizeBy = DriverRule.driverOf(settings.driver, who(t.id));
      return o;
    }),
  };
  if (NEW) m.remoteDesk = server.remoteDesk();
  return m;
}
const push = () => setTimeout(() => server.sendMeta(meta()), 20);
const platform = { os: 'win32', isMac: false, shellName: 'PowerShell', keyHelp: { zoom: 'F6', asks: 'F7' } };

server = new PhoneServer({
  getInit: () => ({ home: 'h', meta: meta(), platform }),
  replayFor: id => `driver deck (${kind} words): tile ${id}\r\n`,
  call: (fn, args, ws) => {
    const t = tiles.find(x => x.id === args[0]);
    if (fn === 'setDriver') {
      // main.js applySettings: a word this deck does not know changes nothing
      const v = NEW ? DriverRule.mode(args[0]) : (['auto', 'phone', 'desk'].includes(args[0]) ? args[0] : null);
      if (v) settings.driver = v;
      push(); return settings.driver;
    }
    if (fn === 'resize' && t) {
      const cols = args[1] | 0, rows = args[2] | 0, from = ws && ws.desk ? 'remote' : 'phone';
      // new: lib/driverrule.js. old: a remote (a desk counted as a phone) drives unless the setting says the desk
      const ok = cols > 1 && rows > 1 && (NEW ? DriverRule.acceptsSize(settings.driver, who(t.id), from) : settings.driver !== 'desk');
      record.sizes.push({ id: t.id, cols, rows, from, accepted: ok, driver: settings.driver }); save();
      if (ok) { t.ptySize = { cols, rows }; push(); }
      return null;
    }
    if (fn === 'userInput' && t) { t.driver = ws && ws.desk ? (NEW ? 'remote' : 'phone') : 'phone'; push(); return null; }
    if (fn === '__office' && t) { t.ptySize = { cols: args[1] | 0, rows: args[2] | 0 }; push(); return true; }
    if (fn === '__flags' && t) { t.mouseOn = !!args[1]; t.sgrMouse = !!args[2]; push(); return true; }
    if (fn === 'getSettings') return { ...settings };
    return null;
  },
  desk: { open: () => ({ home: 'h', meta: meta(), platform, layout: {}, windowIndex: 1, grid: settings.grid }), close: () => push(), shown: () => push(), layout: () => {}, leaving: () => {} },
});
// every frame as it arrived, before the server reads it (so `kind` and deskInit's argument are on record)
const onMessage = server.onMessage.bind(server);
server.onMessage = (ws, m) => { record.frames.push(m); save(); return onMessage(ws, m); };

server.start({ port: PORT, token: TOKEN, bind: 'localhost' }).then(() => {
  save(); fs.writeFileSync(portFile, String(PORT));
  console.log(`driver deck (${kind}) on 127.0.0.1:${PORT}`);
  setInterval(() => server.sendMeta(meta()), 1000);
}).catch(e => { console.error('driver deck:', e.message); process.exit(1); });
