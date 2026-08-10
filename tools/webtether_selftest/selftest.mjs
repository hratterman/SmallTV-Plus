// Checks the framing inside docs/public/tether.html against the firmware's own.
//
// Three implementations of one wire format now exist — C++, Python and this
// JavaScript — and the way that fails is one of them quietly disagreeing about
// byte order or the CRC. None of their own tests would catch it, so the frames
// are passed between them.
import { readFileSync, writeFileSync, mkdtempSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');

// Pull the script out of the page and evaluate it, so what is tested is the
// code that ships rather than a copy of it.
const html = readFileSync(join(root, 'docs/public/tether.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
// The page's tail touches the DOM; keep everything up to the UI wiring.
const pure = script.slice(0, script.indexOf('const $=id=>document.getElementById'));
const mod = await import('data:text/javascript,' + encodeURIComponent(
  pure.replace(/^'use strict';/, '') +
  '\nexport {crc16, encode, Decoder, parseRequest, HELLO, HTTP_DATA, HTTP_END, HTTP_REQ};'));

let failures = 0;
const ck = (cond, what) => {
  console.log(`  ${cond ? 'ok   ' : 'FAIL '} ${what}`);
  if (!cond) failures++;
};
const drain = (dec, bytes) => [...dec.feed(bytes)];

console.log('--- javascript round trip -----------------------------------');
for (const [name, payload] of [
  ['empty', new Uint8Array(0)],
  ['text', new TextEncoder().encode('GET https://api.spotify.com/v1/me')],
  ['all delimiters', new Uint8Array(40).fill(0xC0)],
  ['all escapes', new Uint8Array(40).fill(0xDB)],
  ['binary ramp', Uint8Array.from({length: 512}, (_, i) => i & 0xFF)],
  ['max payload', new Uint8Array(1024).fill(0x41)],
]) {
  const wire = mod.encode(mod.HTTP_DATA, 0x2A, payload);
  const got = drain(new mod.Decoder(), wire);
  ck(got.length === 1 && got[0].id === 0x2A &&
     Buffer.compare(Buffer.from(got[0].payload), Buffer.from(payload)) === 0,
     `${name} survives`);
}

console.log('\n--- log text is not mistaken for a frame --------------------');
{
  const lines = [];
  const dec = new mod.Decoder(s => lines.push(s));
  const frames = drain(dec, new TextEncoder().encode('[boot] settings\n[boot] display\n'));
  ck(frames.length === 0, 'plain log yields no frames');
  ck(lines.join('|') === '[boot] settings|[boot] display', 'and is surfaced as text');
}
{
  const lines = [];
  const dec = new mod.Decoder(s => lines.push(s));
  const mid = mod.encode(mod.HELLO, 3, new TextEncoder().encode('smalltv-mod'));
  const pre = new TextEncoder().encode('[net] up\n');
  const post = new TextEncoder().encode('[net] rssi -50\n');
  const all = new Uint8Array([...pre, ...mid, ...post]);
  const frames = drain(dec, all);
  ck(frames.length === 1 && frames[0].type === mod.HELLO, 'a frame between log lines is found');
}
{
  const wire = mod.encode(mod.HTTP_DATA, 5, new TextEncoder().encode('abcdefgh'));
  wire[6] ^= 0x01;
  ck(drain(new mod.Decoder(), wire).length === 0, 'a flipped bit is rejected');
}

console.log('\n--- against the C++ implementation --------------------------');
{
  const tmp = mkdtempSync(join(tmpdir(), 'webtether-'));
  const emit = join(tmp, 'emit'), decode = join(tmp, 'decode');
  execFileSync('g++', ['-O1', '-std=c++17', '-o', emit,
                       join(root, 'tools/serialframe_selftest/emit.cpp')]);
  execFileSync('g++', ['-O1', '-std=c++17', '-o', decode,
                       join(root, 'tools/serialframe_selftest/decode.cpp')]);

  const produced = execFileSync(emit, {maxBuffer: 1 << 20});
  const got = drain(new mod.Decoder(), new Uint8Array(produced));
  const want = [
    [mod.HELLO, 1, 11],
    [mod.HTTP_DATA, 0x1234, 256],
    [mod.HTTP_END, 0xBEEF, 0],
  ];
  ck(got.length === want.length &&
     got.every((f, i) => f.type === want[i][0] && f.id === want[i][1] &&
                         f.payload.length === want[i][2]),
     'javascript decodes frames the C++ encoder produced');

  const mine = Buffer.concat(want.map(([t, i, n]) =>
    Buffer.from(mod.encode(t, i, Uint8Array.from({length: n}, (_, k) => k & 0xFF)))));
  const out = execFileSync(decode, {input: mine}).toString();
  const expect = want.map(([t, i, n]) => `${t} ${i} ${n}`).join('\n') + '\n';
  ck(out === expect, 'C++ decodes frames javascript encoded');
}

console.log('\n--- the request unpacking matches what the firmware packs ---');
{
  // Byte-for-byte what Tether.cpp's packRequest() emits.
  const url = 'https://api.spotify.com/v1/me/player/currently-playing';
  const hdrs = 'Authorization: Bearer abc.def\nAccept: application/json';
  const body = new TextEncoder().encode('grant_type=refresh_token');
  const u = new TextEncoder().encode(url), h = new TextEncoder().encode(hdrs);
  const buf = new Uint8Array(1 + 2 + u.length + 2 + h.length + 2 + body.length);
  const dv = new DataView(buf.buffer);
  let o = 0;
  buf[o++] = 1;                                   // POST
  dv.setUint16(o, u.length, true); o += 2; buf.set(u, o); o += u.length;
  dv.setUint16(o, h.length, true); o += 2; buf.set(h, o); o += h.length;
  dv.setUint16(o, body.length, true); o += 2; buf.set(body, o);

  const r = mod.parseRequest(buf);
  ck(r.method === 1, 'method');
  ck(r.url === url, 'url');
  ck(r.headers['Authorization'] === 'Bearer abc.def', 'authorization header');
  ck(r.headers['Accept'] === 'application/json', 'second header');
  ck(new TextDecoder().decode(r.body) === 'grant_type=refresh_token', 'body');
}

console.log('\n--- the page itself -----------------------------------------');
{
  // The checks above only exercise the framing half. These cover the whole
  // file, because a syntax error anywhere in the script block stops all of it
  // running while the page still renders — the same failure that once blanked
  // every tab of the device's own web UI.
  const tmp = mkdtempSync(join(tmpdir(), 'webtether-page-'));
  const jsPath = join(tmp, 'page.js');
  writeFileSync(jsPath, script);
  let parsed = true;
  try { execFileSync('node', ['--check', jsPath]); } catch (e) { parsed = false; console.log(String(e.stderr || e)); }
  ck(parsed, 'the whole script parses');

  const ids = new Set([...html.matchAll(/\bid="([^"]+)"/g)].map(m => m[1]));
  const used = new Set([...script.matchAll(/\$\(['"]([A-Za-z_$][\w$-]*)['"]\)/g)].map(m => m[1]));
  const missing = [...used].filter(i => !ids.has(i));
  ck(missing.length === 0,
     missing.length ? `script names ids the page lacks: ${missing.join(', ')}`
                    : 'every element id the script names exists');

  // The settings editor and the firmware have to agree on these numbers, and
  // they are written as bare hex in the page's dispatch.
  for (const [name, val] of [['SF_CFG_GET', 0x40], ['SF_CFG_DATA', 0x41],
                             ['SF_CFG_END', 0x42], ['SF_CFG_SET', 0x43],
                             ['SF_CFG_APPLY', 0x44], ['SF_CFG_OK', 0x45]]) {
    const hdr = readFileSync(join(root, 'src/SerialFrame.h'), 'utf8');
    const m = hdr.match(new RegExp(name + '\\s*=\\s*(0x[0-9a-fA-F]+)'));
    ck(m && parseInt(m[1], 16) === val, `${name} matches the firmware (0x${val.toString(16)})`);
  }
}

console.log('\n-------------------------------------------------------------');
if (failures) { console.log(`${failures} check(s) FAILED`); process.exit(1); }
console.log('all checks passed');
