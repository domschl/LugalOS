// Drives the admin page in a real (headless) Chromium through the DevTools protocol: signs in with
// a key, browses /proc, opens a file, is refused a write on a read-only session, reads the log.
//
//   node browser_check.js CHROMIUM URL USER KEYHEX
'use strict';
const { spawn } = require('child_process');
const os = require('os'), fs = require('fs'), path = require('path');
const [chromium, url, user, key] = process.argv.slice(2);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function main() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lugal-chrome-'));
  const port = 20000 + Math.floor(Math.random() * 20000);
  const proc = spawn(chromium, ['--headless=new', '--no-sandbox', '--disable-gpu', `--user-data-dir=${dir}`,
                                `--remote-debugging-port=${port}`, 'about:blank'], { stdio: 'ignore' });
  let ws, id = 0; const waiting = new Map(); const consoleErrors = [];
  const done = (code) => { try { ws && ws.close(); } catch (e) { /* closing */ } proc.kill(); fs.rmSync(dir, { recursive: true, force: true }); process.exit(code); };
  try {
    let target;
    for (let i = 0; i < 60 && !target; i++) {
      try { target = (await (await fetch(`http://127.0.0.1:${port}/json`)).json()).find((t) => t.type === 'page'); } catch (e) { await sleep(250); }
    }
    if (!target) throw new Error('the browser did not start');
    ws = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error('devtools')); });
    ws.onmessage = (ev) => {
      const m = JSON.parse(ev.data);
      if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); }
      else if (m.method === 'Runtime.exceptionThrown') consoleErrors.push(m.params.exceptionDetails.text + ' ' + JSON.stringify(m.params.exceptionDetails.exception && m.params.exceptionDetails.exception.description));
    };
    const send = (method, params = {}) => new Promise((res) => { const i = ++id; waiting.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
    const ev = async (expr) => { const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true }); if (r.result.exceptionDetails) throw new Error(r.result.exceptionDetails.text + ' in ' + expr); return r.result.result.value; };
    const waitFor = async (expr, what, ms = 15000) => { const t = Date.now(); for (;;) { if (await ev(expr)) return; if (Date.now() - t > ms) throw new Error('timed out waiting for ' + what + ' [page errors: ' + consoleErrors.join('; ') + ']' + '; status=' + await ev("document.getElementById('status').textContent + ' / ' + document.getElementById('lerr').textContent + ' / go.disabled=' + document.getElementById('go').disabled + ' / key.len=' + document.getElementById('key').value.length + ' / login hidden=' + document.getElementById('login').classList.contains('hide')")); await sleep(150); } };

    await send('Runtime.enable'); await send('Page.enable');
    await send('Page.navigate', { url });
    await waitFor("document.readyState === 'complete' && !!document.getElementById('go')", 'the sign-in page');
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
    if (!/read-only/.test(msg)) throw new Error('the write was not refused as read-only: ' + msg);

    await ev("[...document.querySelectorAll('#tabs button')].find(b => b.textContent === 'Log').click()");
    await waitFor("document.getElementById('log') && document.getElementById('log').textContent.length > 20", 'the log');
    await ev("[...document.querySelectorAll('#tabs button')].find(b => b.textContent === 'Status').click()");
    await waitFor("document.querySelector('#view pre') && document.querySelector('#view pre').textContent.includes('LugalOS')", 'the status view');
    if (consoleErrors.length) throw new Error('script errors: ' + consoleErrors.join('; '));
    console.log('browser: ok');
    done(0);
  } catch (e) { console.log('browser: FAIL ' + e.message); done(1); }
}
main();
