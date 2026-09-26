// Переглядач .lhreport у розширенні (CAPS-84). .lhreport — ZIP без стиснення:
// meta.json, log.jsonl, report.html, video.mp4. Читаємо ЛИШЕ потрібні шматки
// файлу (File.slice): центральний каталог із кінця, дані — зрізом. Відео не
// копіюється в пам'ять — <video> грає Blob-зріз прямо з файлу, тож стелі в
// розмірі немає (на відміну від вбудованого в HTML base64).
(function () {
  "use strict";
  const uk = /^uk/i.test((chrome.i18n && chrome.i18n.getUILanguage()) || navigator.language);
  const T = uk ? {
    h: "Відкрити звіт Little Helpers", p: "Перетягніть сюди файл .lhreport — відео й лог браузера відкриються поруч, синхронно.",
    b: "Вибрати файл…", bad: "Це не звіт Little Helpers (.lhreport) або файл пошкоджено.", big: "Звіт стиснуто — такий Little Helpers не пише.",
  } : {
    h: "Open a Little Helpers report", p: "Drop a .lhreport file here — the video and the browser log open side by side, in sync.",
    b: "Choose a file…", bad: "This is not a Little Helpers report (.lhreport) or the file is damaged.", big: "The report is compressed — Little Helpers does not write those.",
  };
  document.querySelectorAll("[data-i]").forEach((e) => { e.textContent = T[e.dataset.i]; });

  const u16 = (v, o) => v.getUint16(o, true), u32 = (v, o) => v.getUint32(o, true);
  async function view(file, a, b) { return new DataView(await file.slice(a, b).arrayBuffer()); }

  // Каталог ZIP: ім'я → { at (початок даних), size, method }.
  async function readZip(file) {
    const tailLen = Math.min(file.size, 65557);
    const tail = await view(file, file.size - tailLen, file.size);
    let eocd = -1;
    for (let i = tail.byteLength - 22; i >= 0; i--) if (u32(tail, i) === 0x06054b50) { eocd = i; break; }
    if (eocd < 0) throw new Error("no EOCD");
    const count = u16(tail, eocd + 10), cdSize = u32(tail, eocd + 12), cdOff = u32(tail, eocd + 16);
    const cd = await view(file, cdOff, cdOff + cdSize);
    const dec = new TextDecoder();
    const out = {};
    let p = 0;
    for (let n = 0; n < count; n++) {
      if (u32(cd, p) !== 0x02014b50) throw new Error("bad CD");
      const method = u16(cd, p + 10), size = u32(cd, p + 20), nameLen = u16(cd, p + 28), extra = u16(cd, p + 30), cmt = u16(cd, p + 32);
      const local = u32(cd, p + 42);
      const name = dec.decode(new Uint8Array(cd.buffer, cd.byteOffset + p + 46, nameLen));
      out[name] = { local, size, method };
      p += 46 + nameLen + extra + cmt;
    }
    for (const name in out) {
      const lh = await view(file, out[name].local, out[name].local + 30);
      if (u32(lh, 0) !== 0x04034b50) throw new Error("bad local header");
      out[name].at = out[name].local + 30 + u16(lh, 26) + u16(lh, 28);
    }
    return out;
  }

  async function open(file) {
    const err = document.getElementById("err");
    err.textContent = "";
    try {
      const z = await readZip(file);
      if (!z["meta.json"] || !z["video.mp4"]) throw new Error("not a report");
      for (const k of ["meta.json", "log.jsonl", "video.mp4"]) if (z[k] && z[k].method !== 0) { err.textContent = T.big; return; }
      const text = async (k) => z[k] ? await file.slice(z[k].at, z[k].at + z[k].size).text() : "";
      const meta = JSON.parse(await text("meta.json"));
      const events = (await text("log.jsonl")).split("\n").filter((l) => l.trim()).map((l) => JSON.parse(l));
      const clicks = z["clicks.json"] ? JSON.parse(await text("clicks.json")) : (meta.clicks || []);
      const v = z["video.mp4"];
      const blob = file.slice(v.at, v.at + v.size, "video/mp4");
      document.getElementById("pick").hidden = true;
      const root = document.getElementById("lh-root");
      root.hidden = false;
      window.LHReport.render(root, { meta, events, clicks }, URL.createObjectURL(blob));
    } catch (e) {
      err.textContent = T.bad;
      console.error(e);
    }
  }

  const pick = document.getElementById("pick");
  document.getElementById("file").addEventListener("change", (e) => { if (e.target.files[0]) open(e.target.files[0]); });
  pick.addEventListener("dragover", (e) => { e.preventDefault(); pick.classList.add("over"); });
  pick.addEventListener("dragleave", () => pick.classList.remove("over"));
  pick.addEventListener("drop", (e) => { e.preventDefault(); pick.classList.remove("over"); if (e.dataTransfer.files[0]) open(e.dataTransfer.files[0]); });
  window.LHViewerOpen = open;   // для харнеса
})();
