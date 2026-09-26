// Little Helpers — звіт «відео + лог DevTools» (CAPS-84).
// Один код для двох місць: report.html (дані вбудовано, відео поруч або всередині)
// і переглядач у розширенні (дані й відео — з файлу .lhreport). Без залежностей.
//
// Час події (ms) — мілісекунди від початку відео. Лог показує, КОЛИ виконався
// код сторінки; на екрані наслідок з'являється трохи пізніше (кадр сторінки,
// композиція, захоплення — у середньому ~90 мс), тож поточна подія підсвічується
// трохи раніше за видиму зміну. Так і має бути: лог точний, екран запізнюється.
(function () {
  "use strict";

  const STR = {
    uk: {
      all: "Усе", console: "Консоль", error: "Помилки", net: "Мережа", nav: "Навігації", clicks: "Кліки",
      search: "Пошук у лозі", follow: "Слідувати за відео", events: (n) => `${n} ${plural(n, "подія", "події", "подій")}`,
      empty: "Нічого не знайдено", noLog: "У цьому записі немає лога браузера — лише відео й кліки.",
      keys: "Пробіл — пуск/пауза · N / P — наступна / попередня подія · E — наступна помилка",
      wall: "Час", src: "Джерело", url: "Адреса", status: "Статус", dur: "Тривалість", size: "Розмір",
      tab: "Вкладка", click: ["Клік", "Правий клік", "Середній клік"], masked: "секрети в адресах приховано", browser: "Браузер",
      failed: "не вдалося",
    },
    en: {
      all: "All", console: "Console", error: "Errors", net: "Network", nav: "Navigation", clicks: "Clicks",
      search: "Search the log", follow: "Follow the video", events: (n) => `${n} event${n === 1 ? "" : "s"}`,
      empty: "Nothing found", noLog: "This recording has no browser log — only video and clicks.",
      keys: "Space — play/pause · N / P — next / previous event · E — next error",
      wall: "Time", src: "Source", url: "URL", status: "Status", dur: "Duration", size: "Size",
      tab: "Tab", click: ["Click", "Right click", "Middle click"], masked: "secrets in URLs hidden", browser: "Browser",
      failed: "failed",
    },
  };
  function plural(n, one, few, many) {
    const m10 = n % 10, m100 = n % 100;
    if (m10 === 1 && m100 !== 11) return one;
    if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return few;
    return many;
  }

  const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  };

  function fmtTime(ms, withMs) {
    ms = Math.max(0, ms);
    const s = Math.floor(ms / 1000), m = Math.floor(s / 60);
    const base = `${m}:${String(s % 60).padStart(2, "0")}`;
    return withMs ? `${base}.${String(Math.floor(ms % 1000)).padStart(3, "0")}` : base;
  }
  function fmtSize(b) {
    if (!b) return "";
    if (b >= 1048576) return (b / 1048576).toFixed(1) + " MB";
    if (b >= 1024) return Math.round(b / 1024) + " KB";
    return b + " B";
  }

  // Категорія для фільтра й кольору.
  function cat(e) {
    if (e.k === "click") return "click";
    if (e.k === "net") return "net";
    if (e.k === "nav" || e.k === "tab") return "nav";
    if (e.s >= 2 || e.k === "error") return "error";
    return "console";
  }

  function render(root, data, videoSrc) {
    const meta = data.meta || {};
    const L = STR[(meta.lang || "").startsWith("en") ? "en" : "uk"];
    const clicks = (data.clicks || []).map((c) => ({ ms: c.ms, k: "click", s: 0, b: c.b || 0 }));
    const all = (data.events || []).concat(clicks).sort((a, b) => a.ms - b.ms);
    for (const e of all) e.c = cat(e);
    document.title = (meta.name || "Little Helpers") + " — Little Helpers";

    root.textContent = "";
    const app = el("div", "lh");
    root.appendChild(app);

    // ---- шапка ----
    const head = el("header", "lh-head");
    const title = el("div", "lh-title");
    title.appendChild(el("h1", null, meta.name || "Little Helpers"));
    const subParts = [];
    if (meta.created) subParts.push(new Date(meta.created).toLocaleString(L === STR.en ? "en-GB" : "uk-UA"));
    if (meta.duration) subParts.push(fmtTime(meta.duration * 1000));
    if (meta.width) subParts.push(`${meta.width} × ${meta.height}`);
    subParts.push(L.events((data.events || []).length));
    if (meta.masked) subParts.push(L.masked);
    title.appendChild(el("div", "sub", subParts.join(" · ")));
    head.appendChild(title);
    const tools = el("div", "lh-tools");
    const counts = { all: all.length, console: 0, error: 0, net: 0, nav: 0, click: 0 };
    for (const e of all) counts[e.c]++;
    let filter = "all", query = "";
    const chips = [];
    for (const [key, label] of [["all", L.all], ["error", L.error], ["console", L.console], ["net", L.net], ["nav", L.nav], ["click", L.clicks]]) {
      if (key !== "all" && !counts[key]) continue;
      const b = el("button", "chip");
      b.type = "button";
      if (key !== "all") b.appendChild(el("span", "dot " + key));
      b.appendChild(document.createTextNode(label));
      b.appendChild(el("span", "n", String(counts[key])));
      b.setAttribute("aria-pressed", key === filter ? "true" : "false");
      b.addEventListener("click", () => { filter = key; chips.forEach((c) => c.b.setAttribute("aria-pressed", c.key === key ? "true" : "false")); rebuild(); });
      chips.push({ key, b });
      tools.appendChild(b);
    }
    const search = el("input", "search");
    search.type = "search";
    search.placeholder = L.search;
    search.setAttribute("aria-label", L.search);
    search.addEventListener("input", () => { query = search.value.trim().toLowerCase(); rebuild(); });
    tools.appendChild(search);
    head.appendChild(tools);
    app.appendChild(head);

    // ---- тіло ----
    const body = el("div", "lh-body");
    const left = el("section", "lh-left");
    const vbox = el("div", "lh-video");
    const video = el("video");
    video.controls = true;
    video.preload = "auto";
    video.src = videoSrc;
    vbox.appendChild(video);
    left.appendChild(vbox);
    const strip = el("div", "lh-strip");
    const canvas = el("canvas");
    strip.appendChild(canvas);
    left.appendChild(strip);
    const times = el("div", "lh-time");
    const tNow = el("span", null, "0:00.000"), tKeys = el("span", "lh-keys", L.keys);
    times.appendChild(tNow); times.appendChild(tKeys);
    left.appendChild(times);
    body.appendChild(left);

    const right = el("section", "lh-right");
    const bar = el("div", "lh-listbar");
    const shown = el("span");
    const followBtn = el("button", "follow", L.follow);
    followBtn.type = "button";
    followBtn.hidden = true;
    bar.appendChild(shown); bar.appendChild(followBtn);
    right.appendChild(bar);
    const list = el("div", "lh-list");
    list.setAttribute("role", "list");
    right.appendChild(list);
    body.appendChild(right);
    app.appendChild(body);

    // ---- рядки ----
    let rows = [];          // { e, node }
    function rowText(e, main) {
      if (e.k === "net") {
        const st = el("span", "status " + (e.err || e.status >= 400 ? "bad" : "ok"), e.err ? (e.canceled ? "canceled" : L.failed) : String(e.status || ""));
        main.appendChild(document.createTextNode((e.method || "GET") + " "));
        main.appendChild(st);
        main.appendChild(document.createTextNode(" " + (e.url || "")));
        const m = [];
        if (e.dur != null) m.push(e.dur + " ms");
        if (e.size) m.push(fmtSize(e.size));
        if (m.length) main.appendChild(el("span", "meta", "  " + m.join(" · ")));
        return;
      }
      if (e.k === "click") { main.textContent = L.click[e.b] || L.click[0]; return; }
      if (e.k === "nav") { main.textContent = "→ " + (e.url || ""); return; }
      if (e.k === "tab") { main.textContent = L.tab + ": " + (e.title ? e.title + " — " : "") + (e.url || ""); return; }
      const t = el("span", "clip", e.text || "");
      main.appendChild(t);
      if (e.src) main.appendChild(el("div", "meta", e.src));
    }
    function details(e) {
      const d = el("div", "det");
      const add = (k, v) => { if (v == null || v === "") return; const p = el("div"); p.appendChild(el("b", null, k + ": ")); p.appendChild(document.createTextNode(String(v))); d.appendChild(p); };
      if (e.t) add(L.wall, new Date(e.t).toLocaleTimeString(undefined, { hour12: false }) + "." + String(Math.floor(e.t % 1000)).padStart(3, "0"));
      if (e.k === "net") { add(L.url, e.url); add(L.status, e.err ? e.err : e.status); add(L.dur, e.dur != null ? e.dur + " ms" : ""); add(L.size, fmtSize(e.size)); if (e.redirect) add("→", e.redirect); }
      else { add(L.src, e.src || e.url); if (e.text && e.text.length > 200) { const p = el("div", null, e.text); p.style.whiteSpace = "pre-wrap"; d.appendChild(p); } }
      if (e.b && typeof e.b === "string") add(L.browser, e.b);
      return d;
    }
    function rebuild() {
      list.textContent = "";
      rows = [];
      const frag = document.createDocumentFragment();
      for (const e of all) {
        if (filter !== "all" && e.c !== filter) continue;
        if (query) {
          const hay = ((e.text || "") + " " + (e.url || "") + " " + (e.src || "") + " " + (e.title || "")).toLowerCase();
          if (!hay.includes(query)) continue;
        }
        const r = el("div", "row" + (e.s ? " s" + e.s : ""));
        r.setAttribute("role", "listitem");
        r.appendChild(el("span", "tm", fmtTime(e.ms, true)));
        r.appendChild(el("span", "dot " + e.c));
        const main = el("div", "main");
        rowText(e, main);
        r.appendChild(main);
        r.appendChild(details(e));
        r.addEventListener("click", () => {
          r.classList.toggle("open");
          seek(e.ms);
        });
        rows.push({ e, node: r });
        frag.appendChild(r);
      }
      list.appendChild(frag);
      if (!rows.length) list.appendChild(el("div", "empty", (data.events || []).length ? L.empty : L.noLog));
      shown.textContent = L.events(rows.length);
      cur = -1;
      sync(true);
    }

    // ---- стрічка подій під відео ----
    const colors = () => {
      const cs = getComputedStyle(document.documentElement);
      return { error: cs.getPropertyValue("--err"), warn: cs.getPropertyValue("--warn"), net: cs.getPropertyValue("--net"),
               nav: cs.getPropertyValue("--nav"), click: cs.getPropertyValue("--click"), console: cs.getPropertyValue("--faint"),
               accent: cs.getPropertyValue("--accent"), line: cs.getPropertyValue("--line") };
    };
    function durMs() { return (isFinite(video.duration) && video.duration > 0 ? video.duration : (meta.duration || 1)) * 1000; }
    function drawStrip() {
      const r = strip.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
      canvas.width = Math.max(1, Math.round(r.width * dpr)); canvas.height = Math.max(1, Math.round(r.height * dpr));
      const g = canvas.getContext("2d"), W = canvas.width, H = canvas.height, c = colors(), D = durMs();
      g.clearRect(0, 0, W, H);
      const lane = { console: [0.62, 0.9], net: [0.62, 0.9], nav: [0.1, 0.38], click: [0.1, 0.38], error: [0.05, 0.95] };
      for (const pass of ["console", "net", "nav", "click", "warn", "error"]) {
        for (const e of all) {
          const k = e.c === "console" && e.s === 1 ? "warn" : e.c;
          if (k !== pass) continue;
          const x = Math.round((e.ms / D) * W);
          const [a, b] = lane[k === "warn" ? "error" : k] || lane.console;
          g.fillStyle = c[k] || c.console;
          if (k === "click") { g.beginPath(); g.arc(x, H * 0.24, 3 * dpr, 0, Math.PI * 2); g.fill(); }
          else g.fillRect(x, Math.round(H * a), Math.max(1, Math.round((k === "error" || k === "warn" ? 2 : 1) * dpr)), Math.round(H * (b - a)));
        }
      }
      const px = Math.round((video.currentTime * 1000 / D) * W);
      g.fillStyle = c.accent;
      g.fillRect(px - dpr, 0, 2 * dpr, H);
    }
    function seek(ms) { video.currentTime = Math.max(0, ms / 1000); following = true; followBtn.hidden = true; }
    strip.addEventListener("click", (ev) => {
      const r = strip.getBoundingClientRect();
      seek(((ev.clientX - r.left) / r.width) * durMs());
    });
    new ResizeObserver(drawStrip).observe(strip);

    // ---- синхрон відео → лог ----
    let cur = -1, following = true, userScrollAt = 0;
    function sync(force) {
      const t = video.currentTime * 1000;
      tNow.textContent = fmtTime(t, true);
      // остання подія, що вже сталась (двійковий пошук по відсортованому)
      let lo = 0, hi = rows.length - 1, idx = -1;
      while (lo <= hi) { const m = (lo + hi) >> 1; if (rows[m].e.ms <= t + 0.5) { idx = m; lo = m + 1; } else hi = m - 1; }
      if (idx !== cur || force) {
        for (let i = 0; i < rows.length; i++) {
          const n = rows[i].node;
          n.classList.toggle("cur", i === idx);
          n.classList.toggle("later", i > idx);
        }
        cur = idx;
        if (following) {
          const top = idx >= 0 ? rows[idx].node.offsetTop - list.clientHeight * 0.35 : 0;   // до першої події — на початок
          list.scrollTo({ top: Math.max(0, top), behavior: force ? "auto" : "smooth" });
        }
      }
      drawStrip();
    }
    list.addEventListener("wheel", () => { userScrollAt = Date.now(); following = false; followBtn.hidden = false; }, { passive: true });
    list.addEventListener("touchmove", () => { following = false; followBtn.hidden = false; }, { passive: true });
    followBtn.addEventListener("click", () => { following = true; followBtn.hidden = true; sync(true); });
    const tick = () => { sync(false); if ("requestVideoFrameCallback" in video) video.requestVideoFrameCallback(tick); };
    if ("requestVideoFrameCallback" in video) video.requestVideoFrameCallback(tick);
    video.addEventListener("timeupdate", () => sync(false));
    video.addEventListener("seeked", () => sync(false));
    video.addEventListener("loadedmetadata", () => drawStrip());

    // ---- клавіші ----
    document.addEventListener("keydown", (ev) => {
      if (ev.target === search || ev.ctrlKey || ev.metaKey || ev.altKey) return;
      const k = ev.key.toLowerCase();
      if (k === " ") { ev.preventDefault(); video.paused ? video.play() : video.pause(); }
      else if (k === "n" || k === "p" || k === "e") {
        const t = video.currentTime * 1000;
        let target = null;
        if (k === "p") { for (let i = rows.length - 1; i >= 0; i--) if (rows[i].e.ms < t - 40) { target = rows[i].e; break; } }
        else for (const r of rows) if (r.e.ms > t + 40 && (k === "n" || r.e.c === "error")) { target = r.e; break; }
        if (target) { ev.preventDefault(); seek(target.ms); }
      }
    });

    // для харнеса: поточний рядок і час
    window.LHReportState = () => ({ cur, rows: rows.length, t: video.currentTime, curMs: cur >= 0 ? rows[cur].e.ms : null,
                                   curText: cur >= 0 ? (rows[cur].e.text || rows[cur].e.url || "") : "" });
    window.LHReportSeek = (ms) => seek(ms);
    rebuild();
  }

  window.LHReport = { render };

  // report.html: дані вбудовано в <script id="lh-data">, відео — атрибутом data-video
  document.addEventListener("DOMContentLoaded", () => {
    const d = document.getElementById("lh-data");
    const root = document.getElementById("lh-root");
    if (!d || !root) return;
    let data;
    try { data = JSON.parse(d.textContent); } catch (e) { root.textContent = "report data is damaged"; return; }
    render(root, data, root.getAttribute("data-video") || "video.mp4");
  });
})();
