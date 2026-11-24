'use strict';
/* global Terminal, FitAddon, WebglAddon, deck */

const $ = s => document.querySelector(s);
const sidebar = $('#sidebar'), rail = $('#rail'), tilesEl = $('#tiles'), mainEl = $('#main'), emptyEl = $('#empty');
const clockEl = $('#clock'), newTile = $('#new-tile'), newAccts = $('.new-accts');
const settingsPanel = $('#settings');

const state = {
  accounts: [],
  tiles: [],          // meta from main, already ordered (pinned first, then by last stop)
  usage: {},
  settings: { fontSize: 14 },
  now: Date.now(),
  active: localStorage.getItem('deck.active') || null,
  confirm: {},        // tileId -> { kind: 'move'|'close', account? }
  platform: {},       // filled by deck.init(): os, isMac, keyHelp, fontStack, shellName
  windowIndex: 0,     // which deck window this renderer is (F5); tiles carry `inWindow` for R5's mark
  triggers: [],       // named booleans for triggered special prompts (R17), from meta
};
// The "New terminal" tile runs PowerShell on Windows and the login shell on a Mac (lib/platform.js
// shellCommand). Every view asks here rather than spelling "PowerShell" into a Mac user's face.
const shellName = () => state.platform.shellName || 'terminal';
const terms = new Map();    // tileId -> { term, fit, wrap }
const tileEls = new Map();  // tileId -> element
const actionSig = new Map();
const renaming = new Set();  // tile ids with an open rename box (the meta tick must not overwrite them)
let acctSig = '';

const acct = id => state.accounts.find(a => a.id === id) || null;
// Per-window remembered things (the selected tile, the collapsed sidebar): the primary window keeps the
// original keys, an auxiliary window (R13) its own, suffixed with its index. state.windowIndex arrives
// with deck.init, so before boot this names the primary's key — 90-boot.js re-reads for an auxiliary.
const lsKey = base => (state.windowIndex ? `${base}:${state.windowIndex}` : base);
const acctLabel = id => { const a = acct(id); return a ? a.label : (id || ''); };
