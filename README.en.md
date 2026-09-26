# Little Helpers

[Українська](README.md) · **English**

<img src="lilhelpers.png" width="128" align="right" alt="Little Helpers icon">

Small Windows 11 conveniences in one tray app. No dependencies, a single exe:

- **Layout** — CapsLock switches the keyboard layout; Shift + CapsLock is the
  regular Caps Lock.
- **Cursor** — shake the mouse and the cursor briefly grows big (as on macOS).
- **Day/night** — light/dark Windows theme by sunrise and sunset or on a
  schedule.
- **Preview** — Space in Explorer shows the selected file, like Quick Look on
  macOS or Peek in PowerToys: images (incl. WebP) and SVG, PDF, STL models
  with seven views, text and code with highlighting, Markdown, docx, formatted
  JSON; video and audio play right in the preview window. ⚠ Experimental,
  since 2.2.0.
- **Screenshots** — a hotkey captures the screen, a region, a window or an
  image from the clipboard, and the editor lets you mark up what matters, hide
  what shouldn't be seen and send the result to the clipboard or a file. Marks
  stay editable: shots are saved in their own format in a library, with
  EXIF/META metadata. Since 3.0.0.
- **Screen recording** — `Alt+Shift+5` records a region, a window (and follows
  it) or the whole screen to MP4 with the same HDR compensation as screenshots;
  cursor, clicks, system sound and microphone are optional. The same editor
  trims and cuts, draws marks over the video, crops and scales it down, exports
  to MP4 or GIF and keeps everything as a `.lhvideo` project. Since 4.13.0 — also a
  DevTools log from Chrome/Edge in sync with the video, and a report for developers
  (video + log) as one file. Since 4.0.0.
- **Dark theme for the app window itself**, auto-updates from GitHub Releases
  with signature verification.
- **Ukrainian and English** UI — from the Windows language or chosen by hand.

| Light theme, Ukrainian | Dark theme, English |
|---|---|
| ![Window, light theme](screenshot.png) | ![Window, dark theme](screenshot-dark.png) |

## Layout

The key can be intercepted in two ways; the switch is on the **Layout** tab
(stored in `HKCU\Software\lilhelpers`, `Mode`):

- **Primary** — a low-level keyboard hook that swallows CapsLock (`return 1`).
  The only way to keep Caps Lock from toggling. The callback does nothing but
  `PostMessage` to the main window: anything heavier risks missing
  `LowLevelHooksTimeout`, after which Windows silently removes the hook. The
  hook itself lives on a **separate thread** with its own message loop, so a
  busy UI thread (for example, synchronous Task Scheduler COM calls when
  enabling autostart) cannot starve the callback.
- **Fallback** — `RegisterHotKey(VK_CAPITAL, MOD_NOREPEAT)`. It intercepts
  nothing and doesn't irritate security software, but Windows **still toggles
  Caps Lock**: the toggle happens a level below the delivery of `WM_HOTKEY`.
  It can't be undone by injecting `SendInput` — our own injection is swallowed
  by our own hotkey registration (verified: `SendInput` reports success,
  `WM_HOTKEY` never arrives, the state doesn't change). That's why this is
  only a fallback, for when the hook is blocked or conflicts.

The rest is the same for both:

- Switching: `WM_INPUTLANGCHANGEREQUEST` (via `PostMessage`, non-blocking) to
  the window that actually has focus (`GetGUIThreadInfo().hwndFocus` — correct
  for UWP too).
- `requireAdministrator` manifest: without it UIPI blocks messages to elevated
  windows (an admin terminal, regedit) — Caps would be swallowed but the layout
  wouldn't switch. Consequence: launching by hand shows a UAC prompt.
- **"Don't interfere in remote and virtual machine windows: Caps Lock and
  cursor magnification"** — Remote Desktop, Windows App, VMware, Hyper-V: let
  the copy of the app on the remote machine switch the layout (and magnify the
  cursor) there. Since 4.11.0 the checkbox also governs cursor magnification
  (CAPS-5): otherwise both copies saw the same gesture — the host through its
  hook, the guest through forwarded movement — and the shrink looked doubled.
  The host stays out when the client is active or merely under the cursor;
  without a copy of the app on the guest there is no magnification in such a
  window — a deliberate price for one switch covering both features. Since
  4.9.0 the key is no longer passed through "as is" in this case (the host
  then managed to toggle its own Caps Lock — letter case, CAPS-13); instead it
  is forwarded to the client window as `WM_KEYDOWN`/`WM_KEYUP` with the same
  scan code: the client passes it into the session and the Caps state on this
  machine doesn't change. The "Forward Caps Lock to the client window…"
  checkbox turns this off if some client doesn't forward the posted message —
  then the key goes through as before. In **Fallback** mode there is no hook,
  so behaviour is unchanged.
- **"Switch keyboard layout with Caps Lock"** enables the interception itself;
  when off, Caps Lock is ordinary and the app keeps running for the cursor and
  day/night features. Autostart is separate, on the **Settings** tab.

## Cursor — shake to find

On a large monitor the cursor is easy to lose. Shake the mouse and it grows big
for a few seconds, then smoothly shrinks back to normal so the eye can follow
it to where it ends up.

- **We enlarge the system cursor itself** (`SystemParametersInfo` 0x2029 →
  `CursorBaseSize`) rather than drawing a magnified copy in an overlay. Reason:
  the Windows hardware cursor is drawn on top of all windows, so an overlay
  copy would always come paired with the live small cursor. The cost is that
  this is a global user setting, so before enlarging, the original size is
  left as a trace in the registry and restored even after the app crashes.
- **The gesture** is recognised not by speed but by the ratio of the path
  travelled to the diagonal of the movement's bounding box: a straight fling
  across the whole screen gives a ratio of ≈1, shaking gives several times
  more. Movement with a button held is ignored entirely — those (scrubbing a
  slider, drawing, dragging a window) are exactly what most often looks like
  shaking.
- **Doesn't work in fullscreen apps**: `SHQueryUserNotificationState` catches
  exclusive D3D and presentation mode (the "busy" state — QUNS_BUSY — is NOT
  counted since 4.10.0: RDP clients hold it for the whole session, and
  magnification went dead even over an inactive maximised window — CAPS-6), and
  borderless windows get an extra check (a window exactly the size of the
  monitor with no caption/border; the desktop and the taskbar are excluded by
  window class).
- **By default the shrink is drawn with a copy of the cursor in a layered
  window** ("Shrink smoothly" on the tab). Animating the system size is not
  possible: every frame costs a synchronous broadcast, and on a loaded machine
  you get 2–3 jumps instead of smoothness. The copy is drawn by ordinary
  composition — dozens of frames for free. The system size is restored with a
  single call on a background thread, and the copy covers the moment of the
  switch. The price: the real cursor is visible under the copy (already at
  normal size and at the same spot), so the option can be turned off. Since
  4.12.0 the copy is crisp (CAPS-4): a cursor bitmap is always 32 px, and
  stretched to 160 px without smoothing it showed 5×5 blocks. For standard
  cursors the frame of the needed size is taken from the system `.cur` (up to
  256 px there); foreign cursors are scaled bicubically from their own frame;
  alpha for 1-bit cursors comes from the mask, otherwise a black arrow would
  be invisible.
- If the copy is off, the system cursor itself shrinks, and then the animation
  is **tied to TIME, not to a number of steps**, and runs on a separate thread.
  Reason: each size change sends a synchronous `WM_SETTINGCHANGE` to all
  windows, and its cost depends on how many are open (measured: 0.1 ms without
  the broadcast versus ~35 ms with it on an empty desktop, and noticeably more
  under load). The size is computed from the elapsed time: a fast system gets
  more frames and a smooth animation, a slow one fewer, but the duration is
  the same. Intermediate values are rounded to Windows steps (32 px + a
  multiple of 16), because between steps the cursor looks the same while the
  call costs a full broadcast.
- Settings — the **Cursor** tab: on/off, how much to magnify (2–8×) and how
  long to hold (0.5–5 s). Under **Details** are the gesture thresholds: the
  recognition window (ms), minimum mouse path (px), the "path / span"
  threshold (%), minimum direction changes, shrink smoothness (ms).

The defaults (1200 px of path, 350 %, 3 direction changes, a 700 ms window)
were tuned on simulated gestures: none of the ordinary movements — a fling
across a 4K screen, pointing with a correction, a trembling hand, moving
through menus — passes, while real shaking counts after 4–6 swings (≈0.4 s at
an amplitude of 250–300 px).

## Day/night — automatic light/dark Windows theme

The **Day/night** tab switches the Windows theme (both apps and system —
together, to avoid a mix) by sunrise/sunset or on a schedule.

- **Mechanics**: two DWORDs in `HKCU\Software\Microsoft\Windows\CurrentVersion\
  Themes\Personalize` (`AppsUseLightTheme`, `SystemUsesLightTheme`; 0 = dark)
  and a `WM_SETTINGCHANGE` broadcast with `ImmersiveColorSet`, without which
  some windows don't repaint. The broadcast goes from a separate thread so a
  hung foreign window can't freeze our UI. The check runs once a minute, and
  also right after waking from sleep and after a time/time-zone change.
- **Sunrise and sunset** are computed locally with the NOAA algorithm (~1 min
  accuracy), no network. Polar day/night are handled (light/dark for the whole
  day).
- **Location** — under **Details**; the default chain: the Windows location
  service → by IP address (one GET to `ip-api.com`) → the Windows time zone
  and region (longitude from the UTC offset, the country's latitude/longitude
  from `GetGeoInfo`; the coarsest — up to an hour off). Or manually: latitude
  and longitude. The last detected location is cached in the registry, so
  after start-up the theme is applied immediately, and the sensor/network are
  polled in the background no more than once a day. Explicitly choosing
  **Windows service** shows the system permission dialog once; in
  **Automatic** mode there are no dialogs. Behind a VPN the IP lookup will
  show the VPN server's location.
- **"Switch now"** — a manual choice that lasts **until the next boundary**
  (the next sunrise/sunset or schedule time). After that the automation again
  computes the required state from the schedule rather than just flipping
  back.
- **While a fullscreen app is in the foreground** (a game, a film), the theme
  doesn't change — it switches as soon as the app closes (the same detector as
  in the cursor finder).
- **Schedule** — one for all days: "dark from" and "light from".

## Space-bar file preview ⚠ experimental

> **This feature is still in development; stable operation is not
> guaranteed.** It appeared in 2.2.0 and is still being run in. If it gets in
> the way, turn it off with the checkbox on the **Preview** tab; Space in
> Explorer then behaves exactly as before.

Space on a selected file in Explorer or on the desktop opens the preview
window; Space again or Esc closes it. The same as Quick Look on macOS and Peek
in PowerToys, but **without a separate runtime**: everything below is drawn by
our own code or by system APIs already in Windows — not a single new
dependency. The whole feature costs **163 KB**: 387 KB in 2.1.1 → 550 KB in
2.6.0.

| STL model | SVG |
|---|---|
| ![STL preview](screenshot-peek-stl.png) | ![SVG preview](screenshot-peek-svg.png) |
| **Markdown, typeset** | **Code with highlighting** |
| ![Markdown preview](screenshot-peek-text.png) | ![Code highlighting](screenshot-peek-code.png) |
| **File card** | |
| ![Folder card](screenshot-peek-card.png) | |

- **The window doesn't take focus** (`WS_EX_NOACTIVATE`). Explorer stays
  active, the arrow keys move through files as usual, and the preview follows
  the selection and picks up the new file by itself. This differs from Peek,
  which does its own browsing: here the Explorer selection and what's on
  screen are always one and the same.
- **Images** (JPEG, PNG, GIF, BMP, TIFF, ICO) — via GDI+, rotated per EXIF, a
  reduced copy cached for the window size; **animated GIFs play**, each frame
  with its own delay. **WebP** (and HEIC, AVIF, JXR if their codecs are
  installed) — via WIC, because GDI+ doesn't know them. This is an image
  decoder, not a preview handler: no UI, no scripts, a narrow contract — this
  is exactly where the line on foreign COM is drawn.
- **PDF** — all pages, via the built-in `Windows.Data.Pdf`. Scroll pages with
  the wheel over the window or with the `‹ ›` arrows in the header; showing
  only the first page makes little sense. The document stays loaded on the
  same thread while the preview is open, so paging costs only a page render —
  reopening the file each time would be noticeably slow on a forty-page
  manual. Keys are deliberately left alone: the arrows in Explorer still move
  through **files**, and Windows sends the wheel to the window under the cursor
  even without focus — that's why this works. It's Windows itself, not a
  handler someone registered, so the rule about foreign COM stays intact.
  There is no header for this API in MinGW or without the Windows SDK, so the
  interfaces are declared by hand; the factory IID wasn't guessed but obtained
  from the factory itself via `IInspectable::GetIids()`, and the method order
  was verified with live calls. ⚠ All work happens on a **separate thread in an
  MTA apartment**: in an STA (and the UI thread is one) a WinRT async operation
  doesn't complete until the thread pumps messages — without this
  `LoadFromStreamAsync` reliably returns `E_FAIL`. Pumping messages inside a
  message handler would mean UI re-entrancy, which is worse than a separate
  thread.
- **Text and code** up to 1 MB — with encoding detection: BOM, then a strict
  UTF-8 check, then UTF-16LE (given away by NULs in odd bytes), and only for
  known text extensions — the system ANSI code page.
- **Markdown is shown typeset**, and code **with syntax highlighting**. Both
  are done by one mechanism: the text is converted to RTF and poured into a
  RichEdit in a single stream (`EM_STREAMIN`). Thousands of `EM_SETCHARFORMAT`
  calls on a file of a few hundred kilobytes are noticeably slow; RTF isn't.
  Highlighting is deliberately **one for all languages**: comments, strings,
  numbers and a shared set of keywords, with the comment style chosen by
  extension. Full grammars for every language would be a different app; this
  is enough for a preview. Above 400 KB highlighting is turned off: the
  benefit is small and the pause is noticeable.
- **Zoom** for everything shown as a picture — images, SVG, STL, PDF pages.
  The wheel zooms **around the cursor** (so "zoom into this" doesn't turn into
  "zoom in and look for where it went"), dragging pans, a double click fits
  back. In a multi-page PDF the plain wheel turns pages, and `Ctrl` + wheel
  zooms.
- **JSON is reformatted** before display: a minified file is otherwise a solid
  wall of text. This is deliberately not a parser — validity isn't checked,
  string contents aren't touched, only whitespace outside string literals is
  rewritten. A file with unbalanced brackets is shown as is rather than
  mangled; the same goes for JSON cut off by the 1 MB ceiling. The caption
  says "formatted" so the view isn't mistaken for the file itself.
- **Video** (mp4, mov, avi, wmv, mkv…) — real playback since 4.10.0 (CAPS-18):
  the same Media Engine in frame-server mode as in the video editor, but its
  own instance, so the preview and the editor live side by side. It starts by
  itself and **muted** — the preview opens very easily, and a loud video would
  be a surprise; the sound button is in the bar under the frame, and the
  choice is remembered. The bar has play/pause, time, a track (click and
  drag), sound; a click on the frame itself pauses/plays; the wheel seeks
  ±5 s. Mouse only: the window has no focus, and Space and the keys remain
  Explorer's. The first frame (resolution and duration in the subtitle) is
  taken, as before, via the Source Reader — it's also the poster until the
  engine is ready. Two places where it's easy to get this wrong: without
  `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING` the reader doesn't deliver RGB32
  for most codecs, and the very first frame in many files is black — so we
  seek a little before reading.
- **Audio** (mp3, m4a, aac, wav, flac, ogg, opus, wma…) — also plays since
  4.10.0, with sound (quieter, 50 %), because silent audio makes no sense; it
  has its own sound button, remembered separately from video. The card: an
  icon, title/artist/album from the tags (via Media Foundation, without
  third-party property handlers), duration and bitrate in the subtitle, the
  same control bar.
- **STL models** (binary and ASCII) — our own rasteriser: z-buffer, flat
  shading, zero dependencies. Binary vs. ASCII is decided by **file size**,
  not by the word `solid` at the start — plenty of exporters write `solid`
  into binary files too. Normals from the file are ignored in favour of ones
  computed from the vertices (in real STLs they're often zero or point the
  wrong way), and lighting is two-sided because triangle winding is also often
  inconsistent. The caption shows the dimensions, which for printing is
  usually the reason to open the file. **Seven views as pages** (since 4.11.0,
  CAPS-55): isometric first — whoever doesn't page sees what they saw before —
  then front, back, left, right, top, bottom; the wheel or the header arrows
  page, `Ctrl`+wheel zooms (as in PDF). A view is drawn when you go to it and
  cached; the triangles live while the card is open and are freed with it.
  This is a deliberately cheaper alternative to rotating with the mouse
  (CAPS-54): an interactive render has to keep up with 16 ms per frame, a page
  doesn't.
- **Word documents** (docx) — as text. The package is read by the system
  Packaging API (`msopc`), so no zip reader of our own was needed. Layout
  can't be reproduced without the Word engine, and the text answers the
  question "what is this document".
- **Other files and folders** — a card: icon, type, size, dates, location, and
  for a folder the number of items too. The card grows to fit its content.
- **STEP** (`.step`, `.stp`) — a card from the file header: author,
  organisation, date, schema, an approximate entity count. We don't draw the
  geometry and don't pretend we can: it's B-rep with NURBS surfaces, which
  needs an engine like OpenCASCADE — tens of megabytes, exactly what this app
  avoids.
- **What stays with Explorer.** Space has native functions there, so the
  feature is deliberately not greedy: Ctrl/Shift/Alt/Win + Space, type-ahead
  search (Space within a second after a letter), Space in the address, search
  and rename fields, in the folder tree and in open/save dialogs. The hook
  swallows Space only when focus is exactly in the file list (`DirectUIHWND` or
  `SysListView32` inside `SHELLDLL_DefView`). If the selection is empty, Space
  is returned to Explorer via `SendInput` with our own marker in
  `dwExtraInfo`, which the hook lets through — native behaviour isn't lost.
- **An image file is read into memory** (up to 64 MB) and decoded from a
  stream rather than from the file: otherwise the preview would keep the file
  open and it couldn't be renamed or deleted. This also enables animation —
  `Clone()`, which used to release the lock, collapsed a multi-frame GIF into
  a single frame.
- **The selection is read as `CF_HDROP`** from the folder view's `IDataObject`
  (`IShellWindows` → `IShellBrowser` → `IShellView`). The more convenient
  `IFolderView2` doesn't work here: it's a proxy to an object in
  `explorer.exe`, and there is no proxy stub for this interface —
  `QueryInterface` silently returns `E_NOINTERFACE`. Explorer tabs in
  Windows 11 are separate entries with the same window HWND, so the view is
  matched by the view's own window rather than the top-level window.
- **Shortcuts show WHERE they lead**, not their own innards: `.url` is read as
  an ini file, `.lnk` via `IShellLink` without `Resolve` (which goes to the
  network and can hang for a long time on an unavailable drive). Otherwise a
  Steam game shortcut would show its text, since `.url` is formally a text
  file.
- **SVG is drawn by the system Direct2D**
  (`ID2D1DeviceContext5::CreateSvgDocument`) — it's Windows itself, not a
  handler someone registered, so the rule about foreign COM in an elevated
  process isn't broken. Probing established its limits: it handles paths,
  gradients, `clipPath`, strokes and group opacity, but **silently ignores**
  `<text>`, `<mask>`, `<filter>` and `<pattern>`. Silently is the worst part,
  because the picture comes out wrong and there's nowhere to see that; so
  files with these elements are drawn, but the caption honestly says what's
  missing ("no effects", "no mask", "no text"). At first I refused to draw
  such files at all — a test on two real icons showed that was too strict:
  without `<filter>` only a shadow disappears, without `<mask>` only a
  highlight, and the image stays recognisable. The danger here is the
  **silence**, not the incompleteness itself. If the render came out
  practically empty, we fall back to the markup — and this is checked by the
  bitmap's **content**, not by a list of elements, so cases we didn't think of
  are caught too.
  A pre-pass fixes four places where real files break Direct2D. Illustrator
  sets fills via CSS classes in `<style>` (without the pre-pass the project's
  own icon came out as a solid **black blob**). Direct2D doesn't see SVG 2's
  `<use href>` — it only recognises the old `xlink:href`. Next: Direct2D
  **honours the root's `width`/`height`** and draws the document at exactly
  that size, ignoring the canvas we gave it — so a 24×24 icon came out as a
  dot, and a drawing sized "724mm" came out entirely empty; when there's a
  `viewBox`, these attributes are removed. And finally, drawings for laser
  cutters come with strokes in fractions of a millimetre: with a 724
  `viewBox`, a 0.15 stroke is thinner than a pixel and simply disappears, so
  sub-pixel strokes are raised to visible ones.
- **System preview handlers (`IPreviewHandler`) are deliberately NOT used.**
  They would give PDF and Office almost for free, but would mean loading
  foreign COM code into our process, which runs with administrator rights.
  That's exactly why Windows runs them in the unprivileged `prevhost.exe`. If
  that is ever needed, it'll be a separate non-elevated helper process, not an
  exception here. For the same reason icons and type names are taken by
  extension (`SHGFI_USEFILEATTRIBUTES`), without calling a specific file's
  handlers.
- Turned off with the checkbox on the **Preview** tab (`Peek` in the
  registry). The keyboard hook is shared with the layout feature: it stays
  while at least one of the features needs it.

## Screenshot editor

Since 3.0.0. A hotkey takes a shot, the editor opens, and the result goes to
the clipboard or a file.

| Key | What it does |
|---|---|
| `Ctrl+Alt+4` | take the image from the clipboard |
| `Alt+Shift+4` | drag a region, click a window, or `Space` for the whole screen; the gesture decides what happens next |
| `Alt+Shift+3` | capture the whole screen — with the "no key" action |
| `Ctrl+Alt+E` | open an empty editor — no shot |

The keys are changed on the **Shots** tab: click the field and press your
combination. If another program already holds it, the field says so that same
second, not later when the key silently fails to work.

The "Capture shots with hotkeys" checkbox at the top of the tab turns the whole
feature off at once (since 4.0.0): the keys are **released** — another program
can take them — and the shot items in the tray go grey. The combinations
themselves stay in the settings and come back as soon as the checkbox is
turned on again.

**Empty start.** The editor also opens without a shot — as a transparent
1280 × 800 canvas where you can paste a picture from the clipboard, open the
library or import a file. Three doors: the tray item **Shots → Screenshot
editor**, the `Ctrl+Alt+E` hotkey and the `--editor` command-line switch (the
second instance passes the command to the first and exits). While the canvas
is still empty, `Ctrl+V` puts the picture **onto the canvas** — replacing the
background; as soon as there's something on it, `Ctrl+V` pastes a separate
object again. Any of the doors just raises an editor that's already open. An
empty canvas and a property strip with no marks hint at where to start — an
empty strip read as a malfunction.

Desktop shortcut: `lilhelpers.exe --editor` works, but the exe requires
administrator rights, so Windows shows UAC before passing the command. Without
UAC — a shortcut to `cmd.exe /c "set __COMPAT_LAYER=RunAsInvoker && start "" "C:\path\lilhelpers.exe" --editor"`
("Run: Minimized"): the second instance starts unelevated and only passes the
command — the main window deliberately accepts this message from below
(UIPI), as it does drag-and-drop of files.

**Why not `BitBlt`.** In HDR mode the desktop is composed in scRGB or PQ, and
an ordinary capture returns that buffer **without tone mapping** — hence
washed-out or dark shots. That can't be fixed afterwards: the original is
already lost. So capture goes through Desktop Duplication, which returns the
output's colour space along with the pixels, and exposure is corrected once,
on input. The editor itself stays 8-bit.

**A mark is not a stroke in pixels but an object in a list.** The colour,
thickness and opacity of a rectangle can be changed an hour after it was
drawn, and undo and redo fall out of this naturally.

**Region selection freezes the screen first** and only then shows the frame
on top of the frozen image. So the dimming and the frame itself don't end up
in the result, and the selection is pixel-exact.

**The overlay decides what to capture, the gesture decides where it goes.** In
the same overlay: drag — a region; click without dragging — the window under
the cursor (highlighted before the click); click on the desktop or `Space` —
the whole monitor. The gesture at the moment of release picks the action:

| Gesture | Default |
|---|---|
| no key | open in the editor |
| `Shift` | copy to the clipboard — the editor doesn't open, "Copied" briefly appears by the cursor |
| `Alt` | edit right here, in the overlay |

The hint about `Shift` and `Alt` sits next to the coordinates and size, and
the action of the held modifier lights up in it. The assignment is changed on
the **Shots** tab with a "gesture × action" matrix: choosing an action taken
by another gesture swaps them, so each action is always reachable by exactly
one gesture. There too — whether to put shots taken without the editor into
the library as well (yes by default; the write happens after the picture is
already on the clipboard, so pasting doesn't wait).

**Loupe** (since 4.3.0). Scroll the wheel up and a magnified piece of the frame
appears next to the crosshair: ×4, then ×8 and ×16, without smoothing, with a
grid and the centre pixel highlighted. Wheel down shrinks the loupe, and at ×4
turns it off. Under the loupe — coordinates, frame size and pixel colour
(`#RRGGBB`) with a swatch. The arrow keys move the cursor by a pixel, with
`Shift` by 10, while dragging too. The loupe covers neither the point nor the
hint, and near the edge of the monitor it jumps to the other side. There is no
separate setting: the picker always opens without the loupe (owner's
decision).

**With a 3 s countdown** (since 4.3.0) — for menus and tooltips that collapse
from the hotkey itself. Double-click on a window or an empty spot (the whole
screen), and for a region — click and drag the frame a second time. The
overlay disappears, a 3-2-1 countdown runs in the corner of the monitor (it
doesn't get into the shot); during that time open the menu you need, and a
fresh frame is taken: the double-clicked window, the screen or the region.
Modifiers work the same as without the countdown. `Esc` cancels the countdown.
To tell a double click apart, a plain click on a window fires with a delay of
up to 0.4 s.

**Overlay** — the same editor over the frozen monitor, without a window. The
tool rail sits to the left of the frame and the property strip above it; if
there's no room they move to the opposite side, and if there's none there
either — inside the frame. There's no status bar and no right panel; no crop
tool either — the frame itself is the crop, and it can be adjusted with its
handles even after you've started drawing (`Ctrl+Z` brings the frame back
too). At the bottom of the rail is **"To the editor window"**, which moves the
shot together with its marks into the full window with tone, size, the library
and export, and below it, lowest of all, **Copy** (both `Enter` and `Ctrl+C`:
copy and close) — in the same place as in the window (CAPS-70). `Ctrl+S` —
save to the library and close. `Esc` peels off layers as in the window, and
the last one — per the setting: just close (the default) or, if there are
marks, save to the library first.

**Smoothness at 4K.** The selection overlay and the editing overlay don't
redraw the frozen frame on every mouse move: on opening it is composed once
into two in-memory layers — light and dimmed — and each frame is assembled
from copies of them. After that only what changed is redrawn: the guides, the
label, the edge and the delta of the frame, and in the overlay — the area
around the mark being drawn or dragged (if the shot has blur or pixelation,
the frame stays full: they depend on everything under them). The frame buffer
lives with the window. In the editor window the scaled shot (bicubic, over a
checkerboard) is cached and rebuilt only when the zoom, scroll, window size or
the shot itself changes, and dragging marks likewise redraws only the area
around them. At 4K the selection overlay frame went from 134 to 5–6 ms, the
editing overlay from 123 to 4–5 ms, a maximised window with a 4K shot from
~570 to ~10 ms; a test harness checks that a partially redrawn frame and the
cached view match the full uncached one pixel for pixel.

The overlay's result matches byte for byte what the path through the window
would give — not by coincidence but by construction: before any output the
document is cut by the frame with the same function as in the window, and the
marks are shifted to its origin. The harness checks this on a real frame with
blur right at the edge of the frame.

**Three formats go to the clipboard at once** — PNG, `CF_DIB` and
`CF_BITMAP` — because none of them alone is accepted everywhere. A file gets
PNG or JPEG, always at the original's size, not the window's.

The editor — both in the window and in the overlay — opens straight away with
the **rectangle** (CAPS-66): on a fresh shot there's nothing to select, and the
rectangle is what's needed most often. A saved document with marks opens on
**Select** — it's opened to fix what was drawn. The first `Esc` behaves as
before: it closes (or clears the selection), rather than switching the
rectangle to **Select**.

Tools: rectangle, ellipse, line, pencil, text, hide, marker, counter, stamp.
Each has a colour, three thicknesses and opacity, and closed shapes also have a
fill in a separate colour. `Shift` keeps a square, a circle, and an angle that
is a multiple of 45°. A tool stays active after drawing so you can place
several marks in a row; this is switchable on the **Shots** tab.

**Colour — a swatch button with a drop-down palette** (CAPS-41, CAPS-49). Eight
swatches in a row no longer eat the strip: there's one button with the current
colour, and behind it a palette with a name, eight colours and its own opacity
slider. Swatches everywhere are squares, like the palette cells (CAPS-66), and
the filling says what kind of colour it is: a **square outline** is a stroke
(line, pencil, shape border, text outline), a **solid square** is a fill or the
colour of the mark itself (letters, stamp, marker). The order is the same
everywhere: outline on the left, solid on the right — so for text the outline
is on the left and the letter colour on the right (CAPS-67). The counter has
its own button look — a square with a number.

**Effects — shadow and glow** (since 4.8.0). The **Effects** button in the
strip opens two rows of three presets: shadow — none, light, strong (a soft
dark shadow offset down-right) and glow — none, light, strong (a white halo
around, so the mark "lifts off" a dark background). An effect is a property
of the mark itself, like colour or thickness: it can be changed on an already
drawn mark, undone in one step, is copied along with it, survives saving and
ends up both in PNG and in video (in the player and in the export). A choice
made with no mark selected becomes the default for the next ones; by default
there are no effects. Hide and marker have no effects — they are operations on
the shot's pixels, not strokes. Technically the mark is drawn by the same code
into a tile with alpha, a mask is taken from the alpha, offset, blurred and
tinted — and laid under the mark; offset and radius are in shot pixels, so the
file doesn't depend on the on-screen zoom, and the shadow doesn't rotate with
the mark (the light comes from above).

Text and marker **remember their colours separately** from the other tools:
changing the rectangle's colour doesn't repaint the next text, and vice versa.
A semi-transparent colour sits on a checkerboard, so the opacity is visible
without opening the palette. Choosing a colour closes the palette; the slider
doesn't.

Next to it is a **second button** — the second colour; each kind has its own:

| Mark | Second button | "None" / "Auto" | Own opacity |
|---|---|---|---|
| rectangle, ellipse | **Fill** | no fill | yes |
| text | **Outline** | no outline | yes (relative to the letters) |
| counter | **Number** | "Auto" — black or white by contrast | no: the number is part of the circle |

For the rectangle and ellipse the fill and the border are independent: a
semi-transparent yellow inside with an opaque red border is perfectly normal.
The border has "None" too — then it's a solid plate, which is what the old
"Fill" used to be; the palette itself won't let you remove both at once (the
shape would vanish). The colours of border, fill, outline and number are
remembered separately and passed to the next mark of the same kind. Images,
emoji, blur and pixelation have no colour — the strip has only an **Opacity**
button there.

Between the two colour buttons is a **swap ⇄** (`X`): the colours swap places
together with their opacity. "Border without fill" becomes "fill without
border" in one click, "white with a black outline" becomes "black with a
white one", the counter circle swaps colour with its number ("Auto" first
turns into the colour you actually see). For text without an outline the swap
is disabled: there's no such thing as "no letters".

**Thickness** (marker height, circle and stamp size) is also a list button.
The icon shows three thicknesses side by side with the current one dark; the
**`[`** and **`]`** keys make it thinner and thicker. When a shape's border is
off, the icon fades: thickness has no effect then.

**A strip without borders** (CAPS-65): buttons at rest have no border or
background, on hover a light backing, selected and open ones an accent
backing. Groups are separated by thin vertical rules. The list arrow stayed,
but smaller and paler: without a border it alone says "a click opens a list".

The keyboard follows the button position: **`1`–`8`** — the colour of the left
button (border, text outline), **`Shift+1`–`8`** — of the right one (fill,
letter colour, number); **`0`** and **`Shift+0`** — "None" in the left or the
right. Documents saved before 3.33 open as they were: the old "Fill" becomes a
shape without a border, light and dark outlines become white and black.

**Line style and arrowheads** — in the strip's drop-down selectors: the button
shows the current choice, the rest of the options are behind it. Outlined
shapes can be solid, dashed or dash-dotted.

There's no separate "arrow" — **arrowheads are a property of the line**, and
the front and back ones are set INDEPENDENTLY: each can be a triangle, a
"bird", a dot, or absent altogether. So the same line becomes a one-way arrow,
a double-headed arrow or a plain segment, even with different heads on each
end. The size is shared and set in shot pixels rather than pen widths, so a
big head stays big even on a thin line.

The line strip has two buttons: **Style** and **Ends** (CAPS-64). **Ends**
draws the whole line with both ends as it will appear on the shot, and "no
head" as a small stop tick: so the buttons don't blend together and the state
is visible without opening them. The **Ends** panel has three labelled rows:
**Start**, **End**, **Size**; it stays open while you click inside it, because
usually both start and end are set.

**Corners of rectangles and pasted images** (CAPS-58) — a drop-down
**Corners** button: no rounding, moderate, strong. The radius is in shot pixels
multiplied by the scale of the monitor the shot came from: on 4K at 150 %
moderate rounding looks the same as on 1080p. The scale is saved in the
document, so after reopening the corners stay the same. The radius is capped
at half the shorter side — a narrow frame doesn't become a pill. The choice for
a rectangle becomes the default for the next ones; the choice for an image
doesn't. A hide plate isn't rounded: the corners would have to be cut out
together with the shot's pixels.

**Text is readable on any background.** Text is drawn by DirectWrite, not
GDI+, and it has an outline in any palette colour (white by default). Red on a
dark screenshot without an outline is invisible; with a white one it's visible
everywhere. There's also size, bold, italic and alignment. It's typed in an
ordinary field right on the shot: `Enter` — done, `Shift+Enter` — new line,
`Esc` — cancel. A double click on the text reopens it for editing even an hour
later.

**`Shift` while resizing keeps the proportions.** A corner handle drives both
sides by whichever changed more; a side handle pulls its own axis while the
other grows from its centre. For a line, "proportion" is the angle: `Shift`
keeps the same eight axes at 45° as while drawing. The same works on the shared
frame of several selected marks.

The handles on the sides of text change the **block width**: stretch it and
lines start to wrap, and centre or right alignment finally makes sense. The
height always comes from the text itself; the letter size is set by the font
size.

**Hide a region** (`B`) — the reason screenshots get edited most often: to
remove a token, an address or someone's name. Three modes in one tool: blur,
pixels and a solid plate, with a strength slider.

⚠ **Only the plate is guaranteed irreversible.** Pixelation replaces a block
with its average — the original can't be recovered from it, so at the default
strength it's reliable too. Blur, however, is reversible in principle: a
strong enough inverse filter partially recovers what's under it. If you're
hiding something truly secret — use the plate or pixels, and leave blur for
cosmetics.

**Marker** (`H`) is dragged along a line of text and multiplies with the
background rather than lying on top: text under a yellow band stays black and
readable. Four colours, three band heights.

**Both effects see everything below them in order** — not only the shot
itself, but also marks drawn earlier, dropped-in images and a canvas turned
into an object. The marker multiplies with what actually lies under it, and
hide blurs or pixelates exactly the pixels visible on screen. What lies ABOVE
the effect it doesn't touch — that's the way to draw an arrow over a blurred
area.

A hidden area stays an ordinary mark: it can be moved or changed even an hour
later. It becomes irreversible only in a saved file or on the clipboard.

**Counter** (`N`) places numbered circles and adds one by itself. The number
isn't stored in the circle but computed from the order: delete the first — the
rest shift; change the starting number — the whole group renumbers; open
another shot — numbering starts over by itself. There can be several groups,
each with its own numbering: the "new group" button at the end of the strip
starts another, and clicking any circle of an old one returns to it. The
property strip shows which group you're in, what number it starts from and
which number comes next.

**Counter shape** (CAPS-68) — a list in the strip: circle, rounded rectangle
or "pin" — a drop with a tail. The pin is placed with its tip at the click
point, and with the rotation handle it can point anywhere; only the shape
rotates, the number stays upright (the same for the rectangle).

A group can be edited as a whole: the toggle before the colour enables a mode
in which colour, shape, number colour, size and opacity go to all the circles
of the group, and a double click on a colour in the palette or on the size
does the same once, without enabling the mode. The "minus in a circle" at the
end of the strip removes the whole group — in one step, so one `Ctrl+Z` too.

**Stamps** (`S`) — six custom outlines (tick, cross, question mark,
exclamation mark, star, warning triangle) and emoji. Outlines take their colour
from the palette and scale without loss. Emoji have their own colours, so they
have no palette — only **Opacity**; the four most frequent sit right in the
strip, the rest behind the "…" button.

**Crop** (`C`) trims the shot without touching it. Outside the crop you see
dimming, inside — rule-of-thirds lines; aspect ratios Free, 16:9, 4:3 or 1:1.
The confirmation sits on the crop itself, next to its size in the original's
pixels, and until you confirm, **Open**, **Copy** and **Save** stay silent.

Crop is a property of the frame, not an operation on pixels: marks outside it
don't go anywhere, the status bar shows how many of them are currently
outside the crop, and **Reset the frame** puts everything back even after a
dozen other actions.

**Duplicate** the selected mark — with the button next to delete or `Ctrl+D`.

**The library** opens with the **Open** button right over the canvas — in the
same window, with the same rail and status bar; the back button and `Esc`
return to the shot. A grid of cards, four per row, newest first: thumbnail,
name, time, size and number of marks. On the right — the selected shot large,
its facts and actions: **Open** (`Enter`, double click), **Show in Explorer**,
**Delete** (`Delete` — to the Recycle Bin, `Shift+Delete` — permanently;
confirmation in place of the buttons, no separate window). `F2` or the pencil
renames right in the card; the name lives inside the file, while the file
name stays the date and time. The shot currently on the canvas is marked "in
the editor", and **Open** simply returns to the editor. The thumbnail is
written into the file itself on save (the `THMB` block), so the library
doesn't read full shots and doesn't breed extra files. No grouping by day and
no search — per the owner's decision, until they're needed.

How many to keep — on the **Shots** tab in the settings: **either** "the last
N" (100 by default) **or** "no more than N MB". The oldest go to the Recycle
Bin when a new one is saved; the one that's open is never touched. There too —
**Show folder** and **Clear…**.

**Three main buttons, and each does the main thing right away** (CAPS-70). On
the left of the status bar sit **Open** and **Save** side by side — both about
files. **Open** opens the library of your shots; its list has **Import…** (a
foreign picture from a file), **From clipboard**, **Region shot**. **Save**
(`Ctrl+S`) puts the shot **together with its marks** into the library — a
folder in `%LOCALAPPDATA%\Little Helpers\Library`; its list has **Export…**
(`Ctrl+Shift+S`, PNG or JPG for those who just need a picture) and **Save
as…** (our own format in any folder; after that `Ctrl+S` writes there).
**Copy** (`Ctrl+C`) is a square button at the very bottom of the left rail,
exactly where it is in the overlay: the hand goes to the same place wherever
the shot is being edited. When the window is short, the tool buttons shrink a
little while **Copy** stays at the bottom. After a successful save or copy the
button's icon turns into a **tick** for a second: a message in the middle of
the status bar wasn't in view after the click.

This isn't a picture: the file holds the original, all marks with their
parameters, the crop, tone and geometry, embedded images and counter
numbering. Open it — and keep moving the same arrow. Pressing again
**overwrites** the same entry rather than breeding copies; a new shot starts a
new entry.

The format is **versioned from the very first file** and built so old shots
can always be read: a file is a sequence of tagged blocks, and the reader skips
unknown blocks or fields. A mark's type is written as a **text tag**, not a
number: when the separate arrow was removed from the app, the ordinal numbers
shifted and the ellipse silently picked up someone else's tooltip — with
numbers in the file, the types in all previously saved shots would have been
just as silently mixed up. The tag table is guarded by a `static_assert`: a new
kind of mark won't let the app build until it has been added to the format.
The format version is 1.2 (since 4.8.0): the mark block gained fields for
effects, names and "hidden". They're written only when non-default, so a shot
without them is byte for byte what 1.1 wrote; an older build skips the new
fields and shows marks without effects and names, and 1.1 files read as
before.

Closing the editor with unsaved changes — it asks whether to save.

**Save a copy** (since 4.9.0) in the list next to **Save** puts a new library
entry with the current state — marks, crop, tone, metadata — named
"<name> (copy)", then "(copy 2)"…, and the editor goes on working with the
copy, as after "Save as" in ordinary apps: the original stays as it was, and
unsaved edits go into the copy. That's how you make variants from one shot —
say, marks in different colours.

**Import…** (`Ctrl+O`) from the list next to **Open** takes an image from a
file — PNG, JPEG, BMP, GIF, TIFF, WebP, and HEIC and AVIF if their codecs are
installed — and our own `.lhshot` shots, which are recognised by content, not
by extension. The list next to it — from the clipboard or from a fresh region
shot. The view zoom is right there: a slider, the current percentage and a
"100 %" button that shows the shot pixel for pixel.

**Image size and canvas size** — two dialogs in the right panel, above tone.
Width and height can be set both in pixels and in percent — **each axis has its
own percentage**, because with proportions off, "70 % wide and 100 % high"
can't be typed into one field. Resizing the image carries the marks along by
their coordinates, but line widths, circles and stamps keep their size:
otherwise font size 18 halved would become a 9 that isn't in the set. Text is
scaled with a separate checkbox. When the canvas grows, the existing shot
**becomes a separate object** — but only if there really is something in the
background: the second and later enlargements just change the canvas size and
no longer breed empty "images". The background around stays transparent — you
can leave it so or put a rectangle of the colour you need underneath. Both
actions are a single `Ctrl+Z` step, pixels included.

**The canvas edges are always visible**, and the transparent background under
the shot is filled with a barely noticeable white-grey checkerboard: after
enlarging the canvas it's immediately clear where it ends and that a mark has
gone past its edge. The "Keep proportions" checkbox in the size dialogs
remembers that you turned it off. Both dialogs have a dark theme along with
the rest of the window.

**Several marks at once.** `Shift`+click adds a mark to the selection and
removes it. When more than one is selected, the property strip changes: it
has only actions on several — align by edges or centres, distribute with equal
spacing, group. Clicking any member of a group selects the whole group; they
also move, get deleted and rotate together, in one `Ctrl+Z` step. You can
drag by any member: the clicked one becomes the main one (since 4.12.0,
CAPS-104) — motion is computed from it, so there is no jump, and the right
panel shows exactly what you grabbed.

Such a selection has a **shared** bounding box, and shared handles on it: the
corner and side ones stretch everyone at once, and the round handle above the
top edge rotates them around the frame's centre. Text, counter and stamp move
by their centre when stretched but keep their size — otherwise font sizes and
circles would drift into values that aren't in the set.

**A mark can be rotated** — the round handle above the top edge; `Shift` keeps
15° steps. Everything rotates except hide and marker: those take the shot's
pixels under them. A rotated mark is caught by clicking on the mark itself,
not on an invisible straight frame, and rotating the whole shot adds its own
90° to it.

**Crop fits the shot into the window by itself**: cropping blind with half the
frame off the edge is impossible.

**An image dropped into the window becomes a mark** — it can be moved, resized,
made semi-transparent and undone like any other. `Ctrl+V` pastes the same way,
and the **Image** button on the rail (`I`, CAPS-69) takes one from a file: it's
also there in the overlay, where you can't drag a file, and puts the picture in
the centre of the frame; replacing the whole shot remains a separate item in
the list next to **Open**. An image larger than the shot itself is scaled down
to fit.

**The arrow keys** move the selected mark by one shot pixel, and with `Shift`
by ten. A whole series of presses goes into one `Ctrl+Z` step, not twenty.

**A zoomed-in canvas scrolls with the wheel** — vertical, and sideways with the
horizontal wheel, `Ctrl` + wheel or `Shift` + wheel. The zoom is changed with
`Alt` + wheel (and the slider at the bottom): trying to scroll to the edge
shouldn't change the zoom. The canvas can also be dragged with the middle
button or with `Space`. While a shot is being taken, the editor hides so as not
to end up in its own frame.

**Right panels.** Since 4.6.0 they live as icon tabs in the strip on the right
(since 4.8.0 there are three). Only one can be open: clicking another icon
switches the panel, clicking the open one collapses it. When the editor opens,
all are collapsed.

- **Properties** (in video — **Video**): where the shot came from, its size,
  how many marks it has and whether it was HDR. There too — rotate 90° either
  way and mirror horizontally and vertically: pixels and marks go together, so
  an arrow keeps pointing at what it pointed at.
- **EXIF/META**: title, description, author, copyright, tags and date taken. A
  change is committed as soon as the field loses focus (Enter, Tab or a click
  elsewhere), and is immediately written to the library entry. The date taken
  is also the time of the file itself: both "created" and "modified". The
  fields go out into exported files: into PNG as `iTXt` text chunks, into JPEG
  as EXIF (including the tags Explorer shows), into MP4 as
  `moov/udta/ilst` metadata that players and Explorer read. **Remove all
  metadata** clears the fields, and then nothing goes into exported files, not
  even the date. Screenshots have no GPS coordinates, so there's nothing to
  remove separately. The metadata is written by the app itself, without system
  property handlers: third-party programs also register those for
  `.mp4`/`.png`, and foreign code has no place in a process with administrator
  rights. Since 4.8.0 the standard "software" field (PNG `Software`, EXIF
  `Software`, MP4 `©too`) always gets "Little Helpers x.y.z" — even when the
  other fields are empty; **Remove all metadata** removes it too.
- **Marks** (since 4.8.0): a list of all the document's marks in layer order —
  the top row is drawn over the rest. A row has the tool icon, a colour swatch,
  the name, and in video also the time span. A click selects the mark on the
  canvas (in video it also goes to a frame where it's visible), a selection on
  the canvas highlights the row and scrolls to it; `Shift`+click adds to the
  selection. A row can be dragged to another place — this changes the layer
  order, and a line shows where it will land. The default name is made of the
  kind and a number ("Rectangle 2", "Text: first words", "Counter 3"); a
  double click or `F2` renames in place, `Enter` commits, `Esc` cancels, an
  empty name restores the automatic one. The eye on the right hides the mark:
  it isn't drawn, isn't caught by the mouse and doesn't go into exports; one
  more click brings it back. Right-click on a row — a menu: rename, duplicate,
  bring to front or send to back, hide, delete. Each action is one undo step.
  Names and "hidden" are saved in `.lhshot` and `.lhvideo`. **A group is one
  node** (since 4.10.0, CAPS-100): "Group N · k" with nested rows, collapsed
  with a triangle; it's dragged only as a whole, and nothing can be inserted
  inside — the line snaps to the node's boundary; click, eye, name (double
  click) and menu on the node act on the whole group, and **Ungroup** is there
  too. When grouping, members are pulled together in z-order under the topmost
  of them (the same when opening old files), otherwise the node would promise
  a drawing order that doesn't exist. Group names are in the file (the `GRPN`
  block, format 1.3 / 1.2).

**Tone** — exposure (−2…+2 EV), gamma (0.50…2.00) and contrast (−50…+50). It's
a palliative and honestly stays one: it doesn't bring back what's lost, it only
pulls out what's still there in 8 bits — needed where automatic HDR mapping
didn't help or there's no original any more because some other program put a
finished picture on the clipboard. The original isn't touched: tone is a
recipe on top of it, so **Reset** returns exactly the captured frame, and
**Compare**, while the button is held, shows the source frame without any
intermediate file.

## Screen recording

Since 4.0.0. `Alt+Shift+5` opens the same selection frame as for shots: drag —
a region, click — a window, `Space` — the whole monitor. Recording starts right
away; **the same key stops it**. The same is in the tray: **Shots → Record
video**, and while recording, the first menu items are **Stop recording** and
**Pause recording**.

Since 4.11.0 a **pause / stop pill** sits by the bottom-right corner of the
frame (CAPS-101): pause (the button turns into "resume"), stop (the same as
the key) and a timer that excludes pauses. It sits outside the region so as
not to get into the frame; no room below — it goes above; no room anywhere
(the whole screen) — inside, but it is always excluded from capture, like the
frame. It's a separate clickable window without focus (the frame lets the
mouse through), and clicks on it don't get into the recording as cursor
clicks. While paused, the frame and the pill turn yellow and the timer blinks
— so you don't forget the recording is on hold. Pause is really a pause, not a
"hole": frames aren't written, after resuming the time shifts by the length of
the pause, so the file has neither a jump nor a frozen frame; sound likewise —
what was captured during the pause is dropped, and the track stays in step
with the frames. There is deliberately no key for pause (owner's decision,
26.09): only the button and the tray. In "follow the window" mode the pill
travels with the frame and hides with a minimised window.

While recording, a thin red frame outlines the region **from outside** — it
doesn't get into the picture (and is also excluded from capture). There's no
frame for the whole monitor. The tray icon has a red dot, and its tooltip the
duration. After stopping, the recording opens in the editor by itself (since
4.4.1). Only if the editor has unsaved work is it left alone: then "Video
saved — click to open" appears by the cursor.

**Window recording (since 4.5.0).** Clicking a window in the selection frame
records **the window itself**. Only it gets into the picture: windows covering
it aren't visible, and if it's moved, even to another monitor, the recording
follows it. The red indicator frame follows the window too. The video size is
the window size at the start. If the window is resized, the frame is fitted in
the centre with margins, without distorting proportions; a smaller window
isn't stretched. A minimised window gives no frames, so the video keeps the
last one. Close the window — recording stops by itself and is saved. Colour
and white level are taken from the monitor the window is currently on, so a
window moved between an HDR and an SDR monitor stays correctly exposed. On the
**Video** tab, "Clicked window: as a region" restores the old behaviour: the
screen rectangle where the window was at the start is recorded. Protected
content (DRM video, windows excluded from capture) comes out black: that's how
Windows itself delivers it. Requires Windows 10 1903 or newer; on older
versions the window is recorded as a region.

How it's recorded: Windows.Graphics.Capture, the frame comes straight in scRGB
(FP16). Then the same tone shader as for the screen, plus fitting. The yellow
border Windows uses to mark capture and the system cursor are turned off: we
draw the cursor ourselves. A test harness records its own window with real
frames: it gets moved, covered, narrowed, minimised and closed, and the
decoded MP4 shows exactly its colours in every phase.

**HDR.** The same compensation as for shots (SDR white level, scRGB and PQ),
but on the graphics card — with a shader: on the CPU, 4K@60 would mean half a
billion pixels per second. White stays white, nothing is blown out. The output
is H.264 in MP4, 8-bit, BT.709: HDR is dealt with once, on input.

**How it's recorded.** Desktop Duplication → a copy of the frame → the tone
shader → the hardware encoder via Media Foundation; the frame never leaves
video memory. The frame rate is constant (30 or 60): the screen delivers a
frame only when something has changed, and between changes the last one is
repeated. A keyframe every second. Fallbacks: BitBlt when duplication isn't
available (Remote Desktop), and a software encoder if there's no hardware one.

**Cursor and clicks** (since 4.3.0, both on by default). The screen delivers
frames without the cursor, so the cursor is drawn by the same shader, after
tone: on an HDR screen a white cursor stays white. A text cursor that inverts
the background inverts it in the video too, so it doesn't vanish on a dark
background. A click is a yellow (red, blue) dot with a ring spreading out for
~350 ms, a right click a double ring, and while the button is held (dragging)
a ring stays under the cursor. Clicks come from a mouse hook with the same
timestamps as the frames, so the ring appears in the frame of the click
itself. The click log goes into `.lhmeta` for the editor and the report later
on.

**Sound** (since 4.3.0, off by default). On the **Video** tab — **System
sound** (what you hear in the speakers or headphones) and **Microphone**, each
with its own device (default or chosen); a meter under the microphone shows
what it hears. Both sources are mixed into one AAC 48 kHz track: most players
and messengers only play the first. Each sound packet goes onto the timeline
by its own timestamp, so gaps (when nothing is playing, system sound is
completely silent) become silence rather than shifting the track; unplug the
headphones — recording continues with silence and picks up the new default
device. If the microphone is blocked in the Windows privacy settings or the
device is busy, recording goes on without it, and an explanation appears in
the tray.

**Where.** Into the shot library, as a clean MP4 — it can be opened with
anything and dragged into a messenger. The name, thumbnail and duration sit
next to it in a hidden `<name>.mp4.lhmeta`: renaming doesn't rewrite the big
file, and opening the library doesn't decode frames every time. A video card
has ▶ and the duration; the "All / Shots / Video" filter is in the library
header. **Open** — in the editor (since 4.1.0), **In player** — with the system
player. The file is written as `.mp4.part` and becomes an entry only once
complete; exiting the app mid-recording finishes the file.

**Settings** — the **Video** tab: an activity checkbox (off = the key is
released; if a recording is in progress, it's finished and saved), the key,
30/60 frames per second, quality (**Lower**, **Normal**, **High**), what a
click on a window records (follow it or a region), cursor and clicks, sound,
and the video size limit in the library (5 GB by default). The limit is
**separate from shots**: above it the oldest recordings go to the Recycle Bin,
a just-made recording is never touched, and it doesn't push out shots.

### Video in the editor

Since 4.1.0 a video opens in the same editor as shots: the mode follows the
document. Open a video — the title reads **Video editor**, under the canvas is
a timeline (to start, frame back, reverse, play, frame forward, to end; time,
ruler, a strip of thumbnails), the strip has speed 0.5× / 1× / 2×, loop and
sound, the right panel has info and **Open frame as a shot**. The rail is the
same, and its tools draw marks over the video (see below). **Copy** at the
bottom of the rail puts the current frame on the clipboard at full size.

| Key | What it does |
|---|---|
| `Space` | play / pause |
| `←` `→` | frame back / forward |
| `Shift` + `←` `→` | second back / forward |
| `Home` `End` | to start / to end |
| `J` `K` `L` | reverse / stop / forward |
| `S` | split before the current frame |
| `Delete` | cut the selection / restore a cut part |
| `I` `O` | start / end at the current frame |
| `Ctrl+Z` `Ctrl+Y` | undo / redo |
| `Ctrl+S` | save to the library |

Click and drag on the ruler seek exactly to a frame; the wheel over it zooms
around the cursor, over the strip it previews the frame and time. A video
opens by itself after recording, from the library, via **Open…** and by
dragging a file into the window.

**How.** The Media Foundation Media Engine delivers the frame straight into a
graphics-card texture, and it stays on the GPU all the way to the screen:
software decoding of 4K gave 18 frames/s, and copying the frame to memory
27.5. Reverse is our own, stepping backwards: the engine answers "yes" to a
negative rate, but MP4 plays forwards.

### Trim, cut, save (since 4.2.0)

On the right of the timeline button row are four buttons: **Split**, **Cut**,
**Start here** and **End here**. What's cut stays on the strip dimmed and
hatched: it's visible, but skipped during playback, reverse and frame
stepping.

- **Trim the start and the end:** drag the bracket handle at the edge of the
  strip or press `I` or `O` on the frame you want.
- **Cut a piece from the middle:** drag the mouse along the strip to select a
  span and press `Delete`.
- **Split:** `S` splits the video before the current frame. Clicking a part
  selects it, `Delete` cuts it, and on a cut part — restores it.
- **Undo / redo:** the buttons in the title bar or `Ctrl+Z` / `Ctrl+Y`.
  Everything is undone, handles included. `Esc` first clears the selection.

**Save** (`Ctrl+S`) since 4.4.0 saves the edits as a `.lhvideo` project, see
below. In 4.2.0 and 4.3.0 it put a new entry marked "(trimmed)" into the
library. A finished MP4 is in the list next to it:

- **Save as…** — MP4 into a chosen folder;
- **Copy as a file** — onto the clipboard as a file, so it can be pasted into a
  messenger, an email or a folder;
- **Save a copy** (since 4.9.0) — a new `.lhvideo` project in the library with
  the same video byte for byte and the current state (edits, marks, crop),
  named "(copy)"; from then on the editor works with the copy, the original
  isn't touched.

Progress is shown in the status bar. If you close the editor with unsaved
edits, it asks: **Yes** saves to the library and closes after writing.

**How.** The frames that remain are re-encoded: the decoder delivers an NV12
texture, and it goes to the hardware encoder without a copy to memory. 4K goes
roughly twice as fast as real time. Timestamps become continuous, a keyframe
every second. Sound is cut by the time of the segments, not by frames, so no
error accumulates at the joins. Every join gets a 10 ms fade against clicks.
Without edits, **Save** simply copies the file, losslessly. It's verified by
decoding: a number "baked" into every frame of a synthetic video must follow
the segments exactly.

### Marks over video (since 4.4.0)

The same tools as for shots: frame, ellipse, line with arrowheads, pencil,
text, hide, marker, counter, stamp and image. Cropping in video is one crop
for the whole recording — see "Crop and scale down video". Pause the video,
pick a tool and draw on the frame. The mark appears from this frame for 3 s,
and its bar appears on the track under the strip. The bar can be dragged to
move the mark in time, or dragged by an edge to change when it's visible.
Under the cursor the bar is highlighted, handles appear on its ends, and the
cursor shows what will happen: "↔" over an edge, "move" over the middle.
Clicking the bar selects the mark.

Marks that overlap in time go onto separate lanes, and bars never overlap. Up
to four lanes are visible. The rest scroll with the wheel over the lanes, and
a thin scrollbar then appears on the right. A taller window or a smaller video
makes room for more lanes: in video mode the zoom can go down to half of
**Fit**.

- **Editing.** While paused, the current frame becomes the editor's image, so
  everything familiar works: selection, handles, colours, thickness, order,
  duplicating and `Delete`. Only the marks alive on this frame are visible and
  reachable. With a drawing tool or a selected mark, the strip under the title
  shows the mark's properties, otherwise — playback.
- **During playback** marks are drawn by a transparent layer over the video.
  Hide is shown there as a hatched plate, because blurring every frame live is
  too expensive. The real blur is visible when paused and in the file.
- **Undo** is one for everything: both timeline edits and marks.
- **Save as…** and **Copy as a file** draw the marks into every frame of their
  span. Hide is computed from the pixels under it on each frame, by the same
  functions as in the screenshot editor, so the blur is "live", not frozen.
  **Copy** and **Open frame as a shot** take the frame together with the marks;
  in a shot they come across editable.

### The `.lhvideo` video project (since 4.4.0)

If there are timeline edits or marks, **Save** puts a project into the library
instead of a re-encoded MP4. It's the video together with everything done to
it: cuts, handles, marks, name, mouse log. Opened again from the library, the
project restores everything for editing. The old MP4 entry goes to the Recycle
Bin as soon as the video is closed. An already open project is updated by
**Save** in place and immediately.

A finished file to pass on — **Save as…** (MP4 into a chosen folder) or
**Copy as a file**. The cuts are already removed there, and the marks are
baked into the frames.

**How.** A project is a single file: the recorded MP4 byte for byte, followed
by an MP4 `uuid` box with the edits. Players skip unknown boxes, so a
`.lhvideo` renamed to `.mp4` plays in any player: the original video, without
the edits. Saving a project re-encodes nothing. The video is copied once, and
after that only the tail is rewritten: kilobytes, not gigabytes. The file's
trailer (`LHVEND` and the box offset) lets the tail be found without parsing
the MP4. The project version is 1.1 (since 4.8.0): the same new mark fields as
in `.lhshot` 1.2. A test harness checks that the video in the project is the
same byte for byte, that reopening restores parts and marks, and that the
tail is rewritten in place while the video is open.

### Crop and scale down video (since 4.7.1)

The **Crop** tool (`C`) works in video too: pause, outline the area you need
and confirm. There's one crop for the whole video, and the canvas really does
shrink, as with shots. **Video size…** in the properties panel scales the video
down to the width or height you need (proportions are kept, enlarging isn't
possible). The "After edits" row shows what you'll get. Both crop and size are
undone with `Ctrl+Z` together with everything else, saved in the `.lhvideo`
project and applied in **Save as…** and **Copy as a file**. **Open frame as a
shot** takes the frame at the source's full resolution, only cropped.

**How.** Crop and size live in "document" coordinates — the video scaled to
its current size. Sizes are always even (H.264 requires it). Scaling down is
area averaging, with no re-encoding until the export itself: only a `GEOM`
block goes into the project. On export the frame is cropped and scaled before
the marks, so the marks land exactly where they were drawn.

### Export to GIF (since 4.7.1)

In the **Save** list — **Export to GIF…**. The dialog is compact: frames per
second (5–30, no more than the source), width in pixels (height proportional;
by default no wider than 1280), colours (256/128/64/32), dithering, loop
forever or once. The fragment is set by trimming on the timeline, as for MP4:
cuts, crop and size are taken into account in the GIF, and marks are baked in.
Settings are remembered. Under the fields is an approximate file size; it's
recomputed while you change the settings, and while it's being computed —
"Calculating…". If it comes out above 25 MB, the dialog suggests reducing the
width or frame rate.

**How.** Our own encoder: a global palette by median cut from a dozen samples
across the whole fragment, one slot for transparency, optional
Floyd–Steinberg dithering, LZW in 255-byte blocks. Each subsequent frame is
written only where it changed (the rest is transparent), identical frames
simply extend the delay, and the last delay is adjusted so the total matches
the fragment's duration. The size estimate is honest: each sample is a pair of
adjacent output frames, the first gives a "full" frame, the second a delta,
and that's exactly how the whole file is computed. A test harness parses the
resulting GIF: frames are LZW-decoded, and the number baked into the synthetic
video must follow frame by frame.

### Browser log while recording (since 4.13.0)

While recording, Little Helpers can write the **DevTools log** of a page in Chrome
or Edge — console, errors, network requests, navigations — tied to the video time
(CAPS-83). It needs the browser extension: **Extension…** on the **Video** tab lays it
out from the app itself into `%LOCALAPPDATA%\Little Helpers\BrowserExtension` and
opens the folder; then `chrome://extensions` (in Edge — `edge://extensions`) →
**Developer mode** → **Load unpacked** → that folder. The **Write the DevTools log…**
checkbox next to it turns the feature on and off, and shows whether the extension
is connected. The log follows the **active tab** and switches with it.

How it works:

- **The channel is a WebSocket on 127.0.0.1 only** (port 47650). Native Messaging,
  where the plan started, does not work here: the browser starts the host as a
  normal process, and the app needs administrator rights. The server accepts only
  our extension's Origin — its ID is fixed by the key in `manifest.json`, and a web
  page cannot fake an Origin. The port cannot be reached from the machine's network
  address.
- **Sync goes by the system clock**, with no pings at all: console, exceptions and
  Log in the DevTools protocol carry milliseconds since the epoch, network — the
  request's `wallTime`. The recording thread notes the same clock on the first
  frame, and pauses (CAPS-101) are subtracted just as for frames and sound. Measured:
  an event lands in video time within ~1 ms.
- **The screen lags, the log does not.** The log shows when the page's code ran;
  the effect appears in the video on average ~90 ms later (the page frame,
  composition, capture). Measured by an end-to-end test with real Chrome: the page
  colour changes in the video 1–3 frames after a `console.log` made at the same
  moment.
- **Never written:** request and response headers and bodies (including
  `Authorization` and `Cookie`). URLs are written in full; the report export hides
  secrets in them.
- The log sits next to the recording in `.lhmeta` (the `DEVT` block) and in the
  `.lhvideo` project. In the video editor it shows as ticks on the marks track:
  errors red, warnings amber, navigations blue, network grey; **Details** shows the
  number of events.
- While the extension is attached to a tab, the browser shows the "extension is
  debugging this browser" bar — a platform limitation, and it ends up in the
  recording.

### Report for developers (since 4.13.0)

**Save ▾ → Report for developers…** in the video editor (CAPS-84) builds one thing for a
bug report: the edited video (with cuts and marks) and the log remapped into its time.
The dialog shows what goes into the report (duration, approximate size, how many
events, errors and clicks, which domains) and offers three formats:

- **A `.lhreport` file** — an uncompressed ZIP: `meta.json`, `log.jsonl`,
  `clicks.json`, `report.html`, `video.mp4`. A double click opens it in Little
  Helpers (the video becomes a new recording in the library, with its log and
  clicks); the viewer in the extension itself (**Open a report…** in its menu) plays
  the video straight from the archive, with no unpacking and no size limit; and
  without either, renaming the file to `.zip` and opening `report.html` is enough.
- **A single HTML file** — the video inside (base64), up to ~100 MB: browsers handle
  bigger ones poorly, so for large recordings this option is off.
- **A folder** — the same as in the archive, as separate files.

The report is one page with no dependencies: the video on the left, a strip with
ticks under it (errors, warnings, network, navigations, clicks), the log on the right
with filters and search. The current event is highlighted, later ones are dimmed, and
the log scrolls with the video by itself; clicking a row seeks the video to the event
and expands its details. Keys: Space — play/pause, `N`/`P` — next/previous event,
`E` — next error. The language follows the app, the theme follows the system.

**Privacy.** **Hide secrets in URLs** (on by default) replaces with `•••` the values
of parameters whose names contain token, key, secret, password, auth, sig, session,
code and the like — in any field, console text included. Other page data (console
text, URLs) stays, and the dialog says so.

**Double click without UAC.** The app runs with administrator rights, so a direct
file association would ask UAC on every double click. Instead Little Helpers
registers `.lhreport`, `.lhvideo` and `.lhshot` for the current user through a silent
forwarder: `conhost --headless` starts the same exe without elevation
(`__COMPAT_LAYER=RunAsInvoker`), which only hands the path to the running app
(`WM_COPYDATA`) and exits — no windows at all. If the app is not running, it starts
normally (one UAC prompt) with that file already.

## Settings

- **Tab pages scroll** (since 4.9.0): the wheel over a page or a thin bar on
  the right, which appears only when the content is longer than the page;
  `Tab` onto a control outside the visible part scrolls to it. The window
  doesn't grow because of this, and a new option on any tab no longer runs
  into the footer. Since 4.13.1 (CAPS-106) each tab is its own container
  window, and the controls live on a "canvas" inside it; scrolling just moves
  the canvas, and the system itself clips everything outside the page.
  Previously the controls were children of the main window clipped by window
  regions: while scrolling they overlapped the header and footer and left
  "ghosts". Scrolling now stops at the last control instead of running into
  empty space. Since 4.11.0 (CAPS-103) the **Video** tab fits at
  100 % without scrolling (**Show folder** is in the "Where recordings go"
  header row), and the bar sits right against the page border: a gap of page
  colour between the dark track and the border read as a light line in the
  dark theme.
- **"Start when you sign in to Windows"** = the Task Scheduler task
  **`lilhelpers`** (trigger "at log on", RunLevel HIGHEST) — starts elevated
  WITHOUT a UAC prompt. The registry Run key doesn't work for elevated
  programs, hence the task. It's created via the scheduler's COM API rather
  than by running `schtasks.exe`: a child process registering a task with the
  highest privileges is a typical malware persistence pattern that antivirus
  heuristics react to. COM also makes it possible to fix scheduler defaults
  that are harmful for a background app: don't block start-up on battery and
  don't kill the process after 3 days of running (`ExecutionTimeLimit`).
- **Language**: **System** (default), **Українська**, **English**. "System" is
  the Windows UI language (`GetUserDefaultUILanguage`) if it's among the
  translations; otherwise English. The choice applies immediately, without a
  restart: the window isn't recreated, only the labels are rewritten, because
  control positions and widths are the same for both languages. That's why
  **the English string in the translation table must not be longer than the
  Ukrainian one** — the Ukrainian one sets the field width, and a longer
  translation doesn't wrap but is silently cut off. The strings themselves are
  one `LH_STRINGS` list in the code: an X-macro generates both `enum Str` and
  both tables from it, so a translation can't drift from its name or order.
- **Window theme**: **Automatic** (follows the Windows app theme; changes are
  picked up immediately — including from our own day/night feature), **Always
  light**, **Always dark**. Windows only makes the title bar (DWM) and a few
  controls dark via the `DarkMode_Explorer`/`DarkMode_CFD` themes; the rest —
  background, canvas and the tabs themselves, checkboxes/radio buttons,
  sliders, time pickers, disabled buttons — is drawn by hand (the Notepad++
  approach).
- **Updates** — **Check for updates daily** (on by default) + the **Check
  now**, **Update**, **Roll back** buttons:
  - **Check**: `GET api.github.com/repos/V-Plum/lilhelpers/releases/latest`,
    the tag is compared with the version from VERSIONINFO. Once a day (a minute
    after start-up, then every 30 min it checks whether a day has passed) or on
    the button.
  - **The notification reaches a person** (CAPS-63). A balloon about a found
    version appears in the tray only when someone is at the computer (last
    input < 5 min): a night-time check postpones it until the first activity.
    A reminder once a day until the version is installed; turned off with the
    daily-check checkbox. While a version is available, the tray icon tooltip
    says "version X available", the tray menu's first item is "Update to X",
    and clicking the balloon opens the settings. The found version is stored in
    the registry (`UpdateAvailableTag`): after a restart **Update** is available
    right away, and a failure of the next check doesn't clear it. The **Update
    check log** in the settings shows the latest entries of
    `%LOCALAPPDATA%\Little Helpers\updates.log`: when it checked, how it ended,
    what was shown.
  - **Authenticity without Authenticode.** CI signs `lilhelpers.exe` with an
    ECDSA P-256 key (the private one lives only in the GitHub Secret
    `LILHELPERS_SIGNING_KEY`) and puts `lilhelpers.exe.sig` next to it. The app
    computes the file's SHA-256 with the built-in BCrypt and verifies the
    signature with the embedded public key (`lilhelpers_signing_pub.pem` in the
    repo; CI checks it against the constant in the code). An update can only be
    forged by stealing the secret.
  - **Replacement**: Windows allows renaming a running exe → the current one
    becomes `lilhelpers.exe.old`, the new one takes its place and is started
    with `--after-update <pid>` (it waits for the old one to exit, because of
    the single-instance mutex), and the old one exits. The process is already
    elevated, so there's no new UAC; the autostart task points to the same
    path. `.old` stays for **Roll back**.
  - SmartScreen doesn't interfere: a file written by the app itself has no
    Mark-of-the-Web. The risk of antivirus false positives is the same as with
    a manual download.

All settings are in `HKCU\Software\lilhelpers`. The window header shows the
version from the exe's own VERSIONINFO and a link to the repository.

## Usage

- `lilhelpers.exe` — lives in the tray. A left click on the icon or launching
  the exe again — the settings window (tabs **Layout**, **Cursor**,
  **Day/night**, **Preview**, **Shots**, **Video** and **Settings**). Right
  click — a menu: "Update to X" when there's a new version; **Stop recording**
  and **Pause recording** while recording; **Settings…**; the **Shots** submenu
  (screen, window, region, from clipboard, record video, editor); **Exit**.
  Everything switches live, without a restart; the window can be closed — the
  app stays in the tray.
- **The tray icon survives both an Explorer restart and the race at sign-in.**
  Before 2.2.2 it survived neither, even though a `TaskbarCreated` handler was
  in the code: Explorer runs with normal rights, the app with admin rights, and
  UIPI doesn't let a broadcast message through from below. Measured:
  `PostMessage` of this message to the app's window from an unprivileged
  session returns `ERROR_ACCESS_DENIED`, i.e. the handler was dead code. Now
  the window explicitly allows this particular message
  (`ChangeWindowMessageFilterEx`). The second half of the problem: autostart
  fires at sign-in at the same time as Explorer, and if
  `Shell_NotifyIcon(NIM_ADD)` is called before the taskbar appears, it simply
  fails — previously nobody checked its result. Now adding is retried every
  2 s until it succeeds. The "icon already there" failure is told apart from
  "no taskbar yet" via `NIM_MODIFY`, otherwise the retry would spin in vain.
- A single instance (named mutex `lilhelpers_single_instance`).

## Ready-made exe

The latest release is on the [Releases](../../releases) tab: `lilhelpers.exe`
is built by GitHub Actions on `windows-latest` and attached to the release. A
new release = pushing a tag:

```powershell
git tag v4.11.0
git push origin v4.11.0
```

Running the workflow manually (Actions tab → build → Run workflow) only builds
the exe and puts it in the run's artifacts; it doesn't create a release.

The exe isn't signed with a certificate, so on the first manual launch Windows
SmartScreen shows a warning ("More info" → "Run anyway"). Updates from within
the app aren't affected by SmartScreen.

### Antivirus false positives

An unsigned binary with zero reputation that asks for administrator rights,
intercepts a global key and registers itself for autostart looks to ML
heuristics like a keylogger with persistence — Defender flagged the first
version as `Trojan:Win32/Wacatac.B!ml` (a typical catch-all for false
positives). What's been done so the file's profile doesn't look anonymous:

- VERSIONINFO metadata (product, version, author, copyright);
- release builds with MSVC rather than MinGW;
- autostart via the scheduler's COM API instead of running `schtasks.exe`.

Only code signing with a certificate settles the issue for good. If a
detection recurs, it's worth submitting it as a false positive to
[Microsoft Security Intelligence](https://www.microsoft.com/en-us/wdsi/filesubmission).

## Icon

The master file is `lilhelpers.svg`. After editing it:

```powershell
powershell -ExecutionPolicy Bypass -File make_icon.ps1   # SVG -> ico + png
powershell -ExecutionPolicy Bypass -File build.ps1       # rebuild the exe
```

`make_icon.ps1` doesn't redraw anything: it renders the vector at each target
size (rather than scaling down a raster) and packs the frames into an `.ico`.
The sizes cover the tray and shell icons at all common DPI scales —
100/125/150/175/200% — so Windows picks a ready frame rather than scaling a
neighbouring one. Small frames are stored as 32-bit DIBs, large ones (from
96 px) as PNG-compressed. Rendering requires Chrome or Edge (headless). To
look at another icon without touching the current one:
`make_icon.ps1 -Svg other.svg -OutDir preview -KeepPngs`.

## Building locally

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

`build.ps1` uses MSVC if it finds it via `vswhere`, otherwise it falls back to
mingw-w64 (`g++`/`windres` from PATH or LLVM-MinGW from winget:
`winget install MartinStorsjo.LLVM-MinGW.UCRT`). To force one —
`-Toolchain msvc` or `-Toolchain mingw`. The result in both cases is a single
self-contained `lilhelpers.exe` (~1.4 MB in the MSVC release build, static CRT,
no external DLLs).

## Files

| File | What it is |
|---|---|
| `README.md`, `README.en.md` | this description in Ukrainian and English |
| `lilhelpers.cpp` | all the code (layout, cursor, day/night, preview, shots and editor, video recording and editor, window theme, localisation, updates) |
| `browser-extension/` | the Chrome/Edge extension (DevTools log, CAPS-83) and the report viewer (`report.*`, `viewer.*`, CAPS-84); built into the exe as resources |
| `lilhelpers.manifest` | requireAdministrator + dpiAware + visual styles |
| `lilhelpers.rc` | icon, manifest, PNG logo and VERSIONINFO into the exe's resources |
| `build.ps1` | build: MSVC, falling back to mingw-w64 |
| `lilhelpers.svg` | **icon source** (vector master file) |
| `make_icon.ps1` | renders SVG → `lilhelpers.ico` (12 frames) + `lilhelpers.png` |
| `lilhelpers.ico` | exe and tray icon: 16/20/24/28/32/40/48/56/64/96/128/256 |
| `lilhelpers.png` | the same logo at 256 px; embedded in the exe and drawn in the window header (GDI+) |
| `lilhelpers_signing_pub.pem` | public key for verifying release signatures |
| `LICENSE` | terms of use: the source is open for reading, not for reuse |
| `screenshot.png`, `screenshot-dark.png` | the window: light theme in Ukrainian, dark theme in English |
| `screenshot-peek-*.png` | the preview window: STL, SVG, Markdown, code, file card |
| `.github/workflows/release.yml` | CI: build on Windows, signing, release on a `v*` tag |

## License

The code is open **for reading, not for reuse**.

The app runs with administrator rights and updates itself. Whoever installs it
should be able to read what exactly it does and check the released exe against
the code it was supposedly built from. That's why the code is in plain sight.

**You may:** read, study and audit the code; build it yourself and run your own
build on your machines; report problems and propose changes to this project.

**You may not, without the author's written permission:** distribute the code
or parts of it, distribute binaries built from it, publish derivative works or
put pieces of this code into other projects, train models on it, remove the
authorship or the release signature check.

Full text — [LICENSE](LICENSE).

Two things worth knowing honestly:

- While the repository is public, **GitHub's own terms** give every user the
  right to view it and fork it within GitHub. That right comes from the
  agreement with GitHub, not from our license, and the license text can't
  revoke it. The license governs everything else — any use and distribution
  outside GitHub.
- Versions up to and including 2.6.0 were released under **MIT**. That
  permission is irrevocable and still applies to those versions. The new terms
  apply from 3.0.0.

## Known limitations

- Hide in video is shown as a plate during playback, and the real blur only
  while paused and in the saved file.
- "Follow the window" mode needs Windows 10
  1903 or newer; on older versions the window is recorded as the region where
  it was at the start. A minimised window gives no frames — the video keeps
  the last one.
- **Save** to a `.lhvideo` project doesn't re-encode the video, but a finished
  file (**Save as…**, **Copy as a file**, GIF) is re-encoded. Sound in
  recording and trimming has been verified mostly with synthetic timestamped
  input; there have been fewer checks with real devices so far.
- Reverse in the video editor steps backwards, so at 4K it's noticeably jerky.
- Over Remote Desktop, when its window is minimised or the session is locked,
  the screen gives frames to no one — neither duplication nor BitBlt.
  Recording then doesn't start and says so plainly.
- The monitor on which the region is selected is recorded; a monitor rotated
  by 90° isn't supported yet.
- The editor's file formats (`.lhshot`, `.lhvideo`) still grow: new versions add
  fields, and old files keep opening (see the notes on format versions).
- Tone works on the 8-bit channels of an already mapped frame. What was
  bleached into pure white during mapping can't be brought back by any slider
  — it only redistributes what's left.
- Rotating and mirroring move marks along with the shot, but text, the counter
  circle and stamps don't turn on their side: only their position moves.
- Emoji are drawn by the system Segoe UI Emoji font, so their look depends on
  the Windows version.
- Rotating several marks together rotates their POSITIONS around the frame's
  centre and adds to each one's own angle. Hide and marker have no angle of
  their own — they only move. Stretching several already rotated marks together
  is computed from their straight bounding boxes, so for strongly tilted ones
  it's approximate.
- Text with no set width doesn't wrap by itself: the user breaks lines with
  `Shift+Enter`. A width set with the handles can only be returned to "fit the
  text" by undo. There's one font — the system Segoe UI.
- The editor window is wide (at least 1200 points): the property strip doesn't
  fit into a narrower one. On a small screen the minimum gives way to the
  screen size, and then the outermost buttons of the strip get cut off.
- The editor's title bar is drawn by the app itself, not by the system. So it
  doesn't change along with non-standard window themes — it only follows the
  app's own light or dark theme.
- HDR: the SDR white level and the frame format are shown in the editor's
  right panel next to the "HDR" label. If a shot still doesn't look like the
  screen, those are the numbers that are needed.
- The monitor the cursor is currently on is captured. The whole virtual
  desktop is deliberately not stitched: neighbouring monitors can have
  different colour spaces and different white levels, and "one shot" of them
  would be a splice of two different exposures.
- **There is no system share menu and there won't be.** It existed, didn't
  work, and was removed in 3.24.0. The reason is architectural: the app runs
  with administrator rights (needed to intercept CapsLock), while the share
  menu and the share targets themselves don't, and COM doesn't let their calls
  through "from below". Measurements on a live machine showed that Windows
  opens the menu but **doesn't ask the app for data at all** — the target
  receives nothing. Allowing calls from below (`CoInitializeSecurity` with a
  permissive descriptor) didn't help either. Instead — **Copy** (`Ctrl+C`) and
  **Export**: both always work.
- JPEG quality is fixed (92). A slider will come with the rest of the editor
  settings.
- If someone else has already registered CapsLock, there will be an error at
  start-up (this is deliberate: more honest than a silent conflict of two
  hooks).
- The autostart task is triggered by the sign-in of any user of the machine;
  on a single-user machine this doesn't matter.
- In the tray at 100 % scale (16 px) the icon's black tile "eats" part of the
  margin around the letters — at 20 px and above the balance is fine.
- The preview is experimental (see the section above): it's turned off with a
  checkbox, and then Space belongs entirely to Explorer.
- The preview reads the file on the UI thread, so a very slow network folder
  freezes the app window for the duration of the read. Key interception isn't
  affected — the hook lives on its own thread — but the preview itself appears
  with a delay.
- The preview has no browsing of its own: files are browsed with the arrows in
  Explorer itself. This is deliberate (see "Space-bar file preview"), but it
  means clicking the preview window doesn't switch anything.
