// Спливне вікно: стан з'єднання з Little Helpers і запису; кнопка переглядача звітів (CAPS-84);
// CAPS-107 — «Записати це вікно» (поки йде запис, спливного вікна немає: клік по значку = стоп).
const uk = /^uk/i.test(chrome.i18n.getUILanguage());
const T = uk ? {
  title: "Little Helpers", lh: "Little Helpers", rec: "Запис", open: "Відкрити звіт…",
  on: "підключено", off: "не знайдено", recOn: "іде — пишу лог", recOff: "ні",
  go: "● Записати це вікно", busy: "Шукаю вікно…",
  page: "Лише сторінка — без вкладок і адресного рядка",
  hintOff: "Запустіть Little Helpers — розширення знайде програму саме.",
  hintOn: "Поки йде запис, клік по значку розширення зупиняє його; пауза — у меню правої кнопки.",
  hintLog: "Поки пишеться лог, браузер показує смугу «розширення налагоджує браузер» — у записі лише сторінки її не видно.",
  why: {
    "no-app": "Little Helpers не знайдено — запустіть програму.",
    disabled: "Запуск запису з розширення вимкнено в Little Helpers (вкладка «Відео»).",
    "video-off": "Запис відео вимкнено в Little Helpers (вкладка «Відео»).",
    busy: "Запис уже йде або вибирається ділянка.",
    "not-found": "Не вдалося знайти це вікно браузера.",
    ambiguous: "Кілька вікон із такою самою вкладкою — перейдіть на іншу вкладку або почніть запис клавішею Alt+Shift+5.",
    minimized: "Вікно згорнуте.",
    wgc: "Ця версія Windows не вміє записувати окреме вікно (потрібна 10 1903 або новіша).",
    cancelled: "Скасовано.", timeout: "Little Helpers не відповів.", failed: "Не вдалося почати запис.",
  },
} : {
  title: "Little Helpers", lh: "Little Helpers", rec: "Recording", open: "Open a report…",
  on: "connected", off: "not found", recOn: "on — writing the log", recOff: "no",
  go: "● Record this window", busy: "Looking for the window…",
  page: "Page only — no tabs or address bar",
  hintOff: "Start Little Helpers — the extension finds the app by itself.",
  hintOn: "While recording, clicking the extension icon stops it; pause is in the right-click menu.",
  hintLog: "While the log is written, the browser shows the \"extension is debugging this browser\" bar — a page-only recording doesn't show it.",
  why: {
    "no-app": "Little Helpers not found — start the app.",
    disabled: "Starting recording from the extension is turned off in Little Helpers (Video tab).",
    "video-off": "Video recording is turned off in Little Helpers (Video tab).",
    busy: "A recording is already running or a region is being picked.",
    "not-found": "Could not find this browser window.",
    ambiguous: "Several windows show the same tab — switch to another tab or start with Alt+Shift+5.",
    minimized: "The window is minimized.",
    wgc: "This Windows version can't record a single window (needs 10 1903 or newer).",
    cancelled: "Cancelled.", timeout: "Little Helpers didn't answer.", failed: "Could not start recording.",
  },
};
document.querySelectorAll("[data-i]").forEach((e) => { e.textContent = T[e.dataset.i]; });
const go = document.getElementById("go"), page = document.getElementById("page"), err = document.getElementById("err");
chrome.storage.local.get("pageOnly").then(({ pageOnly = true }) => { page.checked = pageOnly; });
page.addEventListener("change", () => chrome.storage.local.set({ pageOnly: page.checked }));

function show(st) {
  const c = document.getElementById("conn"), r = document.getElementById("rec");
  c.textContent = st.connected ? T.on : T.off; c.className = st.connected ? "ok" : "no";
  r.textContent = st.recording ? T.recOn : T.recOff; r.className = st.recording ? "ok" : "";
  document.getElementById("hint").textContent = !st.connected ? T.hintOff : (st.log ? T.hintOn + " " + T.hintLog : T.hintOn);
  go.disabled = !st.connected || st.app !== "idle";
  if (st.connected && !st.ctl) err.textContent = T.why.disabled;
  else if (st.lastError && T.why[st.lastError]) err.textContent = T.why[st.lastError];
}
chrome.runtime.sendMessage({ q: "reconnect" }, () => {
  setTimeout(() => chrome.runtime.sendMessage({ q: "state" }, show), 300);
});

go.addEventListener("click", async () => {
  go.disabled = true;
  go.textContent = T.busy;
  err.textContent = "";
  const w = await chrome.windows.getCurrent();
  const [tab] = await chrome.tabs.query({ active: true, windowId: w.id });
  const res = await chrome.runtime.sendMessage({ q: "rec", tabId: tab && tab.id, windowId: w.id, pageOnly: page.checked });
  if (res && res.ok) { window.close(); return; }
  err.textContent = T.why[(res && res.why) || "failed"] || T.why.failed;
  go.textContent = T.go;
  go.disabled = false;
});

document.getElementById("report").addEventListener("click", () => {
  chrome.tabs.create({ url: chrome.runtime.getURL("viewer.html") });
  window.close();
});
