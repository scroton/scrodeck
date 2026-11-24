// core/test/vt/oracle/make-fixtures.js — writes the hand-written, synthesized and fuzz fixtures:
// core/test/fixtures/vt/<name>.bin (raw bytes) + <name>.json ({cols, rows, scrollback?, ops?, chunks?}).
// Real pty captures are made by capture.js. After either: node gen.js to (re)write the expected files.
//   node core/test/vt/oracle/make-fixtures.js
// Hunting for new divergences: write extra fuzz streams to a scratch folder and test that folder —
//   set VT_FIXTURES=<scratch dir>; node make-fixtures.js --fuzz <fromSeed> <toSeed>; node gen.js; vt_test <scratch dir>
'use strict';
const fs = require('fs');
const path = require('path');
const FIX = process.env.VT_FIXTURES || path.resolve(__dirname, '..', '..', 'fixtures', 'vt');
const FUZZ_ONLY = process.argv[2] === '--fuzz' ? [Number(process.argv[3]), Number(process.argv[4])] : null;
fs.mkdirSync(FIX, { recursive: true });

const E = '\x1b', CSI = '\x1b[', OSC = '\x1b]', ST = '\x1b\\', BEL = '\x07';
let count = 0;
// data: string (encoded as UTF-8) or Buffer, or an array of those
function fx(name, cols, rows, data, extra) {
  if (FUZZ_ONLY && !name.startsWith('fuzz-')) return;
  const parts = (Array.isArray(data) ? data : [data]).map(d => Buffer.isBuffer(d) ? d : Buffer.from(d, 'utf8'));
  fs.writeFileSync(path.join(FIX, name + '.bin'), Buffer.concat(parts));
  fs.writeFileSync(path.join(FIX, name + '.json'), JSON.stringify(Object.assign({ cols, rows }, extra || {})) + '\n');
  count++;
}
const cup = (r, c) => CSI + r + ';' + c + 'H';
const lines = (n, f) => Array.from({ length: n }, (_, i) => f(i)).join('\r\n');
const fill = (rows, cols) => lines(rows, i => String.fromCharCode(65 + i % 26).repeat(cols - 1 - (i % 3)));

// ------------------------------------------------------------------ C0
fx('c0-basic', 20, 6, 'hello\r\nworld\rW\n!\tx\ty\x08\x08Z\x07\x0bV\x0cF\x00N\x7fD\r\n\ttab\t\t\t\t\tend');
fx('c0-bs-edges', 10, 4, '\x08\x08a\x08\x08\x08b1234567890\x08X\r\n0123456789\x08\x08Y\r\nabc\x08\x08\x08\x08\x08d');
fx('c0-lf-scroll', 12, 4, lines(9, i => 'line ' + i) + '\n\n\rend');
fx('c0-nul-del-in-seq', 20, 4, 'a' + CSI + '3\x001\x7f m' + 'red?' + CSI + '\x083' + '1mX' + E + '\x0d[32mgreen' + CSI + '1\x0a;4mY');
fx('c0-can-sub-abort', 20, 4, 'a' + CSI + '31\x18mb' + CSI + '32\x1amc' + OSC + '2;no\x18title' + E + '\x18[d' + '\x1b\x1b[33mok');
fx('c0-so-si', 20, 4, 'abc\x0eabc\x0fabc' + E + ')0' + '\x0elqk\x0flqk' + E + '(0lqk' + E + '(Blqk');

// ------------------------------------------------------------------ cursor movement
fx('cursor-moves', 20, 8, fill(8, 20) + cup(4, 10) + '1' + CSI + 'A2' + CSI + '2B3' + CSI + '3C4' + CSI + '5D5' + CSI + 'E6' + CSI + '2F7' +
  CSI + '15G8' + CSI + '7d9' + CSI + '3`a' + CSI + '2ab' + CSI + '2ec' + CSI + '2;3fd' + CSI + 'He' + CSI + ';5Hf' + CSI + '6Hg' + CSI + '0;0Hh');
fx('cursor-clamp', 10, 5, fill(5, 10) + CSI + '99;99HA' + CSI + '99AB' + CSI + '99DC' + CSI + '99BD' + CSI + '99CE' + CSI + '0AF' + CSI + '0CG' +
  CSI + '999999999999GH' + CSI + '2147483647dI' + CSI + '4294967296;4294967297HJ' + CSI + '3;4;5;6HK');
fx('cursor-cnl-cpl', 12, 6, fill(6, 12) + cup(3, 5) + CSI + '2Ea' + CSI + '9Eb' + CSI + '3Fc' + CSI + '99Fd' + CSI + '0Ee');
fx('cursor-vpa-hpa-rel', 15, 6, cup(2, 2) + 'a' + CSI + '5db' + CSI + '10`c' + CSI + '99dd' + CSI + '99`e' + CSI + '1d' + CSI + '1`f' + CSI + '3a' + 'g' + CSI + '2eh' + CSI + '99ai' + CSI + '99ej');
fx('cursor-margins-stop', 12, 8, fill(8, 12) + CSI + '3;6r' + cup(4, 4) + CSI + '9AU' + CSI + '9BD' + cup(1, 1) + CSI + '9Ba' + cup(8, 1) + CSI + '9Ab' + cup(7, 3) + CSI + 'Bc' + cup(2, 3) + CSI + 'Ad');

// ------------------------------------------------------------------ erase
fx('erase-el', 16, 6, fill(6, 16) + cup(1, 6) + CSI + 'K' + cup(2, 6) + CSI + '1K' + cup(3, 6) + CSI + '2K' + cup(4, 6) + CSI + '44m' + CSI + '0K' + cup(5, 1) + CSI + 'K' + cup(6, 16) + 'x' + CSI + 'K' + CSI + '1K');
fx('erase-ed', 16, 6, fill(6, 16) + cup(3, 6) + CSI + '42mJ' + CSI + 'J' + cup(2, 3) + CSI + '45m' + CSI + '1J' + CSI + '0mZ');
fx('erase-ed2', 16, 6, fill(6, 16) + cup(3, 6) + CSI + '48;5;200m' + CSI + '2J' + 'after' + CSI + '9Jx' + CSI + '4Ky');
fx('erase-ed3-scrollback', 16, 5, lines(20, i => 'row' + i) + CSI + '3J' + 'kept' + CSI + '2J' + cup(1, 1) + 'top');
fx('erase-ech', 16, 4, fill(4, 16) + cup(1, 4) + CSI + 'X' + cup(2, 4) + CSI + '5X' + cup(3, 4) + CSI + '41m' + CSI + '99X' + cup(4, 16) + CSI + '0m' + CSI + '3X');
fx('erase-wide', 12, 5, '中文字符测试\r\n中文字符测试\r\n中文字符测试\r\n中文字符测试\r\n中文字符测试' + cup(1, 4) + CSI + 'K' + cup(2, 4) + CSI + '1K' + cup(3, 4) + CSI + '2X' + cup(4, 3) + CSI + '3X' + cup(5, 6) + CSI + 'P');
fx('erase-wrapped-flag', 10, 6, 'a'.repeat(35) + cup(2, 1) + CSI + 'K' + cup(3, 5) + CSI + 'K' + cup(4, 5) + CSI + '2K' + '\r\n' + 'b'.repeat(25) + cup(5, 3) + CSI + '1J');
fx('erase-selective-protected', 16, 5, fill(5, 16) + cup(2, 3) + CSI + '1"q' + 'PROT' + CSI + '0"q' + 'open' + CSI + '1"q' + '保' + CSI + '2"q' + cup(2, 1) + CSI + '?2K' + cup(3, 5) + CSI + '1"qXY' + CSI + '"q' + cup(3, 1) + CSI + '?J' + cup(1, 3) + CSI + '?1J' + cup(4, 1) + 'x' + CSI + '2Ky' + CSI + '?2J' + E + 'P$q"q' + ST);

// ------------------------------------------------------------------ insert / delete chars + lines, scrolling
fx('ich-dch', 16, 6, fill(6, 16) + cup(1, 4) + CSI + '@' + cup(2, 4) + CSI + '3@' + cup(3, 4) + CSI + '99@' + cup(4, 4) + CSI + 'P' + cup(5, 4) + CSI + '3P' + cup(6, 4) + CSI + '46m' + CSI + '99P');
fx('ich-dch-wide', 12, 6, lines(6, () => 'a中文b字符c测') + cup(1, 3) + CSI + '@' + cup(2, 2) + CSI + '@' + cup(3, 3) + CSI + 'P' + cup(4, 2) + CSI + 'P' + cup(5, 4) + CSI + '2P' + cup(6, 11) + CSI + '@');
fx('il-dl', 12, 8, lines(8, i => 'line' + i) + cup(3, 5) + CSI + 'L' + cup(5, 5) + CSI + '2M' + cup(7, 2) + CSI + '99L' + cup(1, 1) + CSI + '43m' + CSI + 'M');
fx('il-dl-region', 12, 9, lines(9, i => 'line' + i) + CSI + '3;7r' + cup(4, 3) + CSI + '2L' + cup(5, 3) + CSI + 'M' + cup(1, 1) + CSI + 'L' + cup(9, 1) + CSI + 'M' + cup(7, 1) + CSI + '9M' + cup(3, 4) + CSI + '99L');
fx('su-sd', 12, 8, lines(8, i => 'line' + i) + CSI + 'S' + CSI + '2T' + CSI + '41m' + CSI + 'S' + CSI + 'T' + CSI + '0m' + CSI + '3;6r' + CSI + '2S' + CSI + 'T' + CSI + '99T');
fx('su-no-scrollback', 12, 4, lines(4, i => 'line' + i) + CSI + '3S' + 'x' + CSI + '100S' + 'y');
fx('sl-sr-decic-decdc', 12, 6, lines(6, i => 'abcdefghij' + i) + CSI + '2;5r' + cup(3, 4) + CSI + '2 @' + CSI + ' A' + CSI + "2'}" + CSI + "'~" + cup(1, 1) + CSI + '3 @');
fx('decstbm-lf', 12, 8, lines(8, i => 'line' + i) + CSI + '2;5r' + cup(5, 1) + '\nA\nB\nC' + cup(8, 1) + '\nD\nE' + cup(1, 1) + E + 'M' + 'top' + cup(2, 1) + E + 'M' + E + 'M' + 'rev');
fx('decstbm-edge', 12, 6, lines(6, i => 'line' + i) + CSI + '4;2r' + 'a' + CSI + '3;3r' + 'b' + CSI + ';4r' + 'c' + CSI + '2;99r' + 'd' + CSI + '0;0r' + 'e' + CSI + '5r' + 'f' + CSI + 'r' + 'g' + CSI + '2;4r\n\n\n\n\nz');
fx('decstbm-scrollback', 12, 6, lines(6, i => 'line' + i) + CSI + '1;4r' + cup(4, 1) + '\nA\nB\nC' + CSI + 'r' + cup(6, 1) + '\nD\nE');
fx('ind-nel-ri', 12, 5, 'ab' + E + 'D' + 'cd' + E + 'E' + 'ef' + E + 'M' + 'gh' + E + 'M' + E + 'M' + E + 'M' + 'top' + cup(5, 3) + E + 'D' + 'x' + E + 'E' + 'y' + E + 'D' + E + 'D' + 'z' + '\xc2\x84' + 'i' + '\xc2\x85' + 'n');
fx('origin-mode', 12, 8, fill(8, 12) + CSI + '3;6r' + CSI + '?6h' + 'A' + cup(2, 3) + 'B' + cup(99, 99) + 'C' + CSI + '5GD' + CSI + '1dE' + CSI + '6n' + CSI + '9AF' + CSI + '?6l' + 'G' + cup(8, 1) + 'H' + CSI + '6n');
fx('origin-decsc', 12, 8, CSI + '3;6r' + CSI + '?6h' + cup(2, 2) + E + '7' + CSI + '?6l' + cup(1, 1) + 'a' + E + '8' + 'b' + CSI + '?6h' + E + '8' + 'c');

// ------------------------------------------------------------------ autowrap
fx('wrap-pending', 10, 5, '0123456789' + CSI + '6n' + 'A' + '\r\n' + '0123456789\rB' + '\r\n' + '0123456789\x08C' + '\r\n' + '0123456789' + CSI + 'DD' + '\r\n' + '0123456789' + CSI + 'CE');
fx('wrap-pending-ops', 10, 8, '0123456789' + CSI + 'KA\r\n' + '0123456789' + CSI + '1KB\r\n' + '0123456789' + CSI + 'XC\r\n' + '0123456789\tD\r\n' + '0123456789' + CSI + '@E\r\n' + '0123456789' + CSI + 'PF\r\n' + '0123456789' + CSI + 'AG');
fx('wrap-pending-lf', 10, 5, '0123456789\nA' + '\r\n0123456789' + E + 'D' + 'B' + '\r\n0123456789' + E + 'M' + 'C' + cup(5, 1) + '0123456789' + E + 'E' + 'D');
fx('wrap-off', 10, 4, CSI + '?7l' + '0123456789ABC' + '\r\n' + 'xx中文中文中文' + '\r\n' + CSI + '?7h' + '0123456789ABC');
fx('wrap-long', 10, 6, 'The quick brown fox jumps over the lazy dog and keeps running far away from here');
fx('wrap-wide-edge', 9, 6, 'abcdefgh中x\r\n中文字符测试中文字符\r\nabcdefghi中');
fx('wrap-scroll-region', 10, 6, lines(6, i => 'line' + i) + CSI + '2;4r' + cup(4, 1) + 'a'.repeat(25) + cup(6, 1) + 'b'.repeat(25));
fx('wrap-bottom-row-below-region', 10, 5, CSI + '1;3r' + cup(5, 8) + 'abcdefg' + 'h'.repeat(12));
fx('wrap-lf-clears-flag', 10, 5, 'a'.repeat(25) + cup(1, 1) + '\n\nX' + cup(1, 1) + 'b'.repeat(12));
fx('wrap-reverse', 10, 5, CSI + '?45h' + 'a'.repeat(25) + '\x08'.repeat(8) + 'X' + '\x08'.repeat(30) + 'Y' + '\r\n\r\n' + 'abcdefgh中' + '\x08\x08Z' + CSI + '?45$p' + CSI + '?45l');
fx('irm', 12, 5, 'abcdefghij' + cup(1, 3) + CSI + '4h' + 'XY' + cup(2, 1) + '0123456789AB' + cup(2, 5) + 'ins' + cup(3, 1) + 'a中b文c字d符' + cup(3, 2) + 'Z' + cup(3, 6) + '宽' + CSI + '4l' + 'ovr' + CSI + '4$p' + CSI + '20h' + cup(4, 5) + 'x\ny' + CSI + '20$p' + CSI + '20l' + '\nz');

// ------------------------------------------------------------------ tabs
fx('tabs', 40, 6, '\ta\tb\tc\td\te\tf' + '\r\n' + CSI + '5G' + E + 'H' + CSI + '12G' + E + 'H' + '\r\tx\ty\tz' + '\r\n' + CSI + '2Ia' + CSI + '99Ib' + CSI + 'Zc' + CSI + '3Zd' + CSI + '99Ze' +
  '\r\n' + CSI + '9G' + CSI + 'g' + '\r\tp\tq' + CSI + '0g' + '\r\n' + CSI + '3g' + '\tend' + '\r\n' + CSI + '20G' + '\xc2\x88' + '\r\tT');
fx('tabs-wrap-edge', 10, 4, '12345678\tA\tB' + '\r\n' + '1234567890\tC' + '\r\n\t\t\t\tD' + CSI + 'Z' + 'E');

// ------------------------------------------------------------------ SGR
fx('sgr-basic', 40, 8, CSI + '1mbold' + CSI + '22m ' + CSI + '2mdim' + CSI + '22m ' + CSI + '3mital' + CSI + '23m ' + CSI + '4mul' + CSI + '24m ' + CSI + '5mblink' + CSI + '25m\r\n' +
  CSI + '7minv' + CSI + '27m ' + CSI + '8mhid' + CSI + '28m ' + CSI + '9mstrike' + CSI + '29m ' + CSI + '53mover' + CSI + '55m ' + CSI + '21mdbl' + CSI + '24m\r\n' +
  CSI + '1;2;3;4;5;7;8;9;53mall' + CSI + 'mnone' + CSI + '1;3m' + CSI + '0;4mx' + CSI + '0m\r\n' +
  [30, 31, 32, 33, 34, 35, 36, 37, 90, 91, 92, 93, 94, 95, 96, 97].map(n => CSI + n + 'm' + (n % 10)).join('') + CSI + '39m.\r\n' +
  [40, 41, 42, 43, 44, 45, 46, 47, 100, 101, 102, 103, 104, 105, 106, 107].map(n => CSI + n + 'm' + (n % 10)).join('') + CSI + '49m.\r\n' +
  CSI + '1;31;42mA' + CSI + '22mB' + CSI + '39mC' + CSI + '49mD' + CSI + '6mE' + CSI + '10mF' + CSI + '26mG' + CSI + '99mH' + CSI + '0m');
fx('sgr-256-rgb', 40, 6, CSI + '38;5;196mA' + CSI + '48;5;21mB' + CSI + '38;2;1;2;3mC' + CSI + '48;2;250;251;252mD' + CSI + '0m\r\n' +
  CSI + '38;5;300mE' + CSI + '38;2;300;400;500mF' + CSI + '38;5mG' + CSI + '38;2;9mH' + CSI + '38mI' + CSI + '38;7;1mJ' + CSI + '0m\r\n' +
  CSI + '38;5;1;48;5;2;1mK' + CSI + '38;2;10;20;30;48;2;40;50;60;4mL' + CSI + '0;38;5;7mM' + CSI + '38;5;4;38;2;1;1;1mN' + CSI + '0m\r\n' +
  CSI + '31m' + CSI + '38;5;31mO' + CSI + '38;2;255;0;0m' + CSI + '32mP' + CSI + '38;2;1;2;3m' + CSI + '38;5;9mQ' + CSI + '48;2;9;9;9m' + CSI + '49mR');
fx('sgr-colon', 40, 6, CSI + '38:5:196mA' + CSI + '48:5:21mB' + CSI + '38:2::1:2:3mC' + CSI + '48:2::250:251:252mD' + CSI + '0m\r\n' +
  CSI + '38:2:1:2:3mE' + CSI + '38:2:7:1:2:3mF' + CSI + '38:5mG' + CSI + '38:2mH' + CSI + '38:2:::mI' + CSI + '38:5:5:9:9:9:9mJ' + CSI + '0m\r\n' +
  CSI + '4:0mK' + CSI + '4:1mL' + CSI + '4:2mM' + CSI + '4:3mN' + CSI + '4:4mO' + CSI + '4:5mP' + CSI + '4:6mQ' + CSI + '4:mR' + CSI + '24mS' + CSI + '4:3;1mT' + CSI + '0m\r\n' +
  CSI + '58:2::255:0:0mU' + CSI + '4:3mV' + CSI + '58;5;9mW' + CSI + '59mX' + CSI + '58:5:2;31mY' + CSI + '0m\r\n' +
  CSI + '1:2:3mZ' + CSI + ':5ma' + CSI + '38;5:4mb' + CSI + '38;2:1:2:3mc' + CSI + '31:2md' + CSI + '0m');
fx('sgr-many-params', 40, 4, CSI + Array.from({ length: 40 }, (_, i) => (i % 2 ? 1 : 31)).join(';') + 'mA' + CSI + '0m' +
  CSI + Array.from({ length: 31 }, () => 0).join(';') + ';1;4mB' + CSI + '0m' + CSI + '38;5;' + '1'.repeat(40) + 'mC' + CSI + ';;;1mD' + CSI + '4;' + Array.from({ length: 40 }, () => '4:3').join(';') + 'mE');
fx('sgr-erase-bg', 16, 6, CSI + '1;4;7;41m' + 'x' + CSI + 'K' + '\r\n' + CSI + '48;2;1;2;3m' + CSI + '2Ky' + CSI + '3@' + '\r\n\r\n\r\n\r\n\r\nscrolled' + CSI + 'T' + CSI + 'S' + CSI + '2L');
fx('sgr-underline-save-restore', 30, 4, CSI + '4:3m' + 'a' + E + '7' + CSI + '0m' + 'b' + E + '8' + 'c' + CSI + '24m' + 'd' + CSI + '4m' + E + '7' + CSI + '4:5m' + E + '8' + 'e' + CSI + '0m' + E + '8' + 'f' + CSI + '21m' + CSI + 's' + CSI + 'm' + CSI + 'u' + 'g');

// ------------------------------------------------------------------ save / restore cursor
fx('decsc-decrc', 20, 6, 'abc' + CSI + '1;31;44m' + E + '7' + cup(4, 10) + CSI + '0m' + 'X' + E + '8' + 'Y' + CSI + '0m' + cup(5, 5) + CSI + 's' + cup(1, 15) + 'Z' + CSI + 'u' + 'W' + E + '8' + 'V');
fx('decsc-charset', 20, 4, E + '(0' + E + '7' + E + '(B' + 'lqk' + E + '8' + 'lqk' + E + '(B' + 'lqk' + E + '8lqk' + E + '(B' + E + '7' + E + '(0' + E + '8' + 'lqk' + '\x0f' + 'lqk');
fx('decsc-scrolled', 12, 4, 'a' + E + '7' + '\r\n\r\n\r\n\r\n\r\n\r\nscroll' + E + '8' + 'R' + cup(4, 12) + 'x' + E + '7' + 'yz' + E + '8' + 'Q' + CSI + '?1048h' + cup(1, 1) + CSI + '?1048l' + 'P');
fx('decsc-unset-restore', 12, 4, 'abc' + CSI + '31m' + E + '8' + 'X' + CSI + 'u' + 'Y');

// ------------------------------------------------------------------ alt screen
fx('alt-1049', 20, 6, 'normal ' + CSI + '31m' + 'red' + CSI + '?1049h' + 'alt text' + cup(3, 3) + 'more' + CSI + '?1049l' + ' back');
fx('alt-1049-stay', 20, 6, 'normal\r\ntwo' + CSI + '44m' + CSI + '?1049h' + cup(2, 2) + 'in alt' + CSI + '0m' + ' plain' + CSI + '?1049h' + '!' + CSI + '6n');
fx('alt-47-1047', 20, 6, 'n1' + CSI + '?47h' + 'a1' + CSI + '?47l' + 'n2' + CSI + '?1047h' + 'a2' + cup(5, 5) + 'x' + CSI + '?1047l' + 'n3' + CSI + '?47h' + 'a3' + CSI + '?1049l' + 'n4');
fx('alt-scroll-no-scrollback', 12, 4, lines(8, i => 'norm' + i) + CSI + '?1049h' + lines(10, i => 'alt' + i) + CSI + '3J' + CSI + '?1049l' + '\r\nend');
fx('alt-region-and-tabs', 20, 6, CSI + '2;4r' + CSI + '?1049h' + CSI + '6n' + CSI + '3;5r' + CSI + '3g' + '\tx' + CSI + '?1049l' + '\ty\n\n\n\n\n\nz' + CSI + '?1049h' + '\tw' + '\n\n\n\n\n\n\nq');
fx('alt-modes-report', 20, 4, CSI + '?1049$p' + CSI + '?1049h' + CSI + '?1049$p' + CSI + '?47$p' + CSI + '?1047$p' + CSI + '?1048$p' + CSI + '?1049l' + CSI + '?47$p');

// ------------------------------------------------------------------ modes
fx('modes-set', 20, 4, CSI + '?25l' + CSI + '?1h' + E + '=' + CSI + '?2004h' + CSI + '?1000h' + CSI + '?1006h' + CSI + '?1004h' + CSI + '?2026h' + CSI + '5 q' + 'x');
fx('modes-reset', 20, 4, CSI + '?25l' + CSI + '?1h' + E + '=' + CSI + '?2004h' + CSI + '?1003h' + CSI + '?1006h' + CSI + '?1004h' + CSI + '?2026h' + CSI + '4 q' +
  CSI + '?25h' + CSI + '?1l' + E + '>' + CSI + '?2004l' + CSI + '?1003l' + CSI + '?1006l' + CSI + '?1004l' + CSI + '?2026l' + CSI + '0 q' + 'x');
fx('modes-mouse', 20, 4, CSI + '?9h' + CSI + '?9$p' + CSI + '?1000h' + CSI + '?9$p' + CSI + '?1000$p' + CSI + '?1002h' + CSI + '?1002$p' + CSI + '?1003h' + CSI + '?1003$p' + CSI + '?1000l' + CSI + '?1003$p' +
  CSI + '?1015h' + CSI + '?1015$p' + CSI + '?1005h' + CSI + '?1005$p' + CSI + '?1016h' + CSI + '?1016$p' + CSI + '?1006h' + CSI + '?1006$p' + CSI + '?1016$p' + CSI + '?1002h');
fx('modes-multi-param', 20, 4, CSI + '?25;1;2004;1004;7l' + '0123456789012345678901234' + CSI + '?7;25h' + CSI + '?1;2004$p');
fx('modes-decrqm', 20, 4, [1, 3, 6, 7, 8, 9, 12, 25, 45, 66, 67, 1000, 1002, 1003, 1004, 1005, 1006, 1015, 1016, 1048, 1049, 2004, 2026, 9999, 0].map(n => CSI + '?' + n + '$p').join('') +
  [2, 4, 12, 20, 99].map(n => CSI + n + '$p').join('') + CSI + '?12l' + CSI + '?12$p' + CSI + '?66h' + CSI + '?66$p' + CSI + '?66l');
fx('modes-decscusr', 20, 4, CSI + ' q' + CSI + '1 q' + CSI + '2 q' + CSI + '3 q' + CSI + '6 q' + CSI + '7 q' + 'x');
fx('modes-decscusr-default', 20, 4, CSI + '4 q' + CSI + '0 q' + 'x' + CSI + '9 q');
fx('modes-keypad', 20, 4, E + '=' + 'a' + CSI + '?66$p' + E + '>' + CSI + '?66$p' + CSI + '?66h');
fx('modes-sync-output', 30, 4, CSI + '?2026h' + 'frame one' + CSI + '?2026$p' + CSI + '?2026l' + CSI + '?2026$p' + CSI + '?2026h' + 'frame two');
fx('modes-charset-reset-2', 20, 4, E + '(0' + E + ')0' + 'lq' + CSI + '?2h' + 'lq' + '\x0e' + 'lq');

// ------------------------------------------------------------------ reports
fx('reports-dsr-da', 20, 6, CSI + '5n' + 'abc' + CSI + '6n' + cup(4, 7) + CSI + '6n' + CSI + '?6n' + CSI + 'c' + CSI + '0c' + CSI + '1c' + CSI + '>c' + CSI + '>0c' + CSI + '>1c' + CSI + '=c' + CSI + '>q' + CSI + '18t' + CSI + '14t' + CSI + '?15n' + CSI + '7n');
fx('reports-decrqss', 20, 6, E + 'P$qm' + ST + E + 'P$qr' + ST + CSI + '2;4r' + E + 'P$qr' + ST + E + 'P$q q' + ST + E + 'P$q"p' + ST + E + 'P$q"q' + ST + E + 'P$qx' + ST + E + 'P$q' + ST + CSI + '?12l' + E + 'P$q q' + ST + E + 'P$qm\x18' + E + 'P1;2$qm' + E + '\\' + E + 'P$qr\x9c');
fx('reports-pending-wrap-cpr', 10, 3, '0123456789' + CSI + '6n' + CSI + '?6n');

// ------------------------------------------------------------------ OSC / DCS / APC / PM / SOS
fx('osc-title', 20, 4, OSC + '0;first' + BEL + 'a' + OSC + '2;second title' + ST + 'b' + OSC + '1;icon only' + BEL + 'c');
fx('osc-title-utf8', 20, 4, OSC + '2;héllo 世界 🎉 title' + BEL + 'x');
fx('osc-title-c1-st', 20, 4, OSC + '2;c1 terminated\xc2\x9c' + 'x' + '\xc2\x9d' + '0;c1 osc' + BEL + 'y');
fx('osc-title-empty', 20, 4, OSC + '2;x' + BEL + OSC + '2;' + BEL + 'a');
fx('osc-title-no-semicolon', 20, 4, OSC + '2;x' + BEL + OSC + '2' + BEL + 'a' + OSC + BEL + 'b' + OSC + ';zz' + BEL + 'c');
fx('osc-abort', 20, 4, OSC + '2;keep' + BEL + OSC + '2;gone\x18' + 'a' + OSC + '2;gone2\x1a' + 'b' + OSC + '2x;bad id' + BEL + 'c' + OSC + '2;esc ends' + E + '[31m' + 'd' + OSC + '0;ctl\x01\x02in\x7ftitle' + BEL);
fx('osc-hyperlink', 30, 4, 'a' + OSC + '8;;http://example.com' + ST + 'link' + OSC + '8;;' + ST + 'b' + OSC + '8;id=1;http://x' + BEL + CSI + '4:3m' + 'L' + CSI + '24m' + 'M' + CSI + '0m' + 'N' + OSC + '8;id=1;' + BEL + 'O' + OSC + '8;;' + BEL + 'P' + OSC + '8' + BEL + OSC + '8;' + BEL + 'Q');
fx('osc-hyperlink-erase', 30, 4, OSC + '8;;http://example.com' + ST + 'link text' + CSI + '5D' + CSI + 'K' + CSI + '2@' + E + '7' + OSC + '8;;' + ST + 'x' + E + '8' + 'y' + CSI + '0m' + 'z');
fx('osc-swallowed', 30, 4, 'a' + OSC + '4;1;rgb:ff/00/00' + BEL + 'b' + OSC + '10;?' + BEL + 'c' + OSC + '11;?' + ST + 'd' + OSC + '52;c;aGVsbG8=' + BEL + 'e' + OSC + '4;1;?' + BEL + 'f' + OSC + '12;?' + BEL + OSC + '104' + BEL + OSC + '110' + BEL + OSC + '7;file://host/c' + BEL + OSC + '133;A' + BEL + OSC + '9;4;1;50' + BEL + OSC + '777;notify;t;b' + ST + 'g');
fx('osc-long', 30, 4, OSC + '2;' + 'T'.repeat(5000) + BEL + 'x' + OSC + '52;c;' + 'QUJD'.repeat(20000) + BEL + 'y');
fx('dcs-swallow', 30, 4, 'a' + E + 'P1;2|payload with \x07 bel and ; stuff' + ST + 'b' + E + 'Pq#0;2;0;0;0#0~~@@vv@@~~$' + ST + 'c' + E + 'P+q544e' + ST + 'd' + E + 'P$qm' + E + '[31m' + 'e' + E + 'Pabc\x18' + 'f' + E + 'P>|x' + '\xc2\x9c' + 'g' + '\xc2\x90' + 'zzz' + ST + 'h' + E + 'P1;2;:3?xyz' + ST + 'i');
fx('apc-pm-sos', 30, 4, 'a' + E + '_apc payload \x07 with bel' + ST + 'b' + E + '^pm payload' + ST + 'c' + E + 'Xsos payload' + ST + 'd' + E + '_unterminated then esc' + E + '[32m' + 'e' + '\xc2\x9f' + 'c1 apc' + '\xc2\x9c' + 'f' + E + '_can\x18' + 'g');
fx('apc-nonascii-leaks', 30, 4, 'a' + E + '_Gkitty;payload ünï' + ST + 'b');
fx('esc-unknown', 30, 4, 'a' + E + 'Zb' + E + '#3c' + E + '#8' + 'd' + E + ' Fe' + E + '(1f' + E + '(<g' + E + '%Gh' + E + '%@i' + E + '\\j' + E + '6k' + E + '9l' + E + 'Nm' + E + 'On' + CSI + '?1;2c' + CSI + '<0;1;1M' + CSI + '>4;2m' + CSI + '=1u' + CSI + '?u' + CSI + '>1uo');
fx('csi-intermediates-ignore', 30, 4, 'a' + CSI + '1 31mb' + CSI + '31!mc' + CSI + '?31md' + CSI + '31?me' + CSI + '1;2 ;3mf' + CSI + '3 1mg' + CSI + '<31mh' + CSI + '31;<mi' + CSI + '$$31mj' + CSI + ' \x7f31mk' + CSI + '31m' + 'l');
fx('c1-controls', 30, 5, 'a' + '\xc2\x9b' + '31m' + 'b' + '\xc2\x9b' + '0m' + 'c' + '\xc2\x84' + 'd' + '\xc2\x85' + 'e' + '\xc2\x8d' + 'f' + '\xc2\x9d' + '2;c1' + '\xc2\x9c' + 'g' + '\xc2\x80\xc2\x81\xc2\x99' + 'h' + '\xc2\xa0' + 'i' + CSI + '3' + '\xc2\x9b' + '1mj');
fx('c1-in-escape-states', 30, 4, 'a' + E + '\xc2\x9b' + '32mb' + CSI + '1;' + '\xc2\x85' + 'c' + OSC + '2;t' + '\xc2\x84' + 'd' + CSI + '1' + E + '[4me');

// ------------------------------------------------------------------ charsets
fx('charset-dec-graphics', 40, 6, E + '(0' + '`abcdefghijklmnopqrstuvwxyz{|}~' + '\r\n' + 'lqqqk\r\nx   x\r\nmqqqj' + E + '(B' + '\r\nlqk ABC _');
fx('charset-g1-g2-g3', 40, 4, E + ')0' + E + '*A' + E + '+0' + 'q#' + '\x0e' + 'q#' + E + 'n' + 'q#' + E + 'o' + 'q#' + E + '~' + 'q#' + E + '}' + 'q#' + E + '|' + 'q#' + '\x0f' + 'q#' + E + '-0' + E + '.A' + E + '/0' + '\x0eq#' + E + 'n' + 'q#');
fx('charset-national', 40, 4, ['A', '4', '5', 'C', 'R', 'Q', 'K', 'Y', 'E', '6', 'Z', 'H', '7', '='].map(f => E + '(' + f + '#@[\\]^_`{|}~').join(' ') + E + '(B#@');
fx('charset-rep', 20, 4, E + '(0' + 'q' + CSI + '5b' + E + '(B' + 'q' + CSI + '3b');

// ------------------------------------------------------------------ resets
fx('ris', 20, 6, lines(12, i => 'line' + i) + CSI + '1;31;44m' + CSI + '2;4r' + CSI + '?6h' + CSI + '?7l' + CSI + '?1h' + CSI + '?2004h' + CSI + '?1000h' + CSI + '?1006h' + CSI + '4h' + E + '(0' + E + 'H' + CSI + '3 q' + OSC + '2;title stays?' + BEL + E + 'c' + 'after\tq' + CSI + '6n');
fx('ris-cursor-hidden', 20, 4, CSI + '?25l' + 'x' + E + 'c' + 'y');
fx('ris-in-alt', 20, 4, 'normal' + CSI + '?1049h' + 'alt' + E + 'c' + 'fresh');
fx('ris-mid-string', 20, 4, OSC + '2;unfinished' + E + 'c' + 'a' + E + 'P$qm' + E + 'c' + 'b' + CSI + '31' + E + 'c' + 'c');
fx('decstr', 20, 6, lines(8, i => 'line' + i) + CSI + '1;31;44m' + CSI + '2;4r' + CSI + '?6h' + CSI + '?7l' + CSI + '?1h' + CSI + '?2004h' + CSI + '?1000h' + CSI + '?25l' + CSI + '4h' + E + '(0' + CSI + '3 q' + E + '7' + CSI + '!p' + 'after' + 'q'.repeat(20) + E + '8' + 'R' + CSI + '6n');
fx('decaln', 12, 5, lines(5, i => 'line' + i) + CSI + '2;4r' + CSI + '1;31;44m' + cup(3, 3) + E + '#8' + 'x' + CSI + '0m' + CSI + '?6h' + E + '#8' + 'y');

// ------------------------------------------------------------------ REP
fx('rep', 20, 6, 'a' + CSI + 'b' + CSI + '3b' + 'x' + CSI + '0b' + '\r\n' + 'y' + CSI + '25b' + '\r\n' + CSI + '3b' + 'z' + CSI + '31m' + CSI + '2b' + 'w' + '\x08' + CSI + '2b' + '\r\n' + '中' + CSI + '3b' + 'e\u0301' + CSI + '2b');
fx('rep-wrap', 10, 4, 'ab' + CSI + '30b' + '中' + CSI + '7b');

// ------------------------------------------------------------------ unicode
fx('unicode-wide', 20, 6, '中文字符 한국어 日本語 ｆｕｌｌ\r\na中b文c\r\n' + '\u2329\u232a\u3000\u303f\ua4cf\ua4d0\uac00\ud7a3\uf900\ufe10\ufe30\uff01\uff60\uff61\uffe0\uffe6\uffe7');
fx('unicode-wide-supplementary', 20, 4, '\u{20000}\u{2fffd}\u{2fffe}\u{30000}\u{3fffd}\u{3fffe}\u{1f600}\u{1f468}\u{10000}x');
fx('unicode-combining', 20, 6, 'e\u0301 a\u0308\u0304 n\u0303\r\n\u0301x\r\nab\u200dcd\ufe0fef\u200b\u200c\u200e\r\n中\u0301文\ufe00\r\n' + 'q' + CSI + 'C' + '\u0301' + '\r\n' + 'z\u0301\u0302\u0303\u0304\u0305\u0306\u0307\u0308');
fx('unicode-combining-wrap', 5, 5, 'abcde\u0301f\r\n中文\u0301中\u0302字\r\nabcd中\u0301');
fx('unicode-emoji', 30, 6, '😀😃 👍🏽 👨‍👩‍👧‍👦 🏳️‍🌈 ❤️ ☺️ ✅ ⚡ 🇺🇸 ©️ 1️⃣\r\n⏺ ✻ ✽ ❯ ● ○ ◆ ▶ ⎿ ⚠ ✓ ✗ → ← ↑ ↓ … • ·');
fx('unicode-box-drawing', 30, 6, '┌──────┬──────┐\r\n│ cell │ cell │\r\n├──────┼──────┤\r\n│ ╭──╮ │ ▀▄█▌ │\r\n└──────┴──────┘\r\n━┃┏┓┗┛╋═║╔╗╚╝╬░▒▓');
fx('unicode-zero-width-first', 20, 4, '\u0301\u0302abc' + cup(2, 1) + '\u200d\u200dxy' + cup(3, 5) + '\ufe0f' + cup(4, 1) + 'a\tb\u0301' + CSI + '31m' + '\u0302');
fx('unicode-overwrite-wide', 12, 6, lines(6, () => '中文字符测试') + cup(1, 2) + 'x' + cup(2, 3) + 'y' + cup(3, 4) + '宽' + cup(4, 1) + 'ab' + cup(5, 11) + 'q' + cup(5, 12) + 'r' + cup(6, 12) + '宽');
fx('unicode-invalid-utf8', 30, 8, [
  'a', Buffer.from([0x80]), 'b', Buffer.from([0xbf]), 'c', Buffer.from([0xc0, 0x80]), 'd', Buffer.from([0xc1, 0xbf]), 'e', Buffer.from([0xc2]), 'f\r\n',
  Buffer.from([0xe0, 0x80, 0x80]), 'g', Buffer.from([0xe0, 0xa0]), 'h', Buffer.from([0xed, 0xa0, 0x80]), 'i', Buffer.from([0xef, 0xbf, 0xbe]), 'j', Buffer.from([0xe2, 0x82]), 'k\r\n',
  Buffer.from([0xf0, 0x80, 0x80, 0x80]), 'l', Buffer.from([0xf0, 0x9f, 0x98]), 'm', Buffer.from([0xf4, 0x90, 0x80, 0x80]), 'n', Buffer.from([0xf5, 0x80]), 'o', Buffer.from([0xf8, 0x88, 0x80, 0x80, 0x80]), 'p\r\n',
  Buffer.from([0xff, 0xfe]), 'q', Buffer.from([0xef, 0xbb, 0xbf]), 'r', Buffer.from([0xf0, 0x9f]), Buffer.from([0xe2, 0x82, 0xac]), 's', Buffer.from([0xc2, 0x1b]), '[31mt', Buffer.from([0xe2, 0x80, 0x1b]), '[0mu\r\n',
  Buffer.from([0xf0, 0x9f, 0x98, 0x80, 0xf0, 0x9f, 0x98, 0x41, 0xe2, 0x80, 0x94, 0xe2, 0x80, 0xa6, 0xe2, 0x94, 0x80]), 'v',
  Buffer.from([0xef, 0xbf, 0xbd]), 'w', Buffer.from([0xed, 0x9f, 0xbf, 0xee, 0x80, 0x80, 0xf4, 0x8f, 0xbf, 0xbf, 0xf1, 0x80, 0x80, 0x80]), 'x',
]);
fx('unicode-utf8-in-osc', 30, 4, [OSC + '2;bad ', Buffer.from([0xff, 0xc3]), ' bytes ✓', BEL, 'x']);
fx('unicode-special', 30, 4, 'a\u00a0b\u00adc\u2028d\u2029e\ufffdf\ufffeg\uffffh\u{10ffff}i\u{e0001}j\u{e0100}k\u1160l\u115fm\u200bn');

// ------------------------------------------------------------------ parser edge cases
fx('params-edge', 30, 6, CSI + ';5H' + 'a' + CSI + '2;H' + 'b' + CSI + ';H' + 'c' + CSI + '0002;0003H' + 'd' + CSI + '2:9;3:9H' + 'e' + CSI + ':::;4;5H' + 'f' +
  CSI + Array.from({ length: 31 }, () => '1').join(';') + ';3;7H' + 'g' + CSI + Array.from({ length: 32 }, () => '1').join(';') + ';5;5H' + 'h' + CSI + '4;' + '9'.repeat(30) + 'H' + 'i');
fx('params-subparam-overflow', 30, 4, CSI + '38:2:' + Array.from({ length: 40 }, (_, i) => i).join(':') + 'mA' + CSI + '0m' + CSI + '4:' + Array.from({ length: 40 }, () => 3).join(':') + ';1mB' + CSI + '0m' +
  CSI + Array.from({ length: 20 }, () => '4:3:3').join(';') + ';38:2::1:2:3mC');
fx('esc-inside-csi', 30, 4, 'a' + CSI + '31' + CSI + '32mb' + CSI + '1;' + E + '[4mc' + CSI + E + E + '[0md' + E + '[' + E + ']2;t' + BEL + 'e' + CSI + '31' + OSC + '2;u' + BEL + 'mf');
fx('split-sequences', 30, 5, CSI + '38;2;10;20;30m' + 'rgb' + OSC + '2;split title' + ST + cup(2, 5) + '中文😀e\u0301' + CSI + '?1049h' + 'alt' + CSI + '?1049l' + E + '(0' + 'lqk' + E + '(B' + E + 'P$qm' + ST, { chunks: [1, 2, 3, 1, 1, 5, 1, 2, 7, 1] });

// ------------------------------------------------------------------ scrollback
fx('scrollback-basic', 20, 5, lines(40, i => 'row ' + i + ' ' + 'x'.repeat(i % 25)));
fx('scrollback-limit', 20, 5, lines(60, i => 'row ' + i + ' ' + 'x'.repeat(i % 25)), { scrollback: 12 });
fx('scrollback-zero', 20, 5, lines(20, i => 'row ' + i), { scrollback: 0 });
fx('scrollback-limit-region', 20, 6, lines(30, i => 'row ' + i) + CSI + '1;4r' + cup(4, 1) + lines(20, i => 'reg ' + i) + CSI + 'r' + cup(6, 1) + '\r\nend', { scrollback: 10 });
fx('scrollback-limit-wrap', 10, 4, lines(20, i => ('w' + i + ' ').repeat(6)), { scrollback: 7 });
fx('scrollback-colors', 20, 4, lines(12, i => CSI + '4' + (i % 8) + 'm' + 'bg' + i + CSI + 'K') + CSI + '0m');
fx('scrollback-ed3-full', 20, 4, lines(30, i => 'row ' + i) + CSI + '3J' + lines(4, i => 'new ' + i), { scrollback: 8 });

// ------------------------------------------------------------------ resize
const wrappedDoc = lines(12, i => ('L' + i + ' ').repeat(3 + (i * 5) % 9) + '|');
fx('resize-rows-grow', 20, 5, lines(12, i => 'row ' + i) + 'X' + '@AFTER', { ops: [{ feed: lines(12, i => 'row ' + i).length + 1 }, { resize: [20, 9] }] });
fx('resize-rows-shrink', 20, 8, lines(12, i => 'row ' + i) + 'X' + '@AFTER', { ops: [{ feed: lines(12, i => 'row ' + i).length + 1 }, { resize: [20, 4] }] });
fx('resize-rows-shrink-cursor-top', 20, 8, 'one\r\ntwo' + '@AFTER', { ops: [{ feed: 8 }, { resize: [20, 3] }] });
fx('resize-rows-shrink-cursor-mid', 20, 8, lines(6, i => 'row ' + i) + cup(3, 2) + '@AFTER', { ops: [{ feed: lines(6, i => 'row ' + i).length + 6 }, { resize: [20, 4] }] });
fx('resize-cols-grow-reflow', 12, 6, wrappedDoc + '\r\n$ ', { ops: [{ feed: wrappedDoc.length + 4 }, { resize: [30, 6] }] });
fx('resize-cols-shrink-reflow', 30, 6, wrappedDoc + '\r\n$ ', { ops: [{ feed: wrappedDoc.length + 4 }, { resize: [11, 6] }] });
fx('resize-cols-shrink-then-grow', 30, 6, wrappedDoc + '\r\n$ more', { ops: [{ feed: wrappedDoc.length + 4 }, { resize: [9, 6] }, { resize: [44, 6] }] });
fx('resize-both', 30, 8, wrappedDoc + '\r\n$ more text here', { ops: [{ feed: wrappedDoc.length + 4 }, { resize: [17, 5] }, { feed: 5 }, { resize: [50, 12] }, { feed: 3 }, { resize: [8, 3] }] });
fx('resize-reflow-wide', 20, 6, lines(6, i => '中文字符测试'.repeat(1 + i % 4) + 'a'.repeat(i)) + '\r\n$ ', { ops: [{ feed: 100000 }, { resize: [11, 6] }, { resize: [7, 6] }, { resize: [26, 6] }] });
fx('resize-reflow-cursor-line', 20, 5, 'short\r\n' + 'c'.repeat(50) + '@' + 'tail', { ops: [{ feed: 58 }, { resize: [12, 5] }, { resize: [33, 5] }] });
fx('resize-reflow-no-scrollback-yet', 30, 10, 'a'.repeat(70) + '\r\n' + 'b'.repeat(40) + '\r\nprompt> ', { ops: [{ feed: 100000 }, { resize: [10, 10] }, { resize: [25, 10] }] });
fx('resize-reflow-content-below-cursor', 30, 10, lines(8, i => ('z' + i).repeat(20)) + cup(2, 3), { ops: [{ feed: 100000 }, { resize: [14, 10] }] });
fx('resize-reflow-scrollback-full', 20, 4, lines(30, i => ('f' + i + ' ').repeat(4 + i % 4)) + '\r\n> ', { scrollback: 15, ops: [{ feed: 100000 }, { resize: [9, 4] }, { resize: [31, 4] }] });
fx('resize-reflow-colors', 20, 5, lines(8, i => CSI + '3' + (i % 8) + ';4' + ((i + 3) % 8) + 'm' + ('c' + i).repeat(14) + CSI + '0m') + '\r\n', { ops: [{ feed: 100000 }, { resize: [13, 5] }, { resize: [28, 7] }] });
fx('resize-alt', 20, 6, 'normal ' + 'n'.repeat(40) + CSI + '?1049h' + lines(6, i => ('alt' + i + ' ').repeat(4).slice(0, 20 - i)) + cup(3, 4) + '@' + 'X' + CSI + 'P' + CSI + '2@' + 'Y', { ops: [{ feed: 100000 - 1 }, { resize: [12, 4] }] });
{
  const pre = 'normal ' + 'n'.repeat(40) + CSI + '?1049h' + lines(6, i => ('alt' + i + ' ').repeat(4).slice(0, 20 - i)) + cup(3, 4);
  const mid = 'X' + CSI + 'P' + CSI + '2@' + 'Y' + cup(1, 1) + CSI + '3P' + cup(2, 2) + CSI + '4@' + cup(4, 12) + 'edge' + cup(4, 1) + CSI + 'K';
  const post = cup(2, 1) + CSI + 'P' + 'Z' + CSI + '?1049l' + 'back';
  fx('resize-alt-shrink-grow', 20, 6, pre + mid + post, { ops: [{ feed: pre.length }, { resize: [12, 4] }, { feed: mid.length }, { resize: [26, 7] }] });
  fx('resize-alt-leave-after', 20, 6, pre + post, { ops: [{ feed: pre.length }, { resize: [30, 9] }] });
}
fx('resize-tabs', 20, 4, CSI + '5G' + E + 'H' + '\r' + '@' + '\ta\tb\tc\td\te\tf', { ops: [{ feed: 6 }, { resize: [50, 4] }, { resize: [30, 4] }, { resize: [60, 4] }] });
fx('resize-margins-reset', 20, 8, lines(8, i => 'row ' + i) + CSI + '3;6r' + CSI + '?6h' + '@' + 'x\n\n\n\n\n\ny' + CSI + '6n', { ops: [{ feed: lines(8, i => 'row ' + i).length + 11 }, { resize: [20, 10] }] });
fx('resize-saved-cursor', 20, 6, lines(10, i => 'row ' + i) + cup(5, 15) + E + '7' + cup(1, 1) + '@' + E + '8' + 'S', { ops: [{ feed: lines(10, i => 'row ' + i).length + 14 }, { resize: [10, 4] }] });
fx('resize-min', 20, 6, 'abcdef\r\nghijkl' + '@' + 'mnop中文', { ops: [{ feed: 14 }, { resize: [1, 1] }, { feed: 4 }, { resize: [2, 1] }] });
fx('resize-pending-wrap', 10, 4, '0123456789' + '@' + 'AB', { ops: [{ feed: 10 }, { resize: [14, 4] }] });
fx('resize-pending-wrap-shrink', 10, 4, 'x\r\n0123456789' + '@' + 'AB', { ops: [{ feed: 13 }, { resize: [6, 4] }] });

// 1-row screens, a full list, and the alt buffer's hidden lines (each was a divergence the fuzz found)
fx('wrap-one-row', 12, 1, 'abcdefghijk中文' + 'x'.repeat(30) + 'yz宽');
fx('wrap-one-row-alt', 12, 1, CSI + '?1049h' + 'abcdefghijk中文' + 'x'.repeat(30) + 'yz宽');
fx('decaln-origin-full-list', 12, 5, lines(14, i => 'row ' + i) + CSI + '3;5r' + CSI + '?6h' + E + '#8' + 'x', { scrollback: 6 });
fx('decaln-origin-not-full', 12, 5, lines(4, i => 'row ' + i) + CSI + '3;5r' + CSI + '?6h' + E + '#8' + 'x');
{
  // shrink while the alt buffer is inactive, enter it, scroll it (xterm keeps 3 hidden lines), then grow:
  // the app repaints after the resize, as every full-screen app does
  const pre = 'normal text\r\n';
  const alt = CSI + '?1049h' + lines(9, i => 'alt line ' + i);
  const repaint = CSI + 'H' + lines(9, i => 'repaint ' + i + CSI + 'K') + CSI + '5;3H' + 'cursor';
  fx('resize-alt-hidden-lines', 20, 9, pre + alt, { ops: [{ feed: pre.length }, { resize: [20, 6] }] });
  fx('resize-alt-hidden-lines-grow', 20, 9, pre + alt + repaint, { ops: [{ feed: pre.length }, { resize: [20, 6] }, { feed: alt.length }, { resize: [24, 9] }] });
  fx('resize-alt-hidden-lines-shrink', 20, 9, pre + alt + 'tail', { ops: [{ feed: pre.length }, { resize: [20, 6] }, { feed: alt.length }, { resize: [16, 4] }] });
  fx('resize-alt-hidden-lines-leave', 20, 9, pre + alt + CSI + '?1049l' + 'back' + CSI + '?1049h' + lines(9, i => 'again ' + i), { ops: [{ feed: pre.length }, { resize: [20, 6] }, { feed: alt.length }, { resize: [20, 8] }] });
}

// ------------------------------------------------------------------ a Claude-Code-like session (Ink)
{
  const rgb = (r, g, b) => CSI + '38;2;' + r + ';' + g + ';' + b + 'm';
  const bgc = (r, g, b) => CSI + '48;2;' + r + ';' + g + ';' + b + 'm';
  const R = CSI + '0m';
  const sync = s => CSI + '?2026h' + s + CSI + '?2026l';
  const W = 100;
  let s = '';
  s += CSI + '?2004h' + CSI + '?1004h' + CSI + '?25l' + OSC + '0;✳ Claude Code' + BEL + CSI + '>1u' + OSC + '11;?' + BEL + CSI + 'c';
  const box = (title, body) => {
    let o = rgb(215, 119, 87) + '╭' + '─'.repeat(W - 2) + '╮' + R + '\r\n';
    o += rgb(215, 119, 87) + '│' + R + ' ' + rgb(215, 119, 87) + '✻' + R + ' ' + CSI + '1m' + title + CSI + '22m' + ' '.repeat(W - 6 - title.length) + rgb(215, 119, 87) + '│' + R + '\r\n';
    for (const b of body) o += rgb(215, 119, 87) + '│' + R + '   ' + CSI + '2m' + b + CSI + '22m' + CSI + 'K' + CSI + W + 'G' + rgb(215, 119, 87) + '│' + R + '\r\n';
    o += rgb(215, 119, 87) + '╰' + '─'.repeat(W - 2) + '╯' + R + '\r\n';
    return o;
  };
  s += sync(box('Welcome to Claude Code!', ['/help for help, /status for your current setup', '', 'cwd: C:\\Users\\dev\\project']));
  const prompt = text => rgb(136, 136, 136) + '╭' + '─'.repeat(W - 2) + '╮' + R + '\r\n' + rgb(136, 136, 136) + '│' + R + ' > ' + text + CSI + '7m' + ' ' + CSI + '27m' + CSI + 'K' + CSI + W + 'G' + rgb(136, 136, 136) + '│' + R + '\r\n' +
    rgb(136, 136, 136) + '╰' + '─'.repeat(W - 2) + '╯' + R + '\r\n' + '  ' + CSI + '2m' + '? for shortcuts' + CSI + '22m' + CSI + 'K';
  s += sync(prompt(''));
  // typing: each keystroke redraws the prompt box (cursor up 3, erase lines)
  const typed = 'fix the flaky test in パーサー 🙂 and explain';
  let cur = '';
  for (const ch of Array.from(typed)) {
    cur += ch;
    s += sync(CSI + '2K' + CSI + '1A' + CSI + '2K' + CSI + '1A' + CSI + '2K' + CSI + '1A' + CSI + '2K' + CSI + 'G' + prompt(cur));
  }
  // submit: erase the prompt, print the message, then a spinner that redraws in place
  s += sync(CSI + '2K' + CSI + '1A' + CSI + '2K' + CSI + '1A' + CSI + '2K' + CSI + '1A' + CSI + '2K' + CSI + 'G' + CSI + '2m' + '> ' + typed + CSI + '22m' + '\r\n\r\n');
  const spin = ['✻', '✽', '✶', '✳', '✢', '·'];
  for (let i = 0; i < 24; i++) {
    s += sync((i ? CSI + '2K' + CSI + '1A' + CSI + '2K' + CSI + 'G' : '') + rgb(215, 119, 87) + spin[i % 6] + ' Pondering…' + R + CSI + '2m' + ' (' + i + 's · ↑ ' + (i * 37) + ' tokens · ' + CSI + '1m' + 'esc' + CSI + '22;2m' + ' to interrupt)' + R + '\r\n');
  }
  s += CSI + '2K' + CSI + '1A' + CSI + '2K' + CSI + 'G';
  // assistant output with a tool call, a diff and a long wrapped paragraph
  s += '⏺ ' + CSI + '1m' + 'Read' + CSI + '22m' + '(src/パーサー/parser.ts)' + '\r\n' + '  ⎿  ' + CSI + '2m' + 'Read 214 lines (ctrl+r to expand)' + CSI + '22m' + '\r\n\r\n';
  s += '⏺ ' + CSI + '1m' + 'Update' + CSI + '22m' + '(src/パーサー/parser.ts)' + '\r\n' + '  ⎿  Updated with ' + CSI + '1m' + '2' + CSI + '22m' + ' additions and ' + CSI + '1m' + '1' + CSI + '22m' + ' removal\r\n';
  for (let i = 0; i < 6; i++) {
    const n = 40 + i;
    if (i === 2) s += '       ' + CSI + '2m' + n + CSI + '22m' + ' ' + bgc(74, 21, 21) + rgb(255, 255, 255) + '-    return parse(input, { strict: false });' + CSI + 'K' + R + '\r\n';
    else if (i === 3 || i === 4) s += '       ' + CSI + '2m' + n + CSI + '22m' + ' ' + bgc(21, 61, 21) + rgb(255, 255, 255) + '+    const 結果 = parse(input, { strict: true }); // ✓' + CSI + 'K' + R + '\r\n';
    else s += '       ' + CSI + '2m' + n + CSI + '22m' + '      ' + rgb(86, 156, 214) + 'const' + R + ' x' + i + ' = ' + rgb(206, 145, 120) + '"value"' + R + ';\r\n';
  }
  s += '\r\n⏺ ' + 'The test was flaky because the parser raced the tokenizer — the fix makes the handoff explicit, and I added a regression test that feeds the stream one byte at a time (which is how the bug showed up in CI). '.repeat(3) + '\r\n\r\n';
  for (let i = 0; i < 30; i++) s += '  ' + CSI + '38;5;' + (16 + i * 7) + 'm' + '● ' + CSI + '39m' + 'log line ' + i + ' ' + OSC + '8;;https://example.com/' + i + ST + 'link' + i + OSC + '8;;' + ST + ' ' + CSI + '4:3m' + CSI + '58:2::255:0:0m' + 'squiggle' + CSI + '4:0m' + CSI + '59m' + ' ' + CSI + '9m' + 'gone' + CSI + '29m' + '\r\n';
  s += sync(prompt(''));
  // a full-screen detour (pager / editor): alt screen, mouse on, draw, leave
  s += CSI + '?1049h' + CSI + '?1000h' + CSI + '?1002h' + CSI + '?1006h' + CSI + '2J' + CSI + 'H';
  for (let i = 0; i < 30; i++) s += cup(i + 1, 1) + CSI + (i % 2 ? '48;5;236' : '49') + 'm' + ' ' + String(i + 1).padStart(3) + ' │ ' + 'alt screen row ' + i + CSI + 'K';
  s += cup(30, 1) + CSI + '7m' + ' -- VIEW -- q to quit ' + CSI + '27m' + CSI + '0m' + CSI + '5;10H' + CSI + '6n';
  s += CSI + '?1006l' + CSI + '?1002l' + CSI + '?1000l' + CSI + '?1049l';
  // paste + final redraw, cursor shown, bracketed paste left on
  s += sync(CSI + '2K' + CSI + '1A' + CSI + '2K' + CSI + '1A' + CSI + '2K' + CSI + '1A' + CSI + '2K' + CSI + 'G' + prompt('[Pasted text #1 +12 lines] ')) + CSI + '?25h' + CSI + '2 q';
  fx('claude-like-session', W, 30, s);
  fx('claude-like-session-chunked', W, 30, s, { chunks: [7, 13, 1, 64, 3, 200, 5, 1, 1, 31] });
  fx('claude-like-session-narrow', 64, 20, s);
  fx('claude-like-session-resized', W, 30, s, { ops: [{ feed: Buffer.byteLength(s, 'utf8') >> 1 }, { resize: [72, 22] }, { feed: 3000 }, { resize: [120, 40] }] });
}

// ------------------------------------------------------------------ seeded fuzz: random mixes of everything
function fuzz(seed, n, cols, rows, opts) {
  let s = seed >>> 0;
  const rnd = () => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s; };
  const pick = a => a[rnd() % a.length];
  const num = () => pick(['', '', '0', '1', '1', '2', '3', '4', '5', '7', '9', '12', String(rows), String(cols), String(rows + 3), String(cols + 5), '99']);
  const texts = ['a', 'hello', 'The quick brown fox ', 'x'.repeat(cols), 'wide中文字', '中', 'e\u0301', 'a\u0308\u0301', '😀', '👨‍👩‍👧', '❤️', '┌─┐', ' ', '\u0301', 'ｆｕｌｌ', 'Z'.repeat(cols * 2 + 3), 'q\u200db', '\u00a0', 'tab\there'];
  // Reflow + combining marks is the one place the engine deliberately does not copy xterm.js (xterm's
  // copyCellsFrom scrambles the combined strings of neighbouring cells), so streams that get resized carry none.
  if (opts && opts.resizes) for (let i = texts.length - 1; i >= 0; i--) if (/[\u0300-\u036f\u200d\ufe0f]/.test(texts[i])) texts.splice(i, 1);
  const resizing = !!(opts && opts.resizes);
  // An alt buffer that was made shorter while inactive keeps a few hidden scrolled-off lines in xterm.js; growing
  // it again leaves xterm's viewport wrapped around its circular list, and from there its line operations go
  // wrong. The engine keeps the picture but not the breakage (terminal.cpp notes), so scratch fuzz runs can
  // leave the alt screen out to keep the resize signal clean.
  const noAlt = !!(opts && opts.noAlt);
  const out = [];
  const sgr = () => pick(['0', '1', '2', '3', '4', '5', '7', '8', '9', '21', '22', '23', '24', '25', '27', '28', '29', '39', '49', '53', '55', '4:3', '4:0', '4:5',
    '3' + (rnd() % 8), '4' + (rnd() % 8), '9' + (rnd() % 8), '10' + (rnd() % 8), '38;5;' + (rnd() % 256), '48;5;' + (rnd() % 256),
    '38;2;' + (rnd() % 256) + ';' + (rnd() % 256) + ';' + (rnd() % 256), '48;2;' + (rnd() % 256) + ';' + (rnd() % 256) + ';' + (rnd() % 256), '38:2::1:2:3', '48:5:77', '58:5:3', '59', '']);
  for (let i = 0; i < n; i++) {
    const k = rnd() % 100;
    if (k < 30) out.push(pick(texts));
    else if (k < 38) out.push(pick(['\r', '\n', '\r\n', '\r\n', '\x08', '\t', '\x0b', '\x0c', '\x07', '\x00', '\x7f']));
    else if (k < 50) out.push(CSI + Array.from({ length: 1 + rnd() % 3 }, sgr).join(';') + 'm');
    else if (k < 68) out.push(CSI + num() + (rnd() % 3 ? '' : ';' + num()) + pick(['A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'H', 'H', 'f', 'd', '`', 'a', 'e', 'I', 'Z']));
    else if (k < 78) {
      // erase / edit. "1J" alone can throw inside xterm (see terminal.cpp notes), so it is always sent from a safe cell
      const f = pick(['J', 'K', 'K', 'X', 'P', '@', 'L', 'M', 'S', 'T', 'b', ' @', ' A', "'}", "'~", '?J', '?K']);
      let p = pick(['', '0', '1', '2', '3', '5', String(cols)]);
      if ((f === 'J' || f === '?J') && p === '1') out.push(cup(1 + rnd() % Math.max(1, rows - 1), resizing ? 1 : 1 + rnd() % Math.max(1, cols - 1)));
      if ((f === 'J' || f === '?J') && p === '3' && rnd() % 4) p = '0';
      out.push(CSI + p + f);
    }
    else if (k < 82) out.push(CSI + pick(['', '', '2;' + (rows - 1), '3;' + Math.max(4, rows - 2), '1;' + rows, num() + ';' + num()]) + 'r');
    else if (k < 88) out.push(CSI + '?' + pick(['1', '6', '7', '25', '45', noAlt ? '7' : '47', noAlt ? '25' : '1047', '1048', noAlt ? '1' : '1049', noAlt ? '6' : '1049', '1000', '1002', '1003', '1004', '1006', '1016', '2004', '2026', '12', '66', '9', '2']) + pick(['h', 'l']));
    else if (k < 90) out.push(CSI + pick(['4', '20']) + pick(['h', 'l', 'l']));
    // DECALN (#8) in origin mode reaches past the end of xterm's line list; after a reflow those slots still
    // alias live lines, so xterm paints E's into unrelated rows. Not copied (terminal.cpp notes), not fuzzed with resizes.
    else if (k < 93) out.push(E + pick(['7', '8', 'D', 'E', 'H', 'M', 'M', '=', '>', '(0', '(B', ')0', ')B', '(A', 'n', 'o', resizing ? 'M' : '#8', '\\']));
    else if (k < 94) out.push(pick(['\x0e', '\x0f']));
    else if (k < 96) out.push(pick([OSC + '2;t' + (rnd() % 100) + BEL, OSC + '0;x' + ST, OSC + '8;;http://e/' + (rnd() % 9) + ST, OSC + '8;;' + ST, OSC + '10;?' + BEL, E + 'P$qr' + ST, E + '_apc' + ST, E + 'Pjunk' + ST]));
    else if (k < 98) out.push(CSI + pick(['6n', '5n', 'c', '>c', '?6n', '?25$p', '?7$p', '4$p', 's', 'u', 's', 'u', '!p', '3g', '0g', '1"q', '0"q', num() + ' q']));
    else if (k < 99) out.push(pick([CSI + '3', CSI + '1;', E, OSC + '2;cut', E + 'P', '\x18', '\x1a', CSI + '?', Buffer.from([0xe4, 0xb8]), Buffer.from([0xf0, 0x9f]), Buffer.from([0xff]), '\xc2\x9b' + '1m', '\xc2\x85']));
    else if (opts && opts.ris && rnd() % 6 === 0) out.push(E + 'c');
    else out.push(lines(3 + rnd() % (rows * 2), j => 'bulk ' + j + ' ' + 'y'.repeat(rnd() % (cols + 10))));
  }
  const bufs = out.map(d => Buffer.isBuffer(d) ? d : Buffer.from(d, 'utf8'));
  const total = bufs.reduce((a, b) => a + b.length, 0);
  const extra = {};
  if (opts && opts.scrollback !== undefined) extra.scrollback = opts.scrollback;
  if (opts && opts.resizes) {
    extra.ops = [];
    let left = total;
    let c = cols, r = rows;
    for (let i = 0; i < opts.resizes; i++) {
      const take = Math.floor(total / (opts.resizes + 1)) + (rnd() % 50);
      extra.ops.push({ feed: Math.min(take, left) });
      left -= Math.min(take, left);
      let nc, nr;
      do { nc = Math.max(2, c + (rnd() % 31) - 15); nr = Math.max(1, r + (rnd() % 9) - 4); } while (nc === c && nr === r);
      c = nc; r = nr;
      extra.ops.push({ resize: [c, r] });
    }
  }
  return [bufs, extra];
}
const FUZZ = [
  [1, 400, 40, 10, {}], [2, 800, 80, 24, {}], [3, 800, 20, 6, {}], [4, 1500, 100, 30, {}], [5, 600, 12, 4, { scrollback: 20 }],
  [6, 1200, 60, 15, { ris: true }], [7, 2500, 80, 24, { scrollback: 50 }], [8, 600, 7, 3, { scrollback: 5 }], [9, 1000, 33, 9, { ris: true, scrollback: 0 }], [10, 3000, 120, 40, {}],
  [11, 800, 40, 10, { resizes: 3 }], [12, 1500, 80, 24, { resizes: 5 }], [13, 1000, 20, 8, { resizes: 6, scrollback: 30 }], [14, 2000, 100, 30, { resizes: 8 }], [15, 1200, 30, 6, { resizes: 10, scrollback: 12 }],
  [16, 900, 50, 12, { resizes: 4, ris: true }], [17, 2500, 64, 20, { resizes: 12, scrollback: 40 }], [18, 700, 9, 5, { resizes: 7, scrollback: 9 }],
];
if (FUZZ_ONLY) {
  // scratch mode: many seeds, sizes and options derived from the seed
  FUZZ.length = 0;
  for (let seed = FUZZ_ONLY[0]; seed <= FUZZ_ONLY[1]; seed++) {
    const h = (seed * 2654435761) >>> 0;
    const o = {};
    if (h % 2) { o.resizes = 1 + (h >>> 3) % 12; o.noAlt = (h >>> 24) % 4 !== 0; }
    if ((h >>> 8) % 3 === 0) o.scrollback = (h >>> 10) % 40;
    if (o.resizes && o.scrollback === 0) o.scrollback = 1;      // scrollback 0 + reflow: xterm's viewport runs off its list (terminal.cpp notes)
    if ((h >>> 16) % 4 === 0) o.ris = true;
    FUZZ.push([seed, 300 + (h >>> 5) % 2500, 2 + (h >>> 12) % 110, 1 + (h >>> 20) % 40, o]);
  }
}
for (const [seed, n, c, r, o] of FUZZ) {
  const [bufs, extra] = fuzz(seed * 7919 + 13, n, c, r, o);
  fx('fuzz-' + String(seed).padStart(2, '0') + (o.resizes ? '-resize' : ''), c, r, bufs, extra);
}

console.log('make-fixtures: wrote ' + count + ' fixtures to ' + FIX);
