// A browser behind a tiny driver, for tests/web/*_check.js: start(), navigate(url), ev(expression) -> value, stop().
// Chromium speaks the DevTools protocol; Firefox speaks WebDriver BiDi. Both over a WebSocket.
'use strict';
const { spawn } = require('child_process');
const os = require('os'), fs = require('fs'), path = require('path');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

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

async function startChromium(chromium, dir, port, errors) {
  // LUGAL_BROWSER_SPKI: the base64 SHA-256 of a test node's public key, so HTTPS to it is trusted without a CA store.
  const extra = process.env.LUGAL_BROWSER_SPKI ? [`--ignore-certificate-errors-spki-list=${process.env.LUGAL_BROWSER_SPKI}`] : [];
  const proc = spawn(chromium, ['--headless=new', '--no-sandbox', '--disable-gpu', `--user-data-dir=${dir}`, ...extra,
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

async function startFirefox(chromium, dir, port) {
  fs.writeFileSync(path.join(dir, 'user.js'), 'user_pref("remote.active-protocols", 1);\nuser_pref("browser.shell.checkDefaultBrowser", false);\nuser_pref("datareporting.policy.dataSubmissionEnabled", false);\n');
  // LUGAL_BROWSER_CA: a test CA's PEM, trusted in this throwaway profile (needs NSS's certutil).
  if (process.env.LUGAL_BROWSER_CA) {
    const { execFileSync } = require('child_process');
    execFileSync('certutil', ['-N', '-d', `sql:${dir}`, '--empty-password']);
    execFileSync('certutil', ['-A', '-d', `sql:${dir}`, '-t', 'C,,', '-n', 'lugalos-test-ca', '-i', process.env.LUGAL_BROWSER_CA]);
  }
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


/* Opens `browserPath` (Chromium or Firefox, headless) and returns { drv, ev, waitFor, done(code) }. */
async function openBrowser(browserPath, label) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lugal-browser-'));
  const port = 20000 + Math.floor(Math.random() * 20000);
  const errors = [];
  const drv = /firefox/i.test(browserPath) ? await startFirefox(browserPath, dir, port) : await startChromium(browserPath, dir, port, errors);
  const ev = drv.ev;
  const waitFor = async (expr, what, ms = 15000) => {
    const t = Date.now();
    for (;;) {
      if (await ev(expr)) return;
      if (Date.now() - t > ms) throw new Error('timed out waiting for ' + what + ' [' + drv.name + ' errors: ' + errors.join('; ') + ']');
      await sleep(150);
    }
  };
  const done = (code) => { try { drv.stop(); drv.proc.kill(); } catch (e) { /* gone */ } fs.rmSync(dir, { recursive: true, force: true }); process.exit(code); };
  return { drv, ev, waitFor, errors, done };
}
module.exports = { openBrowser, sleep };
