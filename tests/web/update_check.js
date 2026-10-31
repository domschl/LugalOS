// Updates a node from the admin page in a real (headless) browser: signs in, opens Update, puts an image
// into the file input as a person's choice would, sends it, and restarts the node into it.
//
//   node update_check.js BROWSER URL USER KEYHEX IMAGE [norestart]
//
// Prints "update: committed" or "update: refused: WHY", then "update: restarting". A refused image (a
// tampered or unsigned one) is a result too, for the caller to judge.
'use strict';
const fs = require('fs');
const { openBrowser } = require('./browser_driver');
const [browser, url, user, key, image, norestart] = process.argv.slice(2);

async function main() {
  let b;
  try {
    b = await openBrowser(browser);
    const { drv, ev, waitFor, done } = b;
    await drv.navigate(url);
    await waitFor("document.readyState === 'complete' && !!document.getElementById('go')", 'the sign-in page');
    await ev(`document.getElementById('uname').value = ${JSON.stringify(user)}; document.getElementById('key').value = ${JSON.stringify(key)}; document.getElementById('go').click();`);
    await waitFor("document.getElementById('status').textContent === 'connected'", 'sign-in');
    await ev("[...document.querySelectorAll('#tabs button')].find(b => b.dataset.t === 'update').click()");
    await waitFor("!!document.getElementById('img') && document.getElementById('ota').textContent.includes('state=')", 'the update view');
    const b64 = fs.readFileSync(image).toString('base64');
    await ev(`(() => {
      const bin = atob(${JSON.stringify(b64)}), a = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) a[i] = bin.charCodeAt(i);
      const dt = new DataTransfer(); dt.items.add(new File([a], 'lugalos-ota.img'));
      const inp = document.getElementById('img'); inp.files = dt.files; inp.dispatchEvent(new Event('change'));
      document.getElementById('send').click(); return true; })()`);
    const t0 = Date.now();
    await waitFor("/^(committed|refused)/.test(document.getElementById('omsg').textContent)", 'the node to commit or refuse', 600000);
    const msg = await ev("document.getElementById('omsg').textContent");
    console.log(`update: ${msg} (${((Date.now() - t0) / 1000).toFixed(1)} s)`);
    if (msg.startsWith('committed') && !norestart) {
      await ev("document.getElementById('restart').click()");
      await waitFor("document.getElementById('omsg').textContent.startsWith('restarting')", 'the restart');
      console.log('update: restarting');
    }
    done(0);
  } catch (e) {
    console.log('update: FAIL ' + e.message);
    if (b) b.done(1); else process.exit(1);
  }
}
main();
