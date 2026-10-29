// Runs the setup page's own code (the <script id="core"> of tools/sd_root/www/setup.html, extracted by
// tests/runner.py) under node: SHA-1, HMAC-SHA1 and PBKDF2 against published vectors -- including the IEEE
// 802.11i example that fixes how a Wi-Fi passphrase becomes a key -- and, given a node, a real claim.
//
//   node setup_core_check.js CORE.js [HOST PORT SECRET SSID PASSPHRASE [NAME]]
'use strict';
const path = require('path');
const core = require(path.resolve(process.argv[2]));
const { sha1, hmacSha1, pbkdf2Sha1, wpaPsk, toHex, claimLine, SetupLink } = core;
const enc = new TextEncoder();
let bad = 0;
const check = (n, got, want) => { if (got !== want) { bad++; console.log(`FAIL ${n}: ${got} != ${want}`); } };

check('sha1 abc', toHex(sha1(enc.encode('abc'))), 'a9993e364706816aba3e25717850c26c9cd0d89d');
check('sha1 empty', toHex(sha1(enc.encode(''))), 'da39a3ee5e6b4b0d3255bfef95601890afd80709');
check('sha1 448 bits', toHex(sha1(enc.encode('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq'))), '84983e441c3bd26ebaae4aa1f95129e5e54670f1');
const crypto = require('crypto');
for (const n of [0, 1, 55, 56, 57, 63, 64, 65, 119, 120, 1000, 70001]) {
  const b = Buffer.alloc(n, 0x41 + (n % 9));
  check(`sha1 ${n} bytes`, toHex(sha1(b)), crypto.createHash('sha1').update(b).digest('hex'));
}
// RFC 2202 HMAC-SHA1 test cases 1 and 6 (a key longer than the block).
check('hmac-sha1 #1', toHex(hmacSha1(Buffer.alloc(20, 0x0b), enc.encode('Hi There'))), 'b617318655057264e28bc0b6fb378c8ef146be00');
check('hmac-sha1 #6', toHex(hmacSha1(Buffer.alloc(80, 0xaa), enc.encode('Test Using Larger Than Block-Size Key - Hash Key First'))), 'aa4ae5e15272d00e95705637ce8a3b55ed402112');
// RFC 6070 PBKDF2-HMAC-SHA1.
check('pbkdf2 c=1', toHex(pbkdf2Sha1(enc.encode('password'), enc.encode('salt'), 1, 20)), '0c60c80f961f0e71f3a9b524af6012062fe037a6');
check('pbkdf2 c=2', toHex(pbkdf2Sha1(enc.encode('password'), enc.encode('salt'), 2, 20)), 'ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957');
check('pbkdf2 c=4096', toHex(pbkdf2Sha1(enc.encode('password'), enc.encode('salt'), 4096, 20)), '4b007901b765489abead49d926f721d065a429c1');
check('pbkdf2 long', toHex(pbkdf2Sha1(enc.encode('passwordPASSWORDpassword'), enc.encode('saltSALTsaltSALTsaltSALTsaltSALTsalt'), 4096, 25)),
      '3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038');
// IEEE 802.11i-2004 annex H.4: the PSK of passphrase "password" on SSID "IEEE" (the same value the kernel checks).
const t0 = Date.now();
check('wpa psk', toHex(wpaPsk('password', 'IEEE')), 'f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e');
check('wpa psk, ThisIsASSID', toHex(wpaPsk('ThisIsAPassword', 'ThisIsASSID')), '0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af');
const took = Date.now() - t0;
check('claim line', claimLine('AAAA-BBBB-CCCC', '00ff', 'node-1', 'My Wi-Fi', 'ab'), 'claim AAAA-BBBB-CCCC 00ff name node-1 wlanhex 4d792057692d4669 ab');
check('claim line, no network', claimLine('AAAA-BBBB-CCCC', '00ff', '', '', ''), 'claim AAAA-BBBB-CCCC 00ff');
console.log(bad ? 'setup core: FAILED' : `setup core: ok (two PBKDF2 key derivations: ${took} ms)`);

async function live() {
  if (process.argv.length < 8) return;
  const [host, port, secret, ssid, pass, name] = process.argv.slice(3);
  const key = crypto.randomBytes(32).toString('hex');
  const link = new SetupLink(`ws://${host}:${port}/9p`);
  await link.connect(); await link.open();
  const st = await link.read();
  console.log('state before: ' + st.state);
  await link.write(claimLine(secret, key, name || '', ssid, ssid ? toHex(wpaPsk(pass, ssid)) : ''));
  let res = '';
  for (let i = 0; i < 40; i++) {
    await new Promise((r) => setTimeout(r, 300));
    res = (await link.read()).result || '';
    if (res && res !== 'pending') break;
  }
  console.log('result: ' + res);
  console.log('key: ' + key);
  link.close();
}
live().then(() => process.exit(bad ? 1 : 0), (e) => { console.log('live: FAIL ' + e.message); process.exit(1); });
