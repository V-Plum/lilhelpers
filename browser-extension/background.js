// Little Helpers — лог DevTools під час запису екрана (CAPS-83).
//
// Розширення тримає з'єднання з Little Helpers через локальний WebSocket
// (127.0.0.1, лише loopback). Little Helpers шле {"cmd":"start"} на початку
// запису й {"cmd":"stop"} наприкінці; між ними розширення під'єднується
// відладчиком (chrome.debugger) до АКТИВНОЇ вкладки, стежить за перемиканням
// вкладок і шле події одна за одною.
//
// Час кожної події — мілісекунди від епохи за СИСТЕМНИМ годинником (те саме,
// що Date.now() і GetSystemTimePreciseAsFileTime у Little Helpers): консоль,
// винятки й Log несуть його самі, мережа — через wallTime запиту, навігації —
// час отримання. Тож синхрон із відео не потребує жодних пінгів.
//
// Що НЕ пишеться ніколи: заголовки запитів і відповідей (Authorization, Cookie
// тощо), тіла запитів і відповідей. Адреси пишуться повністю — маскування
// секретів у query робить Little Helpers при експорті звіту.

const PORT = 47650;
const MAX_TEXT = 4000;

let ws = null;
let recording = false;
let current = null;                 // tabId, до якого під'єднані зараз
const attached = new Set();
const reqs = new Map();             // `${tabId}:${requestId}` → запит у дорозі
const clockOffset = new Map();      // tabId → wallMs − monotonic·1000 (для мережі)
let state = { connected: false, recording: false, tab: null };

const browserName = /Edg\//.test(navigator.userAgent) ? "Edge" : "Chrome";

function send(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    try { ws.send(JSON.stringify(obj)); } catch (e) { /* з'єднання впало — onclose прибере */ }
  }
}

function emit(ev) {
  if (!recording) return;
  ev.b = browserName;
  send(ev);
}

function clip(s) {
  s = String(s == null ? "" : s);
  return s.length > MAX_TEXT ? s.slice(0, MAX_TEXT) + "…" : s;
}

function shortUrl(u) {
  u = String(u || "");
  if (u.startsWith("data:")) return u.slice(0, 64) + "…";
  return u.length > 2000 ? u.slice(0, 2000) + "…" : u;
}

// ---- з'єднання з Little Helpers ----

function connect() {
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;
  try {
    ws = new WebSocket(`ws://127.0.0.1:${PORT}/lilhelpers`);
  } catch (e) {
    ws = null;
    return;
  }
  ws.onopen = () => {
    state.connected = true;
    send({ hello: 1, browser: browserName, ua: navigator.userAgent, ext: chrome.runtime.getManifest().version });
  };
  ws.onmessage = (e) => {
    let m;
    try { m = JSON.parse(e.data); } catch (err) { return; }
    if (m.cmd === "start") startRecording();
    else if (m.cmd === "stop") stopRecording();
  };
  ws.onclose = () => {
    ws = null;
    state.connected = false;
    if (recording) stopRecording();
  };
  ws.onerror = () => { /* далі onclose */ };
}

// Поки з'єднання живе, повідомлення кожні 20 с не дають браузеру приспати
// service worker (Chrome 116+). Нема з'єднання — будильник раз на 30 с пробує знову.
setInterval(() => {
  if (ws && ws.readyState === WebSocket.OPEN) send({ ping: 1 });
  else connect();
}, 20000);
chrome.alarms.create("lilhelpers-connect", { periodInMinutes: 0.5 });
chrome.alarms.onAlarm.addListener(() => connect());
chrome.runtime.onStartup.addListener(() => connect());
chrome.runtime.onInstalled.addListener(() => connect());
connect();

// ---- запис ----

async function activeTab() {
  const [tab] = await chrome.tabs.query({ active: true, lastFocusedWindow: true });
  return tab || null;
}

async function follow(tabId) {
  if (!recording || tabId == null || tabId === current) return;
  // Лог має відповідати екрану: пишемо лише активну вкладку — стару відпускаємо.
  if (current != null) await detach(current);
  current = tabId;
  state.tab = tabId;
  let tab = null;
  try { tab = await chrome.tabs.get(tabId); } catch (e) { /* закрили */ }
  if (tab && tab.url && /^(chrome|edge|about|chrome-extension|devtools):/i.test(tab.url)) {
    emit({ t: Date.now(), k: "tab", s: 0, url: tab.url, title: clip(tab.title), note: "not-debuggable" });
    return;
  }
  try {
    await chrome.debugger.attach({ tabId }, "1.3");
    attached.add(tabId);
    for (const d of ["Runtime.enable", "Log.enable", "Network.enable", "Page.enable"])
      await chrome.debugger.sendCommand({ tabId }, d, d === "Network.enable" ? { maxPostDataSize: 0 } : {});
    emit({ t: Date.now(), k: "tab", s: 0, url: tab ? tab.url : "", title: tab ? clip(tab.title) : "" });
  } catch (e) {
    emit({ t: Date.now(), k: "info", s: 1, text: "attach failed: " + clip(e && e.message) });
  }
}

async function detach(tabId) {
  if (!attached.has(tabId)) return;
  attached.delete(tabId);
  for (const key of [...reqs.keys()]) if (key.startsWith(tabId + ":")) reqs.delete(key);
  clockOffset.delete(tabId);
  try { await chrome.debugger.detach({ tabId }); } catch (e) { /* уже відпущено */ }
}

async function startRecording() {
  if (recording) return;
  recording = true;
  state.recording = true;
  const tab = await activeTab();
  if (tab) await follow(tab.id);
}

async function stopRecording() {
  recording = false;
  state.recording = false;
  state.tab = null;
  current = null;
  for (const id of [...attached]) await detach(id);
  reqs.clear();
}

chrome.tabs.onActivated.addListener(({ tabId }) => { if (recording) follow(tabId); });
chrome.windows.onFocusChanged.addListener(async (winId) => {
  if (!recording || winId === chrome.windows.WINDOW_ID_NONE) return;
  const [tab] = await chrome.tabs.query({ active: true, windowId: winId });
  if (tab) follow(tab.id);
});
chrome.tabs.onUpdated.addListener((tabId, info) => {
  // Перехід на сторінку, до якої відладчик не пускають (chrome://…), і назад.
  if (recording && tabId === current && info.status === "complete" && !attached.has(tabId)) { current = null; follow(tabId); }
});
chrome.debugger.onDetach.addListener((src, reason) => {
  attached.delete(src.tabId);
  if (recording && src.tabId === current) {
    emit({ t: Date.now(), k: "info", s: 1, text: "debugger detached: " + reason });
    current = null;
  }
});

// ---- події CDP ----

function argText(a) {
  if (!a) return "";
  if (a.value !== undefined) return typeof a.value === "string" ? a.value : JSON.stringify(a.value);
  if (a.unserializableValue) return a.unserializableValue;
  if (a.description) return a.description;
  return a.type || "";
}

function frameSrc(st) {
  const f = st && st.callFrames && st.callFrames[0];
  return f ? `${shortUrl(f.url)}:${f.lineNumber + 1}` : "";
}

chrome.debugger.onEvent.addListener((src, method, p) => {
  if (!recording || src.tabId !== current) return;
  const tabId = src.tabId;
  switch (method) {
  case "Runtime.consoleAPICalled": {
    const lvl = p.type === "warning" ? "warn" : p.type;
    const s = lvl === "error" || lvl === "assert" ? 2 : lvl === "warn" ? 1 : 0;
    emit({ t: p.timestamp, k: "console", s, lvl, text: clip((p.args || []).map(argText).join(" ")), src: frameSrc(p.stackTrace) });
    break;
  }
  case "Runtime.exceptionThrown": {
    const d = p.exceptionDetails || {};
    const text = (d.exception && d.exception.description) || d.text || "exception";
    emit({ t: p.timestamp, k: "error", s: 2, text: clip(text),
           src: d.url ? `${shortUrl(d.url)}:${(d.lineNumber || 0) + 1}` : frameSrc(d.stackTrace) });
    break;
  }
  case "Log.entryAdded": {
    const e = p.entry || {};
    if (e.level === "verbose") break;
    const s = e.level === "error" ? 2 : e.level === "warning" ? 1 : 0;
    emit({ t: e.timestamp, k: "log", s, lvl: e.level, text: clip(e.text), src: e.source || "", url: shortUrl(e.url) });
    break;
  }
  case "Network.requestWillBeSent": {
    const key = tabId + ":" + p.requestId;
    const wall = p.wallTime * 1000;
    clockOffset.set(tabId, wall - p.timestamp * 1000);
    if (p.redirectResponse && reqs.has(key)) {   // переадресація — попередній крок як окрема подія
      const r = reqs.get(key);
      emit({ t: r.t, k: "net", s: 0, method: r.method, url: r.url, status: p.redirectResponse.status, type: r.type,
             ms: Math.round((p.timestamp - r.ts) * 1000), size: 0, redirect: shortUrl(p.request.url) });
    }
    reqs.set(key, { t: wall, ts: p.timestamp, url: shortUrl(p.request.url), method: p.request.method, type: p.type || "" });
    break;
  }
  case "Network.responseReceived": {
    const r = reqs.get(tabId + ":" + p.requestId);
    if (r) { r.status = p.response.status; r.mime = p.response.mimeType; r.cache = !!(p.response.fromDiskCache || p.response.fromServiceWorker); }
    break;
  }
  case "Network.loadingFinished":
  case "Network.loadingFailed": {
    const key = tabId + ":" + p.requestId;
    const r = reqs.get(key);
    if (!r) break;
    reqs.delete(key);
    const failed = method === "Network.loadingFailed";
    const bad = failed || (r.status || 0) >= 400;
    const ev = { t: r.t, k: "net", s: bad ? 2 : 0, method: r.method, url: r.url, status: r.status || 0, type: r.type,
                 mime: r.mime || "", ms: Math.round((p.timestamp - r.ts) * 1000), size: failed ? 0 : (p.encodedDataLength || 0) };
    if (r.cache) ev.cache = true;
    if (failed) { ev.err = p.errorText || "failed"; if (p.canceled) ev.canceled = true; }
    emit(ev);
    break;
  }
  case "Page.frameNavigated": {
    if (p.frame && !p.frame.parentId) emit({ t: Date.now(), k: "nav", s: 0, url: shortUrl(p.frame.url) });
    break;
  }
  }
});

// ---- стан для спливного вікна ----
chrome.runtime.onMessage.addListener((msg, _sender, reply) => {
  if (msg && msg.q === "state") { reply({ connected: state.connected, recording: state.recording, browser: browserName, port: PORT }); return true; }
  if (msg && msg.q === "reconnect") { connect(); reply({ ok: true }); return true; }
  return false;
});
