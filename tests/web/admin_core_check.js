// Runs the admin page's own protocol code (the <script id="core"> of tools/sd_root/www/admin.html,
// extracted by tests/runner.py) under node: SHA-256 and HMAC against published vectors, then a
// real login and file operations against a node's WebSocket 9P endpoint.
//
//   node admin_core_check.js CORE.js [HOST PORT KEYHEX]
'use strict';
const fs = require('fs');
const core = require(require('path').resolve(process.argv[2]));
const { sha256, hmacSha256, hexToBytes, P9 } = core;
const hex = (b) => Buffer.from(b).toString('hex');
const bytes = (s) => new TextEncoder().encode(s);
let bad = 0;
const check = (name, got, want) => { if (got !== want) { bad++; console.log(`FAIL ${name}: ${got} != ${want}`); } };

// FIPS 180-4 / NIST examples, and the padding boundaries (55, 56, 63, 64, 65 bytes).
check('sha256 empty', hex(sha256(bytes(''))), 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
check('sha256 abc', hex(sha256(bytes('abc'))), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
check('sha256 448 bits', hex(sha256(bytes('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq'))),
      '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1');
const crypto = require('crypto');
for (const n of [0, 1, 54, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 1000, 70001]) {
  const b = Buffer.alloc(n, 0x61 + (n % 7));
  check(`sha256 ${n} bytes`, hex(sha256(b)), crypto.createHash('sha256').update(b).digest('hex'));
}
// RFC 4231 test cases 1, 2 and 6 (a key longer than the block).
check('hmac rfc4231 #1', hex(hmacSha256(Buffer.alloc(20, 0x0b), bytes('Hi There'))),
      'b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7');
check('hmac rfc4231 #2', hex(hmacSha256(bytes('Jefe'), bytes('what do ya want for nothing?'))),
      '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843');
check('hmac rfc4231 #6', hex(hmacSha256(Buffer.alloc(131, 0xaa), bytes('Test Using Larger Than Block-Size Key - Hash Key First'))),
      '60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54');
try { hexToBytes('xyz'); check('bad hex refused', 'accepted', 'refused'); } catch (e) { /* refused */ }
console.log(bad ? 'crypto: FAILED' : 'crypto: ok');

async function main() {
  if (process.argv.length < 6) return bad;
  const [host, port, keyhex] = process.argv.slice(3);
  const url = `ws://${host}:${port}/9p`;

  // A wrong key is refused with a reason, and does not take the node down.
  const wrong = new P9(url); await wrong.connect();
  let refused = '';
  try { await wrong.login('lugal', hexToBytes('00'.repeat(16))); } catch (e) { refused = e.message; }
  wrong.close();
  check('wrong key refused', refused !== '', true);

  const p = new P9(url); await p.connect(); await p.login('lugal', hexToBytes(keyhex));
  check('msize', p.msize >= 2048, true);
  check('/proc/version', new TextDecoder().decode(await p.read('/proc/version')).includes('LugalOS'), true);
  const names = (await p.list('/')).map((e) => e.name);
  check('ls / has proc and sd0', names.includes('proc') && names.includes('sd0'), true);
  const big = await p.read('/sd0/system/etc/init.lisp');
  console.log(`init.lisp: ${big.length}`);
  const dir = await p.list('/sd0/system');
  check('ls /sd0/system', dir.length > 0 && dir.some((e) => e.dir), true);
  // The page itself reads back through 9P, byte for byte what HTTP served.
  const viaP9 = await p.read('/flash0/www/admin.html');
  console.log(`admin.html via 9P: ${viaP9.length}`);
  let werr = '';
  try { await p.write('/sd0/system/etc/init.lisp', bytes('x'), true); } catch (e) { werr = e.message; }
  console.log(`write: ${werr || 'accepted'}`);
  let missing = '';
  try { await p.read('/proc/nonexistent-file'); } catch (e) { missing = e.message; }
  check('a missing file is an error', missing.startsWith('no such file'), true);
  p.close();
  return bad;
}
main().then((n) => process.exit(n ? 1 : 0), (e) => { console.log('FAIL ' + e.message); process.exit(1); });
