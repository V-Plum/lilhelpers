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
//
// CAPS-107: запис вікна з самого розширення. Спливне вікно (або клавіша з
// chrome://extensions/shortcuts) просить {"cmd":"rec"}; HWND вікна ми не знаємо,
// тож на мить дописуємо в заголовок сторінки мітку «LH-…», Little Helpers
// знаходить вікно з нею, каже «found» — прибираємо мітку, і лише тоді він
// починає. Поки йде запис, спливного вікна немає: клік по значку = стоп, на
// значку — тривалість; пауза й стоп — ще й у меню правої кнопки. Стан приходить
// від Little Helpers ({"state":…}) на кожну зміну, хоч би звідки її зробили.

const PORT = 47650;
const MAX_TEXT = 4000;

let ws = null;
let recording = false;
let current = null;                 // tabId, до якого під'єднані зараз
const attached = new Set();
const reqs = new Map();             // `${tabId}:${requestId}` → запит у дорозі
const clockOffset = new Map();      // tabId → wallMs − monotonic·1000 (для мережі)
let state = { connected: false, recording: false, tab: null };
// CAPS-107: стан запису в Little Helpers (не лише наш лог) і запити на запис у дорозі.
let app = { state: "idle", ms: 0, at: 0, log: 1, ctl: 1 };
const pending = new Map();          // rid → { resolve, marker, tabId, preAttached, timer }
let ridSeq = 0;
let lastError = "";

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
    else if (m.state) onAppState(m);
    else if (m.rec) onRecReply(m);
  };
  ws.onclose = () => {
    ws = null;
    state.connected = false;
    if (recording) stopRecording();
    onAppState({ state: "idle", ms: 0, log: app.log, ctl: app.ctl });
    for (const rid of [...pending.keys()]) finishRec(rid, { ok: false, why: "no-app" });
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
    // CAPS-107: запис із розширення під'єднує відладчик ще до старту — вдруге не можна.
    if (!attached.has(tabId)) await chrome.debugger.attach({ tabId }, "1.3");
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
             dur: Math.round((p.timestamp - r.ts) * 1000), size: 0, redirect: shortUrl(p.request.url) });
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
                 mime: r.mime || "", dur: Math.round((p.timestamp - r.ts) * 1000), size: failed ? 0 : (p.encodedDataLength || 0) };
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

// ---- CAPS-107: запис вікна з розширення ----

const uk = /^uk/i.test(chrome.i18n.getUILanguage());
const L = uk ? {
  tipRec: "Little Helpers — запис {t} · клік — зупинити",
  tipPaused: "Little Helpers — пауза {t} · клік — зупинити",
  menuPause: "Пауза", menuResume: "Продовжити", menuStop: "Зупинити запис",
} : {
  tipRec: "Little Helpers — recording {t} · click to stop",
  tipPaused: "Little Helpers — paused {t} · click to stop",
  menuPause: "Pause", menuResume: "Resume", menuStop: "Stop recording",
};

const debuggable = (url) => !!url && !/^(chrome|edge|about|chrome-extension|devtools|view-source|chrome-search):/i.test(url)
  && !/^https:\/\/(chrome\.google\.com\/webstore|chromewebstore\.google\.com|microsoftedge\.microsoft\.com\/addons)/i.test(url);

function elapsed() {
  return app.state === "rec" ? app.ms + (Date.now() - app.at) : app.ms;
}

function fmt(ms) {
  const s = Math.floor(ms / 1000), m = Math.floor(s / 60);
  if (m >= 100) return Math.floor(m / 60) + "h";
  return m + ":" + String(s % 60).padStart(2, "0");
}

let badgeTimer = null;

function paintAction() {
  const on = app.state === "rec" || app.state === "paused";
  // Поки йде запис, спливного вікна немає — клік по значку приходить у onClicked і зупиняє.
  chrome.action.setPopup({ popup: on ? "" : "popup.html" });
  if (on) {
    const t = fmt(elapsed());
    chrome.action.setBadgeBackgroundColor({ color: app.state === "paused" ? "#FFB300" : "#E53935" });
    if (chrome.action.setBadgeTextColor) chrome.action.setBadgeTextColor({ color: app.state === "paused" ? "#000000" : "#FFFFFF" });
    chrome.action.setBadgeText({ text: t });
    chrome.action.setTitle({ title: (app.state === "paused" ? L.tipPaused : L.tipRec).replace("{t}", t) });
  } else {
    chrome.action.setBadgeText({ text: lastError ? "!" : "" });
    if (lastError) chrome.action.setBadgeBackgroundColor({ color: "#B3261E" });
    chrome.action.setTitle({ title: "Little Helpers" });
  }
  chrome.contextMenus.update("lh-pause", { visible: on, title: app.state === "paused" ? L.menuResume : L.menuPause }, () => void chrome.runtime.lastError);
  chrome.contextMenus.update("lh-stop", { visible: on }, () => void chrome.runtime.lastError);
}

function onAppState(m) {
  const was = app.state;
  app = { state: m.state, ms: Number(m.ms) || 0, at: Date.now(), log: m.log == null ? app.log : m.log, ctl: m.ctl == null ? app.ctl : m.ctl };
  if (app.state !== "idle" && was === "idle") lastError = "";
  paintAction();
  if (app.state === "rec" && !badgeTimer) badgeTimer = setInterval(paintAction, 1000);
  if (app.state !== "rec" && badgeTimer) { clearInterval(badgeTimer); badgeTimer = null; }
}

// Мітку дописуємо в кінець і так само знімаємо: сторінка могла тим часом змінити заголовок.
async function setMarker(tabId, suffix, on) {
  const [r] = await chrome.scripting.executeScript({
    target: { tabId },
    func: (sfx, add) => {
      if (add) { document.title = document.title + sfx; return { iw: innerWidth, ih: innerHeight, dpr: devicePixelRatio }; }
      const t = document.title, i = t.lastIndexOf(sfx);
      if (i >= 0) document.title = t.slice(0, i) + t.slice(i + sfx.length);
      return null;
    },
    args: [suffix, on],
  });
  return r ? r.result : null;
}

function finishRec(rid, res) {
  const p = pending.get(rid);
  if (!p) return;
  pending.delete(rid);
  clearTimeout(p.timer);
  if (p.marked) setMarker(p.tabId, p.suffix, false).catch(() => {});
  // Відладчик під'єднали лише заради запису: запису нема або лог вимкнено — відпускаємо.
  if (p.preAttached && (!res.ok || !res.log) && !(recording && current === p.tabId)) detach(p.tabId);
  lastError = res.ok ? "" : (res.why || "failed");
  paintAction();
  p.resolve(res);
}

function onRecReply(m) {
  const p = pending.get(m.rid);
  if (!p) return;                                   // чужий запит (інший браузер)
  if (m.rec === "found") {
    if (p.marked) { p.marked = false; setMarker(p.tabId, p.suffix, false).catch(() => {}); }
    return;
  }
  finishRec(m.rid, m.rec === "ok" ? { ok: true, log: m.log !== 0 } : { ok: false, why: m.why || "failed" });
}

async function startRec(tabId, windowId, pageOnly) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return { ok: false, why: "no-app" };
  if (!app.ctl) return { ok: false, why: "disabled" };
  if (app.state !== "idle") return { ok: false, why: "busy" };
  let tab, win;
  try { tab = await chrome.tabs.get(tabId); win = await chrome.windows.get(windowId); } catch (e) { return { ok: false, why: "not-found" }; }
  const rid = ++ridSeq;
  const marker = "LH-" + [...crypto.getRandomValues(new Uint8Array(4))].map((b) => b.toString(16).padStart(2, "0")).join("");
  const p = { tabId, suffix: " ⏺ " + marker, marked: false, preAttached: false, timer: 0, resolve: null };
  const done = new Promise((res) => { p.resolve = res; });
  pending.set(rid, p);
  // Лог увімкнено — відладчик ДО старту: смуга «налагоджує браузер» з'являється до першого кадру.
  if (app.log && debuggable(tab.url) && !attached.has(tabId)) {
    try { await chrome.debugger.attach({ tabId }, "1.3"); attached.add(tabId); p.preAttached = true; } catch (e) { /* лог сам спробує */ }
    await new Promise((r) => setTimeout(r, 150));   // смуга з'явилась, сторінка стала нижчою
  }
  let metrics = null;
  try { metrics = await setMarker(tabId, p.suffix, true); p.marked = true; } catch (e) { /* chrome:// тощо — без мітки */ }
  const msg = { cmd: "rec", rid, page: pageOnly ? 1 : 0, title: tab.title || "", wl: win.left, wt: win.top, ww: win.width, wh: win.height };
  if (p.marked) msg.marker = marker;
  if (metrics) { msg.pw = Math.round(metrics.iw * metrics.dpr); msg.ph = Math.round(metrics.ih * metrics.dpr); }
  p.timer = setTimeout(() => finishRec(rid, { ok: false, why: "timeout" }), 8000);
  send(msg);
  return done;
}

async function startFromWindow(windowId) {
  const [tab] = await chrome.tabs.query({ active: true, windowId });
  if (!tab) return { ok: false, why: "not-found" };
  const { pageOnly = true } = await chrome.storage.local.get("pageOnly");
  return startRec(tab.id, windowId, pageOnly);
}

chrome.action.onClicked.addListener(() => send({ cmd: "stop" }));   // лише поки йде запис (спливного вікна тоді нема)

function setupMenus() {
  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({ id: "lh-pause", title: L.menuPause, contexts: ["action"], visible: false });
    chrome.contextMenus.create({ id: "lh-stop", title: L.menuStop, contexts: ["action"], visible: false });
    paintAction();
  });
}
chrome.runtime.onInstalled.addListener(setupMenus);
chrome.runtime.onStartup.addListener(setupMenus);
chrome.contextMenus.onClicked.addListener((info) => {
  if (info.menuItemId === "lh-stop") send({ cmd: "stop" });
  else if (info.menuItemId === "lh-pause") send({ cmd: app.state === "paused" ? "resume" : "pause" });
});

chrome.commands.onCommand.addListener(async (command, tab) => {
  if (command !== "toggle-recording") return;
  if (app.state !== "idle") { send({ cmd: "stop" }); return; }
  const w = tab ? tab.windowId : (await chrome.windows.getLastFocused()).id;
  await startFromWindow(w);
});

// ---- стан для спливного вікна ----
chrome.runtime.onMessage.addListener((msg, _sender, reply) => {
  if (msg && msg.q === "state") {
    reply({ connected: state.connected, recording: state.recording, browser: browserName, port: PORT,
            app: app.state, ctl: app.ctl, log: app.log, lastError });
    return true;
  }
  if (msg && msg.q === "reconnect") { connect(); reply({ ok: true }); return true; }
  if (msg && msg.q === "rec") {                     // CAPS-107: кнопка «Записати це вікно»
    chrome.storage.local.set({ pageOnly: !!msg.pageOnly });
    startRec(msg.tabId, msg.windowId, !!msg.pageOnly).then(reply, (e) => reply({ ok: false, why: "failed" }));
    return true;
  }
  return false;
});
