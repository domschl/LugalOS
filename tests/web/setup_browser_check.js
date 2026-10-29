// Drives the setup page in a real (headless) browser, as a person with a phone would: fills in the setup secret
// and a Wi-Fi network, claims, and reports the owner key the page shows.
//
//   node setup_browser_check.js BROWSER URL SECRET SSID PASSPHRASE [NAME]
'use strict';
const { openBrowser } = require('./browser_driver');
const [browser, url, secret, ssid, pass, name] = process.argv.slice(2);

(async () => {
  let b;
  try {
    b = await openBrowser(browser);
    const { drv, ev, waitFor } = b;
    await drv.navigate(url);
    await waitFor("document.readyState === 'complete' && !!document.getElementById('secret')", 'the setup page');
    const title = await ev('document.title');
    await waitFor("document.getElementById('board').textContent.length > 10", 'the board line');
    const boardLine = await ev("document.getElementById('board').textContent");
    await ev(`document.getElementById('secret').value = ${JSON.stringify(secret)};
              document.getElementById('ssid').value = ${JSON.stringify(ssid)};
              document.getElementById('pass').value = ${JSON.stringify(pass)};
              document.getElementById('name').value = ${JSON.stringify(name || '')};
              document.getElementById('go').click();`);
    await waitFor("!document.getElementById('done').classList.contains('hide') || /^The node said|cannot|refus|did not/i.test(document.getElementById('msg').textContent)", 'the claim to finish', 60000);
    const shown = await ev("!document.getElementById('done').classList.contains('hide')");
    if (!shown) throw new Error('claim failed: ' + await ev("document.getElementById('msg').textContent"));
    const key = await ev("document.getElementById('key').textContent");
    const after = await ev("document.getElementById('after').textContent");
    if (!/^[0-9a-f]{64}$/.test(key)) throw new Error('the page showed no key: ' + key);
    console.log('title: ' + title);
    console.log('board: ' + boardLine);
    console.log('after: ' + after);
    console.log('key: ' + key);
    console.log('setup page: ok');
    b.done(0);
  } catch (e) { console.log('setup page: FAIL ' + e.message); if (b) b.done(1); else process.exit(1); }
})();
