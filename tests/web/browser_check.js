// Drives the admin page in a real (headless) Chromium through the DevTools protocol: signs in with
// a key, browses /proc, opens a file, is refused a write on a session that cannot write there, reads the log.
//
//   node browser_check.js CHROMIUM URL USER KEYHEX
'use strict';
const { spawn } = require('child_process');
const os = require('os'), fs = require('fs'), path = require('path');
const [chromium, url, user, key] = process.argv.slice(2);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* A browser behind a tiny driver: start(), navigate(url), ev(expression) -> value, stop().
 * Chromium speaks the DevTools protocol; Firefox speaks WebDriver BiDi. Both over a WebSocket. */
async function connectRetry(url, tries = 80) {
  for (let i = 0; i < tries; i++) {
    try {
      const ws = new WebSocket(url);
      await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error('no')); });
      return ws;
    } catch (e) { await sleep(250); }
  }
  throw new Error('the browser did not start');
}

async function startChromium(dir, port, errors) {
  const proc = spawn(chromium, ['--headless=new', '--no-sandbox', '--disable-gpu', `--user-data-dir=${dir}`,
                                `--remote-debugging-port=${port}`, 'about:blank'], { stdio: 'ignore' });
  let target;
  for (let i = 0; i < 60 && !target; i++) {
    try { target = (await (await fetch(`http://127.0.0.1:${port}/json`)).json()).find((t) => t.type === 'page'); } catch (e) { await sleep(250); }
  }
  if (!target) throw new Error('the browser did not start');
  const ws = await connectRetry(target.webSocketDebuggerUrl, 4);
  let id = 0; const waiting = new Map();
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); }
    else if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.text + ' ' + JSON.stringify(m.params.exceptionDetails.exception && m.params.exceptionDetails.exception.description));
  };
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; waiting.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  await send('Runtime.enable'); await send('Page.enable');
  return {
    proc, name: 'chromium',
    navigate: (url) => send('Page.navigate', { url }),
    ev: async (expr) => { const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true }); if (r.result.exceptionDetails) throw new Error(r.result.exceptionDetails.text + ' in ' + expr); return r.result.result.value; },
    stop: () => { try { ws.close(); } catch (e) { /* closing */ } },
  };
}

async function startFirefox(dir, port) {
  fs.writeFileSync(path.join(dir, 'user.js'), 'user_pref("remote.active-protocols", 1);\nuser_pref("browser.shell.checkDefaultBrowser", false);\nuser_pref("datareporting.policy.dataSubmissionEnabled", false);\n');
  const proc = spawn(chromium, ['--headless', '--no-remote', '--profile', dir, `--remote-debugging-port=${port}`, 'about:blank'], { stdio: 'ignore' });
  const ws = await connectRetry(`ws://127.0.0.1:${port}/session`);
  let id = 0; const waiting = new Map();
  ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); } };
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; waiting.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  const ok = (r) => { if (r.type === 'error') throw new Error(r.error + ': ' + r.message); return r.result; };
  ok(await send('session.new', { capabilities: {} }));
  const context = ok(await send('browsingContext.getTree', {})).contexts[0].context;
  return {
    proc, name: 'firefox',
    navigate: async (url) => ok(await send('browsingContext.navigate', { context, url, wait: 'none' })),
    ev: async (expr) => {
      const r = ok(await send('script.evaluate', { expression: expr, target: { context }, awaitPromise: true, resultOwnership: 'none' }));
      if (r.type === 'exception') throw new Error(r.exceptionDetails.text + ' in ' + expr);
      return r.result.value;
    },
    stop: () => { try { ws.close(); } catch (e) { /* closing */ } },
  };
}

async function main() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lugal-browser-'));
  const port = 20000 + Math.floor(Math.random() * 20000);
  const consoleErrors = [];
  let drv;
  const done = (code) => { try { drv && drv.stop(); drv && drv.proc.kill(); } catch (e) { /* gone */ } fs.rmSync(dir, { recursive: true, force: true }); process.exit(code); };
  try {
    drv = /firefox/i.test(chromium) ? await startFirefox(dir, port) : await startChromium(dir, port, consoleErrors);
    const ev = drv.ev;
    const waitFor = async (expr, what, ms = 15000) => { const t = Date.now(); for (;;) { if (await ev(expr)) return; if (Date.now() - t > ms) throw new Error('timed out waiting for ' + what + ' [' + drv.name + ' errors: ' + consoleErrors.join('; ') + '] status=' + await ev("document.getElementById('status').textContent + ' / ' + document.getElementById('lerr').textContent")); await sleep(150); } };

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
  } catch (e) { console.log('browser: FAIL ' + e.message); done(1); }
}
main();
