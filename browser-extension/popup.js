// Спливне вікно: стан з'єднання з Little Helpers і запису; кнопка переглядача звітів (CAPS-84).
const uk = /^uk/i.test(chrome.i18n.getUILanguage());
const T = uk ? {
  title: "Little Helpers", lh: "Little Helpers", rec: "Запис", open: "Відкрити звіт…",
  on: "підключено", off: "не знайдено", recOn: "іде — пишу лог", recOff: "ні",
  hintOff: "Запустіть Little Helpers і ввімкніть «Лог браузера» на вкладці «Відео». Розширення знайде програму саме.",
  hintOn: "Поки йде запис, браузер показує смугу «розширення налагоджує браузер» — це обмеження платформи.",
} : {
  title: "Little Helpers", lh: "Little Helpers", rec: "Recording", open: "Open a report…",
  on: "connected", off: "not found", recOn: "on — writing the log", recOff: "no",
  hintOff: "Start Little Helpers and turn on \"Browser log\" on the Video tab. The extension finds the app by itself.",
  hintOn: "While recording, the browser shows the \"extension is debugging this browser\" bar — a platform limitation.",
};
document.querySelectorAll("[data-i]").forEach((e) => { e.textContent = T[e.dataset.i]; });
function show(st) {
  const c = document.getElementById("conn"), r = document.getElementById("rec");
  c.textContent = st.connected ? T.on : T.off; c.className = st.connected ? "ok" : "no";
  r.textContent = st.recording ? T.recOn : T.recOff; r.className = st.recording ? "ok" : "";
  document.getElementById("hint").textContent = st.connected ? T.hintOn : T.hintOff;
}
chrome.runtime.sendMessage({ q: "reconnect" }, () => {
  setTimeout(() => chrome.runtime.sendMessage({ q: "state" }, show), 300);
});
document.getElementById("report").addEventListener("click", () => {
  chrome.tabs.create({ url: chrome.runtime.getURL("viewer.html") });
  window.close();
});
