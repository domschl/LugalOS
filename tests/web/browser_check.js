// Drives the admin page in a real (headless) Chromium through the DevTools protocol: signs in with
// a key, browses /proc, opens a file, is refused a write on a session that cannot write there, reads the log.
//
//   node browser_check.js CHROMIUM URL USER KEYHEX
'use strict';
const { openBrowser, sleep } = require('./browser_driver');
const [chromium, url, user, key] = process.argv.slice(2);

async function main() {
  let b;
  try {
    b = await openBrowser(chromium);
    const { drv, ev, waitFor, errors: consoleErrors, done } = b;
    await drv.navigate(url);
    await waitFor("document.readyState === 'complete' && !!document.getElementById('go')", 'the sign-in page');
    await waitFor("document.getElementById('board').textContent.length > 10", 'the board line before sign-in');
    const boardLine = await ev("document.getElementById('board').textContent + ' | ' + document.title");
    if (!/uid [0-9a-f]+|uid none/.test(boardLine) || !/LugalOS admin/.test(boardLine)) throw new Error('the board line: ' + boardLine);
    console.log('board: ' + boardLine);
    await ev(`document.getElementById('uname').value = ${JSON.stringify(user)}; document.getElementById('key').value = ${JSON.stringify(key)}; document.getElementById('go').click();`);
    await waitFor("document.getElementById('status').textContent === 'connected'", 'sign-in');
    await waitFor("document.querySelectorAll('#view table a').length > 0", 'the root listing');
    const rootNames = await ev("[...document.querySelectorAll('#view table a')].map(a => a.textContent).join(' ')");
    if (!/proc\//.test(rootNames)) throw new Error('root listing: ' + rootNames);

    await ev("[...document.querySelectorAll('#view table a')].find(a => a.textContent === 'proc/').click()");
    await waitFor("document.querySelector('#view .path') && document.querySelector('#view .path').textContent.trim() === '/proc'", '/proc');
    await ev("[...document.querySelectorAll('#view table a')].find(a => a.textContent === 'version').click()");
    await waitFor("document.getElementById('body') && document.getElementById('body').value.includes('LugalOS')", 'the version file');
    await ev("document.getElementById('save').click()");
    await waitFor("document.getElementById('msg').textContent.length > 1 && document.getElementById('msg').textContent !== '…'", 'the write answer');
    const msg = await ev("document.getElementById('msg').textContent");
    if (msg === 'written') throw new Error('a write to /proc/version succeeded');

    await ev("[...document.querySelectorAll('#tabs button')].find(b => b.textContent === 'Log').click()");
    await waitFor("document.getElementById('log') && document.getElementById('log').textContent.length > 20", 'the log');
    await ev("[...document.querySelectorAll('#tabs button')].find(b => b.textContent === 'Status').click()");
    await waitFor("document.querySelector('#view pre') && document.querySelector('#view pre').textContent.includes('LugalOS')", 'the status view');
    if (consoleErrors.length) throw new Error('script errors: ' + consoleErrors.join('; '));
    console.log('browser: ok (' + drv.name + ')');
    done(0);
  } catch (e) { console.log('browser: FAIL ' + e.message); if (b) b.done(1); else process.exit(1); }
}
main();
