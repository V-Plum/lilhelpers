// Little Helpers (lilhelpers) — дрібні зручності для Windows 11 в одному треї:
// розкладка по CapsLock, пошук курсора трусінням, день/ніч, темна тема вікна,
// перегляд файлу по пробілу, автооновлення.
//
// Механізм розкладки: low-level клавіатурний хук, який ковтає CapsLock (повертає 1) і
// віддає роботу головному потоку. RegisterHotKey тут НЕ підходить, хоч і
// виглядає охайніше: він перехоплює доставку повідомлення, але сам тогл
// Caps Lock відбувається рівнем нижче й однаково спрацьовує, тобто після
// кожного непарного перемикання розкладки лишався ввімкнений капс. Скасувати
// той тогл ін'єкцією CapsLock теж не вийде — власну ін'єкцію з'їдає власна ж
// реєстрація хоткея (перевірено: SendInput проходить, WM_HOTKEY не приходить,
// стан не змінюється). Хук — єдиний спосіб не дати капсу перемкнутися.
//
// Shift+CapsLock хук пропускає далі → лишається звичайним Caps Lock.
//
// Ціна хука: Windows знімає його, якщо колбек не встигає за
// LowLevelHooksTimeout (~300 мс). Тому, по-перше, колбек не робить нічого, крім
// перевірки клавіші й PostMessage; по-друге, хук живе на ОКРЕМОМУ потоці з
// власним циклом повідомлень — щоб зайнятість UI-потоку (наприклад, синхронні
// COM-виклики планувальника при вмиканні автозапуску) не могла задушити колбек.
//
// Маніфест requireAdministrator: без нього UIPI блокує
// WM_INPUTLANGCHANGEREQUEST у бік elevated-вікон (адмінський термінал тощо).
// Автозапуск — задача Task Scheduler з RL HIGHEST (Run-ключ реєстру для
// elevated-програм Windows ігнорує, а задача стартує без UAC-промпта).

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <taskschd.h>
#include <gdiplus.h>
// MinGW затягує їх транзитивно, MSVC — ні: sqrt() у детекторі жесту й _wtoi()
// у полях «Детально» інакше валять саме релізну збірку, а не локальну.
#include <math.h>
#include <stdlib.h>
// CAPS-7: день/ніч — геолокація за IP (WinINet), Location API (COM), час, форматування.
#include <wininet.h>
#include <locationapi.h>   // лише інтерфейси; GUID-и нижче свої — SDK MSVC тримає їх у locationapi.lib,
                           // MinGW — у заголовку, і сходяться вони лише через власні копії
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
// CAPS-8: темна тема самого вікна — DWM-заголовок, тема контролів, гліфи чекбоксів.
#include <dwmapi.h>
#include <uxtheme.h>
#include <vsstyle.h>
// CAPS-10: автооновлення — SHA-256 і перевірка ECDSA-підпису вбудованим BCrypt.
#include <bcrypt.h>
#include <vector>
// CAPS-16: перегляд по пробілу — виділення Провідника (IShellWindows → IShellView → CF_HDROP), значки.
#include <exdisp.h>
#include <shlobj.h>
#include <servprov.h>
#include <windowsx.h>
// CAPS-16: рендер SVG — системний Direct2D (ніякого чужого коду в процесі).
#include <d2d1_3.h>
#include <d2d1svg.h>
#include <wincodec.h>
#include <string>
#include <float.h>
// CAPS-16: кадр і метадані відео — Media Foundation, теж системна.
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
// CAPS-16: docx — читаємо пакет системним OPC, а не власним розпакувальником zip.
#include <msopc.h>
// CAPS-16: PDF — вбудований Windows.Data.Pdf (WinRT). Заголовка windows.data.pdf.h
// у MinGW немає, тому потрібні інтерфейси оголошено нижче вручну.
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <asyncinfo.h>
#include <shcore.h>
// CAPS-16: Markdown і підсвітка коду — через RichEdit, якому згодовується RTF.
#include <richedit.h>

namespace {

constexpr UINT WMAPP_TRAY         = WM_APP + 1;
constexpr UINT WMAPP_SHOWSETTINGS = WM_APP + 2;
constexpr UINT WMAPP_SWITCH       = WM_APP + 3;
constexpr UINT WMAPP_SHAKE        = WM_APP + 4;   // від мишачого хука: жест розпізнано
constexpr UINT WMAPP_MAGDONE      = WM_APP + 5;   // потік анімації: зменшення завершено
constexpr UINT WMAPP_THEMELOC     = WM_APP + 6;   // потік геолокації: lp = LocResult* (heap)
constexpr UINT WMAPP_UPDATE       = WM_APP + 7;   // потік оновлення: lp = UpdResult* (heap)
constexpr UINT WMAPP_PEEK         = WM_APP + 8;   // CAPS-16: від хука — пробіл/Esc у списку файлів; lp = SHELLDLL_DefView
constexpr UINT HKW_INSTALL        = WM_APP + 20;  // до вікна потоку хука
constexpr UINT HKW_UNINSTALL      = WM_APP + 21;
constexpr UINT HKW_MOUSE_ON       = WM_APP + 22;
constexpr UINT HKW_MOUSE_OFF      = WM_APP + 23;
constexpr int  IDC_AUTOSTART   = 100;
constexpr int  IDC_COPYRIGHT   = 101;
constexpr int  IDC_MODE_HOOK   = 102;
constexpr int  IDC_MODE_HOTKEY = 103;
constexpr int  IDC_MODE_HINT   = 104;
constexpr int  IDC_PASSTHROUGH      = 105;
constexpr int  IDC_PASSTHROUGH_HINT = 106;
constexpr int  IDC_LAYOUT_ENABLE    = 107;  // CAPS-9: «Переключати розкладки з Caps Lock»
// CAPS-2: вкладка «Курсор»
constexpr int  IDC_TABS          = 110;
constexpr int  IDC_CUR_ENABLE    = 111;
constexpr int  IDC_CUR_SCALE     = 112;
constexpr int  IDC_CUR_HOLD      = 113;
constexpr int  IDC_CUR_ADVANCED  = 114;
constexpr int  IDC_CUR_WINDOWMS  = 115;
constexpr int  IDC_CUR_DIST      = 116;
constexpr int  IDC_CUR_FACTOR    = 117;
constexpr int  IDC_CUR_REVERSALS = 118;
constexpr int  IDC_CUR_SHRINK    = 119;
constexpr int  IDC_CUR_OVERLAY   = 121;
constexpr int  IDC_HINT_GRAY     = 120;  // будь-який сірий пояснювальний текст
// CAPS-7: вкладка «День/ніч»
constexpr int  IDC_TH_ENABLE     = 130;
constexpr int  IDC_TH_BY_SUN     = 131;
constexpr int  IDC_TH_BY_SCHED   = 132;
constexpr int  IDC_TH_DARK_FROM  = 133;
constexpr int  IDC_TH_LIGHT_FROM = 134;
constexpr int  IDC_TH_TOGGLE     = 135;
constexpr int  IDC_TH_ADVANCED   = 136;
constexpr int  IDC_TH_SRC_AUTO   = 137;  // порядок = LocSource
constexpr int  IDC_TH_SRC_WIN    = 138;
constexpr int  IDC_TH_SRC_IP     = 139;
constexpr int  IDC_TH_SRC_MANUAL = 140;
constexpr int  IDC_TH_SRC_TZ     = 141;
constexpr int  IDC_TH_LAT        = 142;
constexpr int  IDC_TH_LON        = 143;
constexpr int  IDC_TH_STATUS     = 144;
constexpr int  IDC_TH_NOW        = 145;
// CAPS-8: тема вікна (вкладка «Налаштування»)
constexpr int  IDC_WT_AUTO       = 150;   // порядок = WinTheme
constexpr int  IDC_WT_LIGHT      = 151;
constexpr int  IDC_WT_DARK       = 152;
// CAPS-10: оновлення (вкладка «Налаштування»)
constexpr int  IDC_UPD_DAILY     = 160;
constexpr int  IDC_UPD_STATUS    = 161;
constexpr int  IDC_UPD_CHECK     = 162;
constexpr int  IDC_UPD_INSTALL   = 163;
constexpr int  IDC_UPD_ROLLBACK  = 164;
// CAPS-12: мова (вкладка «Налаштування»)
constexpr int  IDC_LANG_SYSTEM   = 170;   // порядок = LangPref
constexpr int  IDC_LANG_UK       = 171;
constexpr int  IDC_LANG_EN       = 172;
// CAPS-16: вкладка «Перегляд»
constexpr int  IDC_PEEK_ENABLE   = 180;
constexpr int  IDR_LOGO_PNG    = 100;  // RCDATA з lilhelpers.png
constexpr int  HOTKEY_ID       = 1;
constexpr UINT IDM_SETTINGS    = 1;
constexpr UINT IDM_EXIT        = 2;
constexpr UINT IDM_EDITOR      = 3;   // CAPS-20: редактор знімків
constexpr UINT TIMER_MAG_HOLD   = 1;
constexpr UINT TIMER_MAG_FRAME  = 2;   // кадр оверлейної анімації
constexpr UINT TIMER_THEME      = 3;   // CAPS-7: перевірка теми раз на хвилину
constexpr UINT TIMER_UPDATE     = 4;   // CAPS-10: хвилина після старту, далі кожні 30 хв
constexpr UINT TIMER_TRAY       = 5;   // CAPS-17: повтор додавання іконки, поки панель не готова

const wchar_t* kAppName  = L"Little Helpers";   // заголовки вікна/повідомлень, трей
const wchar_t* kWndClass = L"lilhelpers";
const wchar_t* kTaskName = L"lilhelpers";
const wchar_t* kRegPath  = L"Software\\lilhelpers";
const wchar_t* kRegMode  = L"Mode";
const wchar_t* kRegPassthrough = L"PassthroughRemote";
const wchar_t* kRegLayoutSwitch = L"LayoutSwitch";   // CAPS-9: перемикання розкладок увімкнено (1)
const wchar_t* kRegWindowTheme  = L"WindowTheme";    // CAPS-8: 0 авто / 1 світла / 2 темна
const wchar_t* kRegLang         = L"Language";       // CAPS-12: 0 системна / 1 укр / 2 англ
const wchar_t* kRegUpdDaily     = L"UpdateCheckDaily";  // CAPS-10
const wchar_t* kRegUpdLast      = L"UpdateLastCheck";   // unix (DWORD)
const wchar_t* kRegUpdNotified  = L"UpdateNotifiedTag"; // REG_SZ: про яку версію вже казали
const wchar_t* kRegPeek         = L"Peek";              // CAPS-16: перегляд по пробілу увімкнено (1)

// ---------- CAPS-12: локалізація ----------
//
// Один список рядків, дві колонки. X-макрос генерує з нього і enum, і обидві
// таблиці, тож переклад фізично не може роз'їхатися з іменем чи порядком.
//
// ⚠ Англійський рядок має бути НЕ ДОВШИМ за український: позиції й ширини
// контролів фіксовані (див. сітку у wWinMain) і підібрані саме під українські
// підписи. Довший переклад не переносить рядок, а мовчки обрізається.
#define LH_STRINGS(X)                                                                                  \
X(Tagline,            L"Дрібні зручності для Windows",                                                 \
                      L"Small conveniences for Windows")                                               \
X(Copyright,          L"© 2026 Вадим Слива (Plum)",                                                    \
                      L"© 2026 Vadym Slyva (Plum)")                                                    \
X(Empty,              L"", L"")                                                                        \
/* вкладки */                                                                                          \
X(TabLayout,          L"Розкладка",                    L"Layout")                                      \
X(TabCursor,          L"Курсор",                       L"Cursor")                                      \
X(TabTheme,           L"День/ніч",                     L"Day/night")                                   \
X(TabPeek,            L"Перегляд",                     L"Preview")                                    \
X(TabSettings,        L"Налаштування",                 L"Settings")                                    \
/* вкладка «Розкладка» */                                                                              \
X(LayEnable,          L"Перемикати розкладку клавіатури клавішею Caps Lock",                           \
                      L"Switch the keyboard layout with Caps Lock")                                    \
X(LayHint,            L"Caps Lock — наступна розкладка. Shift + Caps Lock — звичайний Caps Lock.",     \
                      L"Caps Lock — next layout. Shift + Caps Lock — normal Caps Lock.")               \
X(LaySecMode,         L"Спосіб перехоплення",           L"Interception method")                        \
X(LayModeHook,        L"Основний",                      L"Primary")                                    \
X(LayModeHotkey,      L"Запасний",                      L"Fallback")                                   \
X(LayHintOff,         L"Перемикання вимкнено — Caps Lock працює як звичайний Caps Lock.",              \
                      L"Switching is off — Caps Lock works as a normal Caps Lock.")                    \
X(LayHintHook,        L"CapsLock лише перемикає мову й не вмикає великі літери.",                      \
                      L"Caps Lock only switches the language, not capitals.")                          \
X(LayHintHotkey,      L"Оберіть, якщо основний режим не працює або конфліктує з іншою програмою.",     \
                      L"Use it if the primary method fails or conflicts with another app.")            \
X(LaySecRemote,       L"Віддалені та віртуальні машини", L"Remote and virtual machines")                \
X(LayPassthrough,     L"Не перехоплювати Caps Lock у вікнах віддалених і віртуальних машин",           \
                      L"Do not intercept Caps Lock in remote and virtual machine windows")             \
X(LayRemoteList,      L"Remote Desktop, Windows App, VMware, Hyper-V.",                                \
                      L"Remote Desktop, Windows App, VMware, Hyper-V.")                                \
/* вкладка «Курсор» */                                                                                 \
X(CurEnable,          L"Збільшувати курсор, якщо потрусити мишею",                                     \
                      L"Enlarge the cursor when the mouse is shaken")                                  \
X(CurEnableHint,      L"Не працює в іграх та інших повноекранних програмах.",                          \
                      L"Does not work in games or other full-screen programs.")                        \
X(CurScale,           L"Наскільки збільшувати",         L"How much to enlarge")                        \
X(CurHold,            L"Скільки тримати збільшеним",    L"How long to keep it large")                  \
X(CurOverlay,         L"Зменшувати плавно (намальованою копією)",                                      \
                      L"Shrink smoothly (with a drawn copy)")                                          \
X(CurOverlayHint,     L"Інакше зменшується сам системний курсор — помітними стрибками.",               \
                      L"Otherwise the system cursor itself shrinks, in visible steps.")                \
X(Details,            L"Детально ▾",                    L"Details ▾")                                  \
X(DetailsUp,          L"Детально ▴",                    L"Details ▴")                                  \
X(CurAdvWindow,       L"Вікно розпізнавання жесту, мс", L"Gesture detection window, ms")                \
X(CurAdvDist,         L"Мінімальний шлях миші, px",     L"Minimum mouse path, px")                     \
X(CurAdvFactor,       L"Поріг «шлях / розмах», %",      L"“Path / span” threshold, %")                 \
X(CurAdvRevers,       L"Мінімум змін напрямку",         L"Minimum direction changes")                  \
X(CurAdvShrink,       L"Тривалість зменшення, мс",      L"Shrink duration, ms")                        \
X(FmtSeconds,         L"%d,%d с",                       L"%d.%d s")                                    \
/* вкладка «День/ніч» */                                                                               \
X(ThEnable,           L"Автоматично перемикати світлу і темну тему Windows",                           \
                      L"Switch the Windows light and dark theme automatically")                        \
X(ThBySun,            L"За сходом і заходом сонця",     L"By sunrise and sunset")                      \
X(ThBySched,          L"За розкладом",                  L"On a schedule")                              \
X(ThDarkFrom,         L"Темна тема з",                  L"Dark theme from")                            \
X(ThLightFrom,        L"світла з",                      L"light from")                                 \
X(ThToggle,           L"Переключити зараз",             L"Switch now")                                 \
X(ThFullscreenHint,   L"Поки відкрита повноекранна програма, тема не змінюється — "                    \
                      L"перемкнеться після її закриття.",                                              \
                      L"While a full-screen program is open the theme does not change — "              \
                      L"it switches once that program closes.")                                        \
X(ThLocTitle,         L"Розташування для сходу й заходу", L"Location for sunrise and sunset")           \
X(ThSrcAuto,          L"Автоматично",                   L"Automatic")                                  \
X(ThSrcWin,           L"Служба Windows",                L"Windows service")                            \
X(ThSrcIp,            L"За IP-адресою",                 L"By IP address")                              \
X(ThSrcManual,        L"Вручну",                        L"Manually")                                   \
X(ThSrcTz,            L"Часовий пояс і регіон",         L"Time zone and region")                       \
X(ThLat,              L"Широта",                        L"Latitude")                                   \
X(ThLon,              L"Довгота",                       L"Longitude")                                  \
X(ThVpnHint,          L"За IP-адресою під VPN покаже розташування VPN-сервера.",                       \
                      L"Under a VPN the IP lookup shows the VPN server location.")                     \
/* джерело координат — усередині рядка стану, з малої літери */                                        \
X(LocSrcWindows,      L"служба Windows",                L"Windows service")                            \
X(LocSrcIp,           L"за IP-адресою",                 L"by IP address")                              \
X(LocSrcManual,       L"задано вручну",                 L"set manually")                               \
X(LocSrcTz,           L"часовий пояс і регіон",         L"time zone and region")                       \
X(LocSrcAuto,         L"автоматично",                   L"automatic")                                  \
/* рядок стану «День/ніч» */                                                                           \
X(ThFmtSchedule,      L"Розклад: темна тема з %02d:%02d, світла з %02d:%02d.",                         \
                      L"Schedule: dark theme from %02d:%02d, light from %02d:%02d.")                   \
X(ThFmtSun,           L"Схід %s · захід %s · %s · %s",  L"Sunrise %s · sunset %s · %s · %s")           \
X(ThPolarDay,         L"Полярний день",                 L"Polar day")                                  \
X(ThPolarNight,       L"Полярна ніч",                   L"Polar night")                                \
X(ThLocating,         L"Визначаю розташування…",        L"Finding your location…")                     \
X(ThEnterCoords,      L"Введіть широту й довготу в «Детально». Поки що — розклад 07:00/19:00.",        \
                      L"Enter latitude and longitude under “Details”. For now — schedule 07:00/19:00.")\
X(ThNoLoc,            L"Розташування не визначено — тимчасово розклад 07:00/19:00. "                   \
                      L"Джерело — у «Детально».",                                                      \
                      L"Location unknown — using schedule 07:00/19:00 for now. "                       \
                      L"The source is under “Details”.")                                               \
X(ThDark,             L"темна",                         L"dark")                                       \
X(ThLight,            L"світла",                        L"light")                                      \
X(ThDarkAcc,          L"темну",                         L"dark")                                       \
X(ThLightAcc,         L"світлу",                        L"light")                                      \
X(ThNowOff,           L"Зараз %s тема. Автоматика вимкнена.",                                          \
                      L"The %s theme is on. Automation is off.")                                       \
X(ThNowManual,        L"Зараз %s тема (обрано вручну) — автоматика повернеться о %s.",                 \
                      L"The %s theme is on (chosen by hand) — automation resumes at %s.")              \
X(ThNowPending,       L"Перемкну на %s тему, щойно закриється повноекранна програма.",                 \
                      L"Will switch to the %s theme once the full-screen program closes.")             \
X(ThNowNext,          L"Зараз %s тема · наступне перемикання о %s.",                                   \
                      L"The %s theme is on · next switch at %s.")                                      \
/* вкладка «Налаштування» */                                                                           \
X(SetAutostart,       L"Запускати при вході в Windows", L"Start when you sign in to Windows")          \
X(SetAutostartHint,   L"Задача Планувальника з найвищими правами, без запиту UAC. "                    \
                      L"Вікно можна закрити — програма лишається в треї.",                             \
                      L"A Task Scheduler task with the highest privileges, no UAC prompt. "            \
                      L"You can close this window — the program stays in the tray.")                   \
X(SetSecLang,         L"Мова",                          L"Language")                                   \
X(SetLangSystem,      L"Системна",                      L"System")                                     \
X(SetLangUk,          L"Українська",                    L"Українська")                                 \
X(SetLangEn,          L"English",                       L"English")                                    \
X(SetLangHint,        L"«Системна» — мова Windows, якщо вона перекладена; інакше англійська.",         \
                      L"“System” — the Windows language if translated, otherwise English.")            \
X(SetSecTheme,        L"Тема вікна",                    L"Window theme")                               \
X(SetThAuto,          L"Автоматично",                   L"Automatic")                                  \
X(SetThLight,         L"Завжди світла",                 L"Always light")                               \
X(SetThDark,          L"Завжди темна",                  L"Always dark")                                \
X(SetThHint,          L"«Автоматично» — як тема застосунків Windows (див. «День/ніч»).",               \
                      L"“Automatic” — follows the Windows app theme (see “Day/night”).")               \
X(SetSecUpd,          L"Оновлення",                     L"Updates")                                    \
X(UpdDaily,           L"Щоденна перевірка оновлень",    L"Check for updates daily")                    \
X(UpdCheck,           L"Перевірити зараз",              L"Check now")                                  \
X(UpdInstall,         L"Оновити",                       L"Update")                                     \
X(UpdRollback,        L"Повернути попередню",           L"Roll back")                                  \
X(UpdHint,            L"Оновлення з GitHub Releases; підпис релізу перевіряється перед заміною. "      \
                      L"Попередня версія лишається поруч як lilhelpers.exe.old.",                      \
                      L"Updates come from GitHub Releases; the signature is verified first. "          \
                      L"The previous version stays next to it as lilhelpers.exe.old.")                 \
/* рядок стану оновлень */                                                                             \
X(UpdNever,           L"ще не перевірялось",            L"not yet")                                    \
X(UpdChecking,        L"Перевіряю…",                    L"Checking…")                                  \
X(UpdFmtUpToDate,     L"Версія %s — остання. Перевірено %s.",                                          \
                      L"Version %s is the latest. Checked %s.")                                        \
X(UpdFmtAvailable,    L"Доступна версія %s (у вас %s). Натисніть «Оновити».",                          \
                      L"Version %s is available (you have %s). Click “Update”.")                       \
X(UpdFmtDownloading,  L"Завантажую %s і перевіряю підпис…",                                            \
                      L"Downloading %s and verifying the signature…")                                  \
X(UpdVerified,        L"Підпис підтверджено — перезапускаюсь у новій версії…",                         \
                      L"Signature verified — restarting into the new version…")                        \
X(UpdFmtIdle,         L"Версія %s. Остання перевірка: %s.",                                            \
                      L"Version %s. Last check: %s.")                                                  \
X(UpdBalloonFmt,      L"Доступна версія %s. Оновити можна у «Налаштуваннях».",                         \
                      L"Version %s is available. You can update it in Settings.")                      \
/* помилки оновлення (зберігаються кодом, а не текстом — щоб слідувати за мовою) */                    \
X(UpdErrNoNet,        L"Не вдалося перевірити оновлення — немає зв'язку з GitHub.",                    \
                      L"Could not check for updates — no connection to GitHub.")                       \
X(UpdErrApi,          L"GitHub відповів несподівано.",  L"GitHub replied unexpectedly.")               \
X(UpdErrVersion,      L"Незрозумілий номер версії у релізі.",                                          \
                      L"The release has an unrecognizable version number.")                            \
X(UpdErrDownload,     L"Не вдалося завантажити оновлення.",                                            \
                      L"Could not download the update.")                                               \
X(UpdErrSigDownload,  L"Не вдалося завантажити підпис релізу.",                                        \
                      L"Could not download the release signature.")                                    \
X(UpdErrSigMismatch,  L"Підпис не збігається — оновлення відхилено.",                                  \
                      L"The signature does not match — the update was rejected.")                      \
X(UpdErrNotExe,       L"Завантажений файл не схожий на програму.",                                     \
                      L"The downloaded file does not look like a program.")                            \
X(UpdErrReplace,      L"Не вдалося замінити файл програми.",                                           \
                      L"Could not replace the program file.")                                          \
X(UpdErrWrite,        L"Не вдалося записати нову версію.",                                             \
                      L"Could not write the new version.")                                             \
X(UpdErrLaunch,       L"Не вдалося запустити нову версію — повернуто стару.",                          \
                      L"Could not start the new version — the old one was restored.")                  \
/* вкладка «Перегляд» (CAPS-16) */                                                                    \
X(PeekExperimental,   L"⚠ Експериментальна функція: ще в розробці, стабільна робота не гарантована.", \
                      L"⚠ Experimental: still in development, stable operation is not guaranteed.")  \
X(PeekEnable,         L"Швидкий перегляд файлу по пробілу в Провіднику та на робочому столі",           \
                      L"Quick file preview with Space in Explorer and on the desktop")                  \
X(PeekHint,           L"Пробіл або Esc закриває. Стрілки в Провіднику гортають файли — перегляд "       \
                      L"стежить за виділенням.",                                                        \
                      L"Space or Esc closes it. Arrow keys in Explorer move between files; "            \
                      L"the preview follows.")                                                          \
X(PeekSecTypes,       L"Що показується",                L"What is shown")                               \
X(PeekTypesImages,    L"Зображення: JPEG, PNG, GIF (анімовані програються), BMP, TIFF, ICO, WebP, SVG.", \
                      L"Images: JPEG, PNG, GIF (animated ones play), BMP, TIFF, ICO, WebP, SVG.")    \
X(PeekTypesText,      L"Текст і код — з підсвіткою, Markdown зверстаним, JSON форматується, docx текстом.", \
                      L"Text and code highlighted, Markdown rendered, JSON reformatted, docx as text.") \
X(PeekZoomHint,       L"Коліщатко масштабує, перетягування рухає, подвійний клік вписує назад.",     \
                      L"The wheel zooms, dragging moves, a double click fits it back.")              \
X(PeekTypesMedia,     L"PDF — гортається коліщатком і стрілками. Відео — кадр. STL — із габаритами.", \
                      L"PDF — flip by wheel or arrows. Video — a frame. STL — with its sizes.")      \
X(PeekTypesOther,     L"Решта файлів, папки, ярлики та STEP — картка з відомостями про файл.",       \
                      L"Other files, folders, shortcuts and STEP — a card of file details.")         \
X(PeekSecKeeps,       L"Що лишається за Провідником",   L"What stays with Explorer")                    \
X(PeekKeeps,          L"Ctrl + пробіл і Shift + пробіл, пошук набором літер, пробіл у полях адреси, "   \
                      L"пошуку й перейменування, а також у діалогах відкриття та збереження файлів.",   \
                      L"Ctrl + Space and Shift + Space, type-to-search, Space in the address, search "  \
                      L"and rename boxes, and the Open/Save file dialogs.")                             \
/* вікно перегляду */                                                                                  \
X(PeekFmtImage,       L"%d × %d · %s",                  L"%d × %d · %s")                                \
X(PeekFmtTwo,         L"%s · %s",                       L"%s · %s")                                     \
X(PeekFmtItems,       L"елементів: %d%s",               L"items: %d%s")                                 \
X(PeekFmtThree,       L"%s · %s · %s",           L"%s · %s · %s")                                    \
X(PeekReformatted,    L"відформатовано",              L"reformatted")                                \
X(PeekSvgAsCode,      L"SVG з ефектами, яких ми не малюємо — показано розмітку",  L"SVG uses effects we do not draw — markup shown") \
X(PeekFmtStl,         L"%.0f × %.0f × %.0f · трикутників: %u · %s",   L"%.0f × %.0f × %.0f · triangles: %u · %s") \
X(PeekFmtVideo,       L"%d × %d · %s · %s",                      L"%d × %d · %s · %s")               \
X(PeekDocxText,       L"лише текст",                  L"text only")                                  \
X(PeekFmtPdf,         L"%d × %d · сторінок: %u · %s",              L"%d × %d · pages: %u · %s")      \
X(PeekFmtImageNote,   L"%d × %d · %s · %s",          L"%d × %d · %s · %s")                           \
X(PeekSvgNoFx,        L"без ефектів",                L"no effects")                                  \
X(PeekSvgNoMask,      L"без маски",                  L"no mask")                                     \
X(PeekSvgNoText,      L"без тексту",                 L"no text")                                     \
X(PeekSvgPartial,     L"намальовано не все",         L"partly drawn")                                \
X(PeekFmtPdfPage,     L"%d × %d · сторінка %u з %u · %s",       L"%d × %d · page %u of %u · %s")     \
X(PeekLblType,        L"Тип",                           L"Type")                                        \
X(PeekLblSize,        L"Розмір",                        L"Size")                                        \
X(PeekLblItems,       L"Елементів",                     L"Items")                                       \
X(PeekLblTarget,      L"Веде до",                                                                    \
                      L"Points to")                                                                  \
X(PeekFmtImageAnim,   L"%d × %d · кадрів: %d · %s",                                                  \
                      L"%d × %d · frames: %d · %s")                                                  \
X(PeekLblAuthor,      L"Автор",                    L"Author")                                        \
X(PeekLblOrg,         L"Організація",              L"Organisation")                                  \
X(PeekLblSchema,      L"Схема",                    L"Schema")                                        \
X(PeekLblEntities,    L"Сутностей",                L"Entities")                                      \
X(PeekLblModified,    L"Дата зміни",                    L"Modified")                                    \
X(PeekLblCreated,     L"Створено",                      L"Created")                                     \
X(PeekLblWhere,       L"Розташування",                  L"Location")                                    \
X(PeekEmpty,          L"(порожній файл)",               L"(empty file)")                                \
X(PeekTruncated,      L"… показано перший 1 МБ файлу",  L"… first 1 MB shown")                          \
/* повідомлення й меню */                                                                              \
X(MsgHookFailed,      L"Не вдалося перехопити клавішу CapsLock.",                                      \
                      L"Could not intercept the Caps Lock key.")                                       \
X(MsgModeUnavailable, L"Цей режим зараз недоступний — залишено попередній.",                           \
                      L"This method is unavailable right now — the previous one was kept.")            \
X(MsgAutostartFailed, L"Не вдалося змінити задачу автозапуску.",                                       \
                      L"Could not change the autostart task.")                                         \
X(MsgRollbackConfirm, L"Повернути попередню версію і перезапустити Little Helpers?",                   \
                      L"Roll back to the previous version and restart Little Helpers?")                \
X(MenuSettings,       L"Налаштування…",                 L"Settings…")                                  \
X(EdMenu,             L"Редактор знімків…",             L"Screenshot editor…")                         \
X(EdTitle,            L"Редактор знімків",              L"Screenshot editor")                          \
X(EdToolRect,         L"Прямокутник",                   L"Rectangle")                                  \
X(EdKindRect,         L"Вибрано: прямокутник",          L"Selected: rectangle")                        \
X(EdSelHint,          L"Виберіть позначку, щоб змінити її колір, прозорість або розмір.",              \
                      L"Select a mark to change its colour, opacity or size.")                         \
X(EdNoSel,            L"Нічого не вибрано",             L"Nothing selected")                           \
X(EdFmtSel,           L"Вибране %d × %d",               L"Picked %d × %d")                             \
X(EdFit,              L"Вписати",                       L"Fit")                                        \
X(EdSecShot,          L"ЗНІМОК",                        L"IMAGE")                                      \
X(EdFmtSource,        L"Зображення · %d × %d",          L"Image · %d × %d")                            \
X(EdFmtMarks,         L"Позначок: %d",                  L"Marks: %d")                                  \
X(EdOpenTitle,        L"Відкрити зображення",           L"Open image")                                 \
X(EdOpenFilter,       L"Зображення",                    L"Images")                                     \
X(EdErrOpen,          L"Не вдалося відкрити зображення.", L"Could not open the image.")                \
X(EdHelpTitle,        L"Редактор знімків",              L"Screenshot editor")                          \
X(EdHelpBody,         L"Етап 1: каркас вікна і модель позначок.\n\n"                                   \
                      L"V — вибір і переміщення\nR — прямокутник\n"                                    \
                      L"Delete — видалити вибране\nCtrl+Z, Ctrl+Y — скасувати й повторити\n"           \
                      L"Коліщатко — масштаб, подвійний клік — вписати\n"                                \
                      L"Пробіл або середня кнопка — рухати полотно",                                    \
                      L"Stage 1: window frame and the mark model.\n\n"                                 \
                      L"V — select and move\nR — rectangle\n"                                          \
                      L"Delete — remove the selection\nCtrl+Z, Ctrl+Y — undo and redo\n"               \
                      L"Wheel — zoom, double click — fit\n"                                             \
                      L"Space or middle button — pan the canvas")                                       \
X(MenuExit,           L"Вихід",                         L"Exit")                                       \
X(TaskDesc,           L"Little Helpers — розкладка по Caps Lock, пошук курсора, день/ніч, перегляд по пробілу", \
                      L"Little Helpers — Caps Lock layout switching, cursor finder, day/night, space-bar preview")

#define LH_ENUM(name, uk, en) name,
#define LH_UK(name, uk, en)   uk,
#define LH_EN(name, uk, en)   en,
enum class Str { LH_STRINGS(LH_ENUM) Count };
const wchar_t* const kUk[] = { LH_STRINGS(LH_UK) };
const wchar_t* const kEn[] = { LH_STRINGS(LH_EN) };
static_assert(sizeof(kUk) / sizeof(*kUk) == (size_t)Str::Count, "українська таблиця не повна");
static_assert(sizeof(kEn) / sizeof(*kEn) == (size_t)Str::Count, "англійська таблиця не повна");

enum class Lang     { Uk = 0, En = 1 };
enum class LangPref { System = 0, Uk = 1, En = 2 };   // порядок = IDC_LANG_*

// «Системна»: мова інтерфейсу Windows, якщо вона є серед перекладів; інакше
// англійська — так само поводяться й самі застосунки Windows.
Lang ResolveLang(LangPref p)
{
    if (p == LangPref::Uk) return Lang::Uk;
    if (p == LangPref::En) return Lang::En;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_UKRAINIAN ? Lang::Uk : Lang::En;
}

LangPref g_langPref = LangPref::System;
Lang     g_lang     = ResolveLang(LangPref::System);   // до читання реєстру — системна

inline const wchar_t* S(Str id) { return (g_lang == Lang::En ? kEn : kUk)[(int)id]; }

// Контроли зі сталим підписом запам'ятовуються при створенні, щоб ApplyLanguage
// переписала їх усі за один прохід (дінамічні рядки стану оновлюють себе самі).
struct LocCtrl { HWND h; Str id; };
LocCtrl g_locCtrls[96];
int     g_locCtrlsN = 0;
void RememberLoc(HWND h, Str id)
{
    if (g_locCtrlsN < (int)(sizeof(g_locCtrls) / sizeof(*g_locCtrls)))
        g_locCtrls[g_locCtrlsN++] = { h, id };
}

constexpr int kTabCount = 5;
const Str kTabTitles[kTabCount] = { Str::TabLayout, Str::TabCursor, Str::TabTheme, Str::TabPeek, Str::TabSettings };

// Два способи перехопити клавішу. Основний тримає Caps Lock вимкненим, але це
// клавіатурний хук, який деякі захисні програми не люблять; запасний працює
// через системну реєстрацію клавіші й нічого не перехоплює, але тоді Windows
// сама перемикає Caps Lock — див. коментар на початку файлу.
enum class Mode { Hook = 0, Hotkey = 1 };

NOTIFYICONDATAW g_nid = {};
HWND g_checkbox = nullptr;
HWND g_modeHint = nullptr;
UINT g_taskbarCreatedMsg = 0;
Mode g_mode = Mode::Hook;

// CAPS-1: не перехоплювати Caps у вікнах віддалених/віртуальних машин.
volatile bool g_passthrough = true;   // налаштування (чекбокс), збереж. у реєстрі
volatile bool g_inRemote    = false;  // активне вікно — remote/VM (оновлює WinEvent)
bool  g_interceptionOn = false;       // перехоплення активне (для Hotkey-контексту)
bool  g_hotkeyActive   = false;       // RegisterHotKey зараз тримається
HWINEVENTHOOK g_winEvent = nullptr;
HWND  g_passthroughCheckbox = nullptr;

// CAPS-9: перемикання розкладок — окрема функція, яку можна вимкнути, не чіпаючи
// автозапуск (він тепер у вкладці «Налаштування»). Вимкнено = Caps Lock звичайний.
bool  g_layoutOn = true;
HWND  g_layoutCheckbox = nullptr;
HWND  g_pageSettings[32] = {};  int g_pageSettingsN = 0;
HWND  g_pagePeek[24]     = {};  int g_pagePeekN = 0;   // CAPS-16

// ---------- CAPS-8: тема самого вікна ----------
//
// Windows дає темними лише заголовок (DWM) і кілька контролів через недокументовану
// тему «DarkMode_Explorer» (кнопки, up-down) та «DarkMode_CFD» (поля вводу). Решту —
// фон вікна, полотно вкладок і самі вкладки, чекбокси/радіо, повзунки, пікери часу —
// малюємо самі (той самий шлях, що в Notepad++). «Автоматично» = слідувати за темою
// застосунків Windows (AppsUseLightTheme), зміни ловимо через WM_SETTINGCHANGE
// "ImmersiveColorSet" — його ж ми самі й розсилаємо у «День/ніч».
enum class WinTheme { Auto = 0, Light = 1, Dark = 2 };
WinTheme g_winTheme = WinTheme::Auto;
bool     g_dark = false;                 // що зараз застосовано
constexpr COLORREF kDkBg     = RGB(32, 32, 32);     // фон вікна
constexpr COLORREF kDkPage   = RGB(43, 43, 43);     // полотно сторінки
constexpr COLORREF kDkEdit   = RGB(25, 25, 25);     // поля вводу / пікери
constexpr COLORREF kDkBorder = RGB(82, 82, 82);
constexpr COLORREF kDkText   = RGB(240, 240, 240);
constexpr COLORREF kDkGray   = RGB(160, 160, 160);
constexpr COLORREF kDkThumb  = RGB(204, 204, 204);
constexpr COLORREF kDkAccent = RGB(96, 165, 250);   // смужка активної вкладки
HBRUSH g_brDkBg = nullptr, g_brDkPage = nullptr, g_brDkEdit = nullptr;
HBRUSH g_brDkBorder = nullptr, g_brDkThumb = nullptr, g_brDkAccent = nullptr;

// ---------- CAPS-10: автооновлення з GitHub Releases ----------
//
// Перевірка: GET releases/latest → tag_name. Завантаження lilhelpers.exe і
// lilhelpers.exe.sig з releases/download/<tag>/. Справжність — ECDSA P-256 підпис
// SHA-256 файлу, зроблений у CI приватним ключем (GitHub Secret LILHELPERS_SIGNING_KEY);
// публічний ключ зашитий нижче і лежить у репо як lilhelpers_signing_pub.pem — CI
// перевіряє їх збіг. Підпис Authenticode не потрібен: файл, записаний самою
// програмою, не має Mark-of-the-Web, SmartScreen мовчить. Заміна: запущений exe →
// lilhelpers.exe.old, новий на його місце, запуск нового з --after-update <pid>
// (чекає виходу старого, бо м'ютекс одного екземпляра), старий виходить. .old
// лишається для «Повернути попередню версію».
// Ім'я ассету зашите (а не береться з власного імені файла), щоб перейменована
// вручну копія програми не шукала в релізі неіснуючий ассет.
// X||Y публічного ключа (одним літералом — CI звіряє його з lilhelpers_signing_pub.pem)
const char*    kUpdatePubKeyHex = "86a4bec4e053f5a79786c1f5493c1faebd1f1909b4606616eaf93afc39cd100f821fd2724675adb0721049e70df4cc6130bdc4424b75049b21f31c09e5a065c9";
const wchar_t* kUpdApiUrl = L"https://api.github.com/repos/V-Plum/lilhelpers/releases/latest";
const wchar_t* kUpdDlBase = L"https://github.com/V-Plum/lilhelpers/releases/download/";
const wchar_t* kUpdAsset  = L"lilhelpers.exe";

enum class UpdState { Idle, Checking, UpToDate, Available, Downloading, Verified, Error };
struct UpdResult {
    bool    install = false;   // false = лише перевірити
    bool    manual  = false;   // натиснуто кнопку (без балуна в треї)
    bool    ok      = false;
    wchar_t tag[32] = {};
    Str     err = Str::Empty;   // CAPS-12: код помилки, а не текст — щоб слідував за мовою
};
bool          g_updDaily = true;
__time64_t    g_updLast  = 0;
UpdState      g_updState = UpdState::Idle;
wchar_t       g_updTag[32] = {};        // доступна версія (tag)
Str           g_updErr = Str::Empty;    // CAPS-12: остання помилка (код)
wchar_t       g_updNotified[32] = {};
volatile LONG g_updBusy = 0;
HWND g_updDailyCb = nullptr, g_updStatus = nullptr;
HWND g_updCheckBtn = nullptr, g_updInstallBtn = nullptr, g_updRollbackBtn = nullptr;

ULONG_PTR g_gdiplusToken = 0;
Gdiplus::Image* g_logo = nullptr;
RECT g_logoRect = {};  // куди малювати логотип (пікселі клієнтської області)

// CAPS-2: вкладки. Сторінки — звичайні діти головного вікна поверх таб-контрола
// (створені ПІСЛЯ нього, тож лежать вище за z-order); перемикання = show/hide.
HWND g_tabs = nullptr;
HWND g_pageLayout[32] = {};  int g_pageLayoutN = 0;
HWND g_pageCursor[32] = {};  int g_pageCursorN = 0;
HWND g_advCtrls[32]   = {};  int g_advN = 0;
HWND g_curEnable = nullptr, g_curScale = nullptr, g_curHold = nullptr;
HWND g_curOverlay = nullptr;
HWND g_curScaleVal = nullptr, g_curHoldVal = nullptr, g_curAdvBtn = nullptr;
HWND g_edWindow = nullptr, g_edDist = nullptr, g_edFactor = nullptr;
HWND g_edRevers = nullptr, g_edShrink = nullptr;
bool g_advVisible = false;

HHOOK  g_hook = nullptr;
HHOOK  g_mouseHook = nullptr;
HWND   g_mainWnd = nullptr;
bool   g_capsDown = false;  // щоб автоповтор не перемикав розкладку нескінченно

// CAPS-16: перегляд по пробілу — стан, який читає колбек хука
volatile bool g_peekOn     = true;      // налаштування (чекбокс), збереж. у реєстрі
volatile bool g_peekShown  = false;     // вікно перегляду відкрите (пише UI-потік)
volatile HWND g_peekRoot   = nullptr;   // верхнє вікно Провідника/стола, з якого відкрито
volatile bool g_kbHookCaps = false;     // хук обслуговує Caps Lock (розкладка в режимі «Основний»)
UINT  g_peekSwallowedVk = 0;            // клавіша, чий keydown ми з'їли — з'їсти і keyup
DWORD g_lastTypeTick    = 0;            // остання «друкована» клавіша: пошук набором у Провіднику
constexpr ULONG_PTR kInjectMark = 0x4C48504B;   // 'LHPK': наш SendInput, хук пропускає як є

// ---------- CAPS-7: день/ніч — автоматична світла/темна тема Windows ----------
//
// Тема — два DWORD у HKCU\...\Themes\Personalize (AppsUseLightTheme,
// SystemUsesLightTheme; 0 = темна) + бродкаст WM_SETTINGCHANGE "ImmersiveColorSet",
// без якого частина вікон не перемальовується. Перемикаємо обидва разом (рішення
// власника: менше мішанини). Момент — за сходом/заходом сонця (NOAA) або за
// розкладом. Розташування: служба геолокації Windows → за IP → часовий пояс і
// регіон Windows, або вручну; результат кешується в реєстрі, щоб після старту не
// чекати сенсора чи мережі. «Переключити зараз» — ручний вибір ДО НАСТУПНОЇ МЕЖІ
// (наступного сходу/заходу або часу розкладу), далі автоматика знову рахує стан
// від розкладу, а не просто фліпає. Поки на передньому плані повноекранна
// програма, тему не чіпаємо — перемкнемо, щойно вона закриється.
const wchar_t* kRegThemeAuto      = L"ThemeAuto";
const wchar_t* kRegThemeSched     = L"ThemeBySchedule";
const wchar_t* kRegThemeDarkFrom  = L"ThemeDarkFromMin";
const wchar_t* kRegThemeLightFrom = L"ThemeLightFromMin";
const wchar_t* kRegThemeLocSrc    = L"ThemeLocationSource";
const wchar_t* kRegThemeLat       = L"ThemeLatitude";        // ручні координати, REG_SZ
const wchar_t* kRegThemeLon       = L"ThemeLongitude";
const wchar_t* kRegThemeCacheLat  = L"ThemeCacheLatitude";   // останнє визначене розташування
const wchar_t* kRegThemeCacheLon  = L"ThemeCacheLongitude";
const wchar_t* kRegThemeCacheSrc  = L"ThemeCacheSource";
const wchar_t* kRegThemeCacheAt   = L"ThemeCacheAt";         // unix-час (DWORD)
const wchar_t* kRegThemeOvUntil   = L"ThemeOverrideUntil";   // ручний вибір діє до (unix)
const wchar_t* kRegThemeOvDark    = L"ThemeOverrideDark";
const wchar_t* kPersonalize = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

enum class LocSource { Auto = 0, Windows = 1, Ip = 2, Manual = 3, TimeZone = 4 };

// Location API (Win32, COM). Власні копії GUID-ів — див. коментар біля #include.
const GUID kCLSID_Location     = { 0xe5b8e079, 0xee6d, 0x4e33, { 0xa4, 0x38, 0xc8, 0x7f, 0x2e, 0x95, 0x92, 0x54 } };
const GUID kIID_ILocation      = { 0xab2ece69, 0x56d9, 0x4f28, { 0xb5, 0x25, 0xde, 0x1b, 0x0e, 0xe4, 0x42, 0x37 } };
const GUID kIID_ILatLongReport = { 0x7fed806d, 0x0ef8, 0x4f07, { 0x80, 0xac, 0x36, 0xa0, 0xbe, 0xae, 0x31, 0x34 } };

struct ThemeSettings {
    bool enabled    = false;
    bool bySchedule = false;    // false = за сонцем
    int  darkFrom   = 19 * 60;  // хвилини від півночі
    int  lightFrom  = 7 * 60;
    LocSource src   = LocSource::Auto;
    bool   hasManual = false;   // ручні координати задано
    double lat = 0, lon = 0;    // ручні координати
};
struct ThemeFix {               // розташування, за яким рахуємо сонце
    bool   ok = false;
    double lat = 0, lon = 0;
    LocSource src = LocSource::Auto;
    __time64_t at = 0;
};
struct LocResult {              // відповідь потоку геолокації
    bool   ok = false;
    double lat = 0, lon = 0;
    LocSource src = LocSource::Auto;
    bool   prompt = false;      // показати системний діалог дозволу (лише явний вибір «Windows»)
    LONG   gen = 0;
};
ThemeSettings g_th;
ThemeFix      g_fix;
__time64_t    g_thOvUntil = 0;     // 0 = ручного вибору немає
bool          g_thOvDark  = false;
bool          g_thPending = false; // треба перемкнути, чекаємо закриття повноекранної програми
bool          g_locFailed = false; // остання спроба визначити розташування провалилась
bool          g_locPrompted = false;
bool          g_locAgain  = false; // джерело змінили під час визначення — повторити
volatile LONG g_locBusy = 0;
LONG          g_locGen  = 0;
HWND g_pageTheme[40] = {};  int g_pageThemeN = 0;
HWND g_thAdv[32]     = {};  int g_thAdvN = 0;
bool g_thAdvVisible = false;
HWND g_thEnable = nullptr, g_thBySun = nullptr, g_thBySched = nullptr;
HWND g_thDarkFrom = nullptr, g_thLightFrom = nullptr, g_thToggle = nullptr;
HWND g_thAdvBtn = nullptr, g_thStatus = nullptr, g_thNow = nullptr;
HWND g_thLat = nullptr, g_thLon = nullptr, g_thSrc[5] = {};

// ---------- CAPS-2: збільшення курсора по трусінню мишею ----------
//
// Збільшуємо САМ системний курсор (SystemParametersInfo 0x2029 -> CursorBaseSize),
// а не малюємо копію в оверлеї: апаратний курсор Windows малюється поверх усіх
// вікон, тож оверлейна копія завжди йшла б у парі з живим маленьким курсором.
// Ціна рішення — це глобальна настройка користувача, тому її треба вміти
// повернути навіть після аварійного завершення (kRegCursorRestore нижче).
constexpr UINT SPI_SETCURSORSIZE_ = 0x2029;  // недокументований, але стабільний з Win10
constexpr int  kCursorMinPx = 32;
constexpr int  kCursorMaxPx = 256;

const wchar_t* kRegCursorEnable    = L"CursorFind";
const wchar_t* kRegCursorScale     = L"CursorScale";
const wchar_t* kRegCursorHold      = L"CursorHoldMs";
const wchar_t* kRegCursorShrink    = L"CursorShrinkMs";
const wchar_t* kRegShakeWindow     = L"ShakeWindowMs";
const wchar_t* kRegShakeDistance   = L"ShakeMinDistance";
const wchar_t* kRegShakeFactor     = L"ShakeFactor";
const wchar_t* kRegShakeReversals  = L"ShakeReversals";
const wchar_t* kRegCursorOverlay   = L"CursorOverlayShrink";
const wchar_t* kRegCursorRestore   = L"CursorRestorePx";  // аварійний слід

// Дефолти підібрані на симуляції жестів (див. коментар біля ShakeFeed):
// 1000 px — найменший поріг, за якого жоден із перевірених «звичайних» рухів
// не проходить, а справжнє трусіння лишається коротким (4–6 махів, ~0.4 с).
struct CursorSettings {
    bool enabled   = true;
    int  scale     = 5;     // у скільки разів збільшувати (2..8)
    int  holdMs    = 1500;  // тримати збільшеним після жесту
    int  shrinkMs  = 250;   // тривалість плавного зменшення
    int  windowMs  = 700;   // вікно, у якому рахуємо рухи
    int  distance  = 1000;  // мінімальний пройдений шлях, px
    int  factor    = 350;   // шлях / діагональ габариту, %
    int  reversals = 3;     // мінімум змін напрямку
    bool overlay   = true;  // зменшувати намальованою копією, а не системним розміром
};
CursorSettings g_cur;

// Стан збільшення (живе на UI-потоці)
enum class MagState { Idle, Big, Shrinking };
MagState g_magState   = MagState::Idle;
int      g_magOrigPx  = kCursorMinPx;
int      g_magTargetPx = kCursorMinPx;

// Зменшення крутить ОКРЕМИЙ потік і рахує розмір від ЧАСУ, а не від номера кроку.
// Причина: кожне застосування розміру з SPIF_SENDCHANGE — синхронний бродкаст
// WM_SETTINGCHANGE усім вікнам, і його вартість залежить від того, скільки вікон
// відкрито й чи швидко вони відповідають (виміряно: 0.1 мс без бродкасту проти
// ~35 мс з ним на порожньому столі, і значно більше під навантаженням). Прив'язка
// до часу робить тривалість передбачуваною: на швидкій системі кроків більше й
// анімація гладка, на повільній — менше, але вкладаємось у ту саму чверть секунди.
CRITICAL_SECTION g_magLock;
volatile LONG    g_magGen = 0;     // покоління анімації; зміна = скасування
HANDLE           g_magThread = nullptr;

// Оверлейне зменшення. Системний розмір курсора анімувати неможливо: кожен кадр
// коштує синхронного бродкасту, і на завантаженій машині виходить 2-3 стрибки
// замість плавності. Тому тут малюємо ЗМЕНШУВАНУ КОПІЮ курсора у власному
// layered-вікні (це звичайна композиція GPU, десятки кадрів безкоштовно), а
// системний розмір повертаємо одним викликом у фоні. Плата — під копією видно
// справжній курсор; він уже нормального розміру й стоїть у тій самій точці.
HWND  g_overlay     = nullptr;
HICON g_overlayIcon = nullptr;
POINT g_ovHotspot   = {};
int   g_ovBasePx    = 32;   // розмір, у якому задано гарячу точку
int   g_ovFrom = 0, g_ovTo = 0;
DWORD g_ovStart = 0;
void  OverlayDestroy();   // визначення нижче, але потрібне вже у MagnifyRestore

// Буфер жесту (пишеться в колбеку хука, читається там само)
struct ShakeMove { int dx, dy; DWORD tick; };
constexpr int kMaxMoves = 64;
ShakeMove g_moves[kMaxMoves];
int   g_moveCount = 0;
POINT g_lastPt = {};
bool  g_haveLastPt = false;
DWORD g_shakeBlockUntil = 0;
unsigned g_btnMask = 0;   // які кнопки миші затиснуті зараз

// Хук живе на власному потоці (див. коментар біля HookThreadProc)
HANDLE g_hookThread = nullptr;
DWORD  g_hookThreadId = 0;
HWND   g_hookWnd = nullptr;   // message-only вікно того потоку для команд install/uninstall

// ---------- перемикання розкладки ----------

// Реально сфокусоване вікно (для UWP foreground != focus)
HWND GetFocusedWindow()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return nullptr;

    DWORD tid = GetWindowThreadProcessId(fg, nullptr);
    GUITHREADINFO gti = { sizeof(gti) };
    if (GetGUIThreadInfo(tid, &gti) && gti.hwndFocus)
        return gti.hwndFocus;
    return fg;
}

HKL NextLayout(HWND target)
{
    // Запитуємо реальну кількість, а не сподіваємось на фіксований розмір:
    // інакше в людини з багатьма розкладками поточна могла б не потрапити у
    // зрізаний список і перемикання стрибало б на першу.
    UINT n = GetKeyboardLayoutList(0, nullptr);
    if (n < 2) return nullptr;

    HKL list[64];
    if (n > 64) n = 64;
    n = GetKeyboardLayoutList(n, list);
    if (n < 2) return nullptr;

    DWORD tid = target ? GetWindowThreadProcessId(target, nullptr) : 0;
    HKL cur = GetKeyboardLayout(tid);

    for (UINT i = 0; i < n; ++i)
        if (list[i] == cur)
            return list[(i + 1) % n];
    return list[0];
}

void SwitchLayout()
{
    HWND target = GetFocusedWindow();
    HKL next = NextLayout(target);
    if (!next) return;

    if (target)
        PostMessageW(target, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)next);
    else
        ActivateKeyboardLayout(next, 0);
}

// ---------- CAPS-1: виявлення remote/VM-вікон ----------
//
// Вікна цих процесів вважаємо клієнтом віддаленої/віртуальної машини. У них
// Caps треба ПРОПУСТИТИ, щоб розкладку перемкнула гостьова ОС (де теж стоїть
// ця програма), а не перехоплювати його на хості. Список фіксований (v1).
bool IsRemoteWindow(HWND w)
{
    if (!w) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (!pid) return false;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(h, 0, path, &len) != FALSE;
    CloseHandle(h);
    if (!ok) return false;

    const wchar_t* name = PathFindFileNameW(path);
    static const wchar_t* const kRemoteProcs[] = {
        L"mstsc.exe",     // Remote Desktop (класичний RDP)
        L"msrdc.exe",     // Windows App / новий Remote Desktop-клієнт
        L"vmware.exe",    // VMware Workstation/Player (вікно консолі ВМ)
        L"vmconnect.exe", // Hyper-V (консоль підключення до ВМ)
    };
    for (const wchar_t* p : kRemoteProcs)
        if (lstrcmpiW(name, p) == 0)
            return true;
    return false;
}

bool RemotePassthroughActive() { return g_passthrough && g_inRemote; }

// ---------- CAPS-16: пробіл у списку файлів (частина хука) ----------
//
// Виконується в колбеку хука — лише GetClassName / GetParent / GetGUIThreadInfo,
// без COM і без нічого, що може заблокуватись (LowLevelHooksTimeout).

// Список файлів, сфокусований УСЕРЕДИНІ цього вікна (Провідник або робочий стіл);
// повертає його SHELLDLL_DefView. Адресний рядок, пошук, перейменування, дерево
// тек і діалоги відкриття/збереження сюди не потрапляють — там пробіл не наш.
// Фокус беремо в потока самого вікна, а не глобальний: вкладки Windows 11 живуть
// в одному потоці, тож так видно саме активну вкладку потрібного вікна.
HWND ShellListIn(HWND top)
{
    if (!top) return nullptr;
    wchar_t cls[64] = {};
    GetClassNameW(top, cls, 64);
    if (lstrcmpW(cls, L"CabinetWClass") && lstrcmpW(cls, L"Progman") && lstrcmpW(cls, L"WorkerW"))
        return nullptr;
    GUITHREADINFO gti = { sizeof(gti) };
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(top, nullptr), &gti) || !gti.hwndFocus)
        return nullptr;
    GetClassNameW(gti.hwndFocus, cls, 64);
    if (lstrcmpW(cls, L"DirectUIHWND") && lstrcmpW(cls, L"SysListView32"))
        return nullptr;
    for (HWND p = GetParent(gti.hwndFocus); p && p != top; p = GetParent(p)) {
        GetClassNameW(p, cls, 64);
        if (!lstrcmpW(cls, L"SHELLDLL_DefView")) return p;
    }
    return nullptr;
}

// Те саме для активного вікна — цим користується хук, вирішуючи, чи пробіл наш.
HWND ShellListFocused() { return ShellListIn(GetForegroundWindow()); }

// Клавіші, з яких Провідник складає пошук набором: пробіл одразу після них — його.
bool IsTypeAheadKey(DWORD vk)
{
    return (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') ||
           (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) || (vk >= VK_OEM_1 && vk <= VK_OEM_8);
}

LRESULT PeekKeyboardHook(int nCode, WPARAM wParam, LPARAM lParam)
{
    const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lParam;
    const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    if (k->vkCode != VK_SPACE && k->vkCode != VK_ESCAPE) {
        if (down && IsTypeAheadKey(k->vkCode)) g_lastTypeTick = GetTickCount();
        return CallNextHookEx(g_hook, nCode, wParam, lParam);
    }
    // Наш власний пробіл, повернутий Провіднику, проходить наскрізь — і саме ТУТ,
    // до перевірки автоповтору нижче: інакше ми з'їли б його як «та сама клавіша
    // ще тримається», і Провідник не отримав би нічого взагалі.
    if (k->dwExtraInfo == kInjectMark)
        return CallNextHookEx(g_hook, nCode, wParam, lParam);
    // Відпускання клавіші, чиє натискання ми з'їли, — теж наше: інакше Провідник
    // побачив би keyup нізвідки.
    if (!down) {
        if (g_peekSwallowedVk == k->vkCode) { g_peekSwallowedVk = 0; return 1; }
        return CallNextHookEx(g_hook, nCode, wParam, lParam);
    }
    if (g_peekSwallowedVk == k->vkCode) return 1;   // автоповтор, поки тримають
    const bool ours = g_peekOn &&
        (k->vkCode == VK_SPACE || g_peekShown) &&
        !((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_SHIFT) |
           GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000);
    HWND view = ours ? ShellListFocused() : nullptr;
    if (!view || (k->vkCode == VK_SPACE && !g_peekShown && GetTickCount() - g_lastTypeTick < 1000))
        return CallNextHookEx(g_hook, nCode, wParam, lParam);
    g_peekSwallowedVk = k->vkCode;
    PostMessageW(g_mainWnd, WMAPP_PEEK, k->vkCode, (LPARAM)view);
    return 1;
}

// ---------- перехоплення клавіші ----------
//
// Колбек свідомо мінімальний: усе, що складніше за PostMessage, ризикує не
// вкластися в LowLevelHooksTimeout, після чого Windows тихо зніме хук — і
// утиліта "просто перестане працювати" без жодної помилки.
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode != HC_ACTION)
        return CallNextHookEx(g_hook, nCode, wParam, lParam);

    const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lParam;
    if (k->vkCode != VK_CAPITAL)
        return PeekKeyboardHook(nCode, wParam, lParam);   // CAPS-16
    if (!g_kbHookCaps)   // хук стоїть заради перегляду, Caps Lock — не наш
        return CallNextHookEx(g_hook, nCode, wParam, lParam);

    // CAPS-1: у вікні віддаленої/віртуальної машини не перехоплюємо — Caps іде
    // далі, розкладку перемикає гостьова ОС.
    if (g_passthrough && g_inRemote) {
        g_capsDown = false;
        return CallNextHookEx(g_hook, nCode, wParam, lParam);
    }

    // Shift+CapsLock лишається справжнім Caps Lock — пропускаємо як є
    if ((GetAsyncKeyState(VK_LSHIFT) & 0x8000) || (GetAsyncKeyState(VK_RSHIFT) & 0x8000)) {
        g_capsDown = false;
        return CallNextHookEx(g_hook, nCode, wParam, lParam);
    }

    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
        if (!g_capsDown) {
            g_capsDown = true;
            PostMessageW(g_mainWnd, WMAPP_SWITCH, 0, 0);
        }
        return 1;  // саме це не дає перемкнутися регістру
    }

    if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
        g_capsDown = false;
        return 1;
    }

    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

// ---------- CAPS-2: розпізнавання жесту ----------
//
// Дивимось не на швидкість, а на відношення пройденого шляху до діагоналі
// габаритного прямокутника руху. Прямий кидок через увесь екран дає відношення
// близько одиниці, трусіння — у рази більше. Саме це відсікає хибні спрацювання
// при звичайному швидкому наведенні, на яких свого часу погоріли перші версії
// подібної фічі в PowerToys.
int Sign(int v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

// Виконується в колбеку хука, тому — тільки арифметика, жодних викликів,
// здатних заблокуватися: інакше LowLevelHooksTimeout і Windows зніме хук.
void ShakeFeed(POINT pt, DWORD now)
{
    if (!g_haveLastPt) { g_lastPt = pt; g_haveLastPt = true; return; }

    const int dx = pt.x - g_lastPt.x;
    const int dy = pt.y - g_lastPt.y;
    g_lastPt = pt;
    if (dx == 0 && dy == 0) return;

    // забути рухи, старші за вікно детекції
    int drop = 0;
    while (drop < g_moveCount && now - g_moves[drop].tick > (DWORD)g_cur.windowMs) drop++;
    if (drop > 0) {
        for (int i = drop; i < g_moveCount; ++i) g_moves[i - drop] = g_moves[i];
        g_moveCount -= drop;
    }

    const bool sameDir = g_moveCount > 0 &&
                         Sign(g_moves[g_moveCount - 1].dx) == Sign(dx) &&
                         Sign(g_moves[g_moveCount - 1].dy) == Sign(dy);
    if (sameDir) {
        g_moves[g_moveCount - 1].dx += dx;
        g_moves[g_moveCount - 1].dy += dy;
        g_moves[g_moveCount - 1].tick = now;
    } else {
        if (g_moveCount == kMaxMoves) {
            for (int i = 1; i < kMaxMoves; ++i) g_moves[i - 1] = g_moves[i];
            g_moveCount--;
        }
        g_moves[g_moveCount].dx = dx;
        g_moves[g_moveCount].dy = dy;
        g_moves[g_moveCount].tick = now;
        g_moveCount++;
    }

    if (g_moveCount - 1 < g_cur.reversals) return;
    if (now < g_shakeBlockUntil) return;

    double dist = 0.0;
    int x = 0, y = 0, minX = 0, maxX = 0, minY = 0, maxY = 0;
    for (int i = 0; i < g_moveCount; ++i) {
        const ShakeMove& m = g_moves[i];
        dist += sqrt((double)m.dx * m.dx + (double)m.dy * m.dy);
        x += m.dx; y += m.dy;
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
    }
    if (dist < g_cur.distance) return;

    const double bw = maxX - minX, bh = maxY - minY;
    double diag = sqrt(bw * bw + bh * bh);
    if (diag < 1.0) diag = 1.0;
    if (dist * 100.0 < (double)g_cur.factor * diag) return;

    // Жест зарахований. Буфер чистимо, щоб той самий розмах не тригерив двічі,
    // плюс короткий блок — інакше доведення руху одразу дає повторне спрацювання.
    g_moveCount = 0;
    g_shakeBlockUntil = now + 400;
    PostMessageW(g_mainWnd, WMAPP_SHAKE, 0, 0);
}

// Рухи із затиснутою кнопкою ігноруємо повністю. Це знімає найнеприємніший клас
// хибних спрацювань: ривкова перемотка повзунка, малювання/стирання в редакторі,
// перетягування вікна — на симуляції саме вони пролазили крізь усі пороги, бо
// формально це і є трусіння. Без кнопки таких рухів у житті не буває.
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        switch (wParam) {
        case WM_LBUTTONDOWN: g_btnMask |= 1; g_moveCount = 0; g_haveLastPt = false; break;
        case WM_RBUTTONDOWN: g_btnMask |= 2; g_moveCount = 0; g_haveLastPt = false; break;
        case WM_MBUTTONDOWN: g_btnMask |= 4; g_moveCount = 0; g_haveLastPt = false; break;
        case WM_XBUTTONDOWN: g_btnMask |= 8; g_moveCount = 0; g_haveLastPt = false; break;
        case WM_LBUTTONUP:   g_btnMask &= ~1u; g_haveLastPt = false; break;
        case WM_RBUTTONUP:   g_btnMask &= ~2u; g_haveLastPt = false; break;
        case WM_MBUTTONUP:   g_btnMask &= ~4u; g_haveLastPt = false; break;
        case WM_XBUTTONUP:   g_btnMask &= ~8u; g_haveLastPt = false; break;
        case WM_MOUSEMOVE:
            if (!g_btnMask) {
                const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)lParam;
                ShakeFeed(m->pt, GetTickCount());
            }
            break;
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// Потік хука: нічого не робить, крім циклу повідомлень, тож колбек
// обслуговується миттєво незалежно від того, чим зайнятий UI-потік. Команди
// install/uninstall приходять синхронно через SendMessage до цього вікна —
// SetWindowsHookEx мусить викликатися саме на тому потоці, де крутиться цикл,
// бо колбек виконується в його контексті.
LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case HKW_INSTALL:
        if (!g_hook)
            g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                       GetModuleHandleW(nullptr), 0);
        return g_hook != nullptr;
    case HKW_UNINSTALL:
        if (g_hook) {
            UnhookWindowsHookEx(g_hook);
            g_hook = nullptr;
        }
        return 0;
    case HKW_MOUSE_ON:
        if (!g_mouseHook) {
            g_haveLastPt = false;
            g_moveCount  = 0;
            g_btnMask    = 0;
            g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc,
                                            GetModuleHandleW(nullptr), 0);
        }
        return g_mouseHook != nullptr;
    case HKW_MOUSE_OFF:
        if (g_mouseHook) {
            UnhookWindowsHookEx(g_mouseHook);
            g_mouseHook = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI HookThreadProc(LPVOID param)
{
    HANDLE ready = (HANDLE)param;
    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = HookWndProc;
    wc.hInstance     = inst;
    wc.lpszClassName = L"lilhelpers_hook";
    RegisterClassW(&wc);

    g_hookWnd = CreateWindowExW(0, L"lilhelpers_hook", nullptr, 0,
                                0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    SetEvent(ready);  // головний потік чекає, поки вікно готове

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    return 0;
}

bool StartHookThread()
{
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) return false;
    g_hookThread = CreateThread(nullptr, 0, HookThreadProc, ready, 0, &g_hookThreadId);
    if (g_hookThread)
        WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
    return g_hookThread != nullptr && g_hookWnd != nullptr;
}

void StopHookThread()
{
    if (g_hookThreadId)
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    if (g_hookThread) {
        WaitForSingleObject(g_hookThread, 2000);
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
    }
}

// ---------- режим роботи ----------

// Реєстрація/зняття системного хоткея під бажаний стан.
// false — лише при реальній невдачі RegisterHotKey.
bool SetHotkey(bool want)
{
    if (want && !g_hotkeyActive) {
        if (!RegisterHotKey(g_mainWnd, HOTKEY_ID, MOD_NOREPEAT, VK_CAPITAL))
            return false;
        g_hotkeyActive = true;
    } else if (!want && g_hotkeyActive) {
        UnregisterHotKey(g_mainWnd, HOTKEY_ID);
        g_hotkeyActive = false;
    }
    return true;
}

void StopInterception()
{
    g_kbHookCaps = false;
    if (g_hookWnd && !g_peekOn)   // CAPS-16: хук може бути потрібен перегляду
        SendMessageW(g_hookWnd, HKW_UNINSTALL, 0, 0);  // на потоці хука
    SetHotkey(false);
    g_interceptionOn = false;
    g_capsDown = false;
}

bool StartInterception(Mode mode)
{
    if (mode == Mode::Hook) {
        bool ok = g_hookWnd && SendMessageW(g_hookWnd, HKW_INSTALL, 0, 0) != 0;
        if (ok) { g_interceptionOn = true; g_kbHookCaps = true; }
        return ok;
    }
    // Hotkey: якщо ми зараз у remote-вікні з увімкненим пропуском — свідомо НЕ
    // реєструємо (щоб Caps ішов у клієнта); зареєструємо при виході з нього.
    if (RemotePassthroughActive()) {
        g_hotkeyActive = false;
        g_interceptionOn = true;
        return true;
    }
    if (!SetHotkey(true))
        return false;
    g_interceptionOn = true;
    return true;
}

// CAPS-1: привести перехоплення до поточного контексту (режим/налаштування/вікно).
// Hook: колбек читає прапорці наживо. Hotkey: тримаємо реєстрацію лише поза
// remote-вікнами (або коли пропуск вимкнено).
void ApplyRemoteContext()
{
    if (g_interceptionOn && g_mode == Mode::Hotkey)
        SetHotkey(!RemotePassthroughActive());
}

void ThemeTick();   // CAPS-7, визначення нижче
void PeekOnForeground();   // CAPS-16, нижче

// Зміна активного вікна: оновлюємо ознаку remote і підлаштовуємо перехоплення.
// Викликається з WinEvent-колбека на головному потоці — тому RegisterHotKey
// коректно виконується на потоці-власнику g_mainWnd.
void OnForegroundChanged()
{
    g_inRemote = IsRemoteWindow(GetForegroundWindow());
    ApplyRemoteContext();
    if (g_thPending) ThemeTick();   // CAPS-7: повноекранна програма могла закритись
    PeekOnForeground();             // CAPS-16: перегляд живе лише при тому ж Провіднику
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND, LONG, LONG, DWORD, DWORD)
{
    if (event == EVENT_SYSTEM_FOREGROUND)
        OnForegroundChanged();
}

Mode LoadMode()
{
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, kRegMode, RRF_RT_REG_DWORD,
                     nullptr, &value, &size) == ERROR_SUCCESS && value == 1)
        return Mode::Hotkey;
    return Mode::Hook;
}

void SaveMode(Mode mode)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    DWORD value = (mode == Mode::Hotkey) ? 1 : 0;
    RegSetValueExW(key, kRegMode, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(key);
}

bool LoadPassthrough()
{
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, kRegPassthrough, RRF_RT_REG_DWORD,
                     nullptr, &value, &size) == ERROR_SUCCESS)
        return value != 0;
    return true;  // за замовчуванням увімкнено
}

void SavePassthrough(bool on)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    DWORD value = on ? 1 : 0;
    RegSetValueExW(key, kRegPassthrough, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(key);
}

// ---------- CAPS-2: налаштування курсора в реєстрі ----------

int RegLoadInt(const wchar_t* name, int def, int lo, int hi)
{
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, name, RRF_RT_REG_DWORD,
                     nullptr, &value, &size) != ERROR_SUCCESS)
        return def;
    int v = (int)value;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

void RegSaveInt(const wchar_t* name, int value)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    DWORD v = (DWORD)value;
    RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(key);
}

void RegDeleteInt(const wchar_t* name)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    RegDeleteValueW(key, name);
    RegCloseKey(key);
}

void LoadCursorSettings()
{
    g_cur.enabled   = RegLoadInt(kRegCursorEnable,   1,    0,    1) != 0;
    g_cur.scale     = RegLoadInt(kRegCursorScale,    5,    2,    8);
    g_cur.holdMs    = RegLoadInt(kRegCursorHold,     1500, 500,  5000);
    g_cur.shrinkMs  = RegLoadInt(kRegCursorShrink,   300,  100,  1500);
    g_cur.windowMs  = RegLoadInt(kRegShakeWindow,    700,  300,  2000);
    g_cur.distance  = RegLoadInt(kRegShakeDistance,  1200, 300,  5000);
    g_cur.factor    = RegLoadInt(kRegShakeFactor,    350,  150,  1000);
    g_cur.reversals = RegLoadInt(kRegShakeReversals, 3,    2,    10);
    g_cur.overlay   = RegLoadInt(kRegCursorOverlay,  1,    0,    1) != 0;
}

// ---------- CAPS-2: власне збільшення ----------

// Поточний розмір курсора користувача в пікселях (шкала Windows 32..256).
int CursorSizePx()
{
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"CursorBaseSize",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
        value >= (DWORD)kCursorMinPx && value <= (DWORD)kCursorMaxPx)
        return (int)value;
    return kCursorMinPx;
}

// SPIF_UPDATEINIFILE тут свідомо: без запису в профіль частина складань Windows
// не застосовує розмір одразу, а ми все одно зобовʼязані вміти повернути
// вихідне значення (див. kRegCursorRestore), тож персистентність нічого не псує.
void ApplyCursorSizePx(int px)
{
    if (px < kCursorMinPx) px = kCursorMinPx;
    if (px > kCursorMaxPx) px = kCursorMaxPx;
    SystemParametersInfoW(SPI_SETCURSORSIZE_, 0, (PVOID)(INT_PTR)px,
                          SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}

// Повноекранні застосунки (вимога тікета): ігри не повинні ловити наш жест.
// SHQueryUserNotificationState ловить exclusive-D3D і презентаційний режим, але
// мовчить про borderless-вікна, тому додаємо геометричну перевірку — власник
// просив блокувати все, що ПОВОДИТЬСЯ як повноекранна гра.
bool IsFullscreenForeground()
{
    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state)) &&
        (state == QUNS_RUNNING_D3D_FULL_SCREEN ||
         state == QUNS_PRESENTATION_MODE ||
         state == QUNS_BUSY))
        return true;

    HWND fg = GetForegroundWindow();
    if (!fg) return false;

    // Робочий стіл і панель задач теж «на весь екран» і без рамки — це не гра.
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (!lstrcmpiW(cls, L"Progman") || !lstrcmpiW(cls, L"WorkerW") ||
        !lstrcmpiW(cls, L"Shell_TrayWnd"))
        return false;

    RECT wr;
    if (!GetWindowRect(fg, &wr)) return false;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi))
        return false;
    if (!EqualRect(&wr, &mi.rcMonitor)) return false;   // не рівно на монітор

    // Звичайне максимізоване вікно має заголовок/рамку — його не чіпаємо.
    const LONG style = GetWindowLongW(fg, GWL_STYLE);
    return (style & (WS_CAPTION | WS_THICKFRAME)) == 0;
}

// Застосувати розмір, але лише якщо анімація ще актуальна. gen == 0 — виклик із
// UI-потоку, він завжди має пріоритет; лок не дає потоку анімації втиснути свій
// проміжний кадр уже після того, як UI вирішив інше.
void ApplyCursorSizeGuarded(int px, LONG gen)
{
    EnterCriticalSection(&g_magLock);
    if (gen == 0 || g_magGen == gen)
        ApplyCursorSizePx(px);
    LeaveCriticalSection(&g_magLock);
}

void CancelMagAnimation()
{
    InterlockedIncrement(&g_magGen);
    if (g_magThread) {
        WaitForSingleObject(g_magThread, 500);
        CloseHandle(g_magThread);
        g_magThread = nullptr;
    }
}

void MagnifyRestore()
{
    KillTimer(g_mainWnd, TIMER_MAG_HOLD);
    KillTimer(g_mainWnd, TIMER_MAG_FRAME);
    OverlayDestroy();
    CancelMagAnimation();
    if (g_magState != MagState::Idle)
        ApplyCursorSizeGuarded(g_magOrigPx, 0);
    RegDeleteInt(kRegCursorRestore);
    g_magState = MagState::Idle;
}

void MagnifyStart()
{
    if (!g_cur.enabled || !g_mainWnd) return;
    if (IsFullscreenForeground()) return;

    if (g_magState == MagState::Idle) {
        g_magOrigPx = CursorSizePx();
        // слід на випадок аварійного завершення: наступний старт поверне розмір
        RegSaveInt(kRegCursorRestore, g_magOrigPx);
        int target = g_magOrigPx * g_cur.scale;
        if (target > kCursorMaxPx) target = kCursorMaxPx;
        g_magTargetPx = target;
        ApplyCursorSizeGuarded(target, 0);
    } else if (g_magState == MagState::Shrinking) {
        KillTimer(g_mainWnd, TIMER_MAG_FRAME);       // потрусили ще раз під час
        OverlayDestroy();                            // зменшення — вертаємо великий
        CancelMagAnimation();
        ApplyCursorSizeGuarded(g_magTargetPx, 0);
    }

    g_magState = MagState::Big;
    SetTimer(g_mainWnd, TIMER_MAG_HOLD, (UINT)g_cur.holdMs, nullptr);
}

// Плавне зменшення потрібне, щоб око встигло провести курсор до справжнього
// розміру (вимога тікета). Крива ease-out: спочатку швидко, під кінець м'яко.
DWORD WINAPI MagShrinkThread(LPVOID param)
{
    const LONG gen = (LONG)(LONG_PTR)param;
    const int from = g_magTargetPx, to = g_magOrigPx;
    const DWORD duration = (DWORD)g_cur.shrinkMs;
    const DWORD start = GetTickCount();

    int last = from;
    for (;;) {
        if (g_magGen != gen) return 0;               // скасовано новим жестом
        const DWORD elapsed = GetTickCount() - start;
        if (elapsed >= duration) break;
        double t = (double)elapsed / duration;
        t = 1.0 - (1.0 - t) * (1.0 - t);
        // Windows має власну сходинку розмірів курсора (32 px + кратне 16), тож
        // проміжні значення між сходинками виглядають однаково, а коштують по
        // повному бродкасту. Округлюємо — удвічі менше викликів без втрати плавності.
        int px = (int)(from + (to - from) * t);
        px = kCursorMinPx + ((px - kCursorMinPx + 8) / 16) * 16;
        if (px != last) {
            ApplyCursorSizeGuarded(px, gen);
            last = px;
        }
        Sleep(8);
    }
    if (g_magGen == gen) {
        ApplyCursorSizeGuarded(to, gen);
        PostMessageW(g_mainWnd, WMAPP_MAGDONE, 0, (LPARAM)gen);
    }
    return 0;
}

// ---------- оверлейне зменшення ----------

void OverlayDestroy()
{
    if (g_overlay) { DestroyWindow(g_overlay); g_overlay = nullptr; }
    if (g_overlayIcon) { DestroyIcon(g_overlayIcon); g_overlayIcon = nullptr; }
}

// Малюємо копію курсора заданого розміру в layered-вікно під гарячою точкою.
void OverlayFrame(int size)
{
    if (!g_overlay || !g_overlayIcon || size < 1) return;

    POINT pt;
    GetCursorPos(&pt);

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        DrawIconEx(mem, 0, 0, g_overlayIcon, size, size, 0, nullptr, DI_NORMAL);

        // UpdateLayeredWindow хоче premultiplied alpha. Курсори з 1-бітною маскою
        // приходять із нульовою альфою — тоді копія була б невидимою, тож такі
        // пікселі робимо непрозорими за наявністю кольору.
        BYTE* p = (BYTE*)bits;
        const int count = size * size;
        bool anyAlpha = false;
        for (int i = 0; i < count; ++i)
            if (p[i * 4 + 3]) { anyAlpha = true; break; }
        for (int i = 0; i < count; ++i) {
            BYTE* px = p + i * 4;
            if (!anyAlpha)
                px[3] = (px[0] || px[1] || px[2]) ? 255 : 0;
            const int a = px[3];
            px[0] = (BYTE)(px[0] * a / 255);
            px[1] = (BYTE)(px[1] * a / 255);
            px[2] = (BYTE)(px[2] * a / 255);
        }

        POINT dst = { pt.x - MulDiv(g_ovHotspot.x, size, g_ovBasePx),
                      pt.y - MulDiv(g_ovHotspot.y, size, g_ovBasePx) };
        SIZE  wnd = { size, size };
        POINT src = { 0, 0 };
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(g_overlay, screen, &dst, &wnd, mem, &src, 0, &bf, ULW_ALPHA);
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// Системний розмір повертаємо у фоні: один виклик, але дорогий, і блокувати ним
// анімацію не можна.
DWORD WINAPI RestoreSizeThread(LPVOID param)
{
    const LONG gen = (LONG)(LONG_PTR)param;
    ApplyCursorSizeGuarded(g_magOrigPx, gen);
    // слід у реєстрі прибираємо аж тут: поки системний розмір не повернувся
    // насправді, аварійне завершення має лишати можливість його відновити
    PostMessageW(g_mainWnd, WMAPP_MAGDONE, 0, (LPARAM)gen);
    return 0;
}

bool OverlayBeginShrink()
{
    CURSORINFO ci = { sizeof(ci) };
    if (!GetCursorInfo(&ci) || !ci.hCursor || !(ci.flags & CURSOR_SHOWING))
        return false;
    HICON copy = CopyIcon(ci.hCursor);
    if (!copy) return false;

    ICONINFO ii = {};
    if (GetIconInfo(copy, &ii)) {
        g_ovHotspot.x = (LONG)ii.xHotspot;
        g_ovHotspot.y = (LONG)ii.yHotspot;
        BITMAP bm = {};
        HBITMAP src = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
        g_ovBasePx = (GetObjectW(src, sizeof(bm), &bm) && bm.bmWidth > 0) ? bm.bmWidth : 32;
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    } else {
        g_ovHotspot.x = g_ovHotspot.y = 0;
        g_ovBasePx = 32;
    }

    OverlayDestroy();
    g_overlayIcon = copy;
    g_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                                WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                L"lilhelpers_overlay", nullptr, WS_POPUP,
                                0, 0, 1, 1, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!g_overlay) { OverlayDestroy(); return false; }
    ShowWindow(g_overlay, SW_SHOWNOACTIVATE);

    g_ovFrom  = g_magTargetPx;
    g_ovTo    = g_magOrigPx;
    g_ovStart = GetTickCount();
    OverlayFrame(g_ovFrom);

    // системний розмір вертаємо паралельно — копія прикриє момент перемикання
    const LONG gen = InterlockedIncrement(&g_magGen);
    if (HANDLE t = CreateThread(nullptr, 0, RestoreSizeThread, (LPVOID)(LONG_PTR)gen, 0, nullptr))
        CloseHandle(t);

    SetTimer(g_mainWnd, TIMER_MAG_FRAME, 16, nullptr);   // ~60 кадрів/с
    return true;
}

void OverlayFrameTick()
{
    const DWORD elapsed = GetTickCount() - g_ovStart;
    const DWORD duration = (DWORD)g_cur.shrinkMs;
    if (elapsed >= duration) {
        KillTimer(g_mainWnd, TIMER_MAG_FRAME);
        OverlayDestroy();
        g_magState = MagState::Idle;   // слід у реєстрі знімає RestoreSizeThread
        return;
    }
    double t = (double)elapsed / duration;
    t = 1.0 - (1.0 - t) * (1.0 - t);            // ease-out
    OverlayFrame((int)(g_ovFrom + (g_ovTo - g_ovFrom) * t));
}

void MagnifyBeginShrink()
{
    KillTimer(g_mainWnd, TIMER_MAG_HOLD);
    if (g_magState != MagState::Big) return;
    CancelMagAnimation();
    g_magState = MagState::Shrinking;

    if (g_cur.overlay && OverlayBeginShrink())
        return;

    const LONG gen = InterlockedIncrement(&g_magGen);
    g_magThread = CreateThread(nullptr, 0, MagShrinkThread,
                               (LPVOID)(LONG_PTR)gen, 0, nullptr);
    if (!g_magThread)          // потік не створився — просто повертаємо розмір
        MagnifyRestore();
}

// Якщо попередній запуск помер із великим курсором — повертаємо розмір.
void RecoverCursorSize()
{
    const int px = RegLoadInt(kRegCursorRestore, 0, 0, kCursorMaxPx);
    if (px >= kCursorMinPx) {
        ApplyCursorSizePx(px);
        RegDeleteInt(kRegCursorRestore);
    }
}

// Мишачий хук тримаємо лише поки фіча ввімкнена — зайвий глобальний хук
// у системі не потрібен.
void ApplyCursorFeature()
{
    if (!g_hookWnd) return;
    if (g_cur.enabled) {
        SendMessageW(g_hookWnd, HKW_MOUSE_ON, 0, 0);
    } else {
        SendMessageW(g_hookWnd, HKW_MOUSE_OFF, 0, 0);
        MagnifyRestore();
    }
}

// ---------- автозапуск (Task Scheduler через COM) ----------
//
// Свідомо НЕ через запуск schtasks.exe: породження дочірнього процесу, який
// створює задачу з найвищими правами, — типовий персистенс-патерн малварі, і
// ML-евристики антивірусів на нього реагують. COM-шлях робить те саме напряму.

// Підключення до планувальника; при true — звільнити обидва вказівники.
bool OpenTaskRoot(ITaskService** svcOut, ITaskFolder** rootOut)
{
    *svcOut = nullptr;
    *rootOut = nullptr;

    ITaskService* svc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ITaskService, (void**)&svc)))
        return false;

    VARIANT empty;
    VariantInit(&empty);
    if (FAILED(svc->Connect(empty, empty, empty, empty))) {
        svc->Release();
        return false;
    }

    ITaskFolder* root = nullptr;
    BSTR path = SysAllocString(L"\\");
    HRESULT hr = svc->GetFolder(path, &root);
    SysFreeString(path);
    if (FAILED(hr)) {
        svc->Release();
        return false;
    }

    *svcOut = svc;
    *rootOut = root;
    return true;
}

bool AutostartEnabled()
{
    ITaskService* svc;
    ITaskFolder* root;
    if (!OpenTaskRoot(&svc, &root)) return false;

    IRegisteredTask* task = nullptr;
    BSTR name = SysAllocString(kTaskName);
    bool found = SUCCEEDED(root->GetTask(name, &task)) && task;
    SysFreeString(name);

    if (task) task->Release();
    root->Release();
    svc->Release();
    return found;
}

void FillTaskDefinition(ITaskDefinition* def)
{
    IRegistrationInfo* info = nullptr;
    if (SUCCEEDED(def->get_RegistrationInfo(&info)) && info) {
        BSTR s = SysAllocString(S(Str::TaskDesc));
        info->put_Description(s);
        SysFreeString(s);
        info->Release();
    }

    // Найвищі права: без них перемикання не діє в elevated-вікнах (UIPI)
    IPrincipal* principal = nullptr;
    if (SUCCEEDED(def->get_Principal(&principal)) && principal) {
        principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
        principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        principal->Release();
    }

    // Дефолти планувальника розраховані на разові задачі й фоновому застосунку
    // шкідливі: на батареї він би не стартував, а через 3 доби безперервної
    // роботи його вбило б по ExecutionTimeLimit.
    ITaskSettings* settings = nullptr;
    if (SUCCEEDED(def->get_Settings(&settings)) && settings) {
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        BSTR noLimit = SysAllocString(L"PT0S");
        settings->put_ExecutionTimeLimit(noLimit);
        SysFreeString(noLimit);
        settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        settings->put_StartWhenAvailable(VARIANT_TRUE);
        settings->put_Enabled(VARIANT_TRUE);

        IIdleSettings* idle = nullptr;
        if (SUCCEEDED(settings->get_IdleSettings(&idle)) && idle) {
            idle->put_StopOnIdleEnd(VARIANT_FALSE);
            idle->Release();
        }
        settings->Release();
    }

    ITriggerCollection* triggers = nullptr;
    if (SUCCEEDED(def->get_Triggers(&triggers)) && triggers) {
        ITrigger* trigger = nullptr;
        if (SUCCEEDED(triggers->Create(TASK_TRIGGER_LOGON, &trigger)) && trigger)
            trigger->Release();
        triggers->Release();
    }

    IActionCollection* actions = nullptr;
    if (SUCCEEDED(def->get_Actions(&actions)) && actions) {
        IAction* action = nullptr;
        if (SUCCEEDED(actions->Create(TASK_ACTION_EXEC, &action)) && action) {
            IExecAction* exec = nullptr;
            if (SUCCEEDED(action->QueryInterface(IID_IExecAction, (void**)&exec)) && exec) {
                wchar_t exePath[MAX_PATH];
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                BSTR p = SysAllocString(exePath);
                exec->put_Path(p);
                SysFreeString(p);
                exec->Release();
            }
            action->Release();
        }
        actions->Release();
    }
}

bool SetAutostart(bool enable)
{
    ITaskService* svc;
    ITaskFolder* root;
    if (!OpenTaskRoot(&svc, &root)) return false;

    BSTR name = SysAllocString(kTaskName);
    bool ok = false;

    if (!enable) {
        HRESULT hr = root->DeleteTask(name, 0);
        ok = SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    } else {
        ITaskDefinition* def = nullptr;
        if (SUCCEEDED(svc->NewTask(0, &def)) && def) {
            FillTaskDefinition(def);

            VARIANT empty;
            VariantInit(&empty);
            IRegisteredTask* registered = nullptr;
            ok = SUCCEEDED(root->RegisterTaskDefinition(
                name, def, TASK_CREATE_OR_UPDATE,
                empty, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, &registered));
            if (registered) registered->Release();
            def->Release();
        }
    }

    SysFreeString(name);
    root->Release();
    svc->Release();
    return ok;
}

// ---------- GUI ----------

// PNG-логотип із ресурсів (GDI+ малює його з альфа-каналом поверх фону вікна)
void LoadLogo(HINSTANCE hInst)
{
    HRSRC res = FindResourceW(hInst, MAKEINTRESOURCEW(IDR_LOGO_PNG), RT_RCDATA);
    if (!res) return;
    HGLOBAL blob = LoadResource(hInst, res);
    void* data = LockResource(blob);
    DWORD size = SizeofResource(hInst, res);
    if (!data || !size) return;

    if (IStream* stream = SHCreateMemStream((const BYTE*)data, size)) {
        g_logo = Gdiplus::Image::FromStream(stream);
        stream->Release();
        if (g_logo && g_logo->GetLastStatus() != Gdiplus::Ok) {
            delete g_logo;
            g_logo = nullptr;
        }
    }
}

void PaintWindow(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    if (g_logo) {
        Gdiplus::Graphics g(dc);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.DrawImage(g_logo, (INT)g_logoRect.left, (INT)g_logoRect.top,
                    (INT)(g_logoRect.right - g_logoRect.left),
                    (INT)(g_logoRect.bottom - g_logoRect.top));
    }
    EndPaint(hwnd, &ps);
}

// ---------- CAPS-7: день/ніч — реєстр, час, сонце ----------

bool RegLoadStr(const wchar_t* name, wchar_t* buf, DWORD cch)
{
    DWORD size = cch * sizeof(wchar_t);
    return RegGetValueW(HKEY_CURRENT_USER, kRegPath, name, RRF_RT_REG_SZ,
                        nullptr, buf, &size) == ERROR_SUCCESS;
}

void RegSaveStr(const wchar_t* name, const wchar_t* value)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value,
                   (DWORD)((lstrlenW(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

// Координата з поля/реєстру: приймаємо і «50.77», і «50,77».
bool ParseCoord(const wchar_t* s, double lo, double hi, double& out)
{
    wchar_t tmp[32] = {};
    for (int i = 0; i < 31 && s[i]; ++i) tmp[i] = (s[i] == L',') ? L'.' : s[i];
    wchar_t* end = nullptr;
    const double v = wcstod(tmp, &end);
    if (end == tmp || v < lo || v > hi) return false;
    out = v;
    return true;
}

__time64_t NowUnix() { return _time64(nullptr); }

void LocalDate(__time64_t t, int& y, int& m, int& d, int& minOfDay)
{
    struct tm lt = {};
    _localtime64_s(&lt, &t);
    y = lt.tm_year + 1900; m = lt.tm_mon + 1; d = lt.tm_mday;
    minOfDay = lt.tm_hour * 60 + lt.tm_min;
}

void FormatClock(wchar_t* buf, size_t n, __time64_t t)
{
    struct tm lt = {};
    _localtime64_s(&lt, &t);
    swprintf(buf, n, L"%02d:%02d", lt.tm_hour, lt.tm_min);
}

// ---- CAPS-7: sun math begin ----
constexpr double kPi = 3.14159265358979323846;
double Rad(double d) { return d * kPi / 180.0; }
double Deg(double r) { return r * 180.0 / kPi; }

double JulianDay(int y, int m, int d)   // 0:00 UTC заданої дати
{
    if (m <= 2) { y--; m += 12; }
    const int A = y / 100, B = 2 - A + A / 4;
    return floor(365.25 * (y + 4716)) + floor(30.6001 * (m + 1)) + d + B - 1524.5;
}

// Схід/захід за NOAA (точність ~1 хв). riseMin/setMin — хвилини UTC від 0:00 дати
// (можуть виходити за межі доби для далеких поясів). false = сонце цієї доби не
// сходить (polarDay=false) або не заходить (polarDay=true).
bool SunTimesUtc(int y, int m, int d, double lat, double lon,
                 double& riseMin, double& setMin, bool& polarDay)
{
    const double jc = (JulianDay(y, m, d) + 0.5 - 2451545.0) / 36525.0;
    const double L0 = fmod(280.46646 + jc * (36000.76983 + jc * 0.0003032), 360.0);
    const double M  = 357.52911 + jc * (35999.05029 - 0.0001537 * jc);
    const double e  = 0.016708634 - jc * (0.000042037 + 0.0000001267 * jc);
    const double C  = sin(Rad(M)) * (1.914602 - jc * (0.004817 + 0.000014 * jc))
                    + sin(Rad(2 * M)) * (0.019993 - 0.000101 * jc)
                    + sin(Rad(3 * M)) * 0.000289;
    const double omega   = 125.04 - 1934.136 * jc;
    const double appLong = L0 + C - 0.00569 - 0.00478 * sin(Rad(omega));
    const double obl0 = 23.0 + (26.0 + (21.448 - jc * (46.815 + jc * (0.00059 - jc * 0.001813))) / 60.0) / 60.0;
    const double obl  = obl0 + 0.00256 * cos(Rad(omega));
    const double decl = asin(sin(Rad(obl)) * sin(Rad(appLong)));
    const double yy   = tan(Rad(obl / 2)) * tan(Rad(obl / 2));
    const double eqTime = 4 * Deg(yy * sin(2 * Rad(L0)) - 2 * e * sin(Rad(M))
                          + 4 * e * yy * sin(Rad(M)) * cos(2 * Rad(L0))
                          - 0.5 * yy * yy * sin(4 * Rad(L0)) - 1.25 * e * e * sin(2 * Rad(M)));
    const double cosHa = cos(Rad(90.833)) / (cos(Rad(lat)) * cos(decl)) - tan(Rad(lat)) * tan(decl);
    if (cosHa >= 1.0)  { polarDay = false; return false; }
    if (cosHa <= -1.0) { polarDay = true;  return false; }
    const double ha   = Deg(acos(cosHa));
    const double noon = 720.0 - 4.0 * lon - eqTime;
    riseMin = noon - ha * 4.0;
    setMin  = noon + ha * 4.0;
    return true;
}

// Схід/захід (unix) для локальної дати, що містить t. 0 = ок, 1 = полярна ніч,
// 2 = полярний день.
int SunEventsFor(__time64_t t, double lat, double lon, __time64_t& rise, __time64_t& set)
{
    int y, m, d, mod;
    LocalDate(t, y, m, d, mod);
    double r = 0, s = 0; bool pd = false;
    if (!SunTimesUtc(y, m, d, lat, lon, r, s, pd)) return pd ? 2 : 1;
    struct tm g = {};
    g.tm_year = y - 1900; g.tm_mon = m - 1; g.tm_mday = d;
    const __time64_t base = _mkgmtime64(&g);
    rise = base + (__time64_t)llround(r * 60.0);
    set  = base + (__time64_t)llround(s * 60.0);
    return 0;
}
// ---- CAPS-7: sun math end ----

// Що має бути зараз за налаштуваннями (без урахування ручного вибору) і коли
// наступна межа. usedFallback — координат нема, тимчасово рахуємо за 07:00/19:00.
bool ThemeWantDark(__time64_t now, __time64_t& nextBoundary, bool& usedFallback)
{
    usedFallback = false;
    if (!g_th.bySchedule && g_fix.ok) {
        __time64_t rise = 0, set = 0;
        const int kind = SunEventsFor(now, g_fix.lat, g_fix.lon, rise, set);
        if (kind == 0) {
            if (now < rise) { nextBoundary = rise; return true; }
            if (now < set)  { nextBoundary = set;  return false; }
            __time64_t r2 = 0, s2 = 0;      // після заходу — до завтрашнього сходу
            nextBoundary = (SunEventsFor(now + 86400, g_fix.lat, g_fix.lon, r2, s2) == 0)
                           ? r2 : now + 86400;
            return true;
        }
        nextBoundary = now + 6 * 3600;   // полярний день/ніч — перевіримо пізніше
        return kind == 1;
    }
    int df = g_th.darkFrom, lf = g_th.lightFrom;
    if (!g_th.bySchedule) { usedFallback = true; df = 19 * 60; lf = 7 * 60; }
    int y, m, d, mod;
    LocalDate(now, y, m, d, mod);
    auto at = [&](int minutes, int dayOffset) {
        struct tm lt = {};
        lt.tm_year = y - 1900; lt.tm_mon = m - 1; lt.tm_mday = d + dayOffset;
        lt.tm_hour = minutes / 60; lt.tm_min = minutes % 60; lt.tm_isdst = -1;
        return _mktime64(&lt);    // нормалізує d+1 і DST сама
    };
    if (df == lf) { nextBoundary = at(df, mod < df ? 0 : 1); return false; }
    const bool dark = (df < lf) ? (mod >= df && mod < lf) : (mod >= df || mod < lf);
    const __time64_t cands[4] = { at(df, 0), at(lf, 0), at(df, 1), at(lf, 1) };
    nextBoundary = 0;
    for (const __time64_t c : cands)
        if (c > now && (nextBoundary == 0 || c < nextBoundary)) nextBoundary = c;
    return dark;
}

bool ThemeIsDark()
{
    DWORD v = 1, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kPersonalize, L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
        return v == 0;
    return false;
}

// Бродкаст — на окремому потоці: SendMessageTimeout чекає на кожне вікно, і
// зависле вікно не має морозити наш UI.
DWORD WINAPI ThemeBroadcastThread(LPVOID)
{
    DWORD_PTR res = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet",
                        SMTO_ABORTIFHUNG, 2000, &res);
    return 0;
}

void ThemeApply(bool dark)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kPersonalize, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    const DWORD v = dark ? 0 : 1;
    RegSetValueExW(key, L"AppsUseLightTheme",   0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegSetValueExW(key, L"SystemUsesLightTheme", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(key);
    if (HANDLE t = CreateThread(nullptr, 0, ThemeBroadcastThread, nullptr, 0, nullptr))
        CloseHandle(t);
}

// ---------- CAPS-7: розташування ----------

bool LocateWindows(double& lat, double& lon, bool allowPrompt)
{
    ILocation* loc = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_Location, nullptr, CLSCTX_INPROC_SERVER,
                                kIID_ILocation, (void**)&loc)) || !loc)
        return false;
    bool ok = false;
    IID types[1] = { kIID_ILatLongReport };
    if (allowPrompt) loc->RequestPermissions(nullptr, types, 1, TRUE);
    for (int i = 0; i < 20; ++i) {           // до ~10 с: сенсор може прокидатись
        LOCATION_REPORT_STATUS st = REPORT_NOT_SUPPORTED;
        if (FAILED(loc->GetReportStatus(kIID_ILatLongReport, &st))) break;
        if (st == REPORT_RUNNING) {
            ILocationReport* rep = nullptr;
            if (SUCCEEDED(loc->GetReport(kIID_ILatLongReport, &rep)) && rep) {
                ILatLongReport* ll = nullptr;
                if (SUCCEEDED(rep->QueryInterface(kIID_ILatLongReport, (void**)&ll)) && ll) {
                    double la = 0, lo = 0;
                    if (SUCCEEDED(ll->GetLatitude(&la)) && SUCCEEDED(ll->GetLongitude(&lo))) {
                        lat = la; lon = lo; ok = true;
                    }
                    ll->Release();
                }
                rep->Release();
            }
            break;
        }
        if (st == REPORT_ACCESS_DENIED || st == REPORT_NOT_SUPPORTED || st == REPORT_ERROR)
            break;
        Sleep(500);
    }
    loc->Release();
    return ok;
}

// Один GET до ip-api.com (без ключа, HTTP — координати міста, не секрет).
bool LocateIp(double& lat, double& lon)
{
    HINTERNET h = InternetOpenW(L"lilhelpers", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!h) return false;
    DWORD to = 8000;
    InternetSetOptionW(h, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionW(h, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
    bool ok = false;
    HINTERNET u = InternetOpenUrlW(h, L"http://ip-api.com/json/?fields=status,lat,lon", nullptr, 0,
                                   INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    if (u) {
        char buf[1024] = {};
        DWORD n = 0, total = 0;
        while (total < sizeof(buf) - 1 &&
               InternetReadFile(u, buf + total, (DWORD)(sizeof(buf) - 1 - total), &n) && n > 0)
            total += n;
        buf[total] = 0;
        const char* pla = strstr(buf, "\"lat\":");
        const char* plo = strstr(buf, "\"lon\":");
        if (strstr(buf, "\"status\":\"success\"") && pla && plo) {
            lat = atof(pla + 6); lon = atof(plo + 6);
            ok = fabs(lat) <= 90 && fabs(lon) <= 180 && (lat != 0 || lon != 0);
        }
        InternetCloseHandle(u);
    }
    InternetCloseHandle(h);
    return ok;
}

// Найгрубіше: довгота з UTC-зміщення (60 хв = 15°), широта/довгота країни з
// регіону Windows, якщо він є. Похибка сходу/заходу — до години.
bool LocateTimeZone(double& lat, double& lon)
{
    TIME_ZONE_INFORMATION tzi = {};
    if (GetTimeZoneInformation(&tzi) == TIME_ZONE_ID_INVALID) return false;
    lon = -tzi.Bias / 4.0;
    lat = 50.0;
    wchar_t buf[32] = {};
    const GEOID g = GetUserGeoID(GEOCLASS_NATION);
    double v = 0;
    if (g != GEOID_NOT_AVAILABLE && GetGeoInfoW(g, GEO_LATITUDE, buf, 32, 0) > 0 && ParseCoord(buf, -90, 90, v))
        lat = v;
    if (g != GEOID_NOT_AVAILABLE && GetGeoInfoW(g, GEO_LONGITUDE, buf, 32, 0) > 0 && ParseCoord(buf, -180, 180, v))
        lon = v;
    return true;
}

DWORD WINAPI LocateThread(LPVOID p)
{
    LocResult* r = (LocResult*)p;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    r->ok = false;
    auto tryWin = [&](bool prompt) { if (!r->ok && LocateWindows(r->lat, r->lon, prompt)) { r->src = LocSource::Windows;  r->ok = true; } };
    auto tryIp  = [&]()            { if (!r->ok && LocateIp(r->lat, r->lon))              { r->src = LocSource::Ip;       r->ok = true; } };
    auto tryTz  = [&]()            { if (!r->ok && LocateTimeZone(r->lat, r->lon))        { r->src = LocSource::TimeZone; r->ok = true; } };
    switch (r->src) {
    case LocSource::Windows:  tryWin(r->prompt); break;
    case LocSource::Ip:       tryIp();  break;
    case LocSource::TimeZone: tryTz();  break;
    default:                  tryWin(false); tryIp(); tryTz(); break;   // Auto: без діалогів
    }
    CoUninitialize();
    PostMessageW(g_mainWnd, WMAPP_THEMELOC, 0, (LPARAM)r);
    return 0;
}

void SaveFixCache()
{
    wchar_t b[32];
    swprintf(b, 32, L"%.5f", g_fix.lat); RegSaveStr(kRegThemeCacheLat, b);
    swprintf(b, 32, L"%.5f", g_fix.lon); RegSaveStr(kRegThemeCacheLon, b);
    RegSaveInt(kRegThemeCacheSrc, (int)g_fix.src);
    RegSaveInt(kRegThemeCacheAt,  (int)(DWORD)g_fix.at);
}

void UseManualFix()
{
    g_fix.ok = g_th.hasManual;
    g_fix.lat = g_th.lat; g_fix.lon = g_th.lon;
    g_fix.src = LocSource::Manual;
    g_fix.at  = NowUnix();
}

void StartLocate()
{
    if (g_th.src == LocSource::Manual) { UseManualFix(); return; }
    if (InterlockedCompareExchange(&g_locBusy, 1, 0) != 0) return;   // уже визначаємо
    LocResult* r = new LocResult;
    r->src = g_th.src;
    r->gen = ++g_locGen;
    r->prompt = (g_th.src == LocSource::Windows) && !g_locPrompted;
    if (r->prompt) g_locPrompted = true;
    HANDLE t = CreateThread(nullptr, 0, LocateThread, r, 0, nullptr);
    if (!t) { delete r; g_locBusy = 0; return; }
    CloseHandle(t);
}

// ---------- CAPS-7: логіка перемикання ----------

void UpdateThemeStatus();   // нижче, у розділі UI

void ThemeTick()
{
    if (!g_th.enabled) return;
    const __time64_t now = NowUnix();
    __time64_t next = 0; bool fb = false;
    bool want = ThemeWantDark(now, next, fb);
    if (g_thOvUntil) {
        if (now < g_thOvUntil) want = g_thOvDark;
        else { g_thOvUntil = 0; RegDeleteInt(kRegThemeOvUntil); RegDeleteInt(kRegThemeOvDark); }
    }
    if (want != ThemeIsDark()) {
        if (IsFullscreenForeground()) g_thPending = true;
        else { ThemeApply(want); g_thPending = false; }
    } else {
        g_thPending = false;
    }
    // координати старіші за добу — оновити у фоні (сенсор/IP; ручні не старіють)
    if (!g_th.bySchedule && g_th.src != LocSource::Manual && now - g_fix.at > 86400)
        StartLocate();
    UpdateThemeStatus();
}

void ThemeToggleNow()
{
    const bool target = !ThemeIsDark();
    ThemeApply(target);
    g_thPending = false;
    if (g_th.enabled) {
        __time64_t next = 0; bool fb = false;
        ThemeWantDark(NowUnix(), next, fb);
        g_thOvUntil = next; g_thOvDark = target;
        RegSaveInt(kRegThemeOvUntil, (int)(DWORD)next);
        RegSaveInt(kRegThemeOvDark, target ? 1 : 0);
    }
    UpdateThemeStatus();
}

void LoadThemeSettings()
{
    g_th.enabled    = RegLoadInt(kRegThemeAuto,  0, 0, 1) != 0;
    g_th.bySchedule = RegLoadInt(kRegThemeSched, 0, 0, 1) != 0;
    g_th.darkFrom   = RegLoadInt(kRegThemeDarkFrom,  19 * 60, 0, 1439);
    g_th.lightFrom  = RegLoadInt(kRegThemeLightFrom, 7 * 60,  0, 1439);
    g_th.src        = (LocSource)RegLoadInt(kRegThemeLocSrc, 0, 0, 4);
    wchar_t b[32] = {};
    g_th.hasManual = RegLoadStr(kRegThemeLat, b, 32) && ParseCoord(b, -90, 90, g_th.lat)
                  && RegLoadStr(kRegThemeLon, b, 32) && ParseCoord(b, -180, 180, g_th.lon);
    if (RegLoadStr(kRegThemeCacheLat, b, 32) && ParseCoord(b, -90, 90, g_fix.lat)
     && RegLoadStr(kRegThemeCacheLon, b, 32) && ParseCoord(b, -180, 180, g_fix.lon)) {
        g_fix.ok  = true;
        g_fix.src = (LocSource)RegLoadInt(kRegThemeCacheSrc, 0, 0, 4);
        g_fix.at  = (DWORD)RegLoadInt(kRegThemeCacheAt, 0, INT_MIN, INT_MAX);
    }
    if (g_th.src == LocSource::Manual) UseManualFix();
    g_thOvUntil = (DWORD)RegLoadInt(kRegThemeOvUntil, 0, INT_MIN, INT_MAX);
    g_thOvDark  = RegLoadInt(kRegThemeOvDark, 0, 0, 1) != 0;
}

void SaveThemeSettings()
{
    RegSaveInt(kRegThemeAuto,      g_th.enabled ? 1 : 0);
    RegSaveInt(kRegThemeSched,     g_th.bySchedule ? 1 : 0);
    RegSaveInt(kRegThemeDarkFrom,  g_th.darkFrom);
    RegSaveInt(kRegThemeLightFrom, g_th.lightFrom);
    RegSaveInt(kRegThemeLocSrc,    (int)g_th.src);
}

// Версія з VERSIONINFO самого exe — єдине джерело лишається lilhelpers.rc.
void ExeVersionString(wchar_t* buf, size_t n)
{
    buf[0] = 0;
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &dummy);
    if (!size) return;
    BYTE* data = new BYTE[size];
    VS_FIXEDFILEINFO* ffi = nullptr; UINT len = 0;
    if (GetFileVersionInfoW(path, 0, size, data) &&
        VerQueryValueW(data, L"\\", (LPVOID*)&ffi, &len) && ffi)
        swprintf(buf, n, L"%u.%u.%u", HIWORD(ffi->dwFileVersionMS),
                 LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS));
    delete[] data;
}

// ---------- CAPS-10: автооновлення — мережа, крипто, заміна файлу ----------

void ExePath(wchar_t* buf) { GetModuleFileNameW(nullptr, buf, MAX_PATH); }

// HTTPS GET: у пам'ять (toFile == nullptr) або у файл. Редиректи GitHub → CDN WinINet
// проходить сам. Ліміт пам'яті 4 МБ — API-відповідь і підпис малі.
bool HttpGet(const wchar_t* url, std::vector<BYTE>& out, const wchar_t* toFile)
{
    wchar_t ver[32] = {}, ua[64] = {};
    ExeVersionString(ver, 32);
    swprintf(ua, 64, L"lilhelpers/%s", ver);
    HINTERNET h = InternetOpenW(ua, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!h) return false;
    DWORD to = 15000;
    InternetSetOptionW(h, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionW(h, INTERNET_OPTION_SEND_TIMEOUT,    &to, sizeof(to));
    InternetSetOptionW(h, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
    bool ok = false;
    HINTERNET u = InternetOpenUrlW(h, url, L"Accept: application/vnd.github+json\r\n", (DWORD)-1,
                                   INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD |
                                   INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    if (u) {
        DWORD status = 0, sz = sizeof(status);
        HttpQueryInfoW(u, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &sz, nullptr);
        if (status == 200) {
            HANDLE f = INVALID_HANDLE_VALUE;
            if (toFile) f = CreateFileW(toFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
            if (!toFile || f != INVALID_HANDLE_VALUE) {
                ok = true;
                static BYTE buf[65536];
                DWORD n = 0;
                while (InternetReadFile(u, buf, sizeof(buf), &n) && n > 0) {
                    if (toFile) {
                        DWORD w = 0;
                        if (!WriteFile(f, buf, n, &w, nullptr) || w != n) { ok = false; break; }
                    } else {
                        out.insert(out.end(), buf, buf + n);
                        if (out.size() > (4u << 20)) { ok = false; break; }
                    }
                }
                if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            }
        }
        InternetCloseHandle(u);
    }
    InternetCloseHandle(h);
    return ok;
}

// "v1.6.0" / "1.6.0" → [1,6,0]
bool ParseVersion(const wchar_t* s, int v[3])
{
    if (*s == L'v' || *s == L'V') ++s;
    for (int i = 0; i < 3; ++i) {
        wchar_t* end = nullptr;
        v[i] = (int)wcstol(s, &end, 10);
        if (end == s) return false;
        s = end;
        if (i < 2) { if (*s != L'.') return false; ++s; }
    }
    return true;
}

int CompareVersion(const wchar_t* a, const wchar_t* b)
{
    int x[3] = {}, y[3] = {};
    if (!ParseVersion(a, x) || !ParseVersion(b, y)) return 0;
    for (int i = 0; i < 3; ++i) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

// ---- CAPS-10: crypto begin ----
bool HexToBytes(const char* hex, BYTE* out, size_t n)
{
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        const int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (BYTE)((hi << 4) | lo);
    }
    return hex[2 * n] == 0;
}

bool Sha256File(const wchar_t* path, BYTE out[32])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hh = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    if (BCryptCreateHash(alg, &hh, nullptr, 0, nullptr, 0, 0) == 0) {
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            static BYTE buf[65536];
            DWORD n = 0;
            ok = true;
            while (ReadFile(f, buf, sizeof(buf), &n, nullptr) && n > 0)
                if (BCryptHashData(hh, buf, n, 0) != 0) { ok = false; break; }
            CloseHandle(f);
            if (ok) ok = BCryptFinishHash(hh, out, 32, 0) == 0;
        }
        BCryptDestroyHash(hh);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// DER ECDSA-Sig-Value { r INTEGER, s INTEGER } (так пише openssl) → r||s по 32 байти
// (так хоче BCryptVerifySignature).
bool DerSigToRaw(const BYTE* d, size_t n, BYTE raw[64])
{
    size_t i = 0;
    if (n < 8 || d[i++] != 0x30) return false;
    size_t len = d[i++];
    if (len & 0x80) { int k = (int)(len & 0x7f); len = 0; while (k-- > 0 && i < n) len = (len << 8) | d[i++]; }
    for (int part = 0; part < 2; ++part) {
        if (i + 2 > n || d[i++] != 0x02) return false;
        size_t l = d[i++];
        if (l == 0 || i + l > n) return false;
        const BYTE* p = d + i;
        size_t take = l;
        while (take > 32 && *p == 0) { ++p; --take; }   // ASN.1 додає 0x00 перед старшим бітом
        if (take > 32) return false;
        memset(raw + part * 32, 0, 32);
        memcpy(raw + part * 32 + (32 - take), p, take);
        i += l;
    }
    return true;
}

bool VerifySignature(const BYTE hash[32], const BYTE* der, size_t derLen)
{
    BYTE raw[64], pub[64];
    if (!DerSigToRaw(der, derLen, raw) || !HexToBytes(kUpdatePubKeyHex, pub, 64)) return false;
    struct { BCRYPT_ECCKEY_BLOB h; BYTE xy[64]; } blob;
    blob.h.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.h.cbKey   = 32;
    memcpy(blob.xy, pub, 64);
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) return false;
    if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, (PUCHAR)&blob, sizeof(blob), 0) == 0) {
        ok = BCryptVerifySignature(key, nullptr, (PUCHAR)hash, 32, raw, 64, 0) == 0;
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}
// ---- CAPS-10: crypto end ----

// Робота потоку: перевірити, а за r->install — ще й завантажити та перевірити підпис.
void UpdateWork(UpdResult* r)
{
    std::vector<BYTE> body;
    if (!HttpGet(kUpdApiUrl, body, nullptr)) {
        r->err = Str::UpdErrNoNet;
        return;
    }
    body.push_back(0);
    const char* s = strstr((const char*)body.data(), "\"tag_name\":\"");
    if (!s) { r->err = Str::UpdErrApi; return; }
    s += 12;
    int k = 0;
    while (s[k] && s[k] != '"' && k < 30) { r->tag[k] = (wchar_t)s[k]; ++k; }
    r->tag[k] = 0;
    int v[3];
    if (!ParseVersion(r->tag, v)) { r->err = Str::UpdErrVersion; return; }
    if (!r->install) { r->ok = true; return; }

    wchar_t exe[MAX_PATH] = {}, nw[MAX_PATH + 8] = {}, url[256] = {};
    ExePath(exe);
    swprintf(nw, MAX_PATH + 8, L"%s.new", exe);
    swprintf(url, 256, L"%s%s/%s", kUpdDlBase, r->tag, kUpdAsset);
    std::vector<BYTE> sink;
    if (!HttpGet(url, sink, nw)) {
        r->err = Str::UpdErrDownload;
        DeleteFileW(nw);
        return;
    }
    swprintf(url, 256, L"%s%s/%s.sig", kUpdDlBase, r->tag, kUpdAsset);
    std::vector<BYTE> sig;
    if (!HttpGet(url, sig, nullptr) || sig.size() < 8) {
        r->err = Str::UpdErrSigDownload;
        DeleteFileW(nw);
        return;
    }
    BYTE hash[32] = {};
    if (!Sha256File(nw, hash) || !VerifySignature(hash, sig.data(), sig.size())) {
        r->err = Str::UpdErrSigMismatch;
        DeleteFileW(nw);
        return;
    }
    // Здоровий глузд: це Windows-exe розумного розміру
    HANDLE f = CreateFileW(nw, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    bool looksExe = false;
    if (f != INVALID_HANDLE_VALUE) {
        BYTE mz[2] = {}; DWORD n = 0;
        LARGE_INTEGER size = {};
        GetFileSizeEx(f, &size);
        looksExe = ReadFile(f, mz, 2, &n, nullptr) && n == 2 && mz[0] == 'M' && mz[1] == 'Z'
                && size.QuadPart > 100 * 1024 && size.QuadPart < (32ll << 20);
        CloseHandle(f);
    }
    if (!looksExe) { r->err = Str::UpdErrNotExe; DeleteFileW(nw); return; }
    r->ok = true;
}

DWORD WINAPI UpdateThread(LPVOID p)
{
    UpdResult* r = (UpdResult*)p;
    UpdateWork(r);
    PostMessageW(g_mainWnd, WMAPP_UPDATE, 0, (LPARAM)r);
    return 0;
}

void UpdateUpdStatus();   // UI, нижче

void StartUpdate(bool install, bool manual)
{
    if (InterlockedCompareExchange(&g_updBusy, 1, 0) != 0) return;
    UpdResult* r = new UpdResult;
    r->install = install;
    r->manual  = manual;
    g_updState = install ? UpdState::Downloading : UpdState::Checking;
    UpdateUpdStatus();
    HANDLE t = CreateThread(nullptr, 0, UpdateThread, r, 0, nullptr);
    if (!t) { delete r; g_updBusy = 0; g_updState = UpdState::Idle; UpdateUpdStatus(); return; }
    CloseHandle(t);
}

bool OldVersionExists()
{
    wchar_t exe[MAX_PATH] = {}, old[MAX_PATH + 8] = {};
    ExePath(exe);
    swprintf(old, MAX_PATH + 8, L"%s.old", exe);
    return GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES;
}

// Запустити exe (той самий шлях, уже нову/повернуту версію) і штатно вийти.
// Новий процес чекає нашого виходу (--after-update <pid>), бо м'ютекс одного екземпляра.
bool RelaunchAndExit()
{
    wchar_t exe[MAX_PATH] = {}, cmd[MAX_PATH + 64] = {};
    ExePath(exe);
    swprintf(cmd, MAX_PATH + 64, L"\"%s\" --after-update %lu", exe, (unsigned long)GetCurrentProcessId());
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DestroyWindow(g_mainWnd);   // штатний вихід: курсор відновиться, хуки знімуться
    return true;
}

void ApplyDownloadedUpdate()
{
    wchar_t exe[MAX_PATH] = {}, old[MAX_PATH + 8] = {}, nw[MAX_PATH + 8] = {};
    ExePath(exe);
    swprintf(old, MAX_PATH + 8, L"%s.old", exe);
    swprintf(nw,  MAX_PATH + 8, L"%s.new", exe);
    DeleteFileW(old);
    if (!MoveFileExW(exe, old, MOVEFILE_REPLACE_EXISTING)) {
        g_updState = UpdState::Error;
        g_updErr = Str::UpdErrReplace;
        DeleteFileW(nw);
        return;
    }
    if (!MoveFileExW(nw, exe, MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(old, exe, MOVEFILE_REPLACE_EXISTING);
        g_updState = UpdState::Error;
        g_updErr = Str::UpdErrWrite;
        return;
    }
    if (!RelaunchAndExit()) {
        MoveFileExW(exe, nw, MOVEFILE_REPLACE_EXISTING);
        MoveFileExW(old, exe, MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(nw);
        g_updState = UpdState::Error;
        g_updErr = Str::UpdErrLaunch;
    }
}

void RollbackUpdate()
{
    wchar_t exe[MAX_PATH] = {}, old[MAX_PATH + 8] = {}, tmp[MAX_PATH + 8] = {};
    ExePath(exe);
    swprintf(old, MAX_PATH + 8, L"%s.old", exe);
    swprintf(tmp, MAX_PATH + 8, L"%s.tmp", exe);
    if (GetFileAttributesW(old) == INVALID_FILE_ATTRIBUTES) return;
    if (!MoveFileExW(exe, tmp, MOVEFILE_REPLACE_EXISTING)) return;
    if (!MoveFileExW(old, exe, MOVEFILE_REPLACE_EXISTING)) { MoveFileExW(tmp, exe, MOVEFILE_REPLACE_EXISTING); return; }
    MoveFileExW(tmp, old, MOVEFILE_REPLACE_EXISTING);   // теперішня стає .old — можна повернутись
    if (!RelaunchAndExit()) {
        MoveFileExW(exe, tmp, MOVEFILE_REPLACE_EXISTING);
        MoveFileExW(old, exe, MOVEFILE_REPLACE_EXISTING);
        MoveFileExW(tmp, old, MOVEFILE_REPLACE_EXISTING);
    }
}

// ---------- CAPS-17: іконка трею, яка переживає і гонку при вході, і рестарт Explorer ----------
//
// ДВІ незалежні причини, чому іконка зникала назавжди, і потрібні обидва лікування.
//
// 1. Explorer працює зі ЗВИЧАЙНИМИ правами, а ми — з адмінськими (requireAdministrator,
//    див. шапку файлу). UIPI за замовчуванням не пускає повідомлення знизу вгору, тож
//    широкомовне TaskbarCreated до нас НЕ ДОХОДИТЬ — обробник нижче був мертвим кодом.
//    Виміряно 20.09.2026: PostMessage(TaskbarCreated) до нашого вікна з medium IL віддає
//    ERROR_ACCESS_DENIED, а до неелевейтованого вікна Провідника проходить.
//    Лікує ChangeWindowMessageFilterEx — дозвіл саме на це одне повідомлення.
//
// 2. Автозапуск — задача на вхід у систему, і Explorer стартує тієї ж секунди (виміряно:
//    обидва 19:04:03). Якщо ми покликали NIM_ADD до появи панелі задач, виклик просто
//    не вдається. Раніше його результат ніхто не перевіряв. Фільтр з пункту 1 тут не
//    рятує: якщо Explorer розіслав TaskbarCreated ще до створення нашого вікна, ловити
//    вже нічого — тому додавання повторюється за таймером, поки не вдасться.
int g_trayTries = 0;
constexpr int kTrayMaxTries = 150;   // 5 хв по 2 с: із запасом на найповільніший вхід

void TrayEnsure(HWND hwnd)
{
    // NIM_ADD не вдається ще й тоді, коли іконка ВЖЕ стоїть (TaskbarCreated могло
    // прийти, а наша іконка вціліти). Відрізняємо це через NIM_MODIFY: якщо він
    // проходить — іконка на місці, і повторювати нема чого.
    if (Shell_NotifyIconW(NIM_ADD, &g_nid) || Shell_NotifyIconW(NIM_MODIFY, &g_nid)) {
        KillTimer(hwnd, TIMER_TRAY);
        g_trayTries = 0;
        return;
    }
    if (++g_trayTries == 1)
        SetTimer(hwnd, TIMER_TRAY, 2000, nullptr);
    else if (g_trayTries >= kTrayMaxTries)
        KillTimer(hwnd, TIMER_TRAY);   // панелі немає аж 5 хв — це вже не гонка
}

void TrayBalloon(const wchar_t* title, const wchar_t* text)
{
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    lstrcpynW(n.szInfoTitle, title, 64);
    lstrcpynW(n.szInfo, text, 256);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// --after-update <pid>: зачекати, поки попередній екземпляр вийде (м'ютекс).
void WaitForPreviousInstance()
{
    const wchar_t* p = wcsstr(GetCommandLineW(), L"--after-update ");
    if (!p) return;
    const DWORD pid = (DWORD)wcstoul(p + 15, nullptr, 10);
    if (!pid) return;
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        WaitForSingleObject(h, 15000);
        CloseHandle(h);
    }
}

// ---------- CAPS-2: вкладки ----------

// Полотно сторінки таб-контрол сам НЕ малює: він малює заголовки й рамку, а
// всередині просвічує фон батьківського вікна (колір діалогу). Контроли сторінок
// при цьому отримують COLOR_WINDOW (див. WM_CTLCOLORSTATIC) — власник побачив
// білі плашки на сірому (CAPS-7). Тому полотно малюємо самі: смуга із
// заголовками — колір діалогу, область сторінки — колір вікна, як у системних
// property sheet. Так вигляд не залежить від того, що і як малює тема.
// ---- CAPS-8: темний режим — власне малювання ----

bool ThemeIsDark();        // CAPS-7, нижче
void PeekApplyTheme();     // CAPS-16, нижче
bool IsPageControl(HWND c); // нижче, у розділі вкладок

bool ComputeDark()
{
    switch (g_winTheme) {
    case WinTheme::Light: return false;
    case WinTheme::Dark:  return true;
    default:              return ThemeIsDark();   // як застосунки Windows
    }
}

bool IsCheckOrRadio(HWND h)
{
    const LONG t = GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK;
    return t == BS_AUTOCHECKBOX || t == BS_CHECKBOX || t == BS_AUTORADIOBUTTON || t == BS_RADIOBUTTON;
}

// Чекбокс/радіо в темному режимі: тема малює гліф (з «DarkMode_Explorer» — темний),
// текст малюємо самі, бо теми-кнопки ігнорують колір із WM_CTLCOLORSTATIC.
void DrawCheckDark(HWND h, HDC dc, RECT rc)
{
    FillRect(dc, &rc, IsPageControl(h) ? g_brDkPage : g_brDkBg);
    const LONG st = GetWindowLongW(h, GWL_STYLE);
    const LONG type = st & BS_TYPEMASK;
    const bool radio = (type == BS_AUTORADIOBUTTON || type == BS_RADIOBUTTON);
    const bool checked = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool enabled = IsWindowEnabled(h) != FALSE;
    const int part  = radio ? BP_RADIOBUTTON : BP_CHECKBOX;
    const int state = checked ? (enabled ? CBS_CHECKEDNORMAL : CBS_CHECKEDDISABLED)
                              : (enabled ? CBS_UNCHECKEDNORMAL : CBS_UNCHECKEDDISABLED);
    const UINT dpi = GetDpiForSystem();
    SIZE sz = { MulDiv(13, dpi, 96), MulDiv(13, dpi, 96) };
    HTHEME th = OpenThemeData(h, L"Button");
    if (th) GetThemePartSize(th, dc, part, state, nullptr, TS_TRUE, &sz);
    RECT box = { rc.left, (rc.top + rc.bottom - sz.cy) / 2, rc.left + sz.cx, (rc.top + rc.bottom + sz.cy) / 2 };
    if (th) { DrawThemeBackground(th, dc, part, state, &box, nullptr); CloseThemeData(th); }
    else    { FrameRect(dc, &box, g_brDkThumb); }

    wchar_t text[256] = {};
    GetWindowTextW(h, text, 255);
    HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? kDkText : kDkGray);
    RECT tr = rc;
    tr.left = box.right + MulDiv(5, dpi, 96);
    if (st & BS_MULTILINE) {
        RECT calc = tr;
        DrawTextW(dc, text, -1, &calc, DT_WORDBREAK | DT_CALCRECT);
        const int hgt = calc.bottom - calc.top;
        tr.top = (rc.top + rc.bottom - hgt) / 2;
        DrawTextW(dc, text, -1, &tr, DT_WORDBREAK);
    } else {
        DrawTextW(dc, text, -1, &tr, DT_SINGLELINE | DT_VCENTER);
    }
    SelectObject(dc, old);
}

// Таб-контрол у темному режимі малюємо повністю: тема вміє лише світлий.
void PaintTabDark(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    FillRect(dc, &rc, g_brDkBg);
    RECT page = rc;
    SendMessageW(h, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    RECT frame = page;
    InflateRect(&frame, 2, 2);
    FillRect(dc, &frame, g_brDkPage);
    FrameRect(dc, &frame, g_brDkBorder);

    const int n   = (int)SendMessageW(h, TCM_GETITEMCOUNT, 0, 0);
    const int sel = (int)SendMessageW(h, TCM_GETCURSEL, 0, 0);
    HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < n; ++i) {
        RECT ir;
        SendMessageW(h, TCM_GETITEMRECT, i, (LPARAM)&ir);
        wchar_t text[64] = {};
        TCITEMW it = {};
        it.mask = TCIF_TEXT; it.pszText = text; it.cchTextMax = 63;
        SendMessageW(h, TCM_GETITEMW, i, (LPARAM)&it);
        if (i == sel) {
            RECT fill = ir;
            fill.bottom = frame.top + 1;             // зливається зі сторінкою
            FillRect(dc, &fill, g_brDkPage);
            RECT line = ir;
            line.bottom = line.top + 2;
            FillRect(dc, &line, g_brDkAccent);
        }
        SetTextColor(dc, i == sel ? kDkText : kDkGray);
        DrawTextW(dc, text, -1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, old);
    EndPaint(h, &ps);
}

// Пікер часу в темному режимі: у нього немає кольорів — малюємо клієнтську
// область самі (фон, рамка, поточний текст), стрілки up-down — тема DarkMode_Explorer.
LRESULT CALLBACK DtpSubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (g_dark && msg == WM_ERASEBKGND) return 1;
    if (g_dark && msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, g_brDkEdit);
        FrameRect(dc, &rc, g_brDkBorder);
        wchar_t text[64] = {};
        GetWindowTextW(h, text, 63);
        const UINT dpi = GetDpiForSystem();
        HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, IsWindowEnabled(h) ? kDkText : kDkGray);
        RECT tr = rc;
        tr.left  += MulDiv(6, dpi, 96);
        tr.right -= MulDiv(22, dpi, 96);   // місце під стрілки
        DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

BOOL CALLBACK ThemeChildProc(HWND h, LPARAM)
{
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, 32);
    if (!lstrcmpiW(cls, L"Button") || !lstrcmpiW(cls, L"msctls_updown32") || !lstrcmpiW(cls, L"ScrollBar"))
        SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    else if (!lstrcmpiW(cls, L"Edit"))
        SetWindowTheme(h, g_dark ? L"DarkMode_CFD" : nullptr, nullptr);
    // Повзунок (та інші контроли comctl32) тримає власний кеш зображення і на
    // WM_PAINT лише бліттить його — RedrawWindow нічого не міняє (власник: після
    // перемикання теми трекбари лишались у старих кольорах). Перебудувати кеш
    // змушує WM_THEMECHANGED; далі — явне перемальовування з очищенням.
    SendMessageW(h, WM_THEMECHANGED, 0, 0);
    RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
    return TRUE;
}

void ApplyWindowTheme(bool force)
{
    const bool dark = ComputeDark();
    if (!force && dark == g_dark) return;
    g_dark = dark;
    if (!g_brDkBg) {
        g_brDkBg     = CreateSolidBrush(kDkBg);
        g_brDkPage   = CreateSolidBrush(kDkPage);
        g_brDkEdit   = CreateSolidBrush(kDkEdit);
        g_brDkBorder = CreateSolidBrush(kDkBorder);
        g_brDkThumb  = CreateSolidBrush(kDkThumb);
        g_brDkAccent = CreateSolidBrush(kDkAccent);
    }
    const BOOL b = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(g_mainWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &b, sizeof(b));
    EnumChildWindows(g_mainWnd, ThemeChildProc, 0);
    RedrawWindow(g_mainWnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
    PeekApplyTheme();   // CAPS-16
}

LRESULT CALLBACK TabSubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (msg == WM_ERASEBKGND) {
        HDC dc = (HDC)wp;
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, g_dark ? g_brDkBg : GetSysColorBrush(COLOR_BTNFACE));
        RECT page = rc;
        SendMessageW(h, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);   // область сторінки без рамки
        FillRect(dc, &page, g_dark ? g_brDkPage : GetSysColorBrush(COLOR_WINDOW));
        return 1;
    }
    if (msg == WM_PAINT && g_dark) {
        PaintTabDark(h);
        return 0;
    }
    if (msg == WM_PAINT) {
        // Тема може зафарбувати панель по-своєму ПІСЛЯ erase — тому полотно
        // домальовуємо після стандартного малювання. DCX_CLIPSIBLINGS: контроли
        // сторінок — сусіди таба вище за z-order, їх не зачіпаємо.
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        RECT page;
        GetClientRect(h, &page);
        SendMessageW(h, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
        if (HDC dc = GetDCEx(h, nullptr, DCX_CACHE | DCX_CLIPSIBLINGS)) {
            FillRect(dc, &page, GetSysColorBrush(COLOR_WINDOW));
            ReleaseDC(h, dc);
        }
        return r;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// CAPS-12 (2.1.1): контрол додається на сторінку ЛИШЕ через це — ємність береться
// з самого масиву, тож дописати контрол і забути збільшити масив більше не можна.
// У 2.1.0 сторінка «Налаштування» переросла свої 16 елементів: три останні контроли
// писались за межі масиву, і «Оновити» не ховалась на інших вкладках. Переповнення
// тепер не мовчить — прапорець перевіряється одразу після побудови вікна.
bool g_pageOverflow = false;

template <size_t N>
HWND AddTo(HWND (&items)[N], int& n, HWND c)
{
    if (n < (int)N) items[n++] = c;
    else            g_pageOverflow = true;
    return c;
}

void ShowGroup(HWND* items, int n, bool show)
{
    for (int i = 0; i < n; ++i)
        ShowWindow(items[i], show ? SW_SHOW : SW_HIDE);
}

// Контроли сторінок — діти головного вікна, тож за замовчуванням вони малюють
// підкладку кольором діалогу й на білому полотні вкладки виглядають як сірі
// плашки. Тому таким контролам віддаємо колір вікна, решті — колір діалогу.
bool IsPageControl(HWND c)
{
    for (int i = 0; i < g_pageLayoutN; ++i) if (g_pageLayout[i] == c) return true;
    for (int i = 0; i < g_pageCursorN; ++i) if (g_pageCursor[i] == c) return true;
    for (int i = 0; i < g_advN; ++i)        if (g_advCtrls[i]   == c) return true;
    for (int i = 0; i < g_pageThemeN; ++i)  if (g_pageTheme[i]  == c) return true;
    for (int i = 0; i < g_thAdvN; ++i)      if (g_thAdv[i]      == c) return true;
    for (int i = 0; i < g_pageSettingsN; ++i) if (g_pageSettings[i] == c) return true;
    for (int i = 0; i < g_pagePeekN; ++i)     if (g_pagePeek[i]     == c) return true;
    return false;
}

void SetCursorValueLabels()
{
    wchar_t buf[64];
    wsprintfW(buf, L"%d×", g_cur.scale);
    SetWindowTextW(g_curScaleVal, buf);
    swprintf(buf, 64, S(Str::FmtSeconds), g_cur.holdMs / 1000, (g_cur.holdMs % 1000) / 100);
    SetWindowTextW(g_curHoldVal, buf);
}

void SelectTab(int index)
{
    ShowGroup(g_pageLayout, g_pageLayoutN, index == 0);
    ShowGroup(g_pageCursor, g_pageCursorN, index == 1);
    ShowGroup(g_advCtrls, g_advN, index == 1 && g_advVisible);
    ShowGroup(g_pageTheme, g_pageThemeN, index == 2);
    ShowGroup(g_thAdv, g_thAdvN, index == 2 && g_thAdvVisible);
    ShowGroup(g_pagePeek, g_pagePeekN, index == 3);          // CAPS-16
    ShowGroup(g_pageSettings, g_pageSettingsN, index == 4);
}

// CAPS-12: обидві кнопки «Детально» несуть ще й стрілку стану, тож їхній підпис
// збирається окремо — і при перемиканні секції, і при зміні мови.
void UpdateAdvButtons()
{
    SetWindowTextW(g_curAdvBtn, S(g_advVisible   ? Str::DetailsUp : Str::Details));
    SetWindowTextW(g_thAdvBtn,  S(g_thAdvVisible ? Str::DetailsUp : Str::Details));
}

void ToggleAdvanced()
{
    g_advVisible = !g_advVisible;
    UpdateAdvButtons();
    ShowGroup(g_advCtrls, g_advN, g_advVisible);
}

// ---------- CAPS-7: UI вкладки «День/ніч» ----------

void ToggleThemeAdvanced()
{
    g_thAdvVisible = !g_thAdvVisible;
    UpdateAdvButtons();
    ShowGroup(g_thAdv, g_thAdvN, g_thAdvVisible);
}

void SetPickerMinutes(HWND p, int minutes)
{
    SendMessageW(p, DTM_SETFORMATW, 0, (LPARAM)L"HH:mm");
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    st.wHour = (WORD)(minutes / 60); st.wMinute = (WORD)(minutes % 60);
    st.wSecond = 0; st.wMilliseconds = 0;
    SendMessageW(p, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st);
}

int GetPickerMinutes(HWND p, int fallback)
{
    SYSTEMTIME st = {};
    if (SendMessageW(p, DTM_GETSYSTEMTIME, 0, (LPARAM)&st) != GDT_VALID) return fallback;
    return st.wHour * 60 + st.wMinute;
}

const wchar_t* LocSourceName(LocSource s)
{
    switch (s) {
    case LocSource::Windows:  return S(Str::LocSrcWindows);
    case LocSource::Ip:       return S(Str::LocSrcIp);
    case LocSource::Manual:   return S(Str::LocSrcManual);
    case LocSource::TimeZone: return S(Str::LocSrcTz);
    default:                  return S(Str::LocSrcAuto);
    }
}

void UpdateThemeStatus()
{
    wchar_t line[256] = {}, c1[8] = {}, c2[8] = {};
    const __time64_t now = NowUnix();
    if (g_th.bySchedule) {
        swprintf(line, 256, S(Str::ThFmtSchedule),
                 g_th.darkFrom / 60, g_th.darkFrom % 60, g_th.lightFrom / 60, g_th.lightFrom % 60);
    } else if (g_fix.ok) {
        __time64_t r = 0, s = 0;
        const int k = SunEventsFor(now, g_fix.lat, g_fix.lon, r, s);
        wchar_t where[64];
        swprintf(where, 64, L"%.2f°%s %.2f°%s", fabs(g_fix.lat), g_fix.lat >= 0 ? L"N" : L"S",
                 fabs(g_fix.lon), g_fix.lon >= 0 ? L"E" : L"W");
        if (k == 0) {
            FormatClock(c1, 8, r); FormatClock(c2, 8, s);
            swprintf(line, 256, S(Str::ThFmtSun), c1, c2, where, LocSourceName(g_fix.src));
        } else {
            swprintf(line, 256, L"%s · %s · %s", S(k == 2 ? Str::ThPolarDay : Str::ThPolarNight),
                     where, LocSourceName(g_fix.src));
        }
    } else if (g_locBusy) {
        lstrcpyW(line, S(Str::ThLocating));
    } else if (g_th.src == LocSource::Manual) {
        lstrcpyW(line, S(Str::ThEnterCoords));
    } else {
        lstrcpyW(line, S(Str::ThNoLoc));
    }
    SetWindowTextW(g_thStatus, line);

    const bool dark = ThemeIsDark();
    if (!g_th.enabled) {
        swprintf(line, 256, S(Str::ThNowOff), S(dark ? Str::ThDark : Str::ThLight));
        SetWindowTextW(g_thNow, line);
        return;
    }
    __time64_t next = 0; bool fb = false;
    ThemeWantDark(now, next, fb);
    wchar_t nb[8] = L"—";
    if (g_thOvUntil && now < g_thOvUntil) {
        FormatClock(nb, 8, g_thOvUntil);
        swprintf(line, 256, S(Str::ThNowManual), S(dark ? Str::ThDark : Str::ThLight), nb);
    } else if (g_thPending) {
        swprintf(line, 256, S(Str::ThNowPending), S(dark ? Str::ThLightAcc : Str::ThDarkAcc));
    } else {
        if (next) FormatClock(nb, 8, next);
        swprintf(line, 256, S(Str::ThNowNext), S(dark ? Str::ThDark : Str::ThLight), nb);
    }
    SetWindowTextW(g_thNow, line);
}

void EnableThemeControls()
{
    EnableWindow(g_thDarkFrom,  g_th.bySchedule);
    EnableWindow(g_thLightFrom, g_th.bySchedule);
    const bool manual = g_th.src == LocSource::Manual;
    EnableWindow(g_thLat, manual);
    EnableWindow(g_thLon, manual);
}

// Ручні координати приймаються, коли обидва поля валідні (широта ±90, довгота ±180).
void CommitManualCoords()
{
    wchar_t a[32] = {}, b[32] = {};
    GetWindowTextW(g_thLat, a, 31);
    GetWindowTextW(g_thLon, b, 31);
    double la = 0, lo = 0;
    if (ParseCoord(a, -90, 90, la) && ParseCoord(b, -180, 180, lo)) {
        g_th.lat = la; g_th.lon = lo; g_th.hasManual = true;
        swprintf(a, 32, L"%.4f", la); RegSaveStr(kRegThemeLat, a); SetWindowTextW(g_thLat, a);
        swprintf(b, 32, L"%.4f", lo); RegSaveStr(kRegThemeLon, b); SetWindowTextW(g_thLon, b);
        if (g_th.src == LocSource::Manual) { UseManualFix(); ThemeTick(); }
    }
    UpdateThemeStatus();
}

// ---------- CAPS-10: UI оновлень ----------

void FormatDateTime(wchar_t* buf, size_t n, __time64_t t)
{
    struct tm lt = {};
    _localtime64_s(&lt, &t);
    swprintf(buf, n, L"%02d.%02d %02d:%02d", lt.tm_mday, lt.tm_mon + 1, lt.tm_hour, lt.tm_min);
}

void UpdateUpdStatus()
{
    wchar_t cur[32] = {}, when[32] = {}, line[256] = {};
    lstrcpynW(when, S(Str::UpdNever), 32);
    ExeVersionString(cur, 32);
    if (g_updLast) FormatDateTime(when, 32, g_updLast);
    const wchar_t* avail = (g_updTag[0] == L'v') ? g_updTag + 1 : g_updTag;
    switch (g_updState) {
    case UpdState::Checking:    lstrcpyW(line, S(Str::UpdChecking)); break;
    case UpdState::UpToDate:    swprintf(line, 256, S(Str::UpdFmtUpToDate), cur, when); break;
    case UpdState::Available:   swprintf(line, 256, S(Str::UpdFmtAvailable), avail, cur); break;
    case UpdState::Downloading: swprintf(line, 256, S(Str::UpdFmtDownloading), avail); break;
    case UpdState::Verified:    lstrcpyW(line, S(Str::UpdVerified)); break;
    case UpdState::Error:       lstrcpynW(line, S(g_updErr), 256); break;
    default:                    swprintf(line, 256, S(Str::UpdFmtIdle), cur, when); break;
    }
    SetWindowTextW(g_updStatus, line);
    const bool busy = g_updBusy != 0;
    EnableWindow(g_updCheckBtn,    !busy);
    EnableWindow(g_updInstallBtn,  !busy && g_updState == UpdState::Available);
    EnableWindow(g_updRollbackBtn, !busy && OldVersionExists());
}

void ThemeApplySettings()
{
    SaveThemeSettings();
    EnableThemeControls();
    if (g_th.enabled) {
        SetTimer(g_mainWnd, TIMER_THEME, 60 * 1000, nullptr);
        if (!g_th.bySchedule &&
            (!g_fix.ok || (g_th.src != LocSource::Auto && g_fix.src != g_th.src)))
            StartLocate();
        ThemeTick();
    } else {
        KillTimer(g_mainWnd, TIMER_THEME);
        g_thPending = false;
        UpdateThemeStatus();
    }
}

// Прочитати число з поля «Детально», притиснути до допустимого діапазону і
// повернути в поле — щоб користувач бачив, що саме прийнято.
int ReadEditInt(HWND edit, int lo, int hi, int fallback)
{
    wchar_t buf[16] = {};
    GetWindowTextW(edit, buf, 15);
    int v = _wtoi(buf);
    if (v == 0 && buf[0] != L'0') v = fallback;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    wsprintfW(buf, L"%d", v);
    SetWindowTextW(edit, buf);
    return v;
}

void CommitAdvanced()
{
    g_cur.windowMs  = ReadEditInt(g_edWindow, 300, 2000, g_cur.windowMs);
    g_cur.distance  = ReadEditInt(g_edDist,   300, 5000, g_cur.distance);
    g_cur.factor    = ReadEditInt(g_edFactor, 150, 1000, g_cur.factor);
    g_cur.reversals = ReadEditInt(g_edRevers, 2,   10,   g_cur.reversals);
    g_cur.shrinkMs  = ReadEditInt(g_edShrink, 100, 1500, g_cur.shrinkMs);
    RegSaveInt(kRegShakeWindow,    g_cur.windowMs);
    RegSaveInt(kRegShakeDistance,  g_cur.distance);
    RegSaveInt(kRegShakeFactor,    g_cur.factor);
    RegSaveInt(kRegShakeReversals, g_cur.reversals);
    RegSaveInt(kRegCursorShrink,   g_cur.shrinkMs);
}

// ---------- CAPS-16: швидкий перегляд файлу по пробілу ----------
//
// Пробіл на виділеному файлі в Провіднику чи на робочому столі відкриває вікно
// перегляду (як Quick Look у macOS чи Peek у PowerToys); ще раз пробіл або Esc
// закриває. Вікно НЕ забирає фокус (WS_EX_NOACTIVATE): Провідник лишається
// активним, стрілки гортають файли як завжди, а перегляд стежить за виділенням
// і підхоплює новий файл. Це головна відмінність від Peek, який робить власне
// гортання; тут виділення Провідника і те, що на екрані, — одне й те саме.
//
// Хук ковтає пробіл ЛИШЕ коли фокус у самому списку файлів (DirectUIHWND або
// SysListView32 усередині SHELLDLL_DefView у вікні CabinetWClass / Progman /
// WorkerW). У полі перейменування, пошуку чи адресному рядку пробіл іде як є.
// Якщо виділення порожнє, пробіл повертається Провіднику (SendInput з міткою,
// яку хук пропускає) — його рідна поведінка не губиться. Ctrl/Shift/Alt/Win +
// пробіл не чіпаємо; літера, натиснута менш ніж секунду тому, — це пошук
// набором у Провіднику, і пробіл тоді теж його.
//
// Рендерери власні: зображення через GDI+ (уже в збірці заради логотипа), текст
// у полі EDIT, для решти — картка з відомостями. Системні обробники прев'ю
// (IPreviewHandler) сюди свідомо НЕ вантажаться: процес елевейтований
// (requireAdministrator), і чужий COM-код у ньому — дірка. Значки й назви типів
// беруться з SHGFI_USEFILEATTRIBUTES, тобто з реєстру за розширенням, без
// виклику обробників конкретного файлу — з тієї ж причини. Це етап 2 через
// окремий непривілейований процес (decisions 2026-09-20).

// GUID-и оболонки — свої копії з тієї ж причини, що й у Location API вище:
// у MSVC вони в uuid.lib, у MinGW — у libuuid, і набір не завжди повний.
const GUID kCLSID_ShellWindows   = { 0x9ba05972, 0xf6a8, 0x11cf, { 0xa4, 0x42, 0x00, 0xa0, 0xc9, 0x0a, 0x8f, 0x39 } };
const GUID kIID_IShellWindows    = { 0x85cb6900, 0x4d95, 0x11cf, { 0x96, 0x0c, 0x00, 0x80, 0xc7, 0xf4, 0xee, 0x85 } };
const GUID kIID_IWebBrowserApp   = { 0x0002df05, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
const GUID kIID_IServiceProvider = { 0x6d5140c1, 0x7436, 0x11ce, { 0x80, 0x34, 0x00, 0xaa, 0x00, 0x60, 0x09, 0xfa } };
const GUID kSID_STopLevelBrowser = { 0x4c96be40, 0x915c, 0x11cf, { 0x99, 0xd3, 0x00, 0xaa, 0x00, 0x4a, 0xe8, 0x37 } };
const GUID kIID_IShellBrowser    = { 0x000214e2, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
const GUID kIID_IDataObject      = { 0x0000010e, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
const GUID kCLSID_ShellLink      = { 0x00021401, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
const GUID kIID_IShellLinkW      = { 0x000214f9, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
const GUID kIID_IPersistFile     = { 0x0000010b, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
// GDI+ оголошує ці GUID-и через DEFINE_GUID без INITGUID — власна копія надійніша за lib
const GUID kFrameDimensionTime   = { 0x6aedbd6d, 0x3fb5, 0x418a, { 0x83, 0xa6, 0x7f, 0x45, 0x22, 0x9d, 0xc8, 0x72 } };
const GUID kIID_IImageList       = { 0x46eb5926, 0x582e, 0x4017, { 0x9f, 0xdf, 0xe8, 0x99, 0x8d, 0xaa, 0x09, 0x50 } };

constexpr int    kPeekHead    = 48;               // смуга з назвою файлу, лог. px
constexpr int    kPeekMinW    = 400, kPeekMinH = 260;
constexpr size_t kPeekTextMax = 1024 * 1024;      // текст показуємо до 1 МБ
constexpr UINT   TIMER_PEEK_FOLLOW = 1;           // на вікні перегляду: стежити за виділенням
constexpr UINT   TIMER_PEEK_ANIM   = 2;           // наступний кадр анімованого GIF
constexpr size_t kPeekImageMax = 64u * 1024 * 1024;   // більший файл не тягнемо в пам'ять

HFONT CreateUIFont(int percent, int weight);      // визначення нижче, після WndProc

enum class PeekKind { None, Image, Text, Card };

struct PeekInfo {
    wchar_t name[MAX_PATH];
    wchar_t folder[MAX_PATH];
    wchar_t type[128];
    wchar_t size[64];
    wchar_t modified[64];
    wchar_t created[64];
    wchar_t subtitle[320];
    wchar_t target[1024];   // куди веде ярлик (.lnk/.url)
    wchar_t author[160];    // STEP: із шапки файлу
    wchar_t org[160];
    wchar_t schema[200];
    wchar_t created2[64];   // дата з шапки STEP, а не з файлової системи
    unsigned entities;
    int     docW, docH;     // ⚠ SVG: розмір ДОКУМЕНТА для підпису. imgW/imgH — це
                            // розмір БІТМАПА, і саме ним малює PeekPaint.
    bool    isDir;
    int     items;       // для папки: скільки всередині (-1 = не рахували)
    bool    itemsMore;   // лічильник упёрся в стелю
    int     imgW, imgH;
};

HWND     g_peekWnd  = nullptr;
HWND     g_peekEdit = nullptr;
HWND     g_peekView = nullptr;      // SHELLDLL_DefView, за яким стежимо
IShellView* g_peekSv = nullptr;     // його ж вигляд (проксі в explorer.exe) — щоб не шукати щотика
wchar_t  g_peekPath[MAX_PATH] = {};
PeekKind g_peekKind = PeekKind::None;
PeekInfo g_peekInfo = {};
Gdiplus::Bitmap* g_peekImg    = nullptr;   // оригінал (уже повернутий за EXIF)
Gdiplus::Bitmap* g_peekScaled = nullptr;   // під поточний розмір вікна
// Зображення декодується з КОПІЇ файлу в пам'яті, а не з файлу: так перегляд не
// тримає файл відкритим (його можна перейменувати чи видалити) і, головне,
// лишаються всі кадри — Clone() схлопнув би анімований GIF в один.
IStream* g_peekImgStream = nullptr;
int   g_peekFrames = 1;        // кадрів у зображенні (1 = не анімоване)
int   g_peekFrame  = 0;        // який кадр показано
std::vector<UINT> g_peekDelays;   // затримка кадру, мс
HICON    g_peekIconBig = nullptr, g_peekIconSmall = nullptr;
HFONT    g_peekFont = nullptr, g_peekFontBold = nullptr, g_peekFontMono = nullptr;
bool     g_peekCloseHot = false;
int      g_peekPagerHot = 0;   // 0 нічого, 1 «назад», 2 «вперед»
bool     g_peekTracking = false;
bool     g_peekDark     = false;
// Масштаб — МНОЖНИК до «вписаного» розміру, тож 1.0 завжди означає «вміщено у вікно»
// незалежно від розміру картинки й вікна. Зсув у пікселях екрана.
float    g_peekZoom = 1.0f;
int      g_peekPanX = 0, g_peekPanY = 0;
bool     g_peekPanning = false;
POINT    g_peekPanFrom = {};
bool     g_peekJsonFormatted = false;   // показуємо не байт-у-байт, і про це варто сказати
bool     g_peekSvgAsCode     = false;   // SVG не намалювали — скажемо чому, а не промовчимо
Str      g_peekSvgNote       = Str::Empty;  // намалювали, але не все — теж скажемо
HWND     g_peekEnableCb = nullptr;

int PeekPx(int v) { return MulDiv(v, (int)GetDpiForSystem(), 96); }

// Наш власний пробіл, повернутий Провіднику: хук упізнає його за міткою.
void ReinjectSpace()
{
    INPUT in[2] = {};
    for (INPUT& i : in) {
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = VK_SPACE;
        i.ki.wScan = (WORD)MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC);
        i.ki.dwExtraInfo = kInjectMark;
    }
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

// ---- виділення Провідника через IShellWindows ----
//
// Усе це — проксі до об'єктів у explorer.exe, тож працюють лише інтерфейси з
// міжпроцесним маршалінгом: IShellBrowser, IShellView, IDataObject. IFolderView2
// (з його зручним GetSelection) проксі-стаба НЕ має — QueryInterface через
// проксі мовчки повертає E_NOINTERFACE (перевірено 20.09.2026). Тому виділення
// читаємо як CF_HDROP із IDataObject вигляду — так само роблять drag-and-drop
// і буфер обміну, і це працює для будь-якого файлового елемента.

// Перший виділений елемент вигляду як шлях у файловій системі. Не-файлові
// елементи («Цей ПК», бібліотеки) CF_HDROP не мають — тоді показувати нічого.
bool PeekReadSelection(IShellView* sv, wchar_t* out, size_t cch)
{
    out[0] = 0;
    IDataObject* dobj = nullptr;
    if (FAILED(sv->GetItemObject(SVGIO_SELECTION, kIID_IDataObject, (void**)&dobj)) || !dobj) return false;
    FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM sm = {};
    if (SUCCEEDED(dobj->GetData(&fe, &sm)) && sm.hGlobal) {
        if (HDROP drop = (HDROP)GlobalLock(sm.hGlobal)) {
            DragQueryFileW(drop, 0, out, (UINT)cch);
            GlobalUnlock(sm.hGlobal);
        }
        ReleaseStgMedium(&sm);
    }
    dobj->Release();
    return out[0] != 0;
}

// Вигляд, чиє вікно — саме цей SHELLDLL_DefView. У Windows 11 вкладки одного
// вікна Провідника — окремі записи IShellWindows з тим самим HWND, тож збіг
// верхнього вікна недостатній: звіряємо вікно активного вигляду. Повертає
// вигляд з утриманим посиланням.
IShellView* PeekFindShellView(HWND view)
{
    IShellWindows* sw = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                kIID_IShellWindows, (void**)&sw)) || !sw)
        return nullptr;

    const HWND root = GetAncestor(view, GA_ROOT);
    wchar_t cls[32] = {};
    GetClassNameW(root, cls, 32);
    const bool desktop = lstrcmpW(cls, L"CabinetWClass") != 0;

    IShellView* found = nullptr;
    auto probe = [&](IDispatch* disp) {
        IServiceProvider* sp = nullptr;
        if (FAILED(disp->QueryInterface(kIID_IServiceProvider, (void**)&sp)) || !sp) return;
        IShellBrowser* sb = nullptr;
        if (SUCCEEDED(sp->QueryService(kSID_STopLevelBrowser, kIID_IShellBrowser, (void**)&sb)) && sb) {
            IShellView* sv = nullptr;
            if (SUCCEEDED(sb->QueryActiveShellView(&sv)) && sv) {
                HWND svWnd = nullptr;
                sv->GetWindow(&svWnd);
                if (svWnd == view) found = sv;   // посилання переходить до того, хто шукав
                else               sv->Release();
            }
            sb->Release();
        }
        sp->Release();
    };

    if (desktop) {
        VARIANT loc, empty;   // для SWC_DESKTOP обидва аргументи ігноруються
        VariantInit(&loc);
        VariantInit(&empty);
        long hw = 0;
        IDispatch* disp = nullptr;
        if (SUCCEEDED(sw->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hw, SWFO_NEEDDISPATCH, &disp)) && disp) {
            probe(disp);
            disp->Release();
        }
    } else {
        long n = 0;
        sw->get_Count(&n);
        for (long i = 0; i < n && !found; ++i) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_I4;
            v.lVal = i;
            IDispatch* disp = nullptr;
            if (FAILED(sw->Item(v, &disp)) || !disp) continue;
            IWebBrowserApp* wb = nullptr;
            if (SUCCEEDED(disp->QueryInterface(kIID_IWebBrowserApp, (void**)&wb)) && wb) {
                SHANDLE_PTR h = 0;
                wb->get_HWND(&h);
                if ((HWND)h == root) probe(disp);
                wb->Release();
            }
            disp->Release();
        }
    }
    sw->Release();
    return found;
}

// ---- відомості про файл ----

void FormatFileTime(const FILETIME& ft, wchar_t* buf, int n)
{
    buf[0] = 0;
    SYSTEMTIME utc = {}, local = {};
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return;
    wchar_t d[48] = {}, t[32] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, d, 48, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, t, 32);
    swprintf(buf, n, L"%s %s", d, t);
}

bool ExtIn(const wchar_t* ext, const wchar_t* const* list, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (lstrcmpiW(ext, list[i]) == 0) return true;
    return false;
}

bool IsImageExt(const wchar_t* ext)
{
    static const wchar_t* const k[] = { L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".gif",
                                        L".bmp", L".dib", L".tif", L".tiff", L".ico", L".emf", L".wmf",
                                        // через WIC, якщо в системі є декодер:
                                        L".webp", L".heic", L".heif", L".avif", L".jxr", L".jpe" };
    return ExtIn(ext, k, sizeof(k) / sizeof(*k));
}

bool IsTextExt(const wchar_t* ext)
{
    static const wchar_t* const k[] = {
        L".txt", L".md", L".markdown", L".log", L".ini", L".cfg", L".conf", L".json", L".xml",
        L".yaml", L".yml", L".csv", L".tsv", L".nfo", L".srt", L".vtt", L".diff", L".patch",
        L".py", L".js", L".ts", L".jsx", L".tsx", L".c", L".cc", L".cpp", L".h", L".hpp", L".cs",
        L".java", L".kt", L".go", L".rs", L".rb", L".php", L".lua", L".ps1", L".psm1", L".bat",
        L".cmd", L".sh", L".sql", L".html", L".htm", L".css", L".scss", L".reg", L".toml",
        L".gitignore", L".gitattributes", L".editorconfig", L".env", L".properties",
        L".manifest", L".rc", L".svg", L".jsx" };
    return ExtIn(ext, k, sizeof(k) / sizeof(*k));
}

// Проза читається пропорційним шрифтом, код — моноширинним.
bool IsProseExt(const wchar_t* ext)
{
    static const wchar_t* const k[] = { L".txt", L".md", L".markdown", L".log", L".nfo", L".srt", L".vtt" };
    return ExtIn(ext, k, sizeof(k) / sizeof(*k));
}

// ---- текст: читання й розкодування ----

bool DecodeUtf8(const BYTE* b, size_t n, std::vector<wchar_t>& w, bool strict, bool truncated)
{
    // Обрізаний файл може закінчуватись серединою багатобайтового символу —
    // це не привід вважати весь файл не-UTF-8.
    if (truncated) {
        size_t k = 0;
        while (n > 0 && (b[n - 1] & 0xC0) == 0x80 && k++ < 3) --n;
        if (n > 0 && b[n - 1] >= 0xC0) --n;
    }
    if (n == 0) { w.clear(); return true; }
    const DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    const int len = MultiByteToWideChar(CP_UTF8, flags, (LPCSTR)b, (int)n, nullptr, 0);
    if (len <= 0) return false;
    w.resize((size_t)len);
    MultiByteToWideChar(CP_UTF8, flags, (LPCSTR)b, (int)n, w.data(), len);
    return true;
}

void DecodeUtf16(const BYTE* b, size_t n, std::vector<wchar_t>& w, bool bigEndian)
{
    w.resize(n / 2);
    for (size_t i = 0; i < n / 2; ++i)
        w[i] = bigEndian ? (wchar_t)((b[2 * i] << 8) | b[2 * i + 1])
                         : (wchar_t)(b[2 * i] | (b[2 * i + 1] << 8));
}

void DecodeAnsi(const BYTE* b, size_t n, std::vector<wchar_t>& w)
{
    const int len = n ? MultiByteToWideChar(CP_ACP, 0, (LPCSTR)b, (int)n, nullptr, 0) : 0;
    w.resize((size_t)(len > 0 ? len : 0));
    if (len > 0) MultiByteToWideChar(CP_ACP, 0, (LPCSTR)b, (int)n, w.data(), len);
}

// BOM → відповідне кодування; без BOM — сувора перевірка UTF-8, далі UTF-16LE
// (видає себе NUL-ами в непарних байтах), і лише для відомих текстових
// розширень — системне ANSI. NUL-и інакше означають бінарний файл.
bool DecodeText(const BYTE* b, size_t n, std::vector<wchar_t>& w, bool allowAnsi, bool truncated)
{
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) return DecodeUtf8(b + 3, n - 3, w, false, truncated);
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) { DecodeUtf16(b + 2, n - 2, w, false); return true; }
    if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) { DecodeUtf16(b + 2, n - 2, w, true);  return true; }

    const size_t probe = n < 2048 ? n : 2048;
    size_t nul = 0, nulOdd = 0;
    for (size_t i = 0; i < probe; ++i)
        if (!b[i]) { ++nul; if (i & 1) ++nulOdd; }
    if (nul) {
        if (nulOdd * 10 >= probe * 3 && nulOdd * 10 >= nul * 9) { DecodeUtf16(b, n, w, false); return true; }
        return false;
    }
    if (DecodeUtf8(b, n, w, true, truncated)) return true;
    if (!allowAnsi) return false;
    DecodeAnsi(b, n, w);
    return true;
}

bool ReadFileHead(const wchar_t* path, size_t maxBytes, std::vector<BYTE>& out, bool& truncated)
{
    truncated = false;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    GetFileSizeEx(h, &size);
    size_t want = (size.QuadPart > (LONGLONG)maxBytes) ? maxBytes : (size_t)size.QuadPart;
    truncated = size.QuadPart > (LONGLONG)maxBytes;
    out.resize(want);
    DWORD got = 0;
    const bool ok = want == 0 || (ReadFile(h, out.data(), (DWORD)want, &got, nullptr) && got == want);
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

// Файл без відомого розширення вважаємо текстом лише якщо його початок —
// чистий UTF-8/UTF-16 без NUL-ів; ANSI сюди не пускаємо, щоб не показувати
// бінарники як «текст» у кракозябрах.
bool SniffText(const wchar_t* path)
{
    std::vector<BYTE> head;
    bool trunc = false;
    if (!ReadFileHead(path, 4096, head, trunc)) return false;
    std::vector<wchar_t> w;
    return DecodeText(head.data(), head.size(), w, false, trunc);
}

// EDIT розуміє лише CRLF: самотні LF та CR стають CRLF, NUL-и — пробілами.
void NormalizeNewlines(const std::vector<wchar_t>& in, std::vector<wchar_t>& out)
{
    out.clear();
    out.reserve(in.size() + in.size() / 16 + 2);
    for (size_t i = 0; i < in.size(); ++i) {
        const wchar_t c = in[i];
        if (c == L'\r') {
            out.push_back(L'\r'); out.push_back(L'\n');
            if (i + 1 < in.size() && in[i + 1] == L'\n') ++i;
        } else if (c == L'\n') {
            out.push_back(L'\r'); out.push_back(L'\n');
        } else {
            out.push_back(c ? c : L' ');
        }
    }
    out.push_back(0);
}

// Стиснений JSON — суцільна каша (власник приніс 14 КБ в один рядок). Розставляємо
// відступи. Це НАВМИСНО не парсер: валідність не перевіряємо, вміст рядків не чіпаємо,
// лише переносимо поза рядковими літералами. Тому файл із синтаксичною помилкою не
// «зникає» — просто повертаємо false і показуємо його як є.
bool JsonWs(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r'; }

bool JsonPretty(const std::vector<wchar_t>& in, std::vector<wchar_t>& out)
{
    size_t first = 0;
    while (first < in.size() && JsonWs(in[first])) ++first;
    if (first >= in.size() || (in[first] != L'{' && in[first] != L'[')) return false;

    out.clear();
    out.reserve(in.size() + in.size() / 2);
    int depth = 0;
    bool inStr = false, esc = false;
    auto newline = [&](int d) {
        out.push_back(L'\n');
        for (int i = 0; i < d && i < 64; ++i) { out.push_back(L' '); out.push_back(L' '); }
    };
    for (size_t i = first; i < in.size(); ++i) {
        const wchar_t c = in[i];
        if (inStr) {                       // всередині рядка не чіпаємо НІЧОГО
            out.push_back(c);
            if (esc)             esc = false;
            else if (c == L'\\') esc = true;
            else if (c == L'"')  inStr = false;
            continue;
        }
        switch (c) {
        case L'"':
            inStr = true;
            out.push_back(c);
            break;
        case L'{': case L'[': {
            size_t j = i + 1;              // порожній контейнер лишаємо в один рядок
            while (j < in.size() && JsonWs(in[j])) ++j;
            out.push_back(c);
            if (j < in.size() && (in[j] == L'}' || in[j] == L']')) { out.push_back(in[j]); i = j; break; }
            newline(++depth);
            break;
        }
        case L'}': case L']':
            if (--depth < 0) return false;   // дужки розбалансовані — це не JSON
            newline(depth);
            out.push_back(c);
            break;
        case L',':
            out.push_back(c);
            newline(depth);
            break;
        case L':':
            out.push_back(c);
            out.push_back(L' ');
            break;
        default:
            if (!JsonWs(c)) out.push_back(c);   // власні пробіли автора відкидаємо
            break;
        }
        if (out.size() > 8u * 1024 * 1024) return false;
    }
    return depth == 0 && !inStr;
}

bool IsJsonExt(const wchar_t* ext)
{
    static const wchar_t* const k[] = { L".json", L".geojson", L".jsonl", L".webmanifest" };
    return ExtIn(ext, k, sizeof(k) / sizeof(*k));
}


// ---- текст із оформленням: RTF для RichEdit ----
//
// Markdown і підсвітка синтаксису — одна й та сама задача: розкласти текст на
// шматки з різним виглядом. Найдешевший спосіб віддати це RichEdit — зібрати
// RTF і влити одним потоком (EM_STREAMIN). Альтернатива, тисячі
// EM_SETCHARFORMAT, на файлі в кілька сотень кілобайт помітно гальмує.
//
// Підсвітка свідомо ОДНА на всі мови: коментарі, рядки, числа, спільний набір
// ключових слів. Для перегляду цього досить, а повноцінні граматики на кожну
// мову — це вже інший застосунок.

enum RtfColor { RC_TEXT = 1, RC_GRAY, RC_KEYWORD, RC_STRING, RC_COMMENT, RC_NUMBER, RC_HEAD, RC_LINK };

struct RtfBuilder {
    std::string out;
    bool mono = false;

    void Begin(bool dark, int basePt)
    {
        out.reserve(64 * 1024);
        out = "{\\rtf1\\ansi\\ansicpg1251\\deff0{\\fonttbl{\\f0\\fswiss Segoe UI;}{\\f1\\fmodern Consolas;}}";
        out += "{\\colortbl;";
        struct C { int r, g, b; };
        const C light[] = { {32,32,32}, {110,110,110}, {0,0,192}, {163,21,21}, {0,128,0}, {9,134,88}, {17,17,17}, {0,102,204} };
        const C night[] = { {230,230,230}, {155,155,155}, {110,170,240}, {220,150,120}, {130,180,120}, {170,210,160}, {245,245,245}, {120,180,250} };
        const C* p = dark ? night : light;
        for (int i = 0; i < 8; ++i) {
            char buf[64];
            sprintf(buf, "\\red%d\\green%d\\blue%d;", p[i].r, p[i].g, p[i].b);
            out += buf;
        }
        out += "}";
        char hdr[64];
        sprintf(hdr, "\\f0\\fs%d\\cf1 ", basePt * 2);
        out += hdr;
    }

    void Font(bool monoNow)
    {
        if (monoNow == mono) return;
        mono = monoNow;
        out += mono ? "\\f1 " : "\\f0 ";
    }
    void Color(int c) { char b[16]; sprintf(b, "\\cf%d ", c); out += b; }
    void Size(int pt)  { char b[16]; sprintf(b, "\\fs%d ", pt * 2); out += b; }
    void Bold(bool on)   { out += on ? "\\b " : "\\b0 "; }
    void Italic(bool on) { out += on ? "\\i " : "\\i0 "; }
    void Indent(int twips) { char b[24]; sprintf(b, "\\li%d ", twips); out += b; }
    void Par() { out += "\\par\n"; }

    // Не-ASCII віддаємо як \uN? — так RTF лишається чистим ASCII і не залежить
    // від кодової сторінки, у якій його прочитають.
    void Text(const wchar_t* s, size_t n)
    {
        char num[16];
        for (size_t i = 0; i < n; ++i) {
            const wchar_t c = s[i];
            if (c == L'\\' || c == L'{' || c == L'}') { out += '\\'; out += (char)c; }
            else if (c == L'\t') out += "\\tab ";
            else if (c == L'\r') continue;
            else if (c == L'\n') Par();
            else if (c < 128) out += (char)c;
            else {
                sprintf(num, "\\u%d?", (int)(short)c);
                out += num;
            }
        }
    }
    void Text(const std::wstring& s) { Text(s.c_str(), s.size()); }
    void End() { out += "}"; }
};

DWORD CALLBACK RtfStreamIn(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG* done)
{
    std::pair<const char*, size_t>* src = (std::pair<const char*, size_t>*)cookie;
    const LONG n = (LONG)(src->second < (size_t)cb ? src->second : (size_t)cb);
    memcpy(buf, src->first, (size_t)n);
    src->first += n;
    src->second -= (size_t)n;
    *done = n;
    return 0;
}

void PeekSetRtf(const std::string& rtf)
{
    std::pair<const char*, size_t> src(rtf.c_str(), rtf.size());
    EDITSTREAM es = {};
    es.dwCookie = (DWORD_PTR)&src;
    es.pfnCallback = RtfStreamIn;
    SendMessageW(g_peekEdit, WM_SETTEXT, 0, (LPARAM)L"");
    SendMessageW(g_peekEdit, EM_STREAMIN, SF_RTF, (LPARAM)&es);
    SendMessageW(g_peekEdit, EM_SETSEL, 0, 0);
    SendMessageW(g_peekEdit, WM_VSCROLL, SB_TOP, 0);
}

// ---- підсвітка коду ----

bool CodeIdentChar(wchar_t c)
{
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_';
}

bool CodeIsKeyword(const std::wstring& w)
{
    static const wchar_t* const k[] = {
        L"if", L"else", L"elif", L"for", L"while", L"do", L"switch", L"case", L"default", L"break",
        L"continue", L"return", L"goto", L"try", L"catch", L"except", L"finally", L"throw", L"raise",
        L"class", L"struct", L"enum", L"union", L"interface", L"namespace", L"module", L"package",
        L"import", L"from", L"using", L"include", L"require", L"export", L"public", L"private",
        L"protected", L"static", L"const", L"constexpr", L"final", L"virtual", L"override", L"inline",
        L"function", L"func", L"def", L"lambda", L"var", L"let", L"val", L"auto", L"new", L"delete",
        L"this", L"self", L"super", L"null", L"nullptr", L"none", L"nil", L"true", L"false", L"True",
        L"False", L"None", L"and", L"or", L"not", L"in", L"is", L"as", L"with", L"yield", L"await",
        L"async", L"void", L"int", L"long", L"short", L"char", L"float", L"double", L"bool", L"boolean",
        L"string", L"str", L"list", L"dict", L"map", L"set", L"type", L"typedef", L"template", L"typename",
        L"param", L"echo", L"print", L"end", L"then", L"fi", L"esac", L"elseif", L"foreach", L"begin",
        L"select", L"where", L"insert", L"update", L"delete", L"create", L"table", L"join", L"group",
    };
    for (const wchar_t* t : k)
        if (w == t) return true;
    return false;
}

struct CodeStyle { bool slash, hash, dashdash, xml, backtick; };

CodeStyle CodeStyleFor(const wchar_t* ext)
{
    CodeStyle s = {};
    static const wchar_t* const slash[] = { L".c", L".cc", L".cpp", L".h", L".hpp", L".cs", L".java",
                                            L".js", L".ts", L".jsx", L".tsx", L".go", L".rs", L".php",
                                            L".kt", L".swift", L".css", L".scss", L".json", L".rc" };
    static const wchar_t* const hash[] = { L".py", L".sh", L".ps1", L".psm1", L".yaml", L".yml", L".toml",
                                           L".ini", L".conf", L".cfg", L".rb", L".pl", L".r", L".env",
                                           L".gitignore", L".gitattributes", L".properties", L".editorconfig" };
    static const wchar_t* const dd[] = { L".sql", L".lua", L".hs" };
    static const wchar_t* const xml[] = { L".xml", L".html", L".htm", L".svg", L".manifest" };
    s.slash    = ExtIn(ext, slash, sizeof(slash) / sizeof(*slash));
    s.hash     = ExtIn(ext, hash, sizeof(hash) / sizeof(*hash));
    s.dashdash = ExtIn(ext, dd, sizeof(dd) / sizeof(*dd));
    s.xml      = ExtIn(ext, xml, sizeof(xml) / sizeof(*xml));
    s.backtick = ExtIn(ext, slash, sizeof(slash) / sizeof(*slash)) || s.hash;
    return s;
}

void CodeToRtf(const std::wstring& t, const wchar_t* ext, bool dark, int pt, std::string& rtf)
{
    const CodeStyle st = CodeStyleFor(ext);
    RtfBuilder b;
    b.Begin(dark, pt);
    b.Font(true);
    int cur = RC_TEXT;
    auto setc = [&](int c) { if (c != cur) { b.Color(c); cur = c; } };

    const size_t n = t.size();
    size_t i = 0;
    while (i < n) {
        const wchar_t c = t[i];
        // коментарі
        if (st.slash && c == L'/' && i + 1 < n && t[i + 1] == L'/') {
            const size_t e = t.find(L'\n', i);
            setc(RC_COMMENT);
            b.Text(t.c_str() + i, (e == std::wstring::npos ? n : e) - i);
            i = (e == std::wstring::npos) ? n : e;
            continue;
        }
        if (st.slash && c == L'/' && i + 1 < n && t[i + 1] == L'*') {
            size_t e = t.find(L"*/", i + 2);
            e = (e == std::wstring::npos) ? n : e + 2;
            setc(RC_COMMENT);
            b.Text(t.c_str() + i, e - i);
            i = e;
            continue;
        }
        if (st.hash && c == L'#') {
            const size_t e = t.find(L'\n', i);
            setc(RC_COMMENT);
            b.Text(t.c_str() + i, (e == std::wstring::npos ? n : e) - i);
            i = (e == std::wstring::npos) ? n : e;
            continue;
        }
        if (st.dashdash && c == L'-' && i + 1 < n && t[i + 1] == L'-') {
            const size_t e = t.find(L'\n', i);
            setc(RC_COMMENT);
            b.Text(t.c_str() + i, (e == std::wstring::npos ? n : e) - i);
            i = (e == std::wstring::npos) ? n : e;
            continue;
        }
        if (st.xml && c == L'<' && t.compare(i, 4, L"<!--") == 0) {
            size_t e = t.find(L"-->", i + 4);
            e = (e == std::wstring::npos) ? n : e + 3;
            setc(RC_COMMENT);
            b.Text(t.c_str() + i, e - i);
            i = e;
            continue;
        }
        // рядки
        if (c == L'"' || c == L'\'' || (st.backtick && c == L'`')) {
            const wchar_t q = c;
            size_t e = i + 1;
            while (e < n && t[e] != q) {
                if (t[e] == L'\\' && e + 1 < n) ++e;
                if (t[e] == L'\n' && q != L'`') break;      // незакритий рядок не тягнемо на весь файл
                ++e;
            }
            if (e < n && t[e] == q) ++e;
            setc(RC_STRING);
            b.Text(t.c_str() + i, e - i);
            i = e;
            continue;
        }
        // числа
        if (c >= L'0' && c <= L'9' && (i == 0 || !CodeIdentChar(t[i - 1]))) {
            size_t e = i;
            while (e < n && (CodeIdentChar(t[e]) || t[e] == L'.')) ++e;
            setc(RC_NUMBER);
            b.Text(t.c_str() + i, e - i);
            i = e;
            continue;
        }
        // слова
        if (CodeIdentChar(c)) {
            size_t e = i;
            while (e < n && CodeIdentChar(t[e])) ++e;
            const std::wstring w = t.substr(i, e - i);
            setc(CodeIsKeyword(w) ? RC_KEYWORD : RC_TEXT);
            b.Text(w);
            i = e;
            continue;
        }
        setc(RC_TEXT);
        b.Text(t.c_str() + i, 1);
        ++i;
    }
    b.End();
    rtf.swap(b.out);
}

// ---- Markdown ----

// Рядкові прикраси всередині абзацу: **жирний**, *курсив*, `код`, [текст](посилання).
void MdInline(RtfBuilder& b, const std::wstring& s, int baseColor)
{
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const wchar_t c = s[i];
        if (c == L'`') {
            const size_t e = s.find(L'`', i + 1);
            if (e != std::wstring::npos) {
                b.Font(true);
                b.Color(RC_STRING);
                b.Text(s.c_str() + i + 1, e - i - 1);
                b.Color(baseColor);
                b.Font(false);
                i = e + 1;
                continue;
            }
        }
        if ((c == L'*' || c == L'_') && i + 1 < n && s[i + 1] == c) {
            const std::wstring mark(2, c);
            const size_t e = s.find(mark, i + 2);
            if (e != std::wstring::npos) {
                b.Bold(true);
                MdInline(b, s.substr(i + 2, e - i - 2), baseColor);
                b.Bold(false);
                i = e + 2;
                continue;
            }
        }
        if (c == L'*' || c == L'_') {
            const size_t e = s.find(c, i + 1);
            if (e != std::wstring::npos && e > i + 1) {
                b.Italic(true);
                MdInline(b, s.substr(i + 1, e - i - 1), baseColor);
                b.Italic(false);
                i = e + 1;
                continue;
            }
        }
        if (c == L'!' && i + 1 < n && s[i + 1] == L'[') { ++i; continue; }   // зображення: лишаємо підпис
        if (c == L'[') {
            const size_t close = s.find(L']', i);
            if (close != std::wstring::npos && close + 1 < n && s[close + 1] == L'(') {
                const size_t end = s.find(L')', close + 2);
                if (end != std::wstring::npos) {
                    b.Color(RC_LINK);
                    b.Text(s.substr(i + 1, close - i - 1));
                    b.Color(baseColor);
                    i = end + 1;
                    continue;
                }
            }
        }
        b.Text(s.c_str() + i, 1);
        ++i;
    }
}

void MdToRtf(const std::wstring& t, bool dark, int pt, std::string& rtf)
{
    RtfBuilder b;
    b.Begin(dark, pt);
    bool inFence = false;
    size_t pos = 0;
    while (pos <= t.size()) {
        size_t eol = t.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = t.size();
        std::wstring line = t.substr(pos, eol - pos);
        while (!line.empty() && (line.back() == L'\r')) line.pop_back();
        pos = eol + 1;

        // огорожа коду
        if (line.compare(0, 3, L"```") == 0 || line.compare(0, 3, L"~~~") == 0) {
            inFence = !inFence;
            b.Font(inFence);
            b.Color(inFence ? RC_STRING : RC_TEXT);
            b.Indent(inFence ? 240 : 0);
            if (pos > t.size()) break;
            continue;
        }
        if (inFence) {
            b.Text(line);
            b.Par();
            if (pos > t.size()) break;
            continue;
        }

        size_t ind = 0;
        while (ind < line.size() && (line[ind] == L' ' || line[ind] == L'\t')) ++ind;
        const std::wstring body = line.substr(ind);

        // горизонтальна лінія
        if (body.size() >= 3 && (body.find_first_not_of(L"-") == std::wstring::npos ||
                                 body.find_first_not_of(L"*") == std::wstring::npos ||
                                 body.find_first_not_of(L"_") == std::wstring::npos)) {
            b.Color(RC_GRAY);
            b.Text(std::wstring(48, L'\x2500'));
            b.Color(RC_TEXT);
            b.Par();
            if (pos > t.size()) break;
            continue;
        }
        // заголовки
        size_t hashes = 0;
        while (hashes < body.size() && body[hashes] == L'#') ++hashes;
        if (hashes >= 1 && hashes <= 6 && hashes < body.size() && body[hashes] == L' ') {
            const int sizes[6] = { 17, 15, 13, 12, 11, 11 };
            b.Size(sizes[hashes - 1]);
            b.Bold(true);
            b.Color(RC_HEAD);
            MdInline(b, body.substr(hashes + 1), RC_HEAD);
            b.Color(RC_TEXT);
            b.Bold(false);
            b.Size(pt);
            b.Par();
            if (pos > t.size()) break;
            continue;
        }
        // цитата
        if (!body.empty() && body[0] == L'>') {
            b.Indent(240);
            b.Color(RC_GRAY);
            size_t k = 1;
            while (k < body.size() && body[k] == L' ') ++k;
            MdInline(b, body.substr(k), RC_GRAY);
            b.Color(RC_TEXT);
            b.Indent(0);
            b.Par();
            if (pos > t.size()) break;
            continue;
        }
        // списки
        if (body.size() >= 2 && (body[0] == L'-' || body[0] == L'*' || body[0] == L'+') && body[1] == L' ') {
            b.Indent(240 + (int)ind * 120);
            b.Text(L"\x2022  ", 3);
            MdInline(b, body.substr(2), RC_TEXT);
            b.Indent(0);
            b.Par();
            if (pos > t.size()) break;
            continue;
        }
        if (!body.empty() && body[0] >= L'0' && body[0] <= L'9') {
            size_t d = 0;
            while (d < body.size() && body[d] >= L'0' && body[d] <= L'9') ++d;
            if (d + 1 < body.size() && (body[d] == L'.' || body[d] == L')') && body[d + 1] == L' ') {
                b.Indent(240 + (int)ind * 120);
                b.Text(body.substr(0, d + 2));
                MdInline(b, body.substr(d + 2), RC_TEXT);
                b.Indent(0);
                b.Par();
                if (pos > t.size()) break;
                continue;
            }
        }
        MdInline(b, body, RC_TEXT);
        b.Par();
        if (pos > t.size()) break;
    }
    b.End();
    rtf.swap(b.out);
}

void PlainToRtf(const std::wstring& t, bool mono, bool dark, int pt, std::string& rtf)
{
    RtfBuilder b;
    b.Begin(dark, pt);
    b.Font(mono);
    b.Text(t);
    b.End();
    rtf.swap(b.out);
}

bool IsMarkdownExt(const wchar_t* ext)
{
    static const wchar_t* const k[] = { L".md", L".markdown", L".mdown", L".mkd" };
    return ExtIn(ext, k, sizeof(k) / sizeof(*k));
}

// Понад цю межу підсвітку не робимо: користь мала, а пауза помітна.
constexpr size_t kHighlightMax = 400 * 1024;

void PeekShowText(const std::wstring& text, const wchar_t* ext)
{
    const int pt = 10;
    std::string rtf;
    if (IsMarkdownExt(ext) && text.size() <= kHighlightMax)
        MdToRtf(text, g_peekDark, pt, rtf);
    else if (!IsProseExt(ext) && text.size() <= kHighlightMax)
        CodeToRtf(text, ext, g_peekDark, pt, rtf);
    else
        PlainToRtf(text, !IsProseExt(ext), g_peekDark, pt, rtf);
    PeekSetRtf(rtf);
}

bool PeekLoadText(const wchar_t* path, const wchar_t* ext)
{
    std::vector<BYTE> raw;
    bool trunc = false;
    if (!ReadFileHead(path, kPeekTextMax, raw, trunc)) return false;
    std::vector<wchar_t> text;
    if (raw.empty()) {
        text.assign(S(Str::PeekEmpty), S(Str::PeekEmpty) + lstrlenW(S(Str::PeekEmpty)));
    } else if (!DecodeText(raw.data(), raw.size(), text, true, trunc)) {
        return false;   // бінарник із текстовим розширенням — краще картка
    }
    // Переформатовуємо ДО примітки про обрізання: обрізаний JSON не збалансований,
    // JsonPretty його чесно відхилить, і покажемо як є.
    g_peekJsonFormatted = false;
    if (IsJsonExt(ext)) {
        std::vector<wchar_t> pretty;
        if (JsonPretty(text, pretty)) {
            text.swap(pretty);
            g_peekJsonFormatted = true;
        }
    }
    if (trunc) {
        const wchar_t* note = S(Str::PeekTruncated);
        text.push_back(L'\n'); text.push_back(L'\n');
        text.insert(text.end(), note, note + lstrlenW(note));
    }
    PeekShowText(std::wstring(text.begin(), text.end()), ext);
    return true;
}

// ---- ярлики ----
//
// Ярлик — не текст і не картинка, а вказівник. Показувати його нутрощі (власник
// побачив саме це на ярлику гри Steam) — марно: цікаво, КУДИ він веде.

// .url — ini-подібний; .lnk — двійковий, читається оболонкою. Resolve НЕ кличемо:
// він ходить у мережу й може надовго зависнути на недоступному диску.
bool PeekReadShortcut(const wchar_t* path, const wchar_t* ext, wchar_t* out, size_t cch)
{
    out[0] = 0;
    if (lstrcmpiW(ext, L".url") == 0) {
        GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", out, (DWORD)cch, path);
        return out[0] != 0;
    }
    if (lstrcmpiW(ext, L".lnk") != 0) return false;

    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                kIID_IShellLinkW, (void**)&sl)) || !sl)
        return false;
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(kIID_IPersistFile, (void**)&pf)) && pf) {
        if (SUCCEEDED(pf->Load(path, STGM_READ))) {
            wchar_t p[MAX_PATH] = {}, args[512] = {};
            sl->GetPath(p, MAX_PATH, nullptr, SLGP_RAWPATH);
            sl->GetArguments(args, 512);
            if (p[0] && args[0]) swprintf(out, cch, L"%s %s", p, args);
            else if (p[0])       lstrcpynW(out, p, (int)cch);
        }
        pf->Release();
    }
    sl->Release();
    return out[0] != 0;
}

// ---- зображення ----

// ---- фабрики WIC і Direct2D ----
// Спільні для двох споживачів: декодера зображень (webp/heic через WIC) і
// рендера SVG (Direct2D). Створюються ліниво, на першому ж такому файлі.
const GUID kCLSID_WICImagingFactory       = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
const GUID kIID_IWICImagingFactory        = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
const GUID kWICPixelFormat32bppPBGRA      = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10 } };
const GUID kIID_ID2D1Factory1             = { 0xbb12d362, 0xdaee, 0x4b9a, { 0xaa, 0x1d, 0x14, 0xba, 0x40, 0x1c, 0xfa, 0x1f } };
const GUID kIID_ID2D1DeviceContext5       = { 0x7836d248, 0x68cc, 0x4df6, { 0xb9, 0xe8, 0xde, 0x99, 0x1b, 0xf6, 0x2e, 0xb7 } };

ID2D1Factory1*      g_d2d = nullptr;   // створюються ліниво, на першому ж SVG
IWICImagingFactory* g_wic = nullptr;

bool SvgEnsureFactories()
{
    if (!g_wic && FAILED(CoCreateInstance(kCLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                          kIID_IWICImagingFactory, (void**)&g_wic)))
        return false;
    if (!g_d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, kIID_ID2D1Factory1,
                                           nullptr, (void**)&g_d2d)))
        return false;
    return g_wic && g_d2d;
}

// GDI+ не знає webp, heic і avif, а WIC знає — якщо в системі є відповідний
// декодер (webp у Windows 11 є в коробці). Це ДЕКОДЕР зображення, а не обробник
// прев'ю: без інтерфейсу користувача й скриптів, із вузьким контрактом. Запобіжник
// [2026-09-20] про чужий COM стосується саме обробників — межу проведено тут.
Gdiplus::Bitmap* ImageDecodeWic(IStream* stream)
{
    if (!SvgEnsureFactories() || !stream) return nullptr;
    LARGE_INTEGER zero = {};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    Gdiplus::Bitmap* bmp = nullptr;

    if (SUCCEEDED(g_wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && dec &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && frame &&
        SUCCEEDED(g_wic->CreateFormatConverter(&conv)) && conv &&
        SUCCEEDED(conv->Initialize(frame, kWICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        if (w && h && w < 30000 && h < 30000) {
            bmp = new Gdiplus::Bitmap((INT)w, (INT)h, PixelFormat32bppPARGB);
            Gdiplus::BitmapData bd = {};
            Gdiplus::Rect lock(0, 0, (INT)w, (INT)h);
            bool ok = false;
            if (bmp->GetLastStatus() == Gdiplus::Ok &&
                bmp->LockBits(&lock, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &bd) == Gdiplus::Ok) {
                ok = bd.Stride > 0 &&
                     SUCCEEDED(conv->CopyPixels(nullptr, (UINT)bd.Stride, (UINT)bd.Stride * h, (BYTE*)bd.Scan0));
                bmp->UnlockBits(&bd);
            }
            if (!ok) { delete bmp; bmp = nullptr; }
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    return bmp;
}

bool PeekLoadImage(const wchar_t* path)
{
    std::vector<BYTE> raw;
    bool trunc = false;
    if (!ReadFileHead(path, kPeekImageMax, raw, trunc) || trunc || raw.empty()) return false;
    g_peekImgStream = SHCreateMemStream(raw.data(), (UINT)raw.size());
    if (!g_peekImgStream) return false;
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromStream(g_peekImgStream, FALSE);
    if (bmp && (bmp->GetLastStatus() != Gdiplus::Ok || !bmp->GetWidth() || !bmp->GetHeight())) {
        delete bmp;
        bmp = nullptr;
    }
    if (!bmp) bmp = ImageDecodeWic(g_peekImgStream);   // webp/heic/avif — те, чого GDI+ не знає
    if (!bmp) {
        g_peekImgStream->Release();
        g_peekImgStream = nullptr;
        return false;
    }

    // Анімація (GIF): кадри лежать у вимірі «час», затримки — окремим масивом по
    // сотих секунди. Нульову затримку браузери давно трактують як 100 мс — робимо так само.
    if (bmp->GetFrameDimensionsCount() > 0) {
        const UINT n = bmp->GetFrameCount(&kFrameDimensionTime);
        if (n > 1) {
            g_peekFrames = (int)n;
            g_peekDelays.assign(n, 100);
            const UINT dsz = bmp->GetPropertyItemSize(PropertyTagFrameDelay);
            if (dsz) {
                Gdiplus::PropertyItem* pi = (Gdiplus::PropertyItem*)malloc(dsz);
                if (pi && bmp->GetPropertyItem(PropertyTagFrameDelay, dsz, pi) == Gdiplus::Ok && pi->value) {
                    const UINT have = pi->length / sizeof(LONG);
                    const LONG* d = (const LONG*)pi->value;
                    for (UINT i = 0; i < n && i < have; ++i)
                        g_peekDelays[i] = (d[i] > 1) ? (UINT)d[i] * 10 : 100;
                }
                free(pi);
            }
        }
    }

    // Фото з телефона: орієнтація лежить в EXIF, самі пікселі — як зняв сенсор.
    // Для багатокадрових це не робимо: RotateFlip схлопнув би їх в один кадр.
    const UINT sz = (g_peekFrames > 1) ? 0 : bmp->GetPropertyItemSize(PropertyTagOrientation);
    if (sz) {
        Gdiplus::PropertyItem* pi = (Gdiplus::PropertyItem*)malloc(sz);
        if (pi && bmp->GetPropertyItem(PropertyTagOrientation, sz, pi) == Gdiplus::Ok &&
            pi->type == PropertyTagTypeShort && pi->value) {
            switch (*(const WORD*)pi->value) {
            case 2: bmp->RotateFlip(Gdiplus::RotateNoneFlipX);  break;
            case 3: bmp->RotateFlip(Gdiplus::Rotate180FlipNone); break;
            case 4: bmp->RotateFlip(Gdiplus::RotateNoneFlipY);  break;
            case 5: bmp->RotateFlip(Gdiplus::Rotate90FlipX);    break;
            case 6: bmp->RotateFlip(Gdiplus::Rotate90FlipNone); break;
            case 7: bmp->RotateFlip(Gdiplus::Rotate270FlipX);   break;
            case 8: bmp->RotateFlip(Gdiplus::Rotate270FlipNone); break;
            }
        }
        free(pi);
    }
    g_peekImg = bmp;
    g_peekInfo.imgW = (int)bmp->GetWidth();
    g_peekInfo.imgH = (int)bmp->GetHeight();
    return true;
}


// ---- SVG ----
//
// Малюємо системним Direct2D: це сама Windows, а не зареєстрований кимось обробник,
// тож запобіжник про чужий COM в елевейтованому процесі не порушено.
//
// Пробою (scratchpad\svgprobe.cpp) з'ясовано межі D2D: шляхи, градієнти, clipPath,
// обведення й прозорість груп він тягне, а <text>, <mask>, <filter>, <pattern> —
// МОВЧКИ ігнорує. Мовчки — найгірше: користувач побачив би картинку й не знав, що
// вона неправильна. Тому такі файли ми не малюємо взагалі й показуємо розмітку.
//
// Два місця, де реальні файли ламають D2D, виправляє препас:
//  1. Illustrator задає заливки CSS-класами в <style>. D2D їх не застосовує, і весь
//     малюнок виходить ЧОРНОЮ ПЛЯМОЮ — саме так виглядала наша власна іконка.
//     Перекладаємо прості правила «.клас { властивість: значення }» в атрибути.
//  2. <use href="#id"> з SVG 2 D2D не бачить, а старий xlink:href — бачить.


// Що саме D2D пропустить. Раніше на будь-який такий елемент ми ВІДМОВЛЯЛИСЬ малювати;
// перевірка на двох справжніх іконках показала, що це надто категорично: без <filter>
// зникає лише тінь, без <mask> — лише відблиск, і зображення лишається впізнаваним.
// Тому тепер малюємо, але кажемо в підписі, чого бракує. Мовчати про це не можна —
// саме мовчання й робило б картинку брехливою.
Str SvgSkippedNote(const std::wstring& s, bool& any)
{
    any = true;
    if (s.find(L"<text") != std::wstring::npos || s.find(L"<tspan") != std::wstring::npos)
        return Str::PeekSvgNoText;          // втрата змісту — називаємо найперше
    if (s.find(L"<mask") != std::wstring::npos)
        return Str::PeekSvgNoMask;
    if (s.find(L"<filter") != std::wstring::npos)
        return Str::PeekSvgNoFx;
    static const wchar_t* const k[] = { L"<pattern", L"<foreignObject", L"<switch", L"<marker", L"<animate" };
    for (const wchar_t* t : k)
        if (s.find(t) != std::wstring::npos) return Str::PeekSvgPartial;
    any = false;
    return Str::Empty;
}

// Чи вийшов рендер порожнім. Перевіряємо ВМІСТ, а не список елементів: так само
// ловляться випадки, про які ми не здогадались. Поріг свідомо мізерний — тонка
// лінія на 24-піксельній іконці має рахуватись як зображення.
bool BitmapNearlyEmpty(Gdiplus::Bitmap* bmp)
{
    if (!bmp) return true;
    const int w = (int)bmp->GetWidth(), h = (int)bmp->GetHeight();
    if (w < 1 || h < 1) return true;
    Gdiplus::BitmapData bd = {};
    Gdiplus::Rect rc(0, 0, w, h);
    if (bmp->LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bd) != Gdiplus::Ok)
        return false;                        // не змогли перевірити — вважаємо, що щось є
    size_t solid = 0;
    const size_t need = (size_t)w * h / 5000 + 1;   // 0.02 % площі
    for (int y = 0; y < h && solid < need; ++y) {
        const DWORD* p = (const DWORD*)((const BYTE*)bd.Scan0 + (size_t)y * bd.Stride);
        for (int x = 0; x < w; ++x)
            if ((p[x] >> 24) > 8 && ++solid >= need) break;
    }
    bmp->UnlockBits(&bd);
    return solid < need;
}

struct SvgDecl { std::wstring prop, value; };
struct SvgRule { std::wstring name; std::vector<SvgDecl> decls; };

void SvgTrim(std::wstring& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n')) --b;
    s = s.substr(a, b - a);
}

// Свідомо вузький «CSS»: лише «.клас { властивість: значення; }», зокрема через кому.
// Складніші селектори ігноруємо — краще недомалювати, ніж домалювати навмання.
void SvgParseRules(const std::wstring& css, std::vector<SvgRule>& out)
{
    size_t i = 0;
    while (i < css.size()) {
        const size_t open = css.find(L'{', i);
        if (open == std::wstring::npos) break;
        const size_t close = css.find(L'}', open);
        if (close == std::wstring::npos) break;
        std::wstring sel = css.substr(i, open - i);
        std::wstring body = css.substr(open + 1, close - open - 1);
        i = close + 1;

        std::vector<SvgDecl> decls;
        size_t d = 0;
        while (d <= body.size()) {
            const size_t semi = body.find(L';', d);
            std::wstring one = body.substr(d, (semi == std::wstring::npos ? body.size() : semi) - d);
            d = (semi == std::wstring::npos) ? body.size() + 1 : semi + 1;
            const size_t colon = one.find(L':');
            if (colon == std::wstring::npos) continue;
            SvgDecl dd;
            dd.prop = one.substr(0, colon);
            dd.value = one.substr(colon + 1);
            SvgTrim(dd.prop);
            SvgTrim(dd.value);
            // лапки в значенні зіпсували б атрибут
            if (!dd.prop.empty() && !dd.value.empty() && dd.value.find(L'"') == std::wstring::npos)
                decls.push_back(dd);
        }
        if (decls.empty()) continue;

        size_t p = 0;                                  // селектори через кому
        while (p <= sel.size()) {
            const size_t comma = sel.find(L',', p);
            std::wstring one = sel.substr(p, (comma == std::wstring::npos ? sel.size() : comma) - p);
            p = (comma == std::wstring::npos) ? sel.size() + 1 : comma + 1;
            SvgTrim(one);
            if (one.size() < 2 || one[0] != L'.') continue;      // лише простий клас
            const std::wstring name = one.substr(1);
            if (name.find_first_of(L" \t.#:[>+~*") != std::wstring::npos) continue;
            SvgRule r;
            r.name = name;
            r.decls = decls;
            out.push_back(r);
        }
    }
}

// Вирізає всі <style>…</style>, повертаючи їхній вміст.
void SvgTakeStyles(std::wstring& s, std::wstring& css)
{
    for (;;) {
        const size_t a = s.find(L"<style");
        if (a == std::wstring::npos) break;
        const size_t open = s.find(L'>', a);
        if (open == std::wstring::npos) break;
        const size_t b = s.find(L"</style", open);
        if (b == std::wstring::npos) break;
        const size_t end = s.find(L'>', b);
        if (end == std::wstring::npos) break;
        css += s.substr(open + 1, b - open - 1);
        css += L'\n';
        s.erase(a, end - a + 1);
    }
}

void SvgApplyRules(std::wstring& s, const std::vector<SvgRule>& rules)
{
    std::wstring out;
    out.reserve(s.size() + s.size() / 4);
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != L'<') { out.push_back(s[i++]); continue; }
        const size_t close = s.find(L'>', i);
        if (close == std::wstring::npos) { out.append(s, i, std::wstring::npos); break; }
        std::wstring tag = s.substr(i, close - i + 1);
        i = close + 1;

        const size_t cp = tag.find(L" class=\"");
        if (cp != std::wstring::npos) {
            const size_t vs = cp + 8;
            const size_t ve = tag.find(L'"', vs);
            if (ve != std::wstring::npos) {
                const std::wstring names = tag.substr(vs, ve - vs);
                std::wstring add;
                size_t a = 0;
                while (a < names.size()) {
                    while (a < names.size() && names[a] == L' ') ++a;
                    size_t b = a;
                    while (b < names.size() && names[b] != L' ') ++b;
                    if (b > a) {
                        const std::wstring cls = names.substr(a, b - a);
                        for (const SvgRule& r : rules) {
                            if (r.name != cls) continue;
                            for (const SvgDecl& d : r.decls) {
                                // атрибут, заданий прямо на елементі, має перевагу
                                if (tag.find(L' ' + d.prop + L'=') != std::wstring::npos) continue;
                                if (add.find(L' ' + d.prop + L'=') != std::wstring::npos) continue;
                                add += L' ' + d.prop + L"=\"" + d.value + L'"';
                            }
                        }
                    }
                    a = b;
                }
                if (!add.empty()) {
                    const bool self = tag.size() >= 2 && tag[tag.size() - 2] == L'/';
                    tag = tag.substr(0, tag.size() - (self ? 2 : 1)) + add + (self ? L"/>" : L">");
                }
            }
        }
        out += tag;
    }
    s.swap(out);
}

// <use href> → <use xlink:href>; за потреби оголошуємо сам простір імен,
// інакше документ стане невалідним і D2D відмовиться його читати взагалі.
void SvgFixUseHref(std::wstring& s)
{
    bool changed = false;
    size_t i = 0;
    while ((i = s.find(L"<use", i)) != std::wstring::npos) {
        const size_t close = s.find(L'>', i);
        if (close == std::wstring::npos) break;
        const size_t h = s.find(L" href=", i);
        if (h != std::wstring::npos && h < close) {
            s.insert(h + 1, L"xlink:");
            changed = true;
            i = close + 6;
        } else {
            i = close + 1;
        }
    }
    if (!changed) return;
    const size_t tag = s.find(L"<svg");
    if (tag == std::wstring::npos) return;
    const size_t close = s.find(L'>', tag);
    if (close == std::wstring::npos) return;
    // Оголошення шукаємо САМЕ в кореневому <svg>. Якщо воно стоїть на вкладеному
    // елементі, на решту документа воно не поширюється, і доданий нами xlink:href
    // зробить документ невалідним — тоді D2D відмовиться від нього цілком.
    if (s.find(L"xmlns:xlink", tag) < close) return;
    s.insert(tag + 4, L" xmlns:xlink=\"http://www.w3.org/1999/xlink\"");
}

// ⚠ Direct2D шанує width/height КОРЕНЯ і малює документ саме в тому розмірі,
// ігноруючи наш viewport. Через це іконка 24×24 виходила крапкою, а креслення з
// «724mm» — взагалі порожнім. Коли є viewBox, ці атрибути прибираємо: тоді
// документ масштабується під те полотно, яке ми йому дали.
void SvgStripRootSize(std::wstring& s)
{
    const size_t tag = s.find(L"<svg");
    if (tag == std::wstring::npos) return;
    size_t close = s.find(L'>', tag);
    if (close == std::wstring::npos) return;
    if (s.find(L"viewBox", tag) > close) return;        // без viewBox це єдиний розмір — не чіпаємо

    for (const wchar_t* attr : { L" width=\"", L" height=\"" }) {
        const size_t a = s.find(attr, tag);
        if (a == std::wstring::npos || a > close) continue;
        const size_t q = s.find(L'"', a + wcslen(attr));
        if (q == std::wstring::npos || q > close) continue;
        s.erase(a, q - a + 1);
        close = s.find(L'>', tag);
    }
}

// Креслення для лазера приходять із штрихом у частках міліметра: при viewBox 724
// і stroke-width 0.15 лінія на екрані тонша за піксель і просто зникає. Для
// ПЕРЕГЛЯДУ це безглуздо, тож такі штрихи піднімаємо до помітних. Товщі не чіпаємо.
void SvgMinStroke(std::wstring& s, double scale)
{
    if (!(scale > 0)) return;
    const double wantPx = 1.2;
    size_t i = 0;
    while ((i = s.find(L"stroke-width=\"", i)) != std::wstring::npos) {
        const size_t vs = i + 14;
        const size_t ve = s.find(L'"', vs);
        if (ve == std::wstring::npos) break;
        wchar_t* endp = nullptr;
        const std::wstring val = s.substr(vs, ve - vs);
        const double w = wcstod(val.c_str(), &endp);
        const bool bare = endp && *endp == L'\0';        // «0.15mm» пропускаємо
        if (bare && w > 0 && w * scale < wantPx) {
            wchar_t buf[32];
            swprintf(buf, 32, L"%.4f", wantPx / scale);
            s.replace(vs, ve - vs, buf);
            i = vs + wcslen(buf);
        } else {
            i = ve + 1;
        }
    }
}

// Природний розмір документа: viewBox, інакше width/height.
void SvgNaturalSize(const std::wstring& s, double& w, double& h)
{
    w = h = 0;
    const size_t tag = s.find(L"<svg");
    const size_t close = (tag == std::wstring::npos) ? std::wstring::npos : s.find(L'>', tag);
    if (close == std::wstring::npos) return;
    const std::wstring head = s.substr(tag, close - tag);

    const size_t vb = head.find(L"viewBox=\"");
    if (vb != std::wstring::npos) {
        double a = 0, b = 0;
        if (swscanf(head.c_str() + vb + 9, L"%lf %lf %lf %lf", &a, &b, &w, &h) == 4 && w > 0 && h > 0)
            return;
        w = h = 0;
    }
    const size_t wp = head.find(L" width=\"");
    const size_t hp = head.find(L" height=\"");
    if (wp != std::wstring::npos && hp != std::wstring::npos) {
        w = wcstod(head.c_str() + wp + 8, nullptr);   // «100%» дасть 100 — не біда, далі перевірка
        h = wcstod(head.c_str() + hp + 9, nullptr);
    }
}

bool PeekLoadSvg(const wchar_t* path)
{
    std::vector<BYTE> raw;
    bool trunc = false;
    if (!ReadFileHead(path, 8u * 1024 * 1024, raw, trunc) || trunc || raw.empty()) return false;
    std::vector<wchar_t> wide;
    if (!DecodeText(raw.data(), raw.size(), wide, true, false) || wide.empty()) return false;
    std::wstring s(wide.begin(), wide.end());
    if (s.find(L"<svg") == std::wstring::npos) return false;
    bool skipped = false;
    const Str note = SvgSkippedNote(s, skipped);

    std::wstring css;
    SvgTakeStyles(s, css);
    if (!css.empty()) {
        std::vector<SvgRule> rules;
        SvgParseRules(css, rules);
        if (!rules.empty()) SvgApplyRules(s, rules);
    }
    SvgFixUseHref(s);
    {   // оголошення кодування стало б брехнею після переведення в UTF-8
        const size_t d = s.find(L"<?xml");
        if (d != std::wstring::npos) {
            const size_t e = s.find(L"?>", d);
            if (e != std::wstring::npos) s.erase(d, e - d + 2);
        }
    }

    double docW = 0, docH = 0;
    SvgNaturalSize(s, docW, docH);
    SvgStripRootSize(s);   // ПІСЛЯ читання розміру — далі він уже не потрібен у файлі
    if (docW <= 0 || docH <= 0) { docW = 512; docH = 512; }
    // Вектор можна малювати в будь-якій роздільності; беремо природний розмір,
    // але не дрібніше 256 і не більше 1400 по довгій стороні.
    double scale = 1.0;
    const double longSide = (docW > docH) ? docW : docH;
    if (longSide < 256)  scale = 256.0 / longSide;
    if (longSide > 1400) scale = 1400.0 / longSide;
    const int w = (int)(docW * scale + 0.5), h = (int)(docH * scale + 0.5);
    if (w < 1 || h < 1 || !SvgEnsureFactories()) return false;
    SvgMinStroke(s, scale);

    const int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return false;
    std::vector<char> u8((size_t)need);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u8.data(), need, nullptr, nullptr);

    IStream* stream = SHCreateMemStream((const BYTE*)u8.data(), (UINT)u8.size());
    if (!stream) return false;

    IWICBitmap* wicBmp = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    ID2D1DeviceContext5* dc = nullptr;
    ID2D1SvgDocument* doc = nullptr;
    Gdiplus::Bitmap* bmp = nullptr;
    bool ok = false;

    if (SUCCEEDED(g_wic->CreateBitmap((UINT)w, (UINT)h, kWICPixelFormat32bppPBGRA,
                                      WICBitmapCacheOnLoad, &wicBmp)) && wicBmp) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(g_d2d->CreateWicBitmapRenderTarget(wicBmp, props, &rt)) && rt &&
            SUCCEEDED(rt->QueryInterface(kIID_ID2D1DeviceContext5, (void**)&dc)) && dc &&
            SUCCEEDED(dc->CreateSvgDocument(stream, D2D1::SizeF((float)w, (float)h), &doc)) && doc) {
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0.0f));
            dc->DrawSvgDocument(doc);
            if (SUCCEEDED(rt->EndDraw())) {
                bmp = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
                Gdiplus::BitmapData bd = {};
                Gdiplus::Rect lock(0, 0, w, h);
                if (bmp->GetLastStatus() == Gdiplus::Ok &&
                    bmp->LockBits(&lock, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &bd) == Gdiplus::Ok) {
                    if (bd.Stride > 0 &&
                        SUCCEEDED(wicBmp->CopyPixels(nullptr, (UINT)bd.Stride, (UINT)bd.Stride * h, (BYTE*)bd.Scan0)))
                        ok = true;
                    bmp->UnlockBits(&bd);
                }
                if (!ok) { delete bmp; bmp = nullptr; }
            }
        }
    }
    if (doc) doc->Release();
    if (dc) dc->Release();
    if (rt) rt->Release();
    if (wicBmp) wicBmp->Release();
    stream->Release();
    if (!ok) return false;

    // Рендер без вмісту — це не перегляд, а порожнє вікно: краще показати розмітку.
    if (BitmapNearlyEmpty(bmp)) { delete bmp; return false; }
    g_peekSvgNote = skipped ? note : Str::Empty;
    g_peekImg = bmp;
    g_peekInfo.imgW = w;                    // розмір бітмапа — ним малює PeekPaint
    g_peekInfo.imgH = h;
    g_peekInfo.docW = (int)(docW + 0.5);    // а в підписі показуємо розмір документа
    g_peekInfo.docH = (int)(docH + 0.5);
    return true;
}


// ---- STL ----
//
// Формат простий настільки, що власний рендер дешевший за будь-яку залежність:
// z-буфер і плоске затінення, десь двісті рядків і нуль нових DLL.
//
// ⚠ Двійковий чи текстовий визначаємо РОЗМІРОМ файлу, а не словом "solid" на
// початку: купа експортерів пишуть "solid" і в двійковий файл, тож перевірка за
// текстом дає хибний результат на цілком типових моделях.
//
// Нормалі з файлу свідомо ІГНОРУЄМО й рахуємо з вершин: у реальних STL вони
// часто нульові або дивляться не туди. З тієї ж причини освітлення двостороннє —
// намотка трикутників теж буває неузгодженою, а показати дірку в моделі там,
// де її немає, гірше, ніж не відсікти задню грань.

struct StlTri { float v[9]; };
constexpr size_t kStlMaxTris = 1500000;   // ~54 МБ у пам'яті; більше — покажемо картку

bool StlParse(const std::vector<BYTE>& raw, std::vector<StlTri>& tris)
{
    tris.clear();
    if (raw.size() >= 84) {
        UINT32 n = 0;
        memcpy(&n, raw.data() + 80, 4);
        if (n > 0 && n <= kStlMaxTris && raw.size() == 84 + (size_t)n * 50) {
            tris.resize(n);
            for (UINT32 i = 0; i < n; ++i)
                memcpy(tris[i].v, raw.data() + 84 + (size_t)i * 50 + 12, 36);   // нормаль пропускаємо
            return true;
        }
    }
    // текстовий: збираємо всі "vertex x y z" по три
    const char* p = (const char*)raw.data();
    const char* end = p + raw.size();
    float buf[9];
    int got = 0;
    while (p < end) {
        const char* v = (const char*)memchr(p, 'v', (size_t)(end - p));
        if (!v) break;
        if ((size_t)(end - v) < 7 || memcmp(v, "vertex", 6) != 0) { p = v + 1; continue; }
        const char* q = v + 6;
        int comp = 0;
        while (comp < 3 && q < end) {
            char* next = nullptr;
            const double d = strtod(q, &next);
            if (next == q) break;
            buf[got * 3 + comp] = (float)d;
            q = next;
            ++comp;
        }
        p = q;
        if (comp != 3) continue;
        if (++got == 3) {
            StlTri t;
            memcpy(t.v, buf, sizeof(buf));
            tris.push_back(t);
            got = 0;
            if (tris.size() > kStlMaxTris) return false;
        }
    }
    return !tris.empty();
}

// Ортографічна проєкція у фіксованому ізометричному ракурсі (STL — Z вгору),
// растеризація крайовими функціями з z-буфером.
Gdiplus::Bitmap* StlRender(const std::vector<StlTri>& tris, int side, float dims[3])
{
    float mn[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, mx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const StlTri& t : tris)
        for (int k = 0; k < 3; ++k)
            for (int c = 0; c < 3; ++c) {
                const float val = t.v[k * 3 + c];
                if (val < mn[c]) mn[c] = val;
                if (val > mx[c]) mx[c] = val;
            }
    for (int c = 0; c < 3; ++c) dims[c] = mx[c] - mn[c];
    const float ctr[3] = { (mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2 };

    // Rz(-35°) -> Rx(-65°): звичний «погляд згори збоку», як у слайсерах
    const float az = -35.0f * 3.14159265f / 180.0f, el = -65.0f * 3.14159265f / 180.0f;
    const float ca = cosf(az), sa = sinf(az), ce = cosf(el), se = sinf(el);
    auto view = [&](const float* s, float* d) {
        const float x = s[0] - ctr[0], y = s[1] - ctr[1], z = s[2] - ctr[2];
        const float x1 = x * ca - y * sa, y1 = x * sa + y * ca;
        d[0] = x1;
        d[1] = y1 * ce - z * se;
        d[2] = y1 * se + z * ce;
    };

    float vmn[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, vmx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const StlTri& t : tris)
        for (int k = 0; k < 3; ++k) {
            float p[3];
            view(t.v + k * 3, p);
            for (int c = 0; c < 3; ++c) { if (p[c] < vmn[c]) vmn[c] = p[c]; if (p[c] > vmx[c]) vmx[c] = p[c]; }
        }
    const float spanX = vmx[0] - vmn[0], spanY = vmx[1] - vmn[1];
    const float span = (spanX > spanY ? spanX : spanY);
    if (!(span > 0)) return nullptr;
    const float scale = side * 0.86f / span;
    const float offX = side / 2.0f - (vmn[0] + vmx[0]) / 2 * scale;
    const float offY = side / 2.0f + (vmn[1] + vmx[1]) / 2 * scale;   // Y екрана вниз

    std::vector<float> zbuf((size_t)side * side, -FLT_MAX);
    std::vector<DWORD> pix((size_t)side * side, 0);
    const float lx = 0.35f, ly = -0.45f, lz = 0.82f;                 // джерело світла у view-просторі

    for (const StlTri& t : tris) {
        float p[3][3], sx[3], sy[3], sz[3];
        for (int k = 0; k < 3; ++k) {
            view(t.v + k * 3, p[k]);
            sx[k] = p[k][0] * scale + offX;
            sy[k] = offY - p[k][1] * scale;
            sz[k] = p[k][2];
        }
        const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
        if (area == 0) continue;

        float ux = p[1][0] - p[0][0], uy = p[1][1] - p[0][1], uz = p[1][2] - p[0][2];
        float wx = p[2][0] - p[0][0], wy = p[2][1] - p[0][1], wz = p[2][2] - p[0][2];
        float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
        const float nl = sqrtf(nx * nx + ny * ny + nz * nz);
        if (nl <= 0) continue;
        nx /= nl; ny /= nl; nz /= nl;
        if (nz < 0) { nx = -nx; ny = -ny; nz = -nz; }                // двостороннє світло
        float diff = nx * lx + ny * ly + nz * lz;
        if (diff < 0) diff = -diff;
        const float shade = 0.28f + 0.72f * diff;
        const int r = (int)(122 * shade + 0.5f), g = (int)(152 * shade + 0.5f), b = (int)(188 * shade + 0.5f);
        const DWORD color = 0xFF000000u | ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;

        int x0 = (int)floorf(sx[0] < sx[1] ? (sx[0] < sx[2] ? sx[0] : sx[2]) : (sx[1] < sx[2] ? sx[1] : sx[2]));
        int x1 = (int)ceilf (sx[0] > sx[1] ? (sx[0] > sx[2] ? sx[0] : sx[2]) : (sx[1] > sx[2] ? sx[1] : sx[2]));
        int y0 = (int)floorf(sy[0] < sy[1] ? (sy[0] < sy[2] ? sy[0] : sy[2]) : (sy[1] < sy[2] ? sy[1] : sy[2]));
        int y1 = (int)ceilf (sy[0] > sy[1] ? (sy[0] > sy[2] ? sy[0] : sy[2]) : (sy[1] > sy[2] ? sy[1] : sy[2]));
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > side - 1) x1 = side - 1;
        if (y1 > side - 1) y1 = side - 1;

        const float inv = 1.0f / area;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float px = x + 0.5f, py = y + 0.5f;
                const float w0 = ((sx[1] - sx[0]) * (py - sy[0]) - (sy[1] - sy[0]) * (px - sx[0])) * inv;
                const float w1 = ((sx[2] - sx[1]) * (py - sy[1]) - (sy[2] - sy[1]) * (px - sx[1])) * inv;
                const float w2 = ((sx[0] - sx[2]) * (py - sy[2]) - (sy[0] - sy[2]) * (px - sx[2])) * inv;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float z = sz[0] * w1 + sz[1] * w2 + sz[2] * w0;
                const size_t idx = (size_t)y * side + x;
                if (z <= zbuf[idx]) continue;
                zbuf[idx] = z;
                pix[idx] = color;
            }
        }
    }

    Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(side, side, PixelFormat32bppPARGB);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) { delete bmp; return nullptr; }
    Gdiplus::BitmapData bd = {};
    Gdiplus::Rect lock(0, 0, side, side);
    if (bmp->LockBits(&lock, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &bd) != Gdiplus::Ok) {
        delete bmp;
        return nullptr;
    }
    for (int y = 0; y < side; ++y)
        memcpy((BYTE*)bd.Scan0 + (size_t)y * bd.Stride, &pix[(size_t)y * side], (size_t)side * 4);
    bmp->UnlockBits(&bd);
    return bmp;
}

bool PeekLoadStl(const wchar_t* path, float dims[3], unsigned& triCount)
{
    std::vector<BYTE> raw;
    bool trunc = false;
    if (!ReadFileHead(path, 64u * 1024 * 1024, raw, trunc) || trunc || raw.size() < 84) return false;
    std::vector<StlTri> tris;
    if (!StlParse(raw, tris) || tris.empty()) return false;
    triCount = (unsigned)tris.size();
    Gdiplus::Bitmap* bmp = StlRender(tris, 720, dims);
    if (!bmp) return false;
    g_peekImg = bmp;
    g_peekInfo.imgW = (int)bmp->GetWidth();
    g_peekInfo.imgH = (int)bmp->GetHeight();
    return true;
}


// ---- відео ----
//
// Показуємо ОДИН кадр і метадані, а не програємо: справжнє відтворення — це вже
// свій рендер, звук і керування, тобто інша задача. Для «що це за файл» кадру
// достатньо, і коштує він одного виклику Media Foundation, без залежностей.
//
// Два місця, де це легко зробити неправильно:
//  1. Без MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING читач відмовиться віддавати
//     RGB32 для більшості кодеків — треба явно дозволити перетворення.
//  2. Найперший кадр у багатьох файлах чорний (заставка/фейд), тому відмотуємо
//     трохи вперед; якщо перемотка не вдалась — читаємо що є.

bool g_mfStarted = false;

bool VideoEnsureMf()
{
    if (g_mfStarted) return true;
    g_mfStarted = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    return g_mfStarted;
}

bool IsVideoExt(const wchar_t* ext)
{
    static const wchar_t* const k[] = { L".mp4", L".m4v", L".mov", L".avi",
                                        L".wmv", L".asf", L".mkv", L".webm", L".3gp" };
    return ExtIn(ext, k, sizeof(k) / sizeof(*k));
}

void FormatDuration(LONGLONG hundredNs, wchar_t* buf, int n)
{
    const LONGLONG total = hundredNs / 10000000;            // у секунди
    const int h = (int)(total / 3600), m = (int)((total / 60) % 60), s = (int)(total % 60);
    if (h > 0) swprintf(buf, n, L"%d:%02d:%02d", h, m, s);
    else       swprintf(buf, n, L"%d:%02d", m, s);
}

// ⚠ Найпідступніше місце в усьому відео. MF_MT_DEFAULT_STRIDE дорівнює w*4 і
// БРЕШЕ, коли декодер вирівнює кадр: на екранному записі 1918x1050 буфер виявився
// 8 110 080 байт, тобто 1920 x 1056 — вирівняно і ширину, і висоту. Кадр через це
// «їхав» по діагоналі. IMF2DBuffer, який знав би справжній крок, на цих буферах
// ВІДСУТНІЙ (перевірено пробою на двох файлах), тож виводимо крок із довжини:
// шукаємо найменший крок >= w*4, на який довжина ділиться націло й дає не менше
// h рядків. Не вдалося — лишаємо те, що сказав DEFAULT_STRIDE.
UINT DeriveVideoStride(DWORD bufLen, UINT32 w, UINT32 h, UINT fallback)
{
    if (!w || !h || !bufLen) return fallback;
    const UINT minStride = w * 4;
    for (UINT s = minStride; s <= minStride + 4096; s += 4)
        if ((bufLen % s) == 0 && (bufLen / s) >= h)
            return s;
    return fallback;
}

bool PeekLoadVideo(const wchar_t* path, wchar_t* durOut, int durCch)
{
    durOut[0] = 0;
    if (!VideoEnsureMf()) return false;

    IMFAttributes* attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 1)) || !attrs) return false;
    // без цього SetCurrentMediaType(RGB32) провалиться на більшості кодеків
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    IMFSourceReader* reader = nullptr;
    const HRESULT hrOpen = MFCreateSourceReaderFromURL(path, attrs, &reader);
    attrs->Release();
    if (FAILED(hrOpen) || !reader) return false;

    bool ok = false;
    IMFMediaType* want = nullptr;
    IMFMediaType* cur = nullptr;
    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;

    if (SUCCEEDED(MFCreateMediaType(&want)) && want) {
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want)) &&
            SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)) && cur) {
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
            INT32 stride = 0;
            if (FAILED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&stride)) || stride == 0)
                stride = (INT32)w * 4;

            LONGLONG dur = 0;
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))
                && pv.vt == VT_UI8)
                dur = (LONGLONG)pv.uhVal.QuadPart;
            PropVariantClear(&pv);
            if (dur > 0) FormatDuration(dur, durOut, durCch);

            if (dur > 20000000) {          // довше 2 с — відмотати, щоб не впіймати чорну заставку
                PROPVARIANT pos;
                PropVariantInit(&pos);
                pos.vt = VT_I8;
                pos.hVal.QuadPart = (dur / 10 < 30000000) ? dur / 10 : 30000000;
                reader->SetCurrentPosition(GUID_NULL, pos);   // бере посилання, не вказівник
                PropVariantClear(&pos);
            }

            for (int attempt = 0; attempt < 12 && !sample; ++attempt) {
                DWORD idx = 0, flags = 0;
                LONGLONG ts = 0;
                if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &sample)))
                    break;
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            }

            // ⚠ Крок рядка — найпідступніше місце в усьому цьому. MF_MT_DEFAULT_STRIDE
            // бреше, коли декодер вирівнює рядок (кадр 1918 px лежить у буфері по 1920), і
            // кадр виходить зсунутим по діагоналі. Справжній крок знає IMF2DBuffer, АЛЕ
            // ConvertToContiguousBuffer віддає копію, яка цього інтерфейсу вже не має —
            // тому питаємо ОРИГІНАЛЬНИЙ буфер семпла. Якщо й там ні, виводимо крок із
            // довжини буфера: вона враховує вирівнювання, а DEFAULT_STRIDE — ні.
            IMF2DBuffer* buf2d = nullptr;
            if (sample) {
                IMFMediaBuffer* orig = nullptr;
                if (SUCCEEDED(sample->GetBufferByIndex(0, &orig)) && orig) {
                    if (FAILED(orig->QueryInterface(IID_IMF2DBuffer, (void**)&buf2d))) buf2d = nullptr;
                    orig->Release();
                }
            }
            if (sample && w && h && SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer) {
                BYTE* data = nullptr;
                DWORD maxLen = 0, curLen = 0;
                bool locked2d = false;
                if (buf2d) {
                    LONG pitch = 0;
                    if (SUCCEEDED(buf2d->Lock2D(&data, &pitch)) && data) {
                        locked2d = true;
                        stride = (INT32)pitch;
                        curLen = (DWORD)((pitch < 0 ? -pitch : pitch) * (LONG)h);
                    }
                }
                if ((locked2d && data) || (SUCCEEDED(buffer->Lock(&data, &maxLen, &curLen)) && data)) {
                    const bool bottomUp = stride < 0;
                    int absStride = bottomUp ? -stride : stride;
                    if (!locked2d)
                        absStride = (int)DeriveVideoStride(curLen, w, h, (UINT)absStride);
                    // Рядків у буфері може бути БІЛЬШЕ за h (вирівняна висота). Для
                    // перевернутого кадру перший рядок зображення — останній у буфері.
                    const UINT rowsInBuf = absStride ? (curLen / (UINT)absStride) : h;
                    if ((DWORD)absStride * h <= curLen) {
                        Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap((INT)w, (INT)h, PixelFormat32bppPARGB);
                        Gdiplus::BitmapData bd = {};
                        Gdiplus::Rect lock(0, 0, (INT)w, (INT)h);
                        if (bmp->GetLastStatus() == Gdiplus::Ok &&
                            bmp->LockBits(&lock, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &bd) == Gdiplus::Ok) {
                            for (UINT32 y = 0; y < h; ++y) {
                                const BYTE* src = bottomUp ? data + (size_t)(rowsInBuf - 1 - y) * absStride
                                                           : data + (size_t)y * absStride;
                                DWORD* dst = (DWORD*)((BYTE*)bd.Scan0 + (size_t)y * bd.Stride);
                                for (UINT32 x = 0; x < w; ++x) {
                                    const DWORD px = ((const DWORD*)src)[x];
                                    dst[x] = px | 0xFF000000u;   // RGB32 лишає альфу нульовою
                                }
                            }
                            bmp->UnlockBits(&bd);
                            g_peekImg = bmp;
                            g_peekInfo.imgW = (int)w;
                            g_peekInfo.imgH = (int)h;
                            ok = true;
                        } else {
                            delete bmp;
                        }
                    }
                    if (locked2d) buf2d->Unlock2D();
                    else           buffer->Unlock();
                }
                if (buf2d) buf2d->Release();
            }
        }
    }
    if (buffer) buffer->Release();
    if (sample) sample->Release();
    if (cur) cur->Release();
    if (want) want->Release();
    reader->Release();
    return ok;
}


// ---- docx ----
//
// docx — це zip з OOXML. Свій розпакувальник (inflate) писати не довелось: у Windows
// є готовий Packaging API (msopc) для тих самих пакетів, і це знову СИСТЕМНИЙ код,
// а не сторонній обробник, зареєстрований для розширення.
//
// Показуємо ТЕКСТ, а не верстку: відтворити оформлення Word без його ж рушія
// неможливо, а текст відповідає на питання «що це за документ». Підпис про це каже.

const GUID kCLSID_OpcFactory = { 0x6b2d6ba0, 0x9f3e, 0x4f27, { 0x92, 0x0b, 0x31, 0x3c, 0xc4, 0x26, 0xa3, 0x9e } };
const GUID kIID_IOpcFactory  = { 0x6d0b4446, 0xcd73, 0x4ab3, { 0x94, 0xf4, 0x8c, 0xcd, 0xf6, 0x11, 0x61, 0x54 } };

bool OpcReadPart(const wchar_t* path, const wchar_t* partUri, std::vector<BYTE>& out)
{
    out.clear();
    IOpcFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_OpcFactory, nullptr, CLSCTX_INPROC_SERVER,
                                kIID_IOpcFactory, (void**)&factory)) || !factory)
        return false;

    IStream* file = nullptr;
    IOpcPackage* pkg = nullptr;
    IOpcPartSet* parts = nullptr;
    IOpcPartUri* uri = nullptr;
    IOpcPart* part = nullptr;
    IStream* content = nullptr;

    if (SUCCEEDED(factory->CreateStreamOnFile(path, OPC_STREAM_IO_READ, nullptr, 0, &file)) && file &&
        SUCCEEDED(factory->ReadPackageFromStream(file, OPC_READ_DEFAULT, &pkg)) && pkg &&
        SUCCEEDED(pkg->GetPartSet(&parts)) && parts &&
        SUCCEEDED(factory->CreatePartUri(partUri, &uri)) && uri &&
        SUCCEEDED(parts->GetPart(uri, &part)) && part &&
        SUCCEEDED(part->GetContentStream(&content)) && content) {
        BYTE buf[16384];
        for (;;) {
            ULONG got = 0;
            if (FAILED(content->Read(buf, sizeof(buf), &got)) || got == 0) break;
            out.insert(out.end(), buf, buf + got);
            if (out.size() > 32u * 1024 * 1024) break;   // документ явно не для швидкого перегляду
        }
    }
    if (content) content->Release();
    if (part) part->Release();
    if (uri) uri->Release();
    if (parts) parts->Release();
    if (pkg) pkg->Release();
    if (file) file->Release();
    factory->Release();
    return !out.empty();
}

void XmlUnescape(std::wstring& s)
{
    static const struct { const wchar_t* ent; wchar_t ch; } kEnt[] = {
        { L"&lt;", L'<' }, { L"&gt;", L'>' }, { L"&quot;", L'"' },
        { L"&apos;", L'\'' }, { L"&amp;", L'&' }   // амперсанд — ОСТАННІМ, інакше «&amp;lt;» зіпсується
    };
    for (const auto& e : kEnt) {
        const size_t n = wcslen(e.ent);
        size_t i = 0;
        while ((i = s.find(e.ent, i)) != std::wstring::npos) s.replace(i, n, 1, e.ch);
    }
    size_t i = 0;                                   // числові посилання
    while ((i = s.find(L"&#", i)) != std::wstring::npos) {
        const size_t semi = s.find(L';', i);
        if (semi == std::wstring::npos || semi - i > 10) { i += 2; continue; }
        const bool hex = (s[i + 2] == L'x' || s[i + 2] == L'X');
        const long code = wcstol(s.c_str() + i + (hex ? 3 : 2), nullptr, hex ? 16 : 10);
        if (code > 0 && code < 0x10000) s.replace(i, semi - i + 1, 1, (wchar_t)code);
        else i = semi + 1;
    }
}

// WordprocessingML → текст: беремо вміст <w:t>, абзац закриваємо переносом.
void DocxExtractText(const std::wstring& xml, std::wstring& out)
{
    out.clear();
    out.reserve(xml.size() / 8);
    size_t i = 0;
    while (i < xml.size()) {
        const size_t lt = xml.find(L'<', i);
        if (lt == std::wstring::npos) break;
        const size_t gt = xml.find(L'>', lt);
        if (gt == std::wstring::npos) break;
        const std::wstring tag = xml.substr(lt, gt - lt + 1);
        i = gt + 1;

        if (tag.compare(0, 5, L"<w:t>") == 0 || tag.compare(0, 5, L"<w:t ") == 0) {
            const size_t close = xml.find(L"</w:t>", i);
            if (close == std::wstring::npos) break;
            out += xml.substr(i, close - i);
            i = close + 6;
        } else if (tag.compare(0, 7, L"<w:tab/") == 0 || tag.compare(0, 7, L"<w:tab ") == 0) {
            out += L'\t';
        } else if (tag.compare(0, 6, L"<w:br/") == 0 || tag.compare(0, 6, L"<w:br ") == 0) {
            out += L'\n';
        } else if (tag.compare(0, 6, L"</w:p>") == 0) {
            out += L'\n';
        }
    }
    XmlUnescape(out);
}

bool IsDocxExt(const wchar_t* ext)
{
    static const wchar_t* const k[] = { L".docx", L".docm" };
    return ExtIn(ext, k, sizeof(k) / sizeof(*k));
}

bool PeekLoadDocx(const wchar_t* path)
{
    std::vector<BYTE> raw;
    if (!OpcReadPart(path, L"/word/document.xml", raw)) return false;
    std::vector<wchar_t> wide;
    if (!DecodeText(raw.data(), raw.size(), wide, true, false) || wide.empty()) return false;
    std::wstring xml(wide.begin(), wide.end());
    std::wstring text;
    DocxExtractText(xml, text);
    if (text.empty()) return false;

    PeekShowText(text, L".txt");   // docx — проза, без підсвітки
    return true;
}


// ---- STEP ----
//
// Намалювати STEP ми НЕ можемо і не вдаємо, що можемо: це B-rep з NURBS-поверхнями
// й топологією, для якого потрібен рушій штибу OpenCASCADE — десятки мегабайтів,
// тобто рівно те, чого цей застосунок уникає. Але шапка STEP — звичайний текст,
// і з неї виходить корисна картка: хто, чим і коли зробив, за якою схемою.

void StepUnquote(const char* s, size_t n, wchar_t* out, int cch)
{
    out[0] = 0;
    if (!n) return;
    std::string v(s, n);
    // у STEP апостроф усередині рядка подвоюється
    size_t i = 0;
    while ((i = v.find("''", i)) != std::string::npos) { v.erase(i, 1); ++i; }
    MultiByteToWideChar(CP_UTF8, 0, v.c_str(), (int)v.size(), out, cch - 1);
    out[(v.size() < (size_t)cch - 1) ? v.size() : (size_t)cch - 1] = 0;
}

// Витягує i-й рядок у лапках із дужок виклику, напр. FILE_NAME('a','b',('c'),...)
bool StepArg(const std::string& call, int index, wchar_t* out, int cch)
{
    out[0] = 0;
    int depth = 0, argIdx = 0;
    size_t i = call.find('(');
    if (i == std::string::npos) return false;
    ++i;
    depth = 1;
    size_t argStart = i;
    for (; i < call.size() && depth > 0; ++i) {
        const char c = call[i];
        if (c == '\'') {                                  // пропустити рядок цілком
            ++i;
            while (i < call.size()) {
                if (call[i] == '\'' && (i + 1 >= call.size() || call[i + 1] != '\'')) break;
                if (call[i] == '\'' ) ++i;
                ++i;
            }
            continue;
        }
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (c == ',' && depth == 1) {
            if (argIdx == index) break;
            ++argIdx;
            argStart = i + 1;
        }
    }
    if (argIdx != index) return false;
    std::string arg = call.substr(argStart, i - argStart);
    const size_t q1 = arg.find('\'');
    if (q1 == std::string::npos) return false;
    const size_t q2 = arg.rfind('\'');
    if (q2 <= q1) return false;
    StepUnquote(arg.c_str() + q1 + 1, q2 - q1 - 1, out, cch);
    return out[0] != 0;
}

bool IsStepExt(const wchar_t* ext)
{
    static const wchar_t* const k[] = { L".step", L".stp" };
    return ExtIn(ext, k, sizeof(k) / sizeof(*k));
}

bool PeekLoadStep(const wchar_t* path, PeekInfo& I)
{
    std::vector<BYTE> raw;
    bool trunc = false;
    if (!ReadFileHead(path, 64u * 1024 * 1024, raw, trunc) || raw.size() < 32) return false;
    const std::string head((const char*)raw.data(), raw.size() < 8192 ? raw.size() : 8192);
    if (head.find("ISO-10303-21") == std::string::npos) return false;

    auto call = [&](const char* name) -> std::string {
        const size_t a = head.find(name);
        if (a == std::string::npos) return std::string();
        const size_t b = head.find(';', a);
        return head.substr(a, (b == std::string::npos ? head.size() : b) - a);
    };

    const std::string fn = call("FILE_NAME");
    if (!fn.empty()) {
        StepArg(fn, 1, I.created2, 64);      // мітка часу ISO: «T» посередині читати незручно
        for (wchar_t* t = I.created2; *t; ++t)
            if (*t == L'T') { *t = L' '; break; }
        StepArg(fn, 2, I.author, 160);       // автор (перший у списку)
        StepArg(fn, 3, I.org, 160);          // організація
    }
    const std::string fs = call("FILE_SCHEMA");
    if (!fs.empty()) StepArg(fs, 0, I.schema, 200);

    I.entities = 0;                          // рядки виду «#123=» — приблизна складність
    for (size_t i = 0; i + 1 < raw.size(); ++i)
        if (raw[i] == '#' && (i == 0 || raw[i - 1] == '\n' || raw[i - 1] == '\r')) ++I.entities;
    return true;
}


// ---- PDF ----
//
// Малює САМА Windows: Windows.Data.Pdf — вбудований компонент, а не обробник,
// зареєстрований кимось для розширення, тож запобіжник [2026-09-20] цілий.
//
// Заголовка windows.data.pdf.h у MinGW немає (і Windows SDK на машині збірки теж),
// тому інтерфейси оголошено тут вручну. IID IPdfDocumentStatics НЕ вгадано: його
// отримано від самої фабрики через IInspectable::GetIids() у пробі, а порядок
// методів перевірено живими викликами на справжньому PDF.
//
// ⚠ Робимо все на ОКРЕМОМУ потоці з апартаментом MTA. Причина не в швидкості:
// у STA (а UI-потік саме такий) асинхронна операція WinRT не завершується, поки
// потік не прокачує чергу повідомлень, — перевірено, без прокачування LoadFrom-
// StreamAsync стабільно віддає E_FAIL. Прокачувати чергу всередині обробника
// повідомлення означало б реентрантність UI, а це гірше за окремий потік.

struct LhAsyncOp : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void** result) = 0;
};
struct LhAsyncAct : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults() = 0;
};
struct LhPdfPage;
struct LhPdfDocStatics : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileAsync(void*, LhAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileWithPasswordAsync(void*, HSTRING, LhAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamAsync(void*, LhAsyncOp**) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamWithPasswordAsync(void*, HSTRING, LhAsyncOp**) = 0;
};
struct LhPdfDoc : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetPage(UINT32, LhPdfPage**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PageCount(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPasswordProtected(boolean*) = 0;
};
struct LhPdfSize { FLOAT W, H; };
struct LhPdfPage : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RenderToStreamAsync(void*, LhAsyncAct**) = 0;
    virtual HRESULT STDMETHODCALLTYPE RenderWithOptionsToStreamAsync(void*, void*, LhAsyncAct**) = 0;
    virtual HRESULT STDMETHODCALLTYPE PreparePageAsync(LhAsyncAct**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Index(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(LhPdfSize*) = 0;
};

const GUID kIID_IAsyncInfoLh          = { 0x00000036, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
const GUID kIID_IPdfDocumentStatics   = { 0x433A0B5F, 0xC007, 0x4788, { 0x90, 0xF2, 0x08, 0x14, 0x3D, 0x92, 0x25, 0x99 } };
const GUID kIID_IRandomAccessStreamLh = { 0x905A0FE1, 0xBC53, 0x11DF, { 0x8C, 0x49, 0x00, 0x1E, 0x4F, 0xC6, 0x86, 0xDA } };

template <class T> HRESULT PdfAwait(T* op, int ms)
{
    IAsyncInfo* info = nullptr;
    if (FAILED(op->QueryInterface(kIID_IAsyncInfoLh, (void**)&info)) || !info) return E_NOINTERFACE;
    HRESULT hr = E_FAIL;
    for (int i = 0; i < ms / 5; ++i) {
        AsyncStatus st = Started;
        if (FAILED(info->get_Status(&st))) break;
        if (st == Completed) { hr = S_OK; break; }
        if (st == Error)    { info->get_ErrorCode(&hr); if (SUCCEEDED(hr)) hr = E_FAIL; break; }
        if (st == Canceled) { hr = E_ABORT; break; }
        Sleep(5);
    }
    info->Release();
    return hr;
}

// Сесія живе, поки відкритий перегляд PDF: документ лишається завантаженим на
// тому ж потоці, і гортання коштує лише рендеру сторінки. Переоткривати файл на
// кожен гортак було б помітно повільно на 44-сторінковій інструкції.
//
// Належить обом сторонам через лічильник: якщо UI не дочекався відповіді й пішов,
// потік доробить і звільнить сесію сам — інакше він писав би в чужу пам'ять.
struct PdfSession {
    wchar_t path[MAX_PATH];
    HANDLE  evRequest;          // UI -> потік: намалюй сторінку wantPage (або виходь)
    HANDLE  evDone;             // потік -> UI: png готовий
    volatile LONG quit;
    volatile LONG wantPage;
    std::vector<BYTE> png;      // читається UI лише після evDone
    UINT32  pages;
    bool    ok;
    volatile LONG refs;
};

PdfSession* g_pdf = nullptr;
UINT32 g_peekPdfPage = 0;       // 0-based, показана зараз
UINT32 g_peekPdfPages = 0;

void PdfSessionRelease(PdfSession* s)
{
    if (InterlockedDecrement(&s->refs) != 0) return;
    if (s->evRequest) CloseHandle(s->evRequest);
    if (s->evDone) CloseHandle(s->evDone);
    delete s;
}

// Малює одну сторінку у png. Викликається ЛИШЕ з потоку сесії.
bool PdfRenderPage(LhPdfDoc* doc, UINT32 index, HSTRING clsMem, std::vector<BYTE>& png)
{
    png.clear();
    LhPdfPage* page = nullptr;
    IInspectable* memInsp = nullptr;
    void* outRas = nullptr;
    LhAsyncAct* act = nullptr;
    IStream* outStm = nullptr;
    bool ok = false;

    if (SUCCEEDED(doc->GetPage(index, &page)) && page &&
        SUCCEEDED(RoActivateInstance(clsMem, &memInsp)) && memInsp &&
        SUCCEEDED(memInsp->QueryInterface(kIID_IRandomAccessStreamLh, &outRas)) && outRas &&
        SUCCEEDED(page->RenderToStreamAsync(outRas, &act)) && act &&
        SUCCEEDED(PdfAwait(act, 20000)) &&
        SUCCEEDED(CreateStreamOverRandomAccessStream((IUnknown*)outRas, IID_IStream, (void**)&outStm)) && outStm) {
        LARGE_INTEGER zero = {};
        outStm->Seek(zero, STREAM_SEEK_SET, nullptr);
        BYTE buf[65536];
        ULONG got = 0;
        while (SUCCEEDED(outStm->Read(buf, sizeof(buf), &got)) && got) {
            png.insert(png.end(), buf, buf + got);
            if (png.size() > 96u * 1024 * 1024) break;
        }
        ok = !png.empty();
    }
    if (outStm) outStm->Release();
    if (act) act->Release();
    if (outRas) ((IUnknown*)outRas)->Release();
    if (memInsp) memInsp->Release();
    if (page) page->Release();
    return ok;
}

DWORD WINAPI PdfWorker(LPVOID param)
{
    PdfSession* s = (PdfSession*)param;
    const HRESULT hrRo = RoInitialize(RO_INIT_MULTITHREADED);

    HSTRING clsDoc = nullptr, clsMem = nullptr;
    WindowsCreateString(L"Windows.Data.Pdf.PdfDocument", 28, &clsDoc);
    WindowsCreateString(L"Windows.Storage.Streams.InMemoryRandomAccessStream", 50, &clsMem);

    LhPdfDocStatics* statics = nullptr;
    IStream* file = nullptr;
    void* inRas = nullptr;
    LhAsyncOp* op = nullptr;
    LhPdfDoc* doc = nullptr;

    if (SUCCEEDED(RoGetActivationFactory(clsDoc, kIID_IPdfDocumentStatics, (void**)&statics)) && statics &&
        SUCCEEDED(SHCreateStreamOnFileEx(s->path, STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr, &file)) && file &&
        SUCCEEDED(CreateRandomAccessStreamOverStream(file, BSOS_DEFAULT, kIID_IRandomAccessStreamLh, &inRas)) && inRas &&
        SUCCEEDED(statics->LoadFromStreamAsync(inRas, &op)) && op &&
        SUCCEEDED(PdfAwait(op, 20000)) &&
        SUCCEEDED(op->GetResults((void**)&doc)) && doc) {
        doc->get_PageCount(&s->pages);
        s->ok = PdfRenderPage(doc, 0, clsMem, s->png);
    }
    SetEvent(s->evDone);

    while (doc && !s->quit) {
        if (WaitForSingleObject(s->evRequest, INFINITE) != WAIT_OBJECT_0) break;
        if (s->quit) break;
        const UINT32 want = (UINT32)s->wantPage;
        s->ok = (want < s->pages) && PdfRenderPage(doc, want, clsMem, s->png);
        SetEvent(s->evDone);
    }

    if (doc) doc->Release();
    if (op) op->Release();
    if (inRas) ((IUnknown*)inRas)->Release();
    if (file) file->Release();
    if (statics) statics->Release();
    WindowsDeleteString(clsMem);
    WindowsDeleteString(clsDoc);
    if (SUCCEEDED(hrRo)) RoUninitialize();
    PdfSessionRelease(s);
    return 0;
}

// Перетворює вже готовий png сесії на наш бітмап.
bool PdfTakeBitmap(PdfSession* s)
{
    if (!s->ok || s->png.empty()) return false;
    delete g_peekScaled; g_peekScaled = nullptr;
    delete g_peekImg;    g_peekImg = nullptr;
    if (g_peekImgStream) { g_peekImgStream->Release(); g_peekImgStream = nullptr; }

    g_peekImgStream = SHCreateMemStream(s->png.data(), (UINT)s->png.size());
    Gdiplus::Bitmap* bmp = g_peekImgStream ? Gdiplus::Bitmap::FromStream(g_peekImgStream, FALSE) : nullptr;
    if (bmp && (bmp->GetLastStatus() != Gdiplus::Ok || !bmp->GetWidth() || !bmp->GetHeight())) {
        delete bmp;
        bmp = nullptr;
    }
    if (!bmp) {
        if (g_peekImgStream) { g_peekImgStream->Release(); g_peekImgStream = nullptr; }
        return false;
    }
    g_peekImg = bmp;
    g_peekInfo.imgW = (int)bmp->GetWidth();
    g_peekInfo.imgH = (int)bmp->GetHeight();
    return true;
}

void PdfSessionClose()
{
    if (!g_pdf) return;
    InterlockedExchange(&g_pdf->quit, 1);
    SetEvent(g_pdf->evRequest);      // розбудити потік, щоб він побачив прапорець
    PdfSessionRelease(g_pdf);
    g_pdf = nullptr;
    g_peekPdfPage = 0;
    g_peekPdfPages = 0;
}

bool PeekLoadPdf(const wchar_t* path)
{
    PdfSessionClose();
    PdfSession* s = new PdfSession();
    lstrcpynW(s->path, path, MAX_PATH);
    s->evRequest = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s->evDone    = CreateEventW(nullptr, TRUE,  FALSE, nullptr);
    s->quit = 0;
    s->wantPage = 0;
    s->pages = 0;
    s->ok = false;
    s->refs = 2;
    if (!s->evRequest || !s->evDone) { s->refs = 1; PdfSessionRelease(s); return false; }

    HANDLE th = CreateThread(nullptr, 0, PdfWorker, s, 0, nullptr);
    if (!th) { s->refs = 1; PdfSessionRelease(s); return false; }
    CloseHandle(th);

    if (WaitForSingleObject(s->evDone, 25000) != WAIT_OBJECT_0 || !PdfTakeBitmap(s)) {
        InterlockedExchange(&s->quit, 1);
        SetEvent(s->evRequest);
        PdfSessionRelease(s);
        return false;
    }
    g_pdf = s;
    g_peekPdfPage = 0;
    g_peekPdfPages = s->pages;
    return true;
}

// Гортання. Повертає true, якщо сторінка справді змінилась і треба перемалювати.
bool PeekPdfGoto(int page)
{
    if (!g_pdf || g_peekPdfPages < 2) return false;
    if (page < 0 || (UINT32)page >= g_peekPdfPages || (UINT32)page == g_peekPdfPage) return false;
    InterlockedExchange(&g_pdf->wantPage, page);
    ResetEvent(g_pdf->evDone);
    SetEvent(g_pdf->evRequest);
    if (WaitForSingleObject(g_pdf->evDone, 20000) != WAIT_OBJECT_0) return false;
    if (!PdfTakeBitmap(g_pdf)) return false;
    g_peekPdfPage = (UINT32)page;
    g_peekZoom = 1.0f;                 // нова сторінка — знову «вписано»
    g_peekPanX = g_peekPanY = 0;
    swprintf(g_peekInfo.subtitle, 320, S(Str::PeekFmtPdfPage), g_peekInfo.imgW, g_peekInfo.imgH,
             g_peekPdfPage + 1, g_peekPdfPages, g_peekInfo.size);
    return true;
}

// ---- завантаження елемента ----

void PeekReset()
{
    PdfSessionClose();
    if (g_peekWnd) KillTimer(g_peekWnd, TIMER_PEEK_ANIM);
    delete g_peekScaled; g_peekScaled = nullptr;
    delete g_peekImg;    g_peekImg = nullptr;
    if (g_peekImgStream) { g_peekImgStream->Release(); g_peekImgStream = nullptr; }
    g_peekFrames = 1;
    g_peekFrame  = 0;
    g_peekDelays.clear();
    if (g_peekIconBig)   { DestroyIcon(g_peekIconBig);   g_peekIconBig = nullptr; }
    if (g_peekIconSmall) { DestroyIcon(g_peekIconSmall); g_peekIconSmall = nullptr; }
    g_peekKind = PeekKind::None;
    g_peekZoom = 1.0f;
    g_peekPanX = g_peekPanY = 0;
    g_peekPanning = false;
    ZeroMemory(&g_peekInfo, sizeof(g_peekInfo));
}

HICON SysIconByIndex(int shil, int index)
{
    HIMAGELIST il = nullptr;
    if (FAILED(SHGetImageList(shil, kIID_IImageList, (void**)&il)) || !il) return nullptr;
    HICON ico = ImageList_GetIcon(il, index, ILD_TRANSPARENT);
    ((IUnknown*)il)->Release();
    return ico;
}

void PeekLoad(const wchar_t* path)
{
    PeekReset();
    lstrcpynW(g_peekPath, path, MAX_PATH);
    PeekInfo& I = g_peekInfo;

    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    const bool haveAttr = GetFileAttributesExW(path, GetFileExInfoStandard, &fa) != FALSE;
    I.isDir = haveAttr && (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    I.items = -1;
    lstrcpynW(I.name, PathFindFileNameW(path), MAX_PATH);
    lstrcpynW(I.folder, path, MAX_PATH);
    PathRemoveFileSpecW(I.folder);
    if (haveAttr) {
        FormatFileTime(fa.ftLastWriteTime, I.modified, 64);
        FormatFileTime(fa.ftCreationTime,  I.created,  64);
        if (!I.isDir) {
            const LONGLONG bytes = ((LONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            StrFormatByteSizeW(bytes, I.size, 64);
        }
    }

    // Тип і значки — за розширенням, без обробників конкретного файлу (див. шапку розділу)
    SHFILEINFOW sfi = {};
    if (SHGetFileInfoW(path, I.isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME | SHGFI_SYSICONINDEX)) {
        lstrcpynW(I.type, sfi.szTypeName, 128);
        g_peekIconBig   = SysIconByIndex(SHIL_JUMBO, sfi.iIcon);
        g_peekIconSmall = SysIconByIndex(SHIL_SMALL, sfi.iIcon);
    }

    if (I.isDir) {
        // Скільки всередині — лише верхній рівень і зі стелею: мережеві теки й
        // теки на сотні тисяч файлів не мають морозити перегляд.
        wchar_t pattern[MAX_PATH + 4] = {};
        swprintf(pattern, MAX_PATH + 4, L"%s\\*", path);
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            I.items = 0;
            do {
                if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
                if (++I.items >= 9999) { I.itemsMore = true; break; }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        wchar_t items[64] = {};
        if (I.items >= 0) swprintf(items, 64, S(Str::PeekFmtItems), I.items, I.itemsMore ? L"+" : L"");
        if (items[0]) swprintf(I.subtitle, 320, S(Str::PeekFmtTwo), I.type, items);
        else          lstrcpynW(I.subtitle, I.type, 320);
        g_peekKind = PeekKind::Card;
        return;
    }

    const wchar_t* ext = PathFindExtensionW(path);
    // Ярлик — перевіряємо ПЕРШИМ: .url ini-подібний, тобто інакше пройшов би як текст.
    if (PeekReadShortcut(path, ext, I.target, 1024)) {
        swprintf(I.subtitle, 320, S(Str::PeekFmtTwo), I.type, I.size);
        g_peekKind = PeekKind::Card;
        return;
    }
    if (lstrcmpiW(ext, L".pdf") == 0 && PeekLoadPdf(path)) {
        if (g_peekPdfPages > 1)
            swprintf(I.subtitle, 320, S(Str::PeekFmtPdfPage), I.imgW, I.imgH, 1u, g_peekPdfPages, I.size);
        else
            swprintf(I.subtitle, 320, S(Str::PeekFmtPdf), I.imgW, I.imgH, g_peekPdfPages, I.size);
        g_peekKind = PeekKind::Image;
        return;
    }
    if (IsStepExt(ext) && PeekLoadStep(path, I)) {
        swprintf(I.subtitle, 320, S(Str::PeekFmtTwo), I.type, I.size);
        g_peekKind = PeekKind::Card;
        return;
    }
    if (IsDocxExt(ext) && PeekLoadDocx(path)) {
        swprintf(I.subtitle, 320, S(Str::PeekFmtThree), I.type, I.size, S(Str::PeekDocxText));
        g_peekKind = PeekKind::Text;
        return;
    }
    if (IsVideoExt(ext)) {
        wchar_t dur[32] = {};
        if (PeekLoadVideo(path, dur, 32)) {
            swprintf(I.subtitle, 320, S(Str::PeekFmtVideo), I.imgW, I.imgH,
                     dur[0] ? dur : L"?", I.size);
            g_peekKind = PeekKind::Image;
            return;
        }
    }
    if (lstrcmpiW(ext, L".stl") == 0) {
        float dims[3] = {};
        unsigned tri = 0;
        if (PeekLoadStl(path, dims, tri)) {
            swprintf(I.subtitle, 320, S(Str::PeekFmtStl), dims[0], dims[1], dims[2], tri, I.size);
            g_peekKind = PeekKind::Image;
            return;
        }
    }
    g_peekSvgAsCode = false;
    if (lstrcmpiW(ext, L".svg") == 0) {
        if (PeekLoadSvg(path)) {
            if (g_peekSvgNote != Str::Empty)
                swprintf(I.subtitle, 320, S(Str::PeekFmtImageNote), I.docW, I.docH, I.size, S(g_peekSvgNote));
            else
                swprintf(I.subtitle, 320, S(Str::PeekFmtImage), I.docW, I.docH, I.size);
            g_peekKind = PeekKind::Image;
            return;
        }
        g_peekSvgAsCode = true;   // не змогли намалювати чесно — далі покажемо розмітку
    }
    if (IsImageExt(ext) && PeekLoadImage(path)) {
        if (g_peekFrames > 1)
            swprintf(I.subtitle, 320, S(Str::PeekFmtImageAnim), I.imgW, I.imgH, g_peekFrames, I.size);
        else
            swprintf(I.subtitle, 320, S(Str::PeekFmtImage), I.imgW, I.imgH, I.size);
        g_peekKind = PeekKind::Image;
        return;
    }
    // Відоме текстове розширення — текст; невідоме — лише якщо вміст на це схожий.
    if ((IsTextExt(ext) || (!IsImageExt(ext) && SniffText(path))) && PeekLoadText(path, ext)) {
        if (g_peekSvgAsCode)
            swprintf(I.subtitle, 320, S(Str::PeekFmtThree), I.type, I.size, S(Str::PeekSvgAsCode));
        else if (g_peekJsonFormatted)
            swprintf(I.subtitle, 320, S(Str::PeekFmtThree), I.type, I.size, S(Str::PeekReformatted));
        else
            swprintf(I.subtitle, 320, S(Str::PeekFmtTwo), I.type, I.size);
        g_peekKind = PeekKind::Text;
        return;
    }
    swprintf(I.subtitle, 320, S(Str::PeekFmtTwo), I.type, I.size);
    g_peekKind = PeekKind::Card;
}

// ---- вікно перегляду: геометрія, тема, малювання ----

// Скільки рядків намалює картка. Потрібно й для малювання, і ЩОБ РОЗМІР ВІКНА
// збігався з вмістом: у STEP рядків удвічі більше, ніж у звичайного файлу.
int PeekCardRows()
{
    int n = 2;                                     // тип + розмір/елементи
    if (g_peekInfo.target[0])   ++n;
    if (g_peekInfo.author[0])   ++n;
    if (g_peekInfo.org[0])      ++n;
    if (g_peekInfo.schema[0])   ++n;
    if (g_peekInfo.entities)    ++n;
    if (g_peekInfo.modified[0]) ++n;
    if (g_peekInfo.created[0])  ++n;
    if (g_peekInfo.folder[0])   ++n;
    return n;
}

RECT PeekCloseRect(const RECT& rc)
{
    const int s = PeekPx(kPeekHead);
    return { rc.right - s, rc.top, rc.right, rc.top + s };
}

// Стрілки гортання сторінок — ліворуч від хрестика й лише коли сторінок більше однієї.
bool PeekHasPager() { return g_peekPdfPages > 1; }

RECT PeekPrevRect(const RECT& rc)
{
    const int s = PeekPx(kPeekHead);
    return { rc.right - s * 3, rc.top, rc.right - s * 2, rc.top + s };
}

RECT PeekNextRect(const RECT& rc)
{
    const int s = PeekPx(kPeekHead);
    return { rc.right - s * 2, rc.top, rc.right - s, rc.top + s };
}

RECT PeekContentRect(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    rc.top += PeekPx(kPeekHead);
    return rc;
}

void PeekApplyTheme()
{
    if (!g_peekWnd) return;
    g_peekDark = ComputeDark();
    const BOOL dark = g_peekDark ? TRUE : FALSE;
    DwmSetWindowAttribute(g_peekWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const COLORREF border = g_peekDark ? kDkBorder : RGB(200, 200, 200);
    DwmSetWindowAttribute(g_peekWnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
    // Смуга прокрутки поля: як і трекбари в головному вікні, comctl32 тримає власний
    // кеш зображення й на самий лише SetWindowTheme не реагує — будить його WM_THEMECHANGED.
    SetWindowTheme(g_peekEdit, g_peekDark ? L"DarkMode_Explorer" : nullptr, nullptr);
    SendMessageW(g_peekEdit, WM_THEMECHANGED, 0, 0);
    // RichEdit малює фон сам — колір тексту приходить з таблиці кольорів RTF.
    SendMessageW(g_peekEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)(g_peekDark ? kDkBg : RGB(255, 255, 255)));
    InvalidateRect(g_peekWnd, nullptr, TRUE);
    RedrawWindow(g_peekEdit, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
}

void PeekLayout(HWND hwnd)
{
    RECT c = PeekContentRect(hwnd);
    if (g_peekKind == PeekKind::Text) {
        // Поле трохи менше за область вмісту: текст не притискається до країв
        c.top += PeekPx(6);
        SetWindowPos(g_peekEdit, nullptr, c.left, c.top, c.right - c.left, c.bottom - c.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RECT inner = { PeekPx(14), PeekPx(6), (c.right - c.left) - PeekPx(10), (c.bottom - c.top) - PeekPx(6) };
        SendMessageW(g_peekEdit, EM_SETRECT, 0, (LPARAM)&inner);
    } else {
        ShowWindow(g_peekEdit, SW_HIDE);
    }
    delete g_peekScaled;
    g_peekScaled = nullptr;   // під новий розмір перерахується при малюванні
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Скільки місця дати вікну на моніторі, де стоїть Провідник.
void PeekWorkArea(RECT& work)
{
    HMONITOR mon = MonitorFromWindow(g_peekRoot ? g_peekRoot : g_mainWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (mon && GetMonitorInfoW(mon, &mi)) work = mi.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
}

// Показати (або переставити під новий вміст): розмір від вмісту, по центру
// монітора Провідника, без активації.
void PeekShow()
{
    RECT work;
    PeekWorkArea(work);
    const int availW = (work.right - work.left) * 72 / 100;
    const int availH = (work.bottom - work.top) * 80 / 100;
    const int head = PeekPx(kPeekHead);
    int w = PeekPx(720), h = PeekPx(560);
    if (g_peekKind == PeekKind::Image && g_peekImg) {
        double scale = 1.0;
        if (g_peekInfo.imgW > availW) scale = (double)availW / g_peekInfo.imgW;
        if (g_peekInfo.imgH * scale > availH - head) scale = (double)(availH - head) / g_peekInfo.imgH;
        w = (int)(g_peekInfo.imgW * scale + 0.5);
        h = (int)(g_peekInfo.imgH * scale + 0.5) + head;
    } else if (g_peekKind == PeekKind::Card) {
        const int rows = PeekCardRows() * PeekPx(24);
        w = PeekPx(580);
        h = head + PeekPx(56) + (rows > PeekPx(96) ? rows : PeekPx(96));
    }
    if (w < PeekPx(kPeekMinW)) w = PeekPx(kPeekMinW);
    if (h < PeekPx(kPeekMinH)) h = PeekPx(kPeekMinH);
    if (w > availW) w = availW;
    if (h > availH) h = availH;
    const int x = work.left + ((work.right - work.left) - w) / 2;
    const int y = work.top  + ((work.bottom - work.top) - h) / 2;

    PeekApplyTheme();
    SetWindowPos(g_peekWnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    PeekLayout(g_peekWnd);
}

void PeekClose()
{
    if (g_peekWnd) {
        KillTimer(g_peekWnd, TIMER_PEEK_FOLLOW);
        ShowWindow(g_peekWnd, SW_HIDE);
    }
    g_peekShown = false;
    g_peekRoot  = nullptr;
    g_peekView  = nullptr;
    if (g_peekSv) { g_peekSv->Release(); g_peekSv = nullptr; }
    if (g_peekEdit) SetWindowTextW(g_peekEdit, L"");
    PeekReset();
    g_peekPath[0] = 0;
}

// Пробіл у списку файлів (з хука). view — SHELLDLL_DefView, де стоїть фокус.
void PeekToggle(HWND view)
{
    if (g_peekShown) { PeekClose(); return; }
    if (!g_peekWnd || !view || !IsWindow(view)) return;

    IShellView* sv = PeekFindShellView(view);
    wchar_t path[MAX_PATH] = {};
    if (!sv || !PeekReadSelection(sv, path, MAX_PATH)) {
        // Нічого показувати — пробіл повертається Провіднику (виділити елемент у фокусі тощо)
        if (sv) sv->Release();
        ReinjectSpace();
        return;
    }
    g_peekSv   = sv;
    g_peekView = view;
    g_peekRoot = GetAncestor(view, GA_ROOT);
    PeekLoad(path);
    PeekShow();
    g_peekShown = true;
    SetTimer(g_peekWnd, TIMER_PEEK_FOLLOW, 150, nullptr);
    if (g_peekFrames > 1) SetTimer(g_peekWnd, TIMER_PEEK_ANIM, g_peekDelays[0], nullptr);
}

// Раз на 150 мс: Провідник ще той самий і в фокусі? виділення те саме?
void PeekFollowTick()
{
    if (!g_peekShown) return;
    const HWND fg = GetForegroundWindow();
    if (!g_peekSv || !IsWindow(g_peekView) || (fg != g_peekRoot && fg != g_peekWnd)) { PeekClose(); return; }
    // Вкладки Провідника (Windows 11) живуть в одному вікні, тож перевірки вікна мало:
    // перемикання вкладки лишає наш вигляд живим, але показане більше не те, що виділено.
    // Питаємо саме НАШЕ вікно: глобальний фокус тут не годиться — він міг піти будь-куди.
    if (const HWND now = ShellListIn(g_peekRoot))
        if (now != g_peekView) { PeekClose(); return; }
    wchar_t path[MAX_PATH] = {};
    if (!PeekReadSelection(g_peekSv, path, MAX_PATH)) { PeekClose(); return; }   // виділення зникло або вигляд змінився
    if (lstrcmpiW(path, g_peekPath) != 0) {
        PeekLoad(path);
        PeekShow();
        KillTimer(g_peekWnd, TIMER_PEEK_ANIM);
        if (g_peekFrames > 1) SetTimer(g_peekWnd, TIMER_PEEK_ANIM, g_peekDelays[0], nullptr);
    }
}

// Активне вікно змінилось (WinEvent на головному потоці): перегляд живе лише
// поки активний той самий Провідник.
void PeekOnForeground()
{
    if (g_peekShown && GetForegroundWindow() != g_peekRoot) PeekClose();
}

// Куди саме лягає зображення. Одна функція і для малювання, і для миші — інакше
// зум із панорамуванням неминуче розійдуться між тим, що видно, і тим, що клікаєш.
double PeekFitScale(const RECT& content)
{
    const int cw = content.right - content.left, ch = content.bottom - content.top;
    if (!g_peekInfo.imgW || !g_peekInfo.imgH || cw <= 0 || ch <= 0) return 1.0;
    double fit = 1.0;
    if (g_peekInfo.imgW > cw) fit = (double)cw / g_peekInfo.imgW;
    if (g_peekInfo.imgH * fit > ch) fit = (double)ch / g_peekInfo.imgH;
    return fit;
}

void PeekClampPan(const RECT& content, int dw, int dh)
{
    const int cw = content.right - content.left, ch = content.bottom - content.top;
    const int maxX = (dw > cw) ? (dw - cw) / 2 : 0;
    const int maxY = (dh > ch) ? (dh - ch) / 2 : 0;
    if (g_peekPanX >  maxX) g_peekPanX =  maxX;
    if (g_peekPanX < -maxX) g_peekPanX = -maxX;
    if (g_peekPanY >  maxY) g_peekPanY =  maxY;
    if (g_peekPanY < -maxY) g_peekPanY = -maxY;
}

RECT PeekImageRect(const RECT& content)
{
    const double scale = PeekFitScale(content) * g_peekZoom;
    int dw = (int)(g_peekInfo.imgW * scale + 0.5), dh = (int)(g_peekInfo.imgH * scale + 0.5);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    PeekClampPan(content, dw, dh);
    const int cw = content.right - content.left, ch = content.bottom - content.top;
    const int x = content.left + (cw - dw) / 2 + g_peekPanX;
    const int y = content.top + (ch - dh) / 2 + g_peekPanY;
    RECT r = { x, y, x + dw, y + dh };
    return r;
}

// Зум навколо курсора: точка під ним має лишитись на місці, інакше
// «наблизити оце» перетворюється на «наблизити й шукати, куди воно поїхало».
void PeekZoomAt(HWND hwnd, POINT cur, bool in)
{
    if (g_peekKind != PeekKind::Image || !g_peekImg) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    const RECT c = PeekContentRect(hwnd);
    const double fit = PeekFitScale(c);
    const float oldZoom = g_peekZoom;
    float z = in ? g_peekZoom * 1.25f : g_peekZoom / 1.25f;
    if (z < 1.0f) z = 1.0f;                                  // менше «вписаного» не зменшуємо
    const double maxDim = 20000.0;                           // стеля, щоб GDI+ не вдавився
    const double longSide = (g_peekInfo.imgW > g_peekInfo.imgH ? g_peekInfo.imgW : g_peekInfo.imgH) * fit;
    if (longSide > 0 && longSide * z > maxDim) z = (float)(maxDim / longSide);
    if (z == oldZoom) return;

    const RECT before = PeekImageRect(c);
    const double sOld = fit * oldZoom;
    const double imgX = (before.right > before.left) ? (cur.x - before.left) / sOld : 0.0;
    const double imgY = (before.bottom > before.top) ? (cur.y - before.top) / sOld : 0.0;

    g_peekZoom = z;
    const double sNew = fit * z;
    const int dw = (int)(g_peekInfo.imgW * sNew + 0.5), dh = (int)(g_peekInfo.imgH * sNew + 0.5);
    const int cw = c.right - c.left, chh = c.bottom - c.top;
    g_peekPanX = (int)(cur.x - imgX * sNew - c.left - (cw - dw) / 2.0 + 0.5);
    g_peekPanY = (int)(cur.y - imgY * sNew - c.top - (chh - dh) / 2.0 + 0.5);
    PeekClampPan(c, dw, dh);
    delete g_peekScaled;
    g_peekScaled = nullptr;
    InvalidateRect(hwnd, nullptr, FALSE);
}

void PeekZoomReset(HWND hwnd)
{
    if (g_peekZoom == 1.0f && !g_peekPanX && !g_peekPanY) return;
    g_peekZoom = 1.0f;
    g_peekPanX = g_peekPanY = 0;
    delete g_peekScaled;
    g_peekScaled = nullptr;
    InvalidateRect(hwnd, nullptr, FALSE);
}

void PeekPaintArrow(HDC dc, const RECT& r, COLORREF fg, COLORREF dim, bool left, bool hot, bool enabled)
{
    if (hot && enabled) {
        HBRUSH b = CreateSolidBrush(g_peekDark ? RGB(64, 64, 64) : RGB(232, 232, 232));
        FillRect(dc, &r, b);
        DeleteObject(b);
    }
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const COLORREF c = enabled ? fg : dim;
    Gdiplus::Pen pen(Gdiplus::Color(enabled ? 255 : 110, GetRValue(c), GetGValue(c), GetBValue(c)),
                     (Gdiplus::REAL)PeekPx(1) * 1.4f);
    const float cx = (r.left + r.right) / 2.0f, cy = (r.top + r.bottom) / 2.0f;
    const float dx = PeekPx(4) * 1.0f, dy = PeekPx(6) * 1.0f;
    if (left) {
        g.DrawLine(&pen, cx + dx / 2, cy - dy, cx - dx / 2, cy);
        g.DrawLine(&pen, cx - dx / 2, cy, cx + dx / 2, cy + dy);
    } else {
        g.DrawLine(&pen, cx - dx / 2, cy - dy, cx + dx / 2, cy);
        g.DrawLine(&pen, cx + dx / 2, cy, cx - dx / 2, cy + dy);
    }
}

void PeekPaintClose(HDC dc, const RECT& r, COLORREF fg)
{
    if (g_peekCloseHot) {
        HBRUSH b = CreateSolidBrush(RGB(196, 43, 28));
        FillRect(dc, &r, b);
        DeleteObject(b);
        fg = RGB(255, 255, 255);
    }
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(fg), GetGValue(fg), GetBValue(fg)), (Gdiplus::REAL)PeekPx(1) * 1.2f);
    const float cx = (r.left + r.right) / 2.0f, cy = (r.top + r.bottom) / 2.0f, d = PeekPx(5) * 1.0f;
    g.DrawLine(&pen, cx - d, cy - d, cx + d, cy + d);
    g.DrawLine(&pen, cx - d, cy + d, cx + d, cy - d);
}

void PeekPaint(HDC dc, const RECT& rc)
{
    const COLORREF bg     = g_peekDark ? kDkBg : RGB(255, 255, 255);
    const COLORREF text   = g_peekDark ? kDkText : GetSysColor(COLOR_WINDOWTEXT);
    const COLORREF gray   = g_peekDark ? kDkGray : GetSysColor(COLOR_GRAYTEXT);
    const COLORREF line   = g_peekDark ? kDkBorder : RGB(229, 229, 229);
    HBRUSH bgBrush = CreateSolidBrush(bg);
    FillRect(dc, &rc, bgBrush);
    DeleteObject(bgBrush);
    SetBkMode(dc, TRANSPARENT);

    // ---- смуга з назвою ----
    const int head = PeekPx(kPeekHead), pad = PeekPx(16);
    const RECT closeR = PeekCloseRect(rc);
    int x = rc.left + pad;
    if (g_peekIconSmall) {
        const int s = GetSystemMetrics(SM_CXSMICON);
        DrawIconEx(dc, x, rc.top + (head - s) / 2, g_peekIconSmall, s, s, 0, nullptr, DI_NORMAL);
        x += s + PeekPx(10);
    }
    const int textRight = (PeekHasPager() ? PeekPrevRect(rc).left : closeR.left) - PeekPx(8);
    RECT nameR = { x, rc.top + PeekPx(7), textRight, rc.top + PeekPx(7) + PeekPx(20) };
    HGDIOBJ old = SelectObject(dc, g_peekFontBold);
    SetTextColor(dc, text);
    DrawTextW(dc, g_peekInfo.name, -1, &nameR, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT subR = { x, nameR.bottom, textRight, rc.top + head - PeekPx(4) };
    SelectObject(dc, g_peekFont);
    SetTextColor(dc, gray);
    DrawTextW(dc, g_peekInfo.subtitle, -1, &subR, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (PeekHasPager()) {
        const RECT pr = PeekPrevRect(rc), nr = PeekNextRect(rc);
        PeekPaintArrow(dc, pr, text, gray, true,  g_peekPagerHot == 1, g_peekPdfPage > 0);
        PeekPaintArrow(dc, nr, text, gray, false, g_peekPagerHot == 2, g_peekPdfPage + 1 < g_peekPdfPages);
    }
    PeekPaintClose(dc, closeR, text);
    {
        RECT sep = { rc.left, rc.top + head - 1, rc.right, rc.top + head };
        HBRUSH b = CreateSolidBrush(line);
        FillRect(dc, &sep, b);
        DeleteObject(b);
    }

    // ---- вміст ----
    RECT c = rc;
    c.top += head;
    const int cw = c.right - c.left, ch = c.bottom - c.top;

    if (g_peekKind == PeekKind::Image && g_peekImg && cw > 0 && ch > 0) {
        RECT dst = PeekImageRect(c);
        const int dw = dst.right - dst.left, dh = dst.bottom - dst.top;
        Gdiplus::Graphics g(dc);
        // Збільшене зображення більше за область вмісту й інакше лізло б на шапку
        // з назвою файлу — обмежуємо малювання рівно областю вмісту.
        g.SetClip(Gdiplus::Rect(c.left, c.top, cw, ch));
        if (dw == g_peekInfo.imgW && dh == g_peekInfo.imgH) {
            g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.DrawImage(g_peekImg, dst.left, dst.top, dw, dh);
        } else if (dw > g_peekInfo.imgW) {
            // Збільшення малюємо НАПРЯМУ: кешований бітмап у 8× зайняв би сотні МБ.
            // Дрібні картинки при цьому лишаються різкими, як і має бути при зумі.
            g.SetInterpolationMode(dw > g_peekInfo.imgW * 3 ? Gdiplus::InterpolationModeNearestNeighbor
                                                            : Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.DrawImage(g_peekImg, dst.left, dst.top, dw, dh);
        } else {
            // Зменшена копія кешується: перемальовування (наведення на ✕) не має
            // щоразу масштабувати десятки мегапікселів.
            if (!g_peekScaled || (int)g_peekScaled->GetWidth() != dw || (int)g_peekScaled->GetHeight() != dh) {
                delete g_peekScaled;
                g_peekScaled = new Gdiplus::Bitmap(dw, dh, PixelFormat32bppPARGB);
                Gdiplus::Graphics sg(g_peekScaled);
                sg.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                sg.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                Gdiplus::ImageAttributes ia;
                ia.SetWrapMode(Gdiplus::WrapModeTileFlipXY);   // без напівпрозорої рамки по краю
                sg.DrawImage(g_peekImg, Gdiplus::Rect(0, 0, dw, dh), 0, 0, g_peekInfo.imgW, g_peekInfo.imgH,
                             Gdiplus::UnitPixel, &ia);
            }
            g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.DrawImage(g_peekScaled, dst.left, dst.top, dw, dh);
        }
    } else if (g_peekKind == PeekKind::Card) {
        const int icon = PeekPx(96);
        int ix = c.left + PeekPx(28), iy = c.top + PeekPx(28);
        if (g_peekIconBig) DrawIconEx(dc, ix, iy, g_peekIconBig, icon, icon, 0, nullptr, DI_NORMAL);
        const int lx = ix + icon + PeekPx(28), vx = lx + PeekPx(112), rowH = PeekPx(24);
        int y = iy + PeekPx(2);
        auto row = [&](Str label, const wchar_t* value) {
            if (!value || !*value) return;
            RECT lr = { lx, y, vx - PeekPx(8), y + rowH };
            RECT vr = { vx, y, c.right - PeekPx(20), y + rowH };
            SelectObject(dc, g_peekFont);
            SetTextColor(dc, gray);
            DrawTextW(dc, S(label), -1, &lr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            SetTextColor(dc, text);
            DrawTextW(dc, value, -1, &vr, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
            y += rowH;
        };
        wchar_t items[64] = {};
        if (g_peekInfo.isDir && g_peekInfo.items >= 0)
            swprintf(items, 64, L"%d%s", g_peekInfo.items, g_peekInfo.itemsMore ? L"+" : L"");
        wchar_t ents[32] = {};
        if (g_peekInfo.entities) swprintf(ents, 32, L"%u", g_peekInfo.entities);
        row(Str::PeekLblType, g_peekInfo.type);
        if (g_peekInfo.isDir) row(Str::PeekLblItems, items);
        else                  row(Str::PeekLblSize,  g_peekInfo.size);
        row(Str::PeekLblTarget,   g_peekInfo.target);
        row(Str::PeekLblAuthor,   g_peekInfo.author);
        row(Str::PeekLblOrg,      g_peekInfo.org);
        row(Str::PeekLblSchema,   g_peekInfo.schema);
        row(Str::PeekLblEntities, ents);
        row(Str::PeekLblModified, g_peekInfo.modified);
        row(Str::PeekLblCreated,  g_peekInfo.created2[0] ? g_peekInfo.created2 : g_peekInfo.created);
        row(Str::PeekLblWhere,    g_peekInfo.folder);
    }
    SelectObject(dc, old);
}

// Поле тексту не сміє брати фокус: вікно перегляду не активується, а клік у
// EDIT інакше потягнув би SetFocus і активацію. Прокрутка колесом лишається.
LRESULT CALLBACK PeekEditSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK PeekWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCCALCSIZE:
        // Уся площа — клієнтська: WS_THICKFRAME лишається заради тіні DWM,
        // округлених кутів Windows 11 і зміни розміру за край, а рамку й
        // заголовок малюємо самі.
        return 0;

    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        RECT rc;
        GetClientRect(hwnd, &rc);
        const int b = PeekPx(6);
        const bool l = pt.x < b, r = pt.x >= rc.right - b, t = pt.y < b, bt = pt.y >= rc.bottom - b;
        if (t && l) return HTTOPLEFT;
        if (t && r) return HTTOPRIGHT;
        if (bt && l) return HTBOTTOMLEFT;
        if (bt && r) return HTBOTTOMRIGHT;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
        if (t) return HTTOP;
        if (bt) return HTBOTTOM;
        const RECT closeR = PeekCloseRect(rc);
        if (PtInRect(&closeR, pt)) return HTCLIENT;
        if (PeekHasPager()) {
            const RECT pr = PeekPrevRect(rc), nr = PeekNextRect(rc);
            if (PtInRect(&pr, pt) || PtInRect(&nr, pt)) return HTCLIENT;
        }
        if (pt.y < PeekPx(kPeekHead)) return HTCAPTION;   // тягнути за смугу з назвою
        return HTCLIENT;
    }

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = PeekPx(kPeekMinW);
        mm->ptMinTrackSize.y = PeekPx(kPeekMinH);
        return 0;
    }

    case WM_SIZE:
        if (IsWindowVisible(hwnd)) PeekLayout(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        PeekPaint(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (g_peekPanning) {
            const POINT now = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            g_peekPanX += now.x - g_peekPanFrom.x;
            g_peekPanY += now.y - g_peekPanFrom.y;
            g_peekPanFrom = now;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        const RECT closeR = PeekCloseRect(rc);
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const bool hot = PtInRect(&closeR, pt) != FALSE;
        if (hot != g_peekCloseHot) { g_peekCloseHot = hot; InvalidateRect(hwnd, &closeR, FALSE); }
        int pager = 0;
        if (PeekHasPager()) {
            const RECT pr = PeekPrevRect(rc), nr = PeekNextRect(rc);
            if (PtInRect(&pr, pt)) pager = 1;
            else if (PtInRect(&nr, pt)) pager = 2;
        }
        if (pager != g_peekPagerHot) {
            g_peekPagerHot = pager;
            RECT head = rc;
            head.bottom = rc.top + PeekPx(kPeekHead);
            InvalidateRect(hwnd, &head, FALSE);
        }
        if (!g_peekTracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_peekTracking = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_peekTracking = false;
        if (g_peekCloseHot || g_peekPagerHot) {
            g_peekCloseHot = false;
            g_peekPagerHot = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    // Гортання коліщатком. Вікно не має фокуса, але Windows шле коліщатко вікну
    // під курсором — саме тому це працює, а клавіші лишаються Провіднику.
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        const bool ctrl = (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) != 0;
        POINT cur = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &cur);
        // У багатосторінковому PDF просте коліщатко гортає сторінки, а Ctrl масштабує;
        // усюди інакше коліщатко саме масштабує — гортати там нічого.
        if (PeekHasPager() && !ctrl) {
            const int to = (int)g_peekPdfPage + (delta < 0 ? 1 : -1);
            if (PeekPdfGoto(to)) InvalidateRect(hwnd, nullptr, FALSE);
        } else {
            PeekZoomAt(hwnd, cur, delta > 0);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (pt.y >= PeekPx(kPeekHead) && g_peekKind == PeekKind::Image && g_peekZoom > 1.0f) {
            g_peekPanning = true;
            g_peekPanFrom = pt;
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK:
        PeekZoomReset(hwnd);            // подвійний клік — знову вписати у вікно
        return 0;

    case WM_LBUTTONUP: {
        if (g_peekPanning) {
            g_peekPanning = false;
            ReleaseCapture();
            return 0;
        }
        RECT rc;
        GetClientRect(hwnd, &rc);
        const RECT closeR = PeekCloseRect(rc);
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (PtInRect(&closeR, pt)) { PeekClose(); return 0; }
        if (PeekHasPager()) {
            const RECT pr = PeekPrevRect(rc), nr = PeekNextRect(rc);
            int to = -1;
            if (PtInRect(&pr, pt)) to = (int)g_peekPdfPage - 1;
            else if (PtInRect(&nr, pt)) to = (int)g_peekPdfPage + 1;
            if (to >= 0 && PeekPdfGoto(to)) InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_KEYDOWN:   // страховка: якщо вікно все ж отримало фокус
        if (wp == VK_ESCAPE || wp == VK_SPACE) PeekClose();
        return 0;

    case WM_TIMER:
        if (wp == TIMER_PEEK_FOLLOW) PeekFollowTick();
        else if (wp == TIMER_PEEK_ANIM && g_peekImg && g_peekFrames > 1) {
            // Кадри мають РІЗНУ тривалість, тож таймер переставляється щокадру.
            g_peekFrame = (g_peekFrame + 1) % g_peekFrames;
            g_peekImg->SelectActiveFrame(&kFrameDimensionTime, (UINT)g_peekFrame);
            delete g_peekScaled;               // кеш був від попереднього кадру
            g_peekScaled = nullptr;
            SetTimer(hwnd, TIMER_PEEK_ANIM, g_peekDelays[g_peekFrame], nullptr);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_CTLCOLORSTATIC:   // EDIT лише для читання шле саме це
        if ((HWND)lp == g_peekEdit) {
            SetBkMode((HDC)wp, TRANSPARENT);
            SetBkColor((HDC)wp, g_peekDark ? kDkBg : RGB(255, 255, 255));
            SetTextColor((HDC)wp, g_peekDark ? kDkText : GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)(g_peekDark ? g_brDkBg : GetStockObject(WHITE_BRUSH));
        }
        break;

    case WM_CLOSE:
        PeekClose();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void PeekCreateWindow(HINSTANCE hInst)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = PeekWndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"lilhelpers_peek";
    wc.style         = CS_DBLCLKS;      // без цього подвійний клік не приходить
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);

    g_peekWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"lilhelpers_peek", kAppName,
                                WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
                                0, 0, PeekPx(kPeekMinW), PeekPx(kPeekMinH), nullptr, nullptr, hInst, nullptr);
    if (!g_peekWnd) return;
    {
        MARGINS m = { 0, 0, 0, 1 };   // ненульовий відступ = DWM малює тінь навколо
        DwmExtendFrameIntoClientArea(g_peekWnd, &m);
        const int round = 2;          // DWMWCP_ROUND
        DwmSetWindowAttribute(g_peekWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    }

    g_peekFont     = CreateUIFont(100, FW_NORMAL);
    g_peekFontBold = CreateUIFont(105, FW_SEMIBOLD);
    {
        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        LOGFONTW lf = ncm.lfMessageFont;
        lstrcpyW(lf.lfFaceName, L"Consolas");
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        g_peekFontMono = CreateFontIndirectW(&lf);
    }

    // RichEdit, а не EDIT: він уміє шрифти, кольори й відступи, тобто і Markdown,
    // і підсвітку коду. Бібліотеку вантажимо тут, бо клас реєструє саме вона.
    LoadLibraryW(L"Msftedit.dll");
    g_peekEdit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                                 WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                                 0, 0, 10, 10, g_peekWnd, nullptr, hInst, nullptr);
    SendMessageW(g_peekEdit, EM_SETLIMITTEXT, 0, 0);
    SendMessageW(g_peekEdit, EM_SETEVENTMASK, 0, 0);
    SendMessageW(g_peekEdit, EM_EXLIMITTEXT, 0, 64 * 1024 * 1024);
    SetWindowSubclass(g_peekEdit, PeekEditSubclass, 1, 0);
}

// Увімкнути/вимкнути: клавіатурний хук потрібен і тут, навіть якщо розкладка
// в запасному режимі чи вимкнена; знімаємо його лише коли нікому не потрібен.
void ApplyPeekFeature()
{
    if (!g_hookWnd) return;
    if (g_peekOn) {
        SendMessageW(g_hookWnd, HKW_INSTALL, 0, 0);
    } else {
        PeekClose();
        if (!g_kbHookCaps) SendMessageW(g_hookWnd, HKW_UNINSTALL, 0, 0);
    }
}

void ShowSettings(HWND hwnd)
{
    SendMessageW(g_checkbox, BM_SETCHECK,
                 AutostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

void UpdateModeHint()
{
    SetWindowTextW(g_modeHint, S(
        !g_layoutOn           ? Str::LayHintOff
        : g_mode == Mode::Hook ? Str::LayHintHook
                               : Str::LayHintHotkey));
}

// CAPS-12: перемалювати інтерфейс новою мовою. Вікно не перестворюється —
// позиції й розміри однакові для обох мов (див. вимогу до довжини перекладу),
// тож достатньо переписати підписи й оновити рядки стану.
void ApplyLanguage()
{
    for (int i = 0; i < g_locCtrlsN; ++i)
        SetWindowTextW(g_locCtrls[i].h, S(g_locCtrls[i].id));

    TCITEMW t = {};
    t.mask = TCIF_TEXT;
    for (int i = 0; i < kTabCount; ++i) {
        t.pszText = (LPWSTR)S(kTabTitles[i]);
        SendMessageW(g_tabs, TCM_SETITEMW, i, (LPARAM)&t);
    }

    UpdateAdvButtons();
    SetCursorValueLabels();
    UpdateModeHint();
    UpdateThemeStatus();
    UpdateUpdStatus();
    InvalidateRect(g_mainWnd, nullptr, TRUE);
}

// CAPS-9: режим і пропуск у remote мають сенс лише поки перемикання ввімкнено.
void SetLayoutControlsEnabled(HWND hwnd)
{
    EnableWindow(GetDlgItem(hwnd, IDC_MODE_HOOK),   g_layoutOn);
    EnableWindow(GetDlgItem(hwnd, IDC_MODE_HOTKEY), g_layoutOn);
    EnableWindow(g_passthroughCheckbox,             g_layoutOn);
}

// CAPS-9: увімкнути/вимкнути саме перемикання розкладок. Не чіпає автозапуск:
// програма може стартувати з Windows заради курсора чи дня/ночі, а Caps Lock
// лишатиметься звичайним.
void ApplyLayoutSwitch(HWND hwnd, bool on)
{
    if (on && !g_interceptionOn) {
        if (!StartInterception(g_mode)) {
            const Mode other = (g_mode == Mode::Hook) ? Mode::Hotkey : Mode::Hook;
            if (StartInterception(other)) {
                g_mode = other;
                SaveMode(other);
            } else {
                MessageBoxW(hwnd, S(Str::MsgHookFailed), kAppName, MB_ICONERROR | MB_OK);
                on = false;
            }
        }
    } else if (!on && g_interceptionOn) {
        StopInterception();
    }
    g_layoutOn = on;
    RegSaveInt(kRegLayoutSwitch, on ? 1 : 0);
    SendMessageW(g_layoutCheckbox, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    CheckRadioButton(hwnd, IDC_MODE_HOOK, IDC_MODE_HOTKEY,
                     g_mode == Mode::Hook ? IDC_MODE_HOOK : IDC_MODE_HOTKEY);
    SetLayoutControlsEnabled(hwnd);
    UpdateModeHint();
}

// Перемикання режиму наживо: знімаємо поточний перехоплювач і ставимо інший.
void ApplyMode(HWND hwnd, Mode mode)
{
    StopInterception();
    if (!StartInterception(mode)) {
        // не вийшло — вертаємось на те, що працювало
        if (StartInterception(g_mode)) {
            MessageBoxW(hwnd, S(Str::MsgModeUnavailable), kAppName, MB_ICONWARNING | MB_OK);
        } else {
            MessageBoxW(hwnd, S(Str::MsgHookFailed), kAppName, MB_ICONERROR | MB_OK);
        }
    } else {
        g_mode = mode;
        SaveMode(mode);
    }

    CheckRadioButton(hwnd, IDC_MODE_HOOK, IDC_MODE_HOTKEY,
                     g_mode == Mode::Hook ? IDC_MODE_HOOK : IDC_MODE_HOTKEY);
    UpdateModeHint();
}

// ===================== CAPS-20: редактор знімків =====================
// Три зони за макетом CAPS-19: ліворуч чим малюю, зверху властивості ВИБРАНОЇ
// позначки, праворуч сам знімок. Правило просте настільки, що його не треба
// запам'ятовувати, і кожна нова властивість одразу знає, куди їй.
//
// Позначка — не мазок у пікселях, а об'єкт зі списку (вимога 1 епіка). Через це
// Undo/Redo виходить сам собою: достатньо зберегти список перед зміною. Ціна —
// перемальовування всього поверх базового бітмапа на кожен WM_PAINT, але при
// десятках об'єктів це ніщо.
//
// Дочірніх контролів тут немає навмисно. Їх довелося б і фарбувати під тему
// (DarkMode_Explorer бреше на половині класів), і совати при кожній зміні
// розміру. Малюємо самі в один буфер, а клікабельні місця тримаємо списком
// прямокутників — це і є вся «система контролів».
//
// Етап 1 свідомо не має: захоплення екрана (CAPS-21), виходу в буфер і файл
// (CAPS-22), решти інструментів (CAPS-23..27) і тону (CAPS-28).

constexpr int kEdStrip   = 48;    // смуга властивостей
constexpr int kEdRail    = 52;    // панель інструментів
constexpr int kEdPanel   = 260;   // права панель
constexpr int kEdPanelLo = 30;    // вона ж згорнута
constexpr int kEdStatus  = 48;
constexpr int kEdMinW    = 880;
constexpr int kEdMinH    = 560;
constexpr int kEdUndoMax = 120;   // глибина скасування; знімок списку дешевий

struct EdTheme {
    COLORREF chrome, surface, border, text, text2, accent, accentBg, accentBd,
             canvas, btn, btnBd, dangerBg, dangerBd, dangerFg, hot;
};

EdTheme EdColors(bool dark)
{
    EdTheme t;
    if (dark) {
        t.chrome   = RGB(32, 32, 35);    t.surface  = RGB(43, 43, 46);
        t.border   = RGB(58, 58, 62);    t.text     = RGB(242, 242, 243);
        t.text2    = RGB(169, 169, 175); t.accent   = RGB(76, 194, 255);
        t.accentBg = RGB(16, 58, 82);    t.accentBd = RGB(47, 111, 146);
        t.canvas   = RGB(48, 48, 53);    t.btn      = RGB(53, 53, 58);
        t.btnBd    = RGB(69, 69, 74);    t.dangerBg = RGB(74, 32, 30);
        t.dangerBd = RGB(122, 60, 56);   t.dangerFg = RGB(255, 153, 145);
        t.hot      = RGB(64, 64, 70);
    } else {
        t.chrome   = RGB(243, 243, 243); t.surface  = RGB(255, 255, 255);
        t.border   = RGB(227, 227, 229); t.text     = RGB(27, 27, 31);
        t.text2    = RGB(93, 93, 99);    t.accent   = RGB(0, 95, 184);
        t.accentBg = RGB(234, 242, 251); t.accentBd = RGB(168, 207, 240);
        t.canvas   = RGB(86, 86, 92);    t.btn      = RGB(255, 255, 255);
        t.btnBd    = RGB(214, 214, 216); t.dangerBg = RGB(253, 243, 242);
        t.dangerBd = RGB(232, 180, 174); t.dangerFg = RGB(164, 38, 44);
        t.hot      = RGB(232, 232, 234);
    }
    return t;
}

// Палітра позначок однакова в обох темах: вона належить знімку, а не вікну.
const COLORREF kEdPalette[8] = {
    RGB(232, 17, 35), RGB(247, 99, 12), RGB(255, 212, 0), RGB(16, 124, 16),
    RGB(0, 120, 212), RGB(123, 63, 228), RGB(27, 27, 31), RGB(255, 255, 255)
};

enum class EdTool { Select, Rect };
enum class EdKind { Rect };

struct EdObj {
    EdKind   kind;
    int      x, y, w, h;      // у координатах ЗОБРАЖЕННЯ, не екрана
    COLORREF color;
    int      thick;           // товщина контуру в пікселях зображення
    int      alpha;           // 10..100 %
};

struct EdSnap {
    std::vector<EdObj> objs;
    int sel;
};

enum class EdHit { None, Canvas, Tool, Swatch, Opacity, Undo, Redo, Help,
                   Front, Back, Del, ZoomOut, ZoomIn, Fit, Panel };

struct EdRegion { RECT r; EdHit what; int idx; };

enum EdIco { IcoSelect, IcoRect, IcoUndo, IcoRedo, IcoHelp, IcoFront, IcoBack,
             IcoDel, IcoOpacity, IcoChevR, IcoChevL, IcoMinus, IcoPlus };

enum class EdDrag { None, New, Move, Resize, Pan, Slider };

HWND  g_edWnd = nullptr;
HFONT g_edFont = nullptr, g_edFontBold = nullptr, g_edFontSmall = nullptr;
int   g_edDpi = 96;
bool  g_edDark = false;
bool  g_edPanelOpen = true;

Gdiplus::Bitmap* g_edImg = nullptr;
int      g_edImgW = 0, g_edImgH = 0;
wchar_t  g_edSource[MAX_PATH] = {};

std::vector<EdObj> g_edObjs;
int      g_edSel = -1;
std::vector<EdSnap> g_edUndo, g_edRedo;

EdTool   g_edTool  = EdTool::Select;
COLORREF g_edColor = RGB(232, 17, 35);   // типовий колір нових позначок
int      g_edThick = 4;
int      g_edAlpha = 100;

float g_edZoom = 1.0f;                   // множник до «вписаного», як у перегляді
int   g_edPanX = 0, g_edPanY = 0;

std::vector<EdRegion> g_edRegions;
RECT  g_edRcStrip = {}, g_edRcRail = {}, g_edRcCanvas = {}, g_edRcPanel = {}, g_edRcStatus = {};
EdHit g_edHotWhat = EdHit::None;
int   g_edHotIdx  = -1;
bool  g_edTracking = false;

EdDrag g_edDrag = EdDrag::None;
int    g_edHandle = -1;
POINT  g_edDragFrom = {};
EdObj  g_edDragOrig = {};
EdObj  g_edNew = {};

int EdPx(int v) { return MulDiv(v, g_edDpi, 96); }

inline Gdiplus::Color EdC(COLORREF c, int a = 255)
{
    return Gdiplus::Color((BYTE)a, GetRValue(c), GetGValue(c), GetBValue(c));
}

inline int EdMin(int a, int b) { return a < b ? a : b; }

void EdRoundPath(Gdiplus::GraphicsPath& p, const RECT& r, float rad)
{
    const float x = (float)r.left, y = (float)r.top;
    const float w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
    p.Reset();
    if (w <= 0 || h <= 0) return;
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    if (rad < 0.6f) { p.AddRectangle(Gdiplus::RectF(x, y, w, h)); return; }
    const float d = rad * 2;
    p.AddArc(x, y, d, d, 180.0f, 90.0f);
    p.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    p.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    p.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}

void EdFillRound(Gdiplus::Graphics& g, const RECT& r, float rad,
                 const Gdiplus::Color* fill, const Gdiplus::Color* border, float bw = 1.0f)
{
    Gdiplus::GraphicsPath p;
    RECT rr = r;
    if (border) { rr.right -= 1; rr.bottom -= 1; }
    EdRoundPath(p, rr, rad);
    if (fill) { Gdiplus::SolidBrush b(*fill); g.FillPath(&b, &p); }
    if (border) { Gdiplus::Pen pen(*border, bw); g.DrawPath(&pen, &p); }
}

// Іконки малюються в сітці 20×20 і масштабуються у квадрат кнопки. Так одна
// правка форми діє на всі розміри й на будь-який DPI.
void EdIcon(Gdiplus::Graphics& g, int id, const RECT& box, Gdiplus::Color c, float sw = 1.7f)
{
    const int side = EdMin(box.right - box.left, box.bottom - box.top);
    if (side <= 0) return;
    const float s = side / 20.0f;
    const float ox = box.left + (box.right - box.left - side) / 2.0f;
    const float oy = box.top + (box.bottom - box.top - side) / 2.0f;

    Gdiplus::GraphicsState st = g.Save();
    g.TranslateTransform(ox, oy);
    g.ScaleTransform(s, s);

    Gdiplus::Pen pen(c, sw);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush br(c);

    switch (id) {
    case IcoSelect: {
        Gdiplus::PointF pts[4] = { { 5.0f, 3.4f }, { 15.2f, 9.6f }, { 10.8f, 10.7f }, { 8.9f, 15.0f } };
        g.DrawPolygon(&pen, pts, 4);
        break;
    }
    case IcoRect:
        g.DrawRectangle(&pen, 3.2f, 5.2f, 13.6f, 9.6f);
        break;
    case IcoUndo:
        g.DrawLine(&pen, 6.0f, 8.0f, 12.5f, 8.0f);
        g.DrawArc(&pen, 9.0f, 8.0f, 7.0f, 7.0f, -90.0f, 180.0f);
        g.DrawLine(&pen, 12.5f, 15.0f, 9.0f, 15.0f);
        g.DrawLine(&pen, 8.6f, 5.0f, 5.4f, 8.0f);
        g.DrawLine(&pen, 5.4f, 8.0f, 8.6f, 11.0f);
        break;
    case IcoRedo:
        g.DrawLine(&pen, 14.0f, 8.0f, 7.5f, 8.0f);
        g.DrawArc(&pen, 4.0f, 8.0f, 7.0f, 7.0f, 90.0f, 180.0f);
        g.DrawLine(&pen, 7.5f, 15.0f, 11.0f, 15.0f);
        g.DrawLine(&pen, 11.4f, 5.0f, 14.6f, 8.0f);
        g.DrawLine(&pen, 14.6f, 8.0f, 11.4f, 11.0f);
        break;
    case IcoHelp:
        g.DrawEllipse(&pen, 2.8f, 2.8f, 14.4f, 14.4f);
        g.DrawArc(&pen, 7.0f, 5.2f, 6.0f, 6.0f, 170.0f, 230.0f);
        g.DrawLine(&pen, 10.0f, 10.4f, 10.0f, 12.2f);
        g.FillEllipse(&br, 9.05f, 13.6f, 1.9f, 1.9f);
        break;
    case IcoFront:
        g.DrawRectangle(&pen, 3.2f, 3.2f, 9.6f, 9.6f);
        g.FillRectangle(&br, 7.6f, 7.6f, 9.2f, 9.2f);
        break;
    case IcoBack:
        g.FillRectangle(&br, 3.2f, 3.2f, 9.2f, 9.2f);
        g.DrawRectangle(&pen, 7.2f, 7.2f, 9.6f, 9.6f);
        break;
    case IcoDel:
        g.DrawLine(&pen, 4.2f, 5.8f, 15.8f, 5.8f);
        g.DrawLine(&pen, 8.0f, 5.8f, 8.0f, 4.2f);
        g.DrawLine(&pen, 8.0f, 4.2f, 12.0f, 4.2f);
        g.DrawLine(&pen, 12.0f, 4.2f, 12.0f, 5.8f);
        g.DrawLine(&pen, 5.9f, 5.8f, 6.7f, 16.2f);
        g.DrawLine(&pen, 14.1f, 5.8f, 13.3f, 16.2f);
        g.DrawLine(&pen, 6.7f, 16.2f, 13.3f, 16.2f);
        break;
    case IcoOpacity:
        g.DrawEllipse(&pen, 3.0f, 3.0f, 14.0f, 14.0f);
        g.FillPie(&br, 3.0f, 3.0f, 14.0f, 14.0f, -90.0f, 180.0f);
        break;
    case IcoChevR:
        g.DrawLine(&pen, 8.0f, 5.0f, 13.0f, 10.0f);
        g.DrawLine(&pen, 13.0f, 10.0f, 8.0f, 15.0f);
        break;
    case IcoChevL:
        g.DrawLine(&pen, 12.0f, 5.0f, 7.0f, 10.0f);
        g.DrawLine(&pen, 7.0f, 10.0f, 12.0f, 15.0f);
        break;
    case IcoMinus:
        g.DrawLine(&pen, 5.0f, 10.0f, 15.0f, 10.0f);
        break;
    case IcoPlus:
        g.DrawLine(&pen, 5.0f, 10.0f, 15.0f, 10.0f);
        g.DrawLine(&pen, 10.0f, 5.0f, 10.0f, 15.0f);
        break;
    default: break;
    }
    g.Restore(st);
}

void EdDrawText(HDC dc, const RECT& r, const wchar_t* s, HFONT f, COLORREF c, UINT flags)
{
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    RECT t = r;
    DrawTextW(dc, s, -1, &t, flags);
    SelectObject(dc, old);
}

int EdTextWidth(HDC dc, const wchar_t* s, HFONT f)
{
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

// ---- модель: знімки для скасування -------------------------------------

void EdPushUndo()
{
    EdSnap s;
    s.objs = g_edObjs;
    s.sel  = g_edSel;
    g_edUndo.push_back(s);
    if ((int)g_edUndo.size() > kEdUndoMax) g_edUndo.erase(g_edUndo.begin());
    g_edRedo.clear();
}

void EdApply(const EdSnap& s)
{
    g_edObjs = s.objs;
    g_edSel  = s.sel;
    if (g_edSel >= (int)g_edObjs.size()) g_edSel = -1;
}

void EdUndoAction()
{
    if (g_edUndo.empty()) return;
    EdSnap cur;
    cur.objs = g_edObjs;
    cur.sel  = g_edSel;
    g_edRedo.push_back(cur);
    EdApply(g_edUndo.back());
    g_edUndo.pop_back();
    InvalidateRect(g_edWnd, nullptr, FALSE);
}

void EdRedoAction()
{
    if (g_edRedo.empty()) return;
    EdSnap cur;
    cur.objs = g_edObjs;
    cur.sel  = g_edSel;
    g_edUndo.push_back(cur);
    EdApply(g_edRedo.back());
    g_edRedo.pop_back();
    InvalidateRect(g_edWnd, nullptr, FALSE);
}

// ---- геометрія полотна --------------------------------------------------

double EdFitScale()
{
    const int cw = g_edRcCanvas.right - g_edRcCanvas.left - EdPx(24);
    const int ch = g_edRcCanvas.bottom - g_edRcCanvas.top - EdPx(24);
    if (!g_edImgW || !g_edImgH || cw <= 0 || ch <= 0) return 1.0;
    double fit = 1.0;
    if (g_edImgW > cw) fit = (double)cw / g_edImgW;
    if (g_edImgH * fit > ch) fit = (double)ch / g_edImgH;
    return fit;
}

void EdClampPan(int dw, int dh)
{
    const int cw = g_edRcCanvas.right - g_edRcCanvas.left;
    const int ch = g_edRcCanvas.bottom - g_edRcCanvas.top;
    const int maxX = (dw > cw) ? (dw - cw) / 2 : 0;
    const int maxY = (dh > ch) ? (dh - ch) / 2 : 0;
    if (g_edPanX >  maxX) g_edPanX =  maxX;
    if (g_edPanX < -maxX) g_edPanX = -maxX;
    if (g_edPanY >  maxY) g_edPanY =  maxY;
    if (g_edPanY < -maxY) g_edPanY = -maxY;
}

double EdScale() { return EdFitScale() * g_edZoom; }

RECT EdImageRect()
{
    const double s = EdScale();
    int dw = (int)(g_edImgW * s + 0.5), dh = (int)(g_edImgH * s + 0.5);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    EdClampPan(dw, dh);
    const int cw = g_edRcCanvas.right - g_edRcCanvas.left;
    const int ch = g_edRcCanvas.bottom - g_edRcCanvas.top;
    const int x = g_edRcCanvas.left + (cw - dw) / 2 + g_edPanX;
    const int y = g_edRcCanvas.top + (ch - dh) / 2 + g_edPanY;
    RECT r = { x, y, x + dw, y + dh };
    return r;
}

POINT EdToImage(POINT scr)
{
    const RECT ir = EdImageRect();
    const double s = EdScale();
    POINT p;
    p.x = (s > 0) ? (int)((scr.x - ir.left) / s + 0.5) : 0;
    p.y = (s > 0) ? (int)((scr.y - ir.top) / s + 0.5) : 0;
    return p;
}

RECT EdObjScreen(const EdObj& o)
{
    const RECT ir = EdImageRect();
    const double s = EdScale();
    RECT r;
    r.left   = ir.left + (int)(o.x * s + 0.5);
    r.top    = ir.top + (int)(o.y * s + 0.5);
    r.right  = ir.left + (int)((o.x + o.w) * s + 0.5);
    r.bottom = ir.top + (int)((o.y + o.h) * s + 0.5);
    return r;
}

// Ручки — сталого розміру на екрані. Якби вони масштабувалися разом із
// зображенням, на 8× вони перекрили б сам об'єкт.
void EdHandles(const EdObj& o, RECT out[8])
{
    const RECT r = EdObjScreen(o);
    const int h = EdPx(9), k = h / 2;
    const int xs[3] = { r.left, (r.left + r.right) / 2, r.right };
    const int ys[3] = { r.top, (r.top + r.bottom) / 2, r.bottom };
    const int ix[8] = { 0, 1, 2, 2, 2, 1, 0, 0 };
    const int iy[8] = { 0, 0, 0, 1, 2, 2, 2, 1 };
    for (int i = 0; i < 8; ++i) {
        out[i].left   = xs[ix[i]] - k;
        out[i].top    = ys[iy[i]] - k;
        out[i].right  = out[i].left + h;
        out[i].bottom = out[i].top + h;
    }
}

void EdZoomAt(POINT cur, bool in)
{
    if (!g_edImg) return;
    const double fit = EdFitScale();
    const float oldZoom = g_edZoom;
    float z = in ? g_edZoom * 1.25f : g_edZoom / 1.25f;
    if (z < 1.0f) z = 1.0f;
    const double longSide = (g_edImgW > g_edImgH ? g_edImgW : g_edImgH) * fit;
    if (longSide > 0 && longSide * z > 20000.0) z = (float)(20000.0 / longSide);
    if (z == oldZoom) return;

    const RECT before = EdImageRect();
    const double sOld = fit * oldZoom;
    const double ix = (sOld > 0) ? (cur.x - before.left) / sOld : 0.0;
    const double iy = (sOld > 0) ? (cur.y - before.top) / sOld : 0.0;

    g_edZoom = z;
    const double sNew = fit * z;
    const int dw = (int)(g_edImgW * sNew + 0.5), dh = (int)(g_edImgH * sNew + 0.5);
    const int cw = g_edRcCanvas.right - g_edRcCanvas.left;
    const int ch = g_edRcCanvas.bottom - g_edRcCanvas.top;
    g_edPanX = (int)(cur.x - ix * sNew - g_edRcCanvas.left - (cw - dw) / 2.0 + 0.5);
    g_edPanY = (int)(cur.y - iy * sNew - g_edRcCanvas.top - (ch - dh) / 2.0 + 0.5);
    EdClampPan(dw, dh);
    InvalidateRect(g_edWnd, nullptr, FALSE);
}

void EdFitView()
{
    g_edZoom = 1.0f;
    g_edPanX = g_edPanY = 0;
    InvalidateRect(g_edWnd, nullptr, FALSE);
}

// ---- розкладка: рахуємо прямокутники один раз, малюємо й клікаємо по них ----

void EdAdd(const RECT& r, EdHit what, int idx) { g_edRegions.push_back({ r, what, idx }); }

RECT EdPill(int x, int cy, int w, int h) { RECT r = { x, cy - h / 2, x + w, cy + h / 2 }; return r; }

void EdLayout(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    g_edRegions.clear();

    const int strip = EdPx(kEdStrip), rail = EdPx(kEdRail), status = EdPx(kEdStatus);
    const int panel = g_edPanelOpen ? EdPx(kEdPanel) : EdPx(kEdPanelLo);

    g_edRcStrip  = { 0, 0, rc.right, strip };
    g_edRcStatus = { 0, rc.bottom - status, rc.right, rc.bottom };
    g_edRcRail   = { 0, strip, rail, rc.bottom - status };
    g_edRcPanel  = { rc.right - panel, strip, rc.right, rc.bottom - status };
    g_edRcCanvas = { rail, strip, rc.right - panel, rc.bottom - status };

    EdAdd(g_edRcCanvas, EdHit::Canvas, 0);   // найнижчий пріоритет: перевіряємо останнім

    // панель інструментів
    {
        const int b = EdPx(40), gap = EdPx(4);
        int y = g_edRcRail.top + EdPx(8);
        const int x = g_edRcRail.left + (rail - b) / 2;
        for (int i = 0; i < 2; ++i) {
            RECT r = { x, y, x + b, y + b };
            EdAdd(r, EdHit::Tool, i);
            y += b + gap;
        }
    }

    // смуга властивостей
    {
        HDC dc = GetDC(hwnd);
        const int cy = (g_edRcStrip.top + g_edRcStrip.bottom) / 2;
        const int gap = EdPx(14);
        int x = EdPx(14);

        const bool hasSel = (g_edSel >= 0 && g_edSel < (int)g_edObjs.size());
        // Чіп із назвою: для вибраного — що саме вибрано, інакше — активний інструмент.
        const wchar_t* chip = hasSel ? S(Str::EdKindRect)
                                     : (g_edTool == EdTool::Rect ? S(Str::EdToolRect) : nullptr);
        if (chip) {
            const int w = EdTextWidth(dc, chip, g_edFontBold) + EdPx(20);
            RECT r = EdPill(x, cy, w, EdPx(26));
            EdAdd(r, EdHit::None, 0);
            x = r.right + gap;
        }

        const bool showProps = hasSel || g_edTool == EdTool::Rect;
        if (showProps) {
            const int sw = EdPx(22), sg = EdPx(5);
            for (int i = 0; i < 8; ++i) {
                RECT r = EdPill(x, cy, sw, sw);
                EdAdd(r, EdHit::Swatch, i);
                x = r.right + sg;
            }
            x += gap - sg;

            RECT ic = EdPill(x, cy, EdPx(18), EdPx(18));
            EdAdd(ic, EdHit::None, 0);
            x = ic.right + EdPx(8);
            RECT sl = EdPill(x, cy, EdPx(96), EdPx(20));
            EdAdd(sl, EdHit::Opacity, 0);
            x = sl.right + EdPx(8) + EdPx(44) + gap;
        }

        if (hasSel) {
            const int b = EdPx(32), bh = EdPx(28);
            RECT r1 = EdPill(x, cy, b, bh); EdAdd(r1, EdHit::Front, 0); x = r1.right + EdPx(4);
            RECT r2 = EdPill(x, cy, b, bh); EdAdd(r2, EdHit::Back, 0);  x = r2.right + EdPx(4);
            RECT r3 = EdPill(x, cy, b, bh); EdAdd(r3, EdHit::Del, 0);   x = r3.right + gap;
        }

        // Скасувати, Повторити й довідка притиснуті праворуч. У макеті вони в
        // заголовку вікна; поки заголовок системний, їх дім — тут.
        {
            const int b = EdPx(32);
            int rx = rc.right - EdPx(14) - b;
            RECT h = EdPill(rx, cy, b, b); EdAdd(h, EdHit::Help, 0);
            rx -= b + EdPx(10);
            RECT rr = EdPill(rx, cy, b, b); EdAdd(rr, EdHit::Redo, 0);
            rx -= b + EdPx(2);
            RECT ru = EdPill(rx, cy, b, b); EdAdd(ru, EdHit::Undo, 0);
        }
        ReleaseDC(hwnd, dc);
    }

    // рядок стану: масштаб
    {
        HDC dc = GetDC(hwnd);
        const int cy = (g_edRcStatus.top + g_edRcStatus.bottom) / 2;
        int x = EdPx(14);
        x += EdTextWidth(dc, L"8888 × 8888", g_edFont) + EdPx(12) + 1 + EdPx(12);
        x += EdPx(190) + EdPx(12) + 1 + EdPx(12);   // місце під опис виділення

        RECT m = EdPill(x, cy, EdPx(26), EdPx(26)); EdAdd(m, EdHit::ZoomOut, 0);
        x = m.right + EdPx(4) + EdPx(48) + EdPx(4);
        RECT p = EdPill(x, cy, EdPx(26), EdPx(26)); EdAdd(p, EdHit::ZoomIn, 0);
        x = p.right + EdPx(8);
        const int fw = EdTextWidth(dc, S(Str::EdFit), g_edFont) + EdPx(20);
        RECT f = EdPill(x, cy, fw, EdPx(26)); EdAdd(f, EdHit::Fit, 0);
        ReleaseDC(hwnd, dc);
    }

    // згортання правої панелі
    {
        const int b = EdPx(24);
        RECT r = { g_edRcPanel.right - EdPx(14) - b, g_edRcPanel.top + EdPx(12),
                   g_edRcPanel.right - EdPx(14), g_edRcPanel.top + EdPx(12) + b };
        if (!g_edPanelOpen) {
            r.left = g_edRcPanel.left + (EdPx(kEdPanelLo) - b) / 2;
            r.right = r.left + b;
        }
        EdAdd(r, EdHit::Panel, 0);
    }
}

const EdRegion* EdFind(POINT pt)
{
    for (size_t i = g_edRegions.size(); i-- > 0; ) {
        const EdRegion& r = g_edRegions[i];
        if (r.what != EdHit::None && PtInRect(&r.r, pt)) return &r;
    }
    return nullptr;
}

const RECT* EdRegionRect(EdHit what, int idx)
{
    for (size_t i = 0; i < g_edRegions.size(); ++i)
        if (g_edRegions[i].what == what && g_edRegions[i].idx == idx) return &g_edRegions[i].r;
    return nullptr;
}

// ---- малювання ----------------------------------------------------------

void EdPaintButton(Gdiplus::Graphics& g, const RECT& r, const EdTheme& t,
                   bool active, bool hot, bool flat, bool danger = false)
{
    Gdiplus::Color fill, bd;
    if (danger)      { fill = EdC(t.dangerBg); bd = EdC(t.dangerBd); }
    else if (active) { fill = EdC(t.accentBg); bd = EdC(t.accentBd); }
    else if (flat)   { fill = EdC(t.hot, hot ? 255 : 0); bd = EdC(t.btnBd, 0); }
    else             { fill = EdC(hot ? t.hot : t.btn); bd = EdC(t.btnBd); }
    EdFillRound(g, r, (float)EdPx(6), &fill, (bd.GetAlpha() ? &bd : nullptr));
}

void EdPaintSlider(Gdiplus::Graphics& g, const RECT& r, const EdTheme& t, int percent)
{
    const int cy = (r.top + r.bottom) / 2;
    RECT track = { r.left, cy - EdPx(2), r.right, cy + EdPx(2) };
    Gdiplus::Color bg = EdC(t.border);
    EdFillRound(g, track, (float)EdPx(2), &bg, nullptr);
    const int w = r.right - r.left;
    const int fx = r.left + (int)((percent - 10) / 90.0 * w + 0.5);
    RECT fill = { r.left, track.top, fx, track.bottom };
    if (fill.right > fill.left) {
        Gdiplus::Color ac = EdC(t.accent);
        EdFillRound(g, fill, (float)EdPx(2), &ac, nullptr);
    }
    const int k = EdPx(14);
    RECT knob = { fx - k / 2, cy - k / 2, fx + k / 2, cy + k / 2 };
    Gdiplus::Color kf = EdC(g_edDark ? RGB(230, 230, 230) : RGB(255, 255, 255));
    Gdiplus::Color kb = EdC(t.btnBd);
    EdFillRound(g, knob, k / 2.0f, &kf, &kb);
}

void EdPaintStrip(HDC dc, Gdiplus::Graphics& g, const EdTheme& t)
{
    HBRUSH b = CreateSolidBrush(t.surface);
    FillRect(dc, &g_edRcStrip, b);
    DeleteObject(b);
    RECT line = { g_edRcStrip.left, g_edRcStrip.bottom - 1, g_edRcStrip.right, g_edRcStrip.bottom };
    b = CreateSolidBrush(t.border);
    FillRect(dc, &line, b);
    DeleteObject(b);

    const bool hasSel = (g_edSel >= 0 && g_edSel < (int)g_edObjs.size());
    const wchar_t* chip = hasSel ? S(Str::EdKindRect)
                                 : (g_edTool == EdTool::Rect ? S(Str::EdToolRect) : nullptr);
    const int cy = (g_edRcStrip.top + g_edRcStrip.bottom) / 2;

    if (chip) {
        // Чіп не інтерактивний, тож у списку регіонів його немає — рахуємо на місці
        // рівно так само, як це робить розкладка.
        const int w = EdTextWidth(dc, chip, g_edFontBold) + EdPx(20);
        const RECT r = EdPill(EdPx(14), cy, w, EdPx(26));
        Gdiplus::Color f = EdC(t.accentBg);
        EdFillRound(g, r, (float)EdPx(13), &f, nullptr);
        EdDrawText(dc, r, chip, g_edFontBold, t.accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (!hasSel && g_edTool == EdTool::Select && !g_edObjs.empty()) {
        RECT r = { EdPx(14), g_edRcStrip.top, g_edRcStrip.right, g_edRcStrip.bottom };
        EdDrawText(dc, r, S(Str::EdSelHint), g_edFont, t.text2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    const COLORREF cur = hasSel ? g_edObjs[g_edSel].color : g_edColor;
    for (int i = 0; i < 8; ++i) {
        const RECT* r = EdRegionRect(EdHit::Swatch, i);
        if (!r) break;
        Gdiplus::Color f = EdC(kEdPalette[i]);
        Gdiplus::Color bd = EdC(g_edDark ? RGB(90, 90, 96) : RGB(201, 201, 204));
        EdFillRound(g, *r, (float)EdPx(5), &f, &bd);
        if (kEdPalette[i] == cur) {
            RECT ring = { r->left - EdPx(3), r->top - EdPx(3), r->right + EdPx(3), r->bottom + EdPx(3) };
            Gdiplus::Color ac = EdC(t.accent);
            EdFillRound(g, ring, (float)EdPx(7), nullptr, &ac, (float)EdPx(2));
        }
    }

    if (const RECT* sl = EdRegionRect(EdHit::Opacity, 0)) {
        const int a = hasSel ? g_edObjs[g_edSel].alpha : g_edAlpha;
        RECT ic = { sl->left - EdPx(8) - EdPx(18), (sl->top + sl->bottom) / 2 - EdPx(9),
                    sl->left - EdPx(8), (sl->top + sl->bottom) / 2 + EdPx(9) };
        EdIcon(g, IcoOpacity, ic, EdC(t.text2), 1.6f);
        EdPaintSlider(g, *sl, t, a);
        wchar_t buf[32];
        wsprintfW(buf, L"%d %%", a);
        RECT tv = { sl->right + EdPx(8), g_edRcStrip.top, sl->right + EdPx(8) + EdPx(44), g_edRcStrip.bottom };
        EdDrawText(dc, tv, buf, g_edFont, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    struct { EdHit what; int ico; bool danger; } acts[3] = {
        { EdHit::Front, IcoFront, false }, { EdHit::Back, IcoBack, false }, { EdHit::Del, IcoDel, true }
    };
    for (int i = 0; i < 3; ++i) {
        const RECT* r = EdRegionRect(acts[i].what, 0);
        if (!r) continue;
        const bool hot = (g_edHotWhat == acts[i].what);
        EdPaintButton(g, *r, t, false, hot, false, acts[i].danger);
        EdIcon(g, acts[i].ico, *r, EdC(acts[i].danger ? t.dangerFg : t.text), 1.6f);
    }

    struct { EdHit what; int ico; bool on; } cmds[3] = {
        { EdHit::Undo, IcoUndo, !g_edUndo.empty() },
        { EdHit::Redo, IcoRedo, !g_edRedo.empty() },
        { EdHit::Help, IcoHelp, true }
    };
    for (int i = 0; i < 3; ++i) {
        const RECT* r = EdRegionRect(cmds[i].what, 0);
        if (!r) continue;
        const bool hot = (g_edHotWhat == cmds[i].what) && cmds[i].on;
        EdPaintButton(g, *r, t, false, hot, true);
        EdIcon(g, cmds[i].ico, *r, EdC(cmds[i].on ? t.text : t.text2, cmds[i].on ? 255 : 130), 1.6f);
    }
}

void EdPaintRail(HDC dc, Gdiplus::Graphics& g, const EdTheme& t)
{
    HBRUSH b = CreateSolidBrush(t.chrome);
    FillRect(dc, &g_edRcRail, b);
    DeleteObject(b);
    RECT line = { g_edRcRail.right - 1, g_edRcRail.top, g_edRcRail.right, g_edRcRail.bottom };
    b = CreateSolidBrush(t.border);
    FillRect(dc, &line, b);
    DeleteObject(b);

    const int icos[2] = { IcoSelect, IcoRect };
    for (int i = 0; i < 2; ++i) {
        const RECT* r = EdRegionRect(EdHit::Tool, i);
        if (!r) break;
        const bool active = ((int)g_edTool == i);
        const bool hot = (g_edHotWhat == EdHit::Tool && g_edHotIdx == i);
        EdPaintButton(g, *r, t, active, hot, !active);
        EdIcon(g, icos[i], *r, EdC(active ? t.accent : t.text), 1.7f);
    }
}

void EdPaintCanvas(HDC dc, Gdiplus::Graphics& g, const EdTheme& t)
{
    HBRUSH b = CreateSolidBrush(t.canvas);
    FillRect(dc, &g_edRcCanvas, b);
    DeleteObject(b);
    if (!g_edImg) return;

    // Обов'язково клипаємо: збільшене зображення інакше малюється поверх смуги
    // властивостей і рядка стану (граблі CAPS-16).
    Gdiplus::GraphicsState st = g.Save();
    g.SetClip(Gdiplus::Rect(g_edRcCanvas.left, g_edRcCanvas.top,
                            g_edRcCanvas.right - g_edRcCanvas.left,
                            g_edRcCanvas.bottom - g_edRcCanvas.top));

    const RECT ir = EdImageRect();
    g.SetInterpolationMode(EdScale() < 1.0 ? Gdiplus::InterpolationModeHighQualityBicubic
                                           : Gdiplus::InterpolationModeNearestNeighbor);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.DrawImage(g_edImg, Gdiplus::Rect(ir.left, ir.top, ir.right - ir.left, ir.bottom - ir.top),
                0, 0, g_edImgW, g_edImgH, Gdiplus::UnitPixel);

    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const double s = EdScale();
    for (size_t i = 0; i < g_edObjs.size(); ++i) {
        const EdObj& o = g_edObjs[i];
        const RECT r = EdObjScreen(o);
        float pw = (float)(o.thick * s);
        if (pw < 1.0f) pw = 1.0f;              // тонше пікселя — це вже невидимо
        Gdiplus::Pen pen(EdC(o.color, o.alpha * 255 / 100), pw);
        pen.SetLineJoin(Gdiplus::LineJoinMiter);
        const float half = pw / 2.0f;
        g.DrawRectangle(&pen, r.left + half, r.top + half,
                        (float)(r.right - r.left) - pw, (float)(r.bottom - r.top) - pw);
    }

    // Те, що зараз тягнуть мишею, ще не в списку — малюємо окремо.
    if (g_edDrag == EdDrag::New) {
        const RECT r = EdObjScreen(g_edNew);
        float pw = (float)(g_edNew.thick * s);
        if (pw < 1.0f) pw = 1.0f;
        Gdiplus::Pen pen(EdC(g_edNew.color, g_edNew.alpha * 255 / 100), pw);
        const float half = pw / 2.0f;
        g.DrawRectangle(&pen, r.left + half, r.top + half,
                        (float)(r.right - r.left) - pw, (float)(r.bottom - r.top) - pw);
    }

    if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size()) {
        const RECT r = EdObjScreen(g_edObjs[g_edSel]);
        Gdiplus::Pen mark(EdC(RGB(255, 255, 255), 190), 1.0f);
        mark.SetDashStyle(Gdiplus::DashStyleDash);
        g.DrawRectangle(&mark, (float)r.left, (float)r.top,
                        (float)(r.right - r.left), (float)(r.bottom - r.top));
        RECT hs[8];
        EdHandles(g_edObjs[g_edSel], hs);
        Gdiplus::SolidBrush wb(EdC(RGB(255, 255, 255)));
        Gdiplus::Pen hp(EdC(t.accent), 2.0f);
        for (int i = 0; i < 8; ++i) {
            g.FillRectangle(&wb, (INT)hs[i].left, (INT)hs[i].top,
                            (INT)(hs[i].right - hs[i].left), (INT)(hs[i].bottom - hs[i].top));
            g.DrawRectangle(&hp, (float)hs[i].left, (float)hs[i].top,
                            (float)(hs[i].right - hs[i].left), (float)(hs[i].bottom - hs[i].top));
        }
    }
    g.Restore(st);
}

void EdPaintPanel(HDC dc, Gdiplus::Graphics& g, const EdTheme& t)
{
    HBRUSH b = CreateSolidBrush(g_edDark ? t.chrome : RGB(250, 250, 250));
    FillRect(dc, &g_edRcPanel, b);
    DeleteObject(b);
    RECT line = { g_edRcPanel.left, g_edRcPanel.top, g_edRcPanel.left + 1, g_edRcPanel.bottom };
    b = CreateSolidBrush(t.border);
    FillRect(dc, &line, b);
    DeleteObject(b);

    if (const RECT* r = EdRegionRect(EdHit::Panel, 0)) {
        const bool hot = (g_edHotWhat == EdHit::Panel);
        EdPaintButton(g, *r, t, false, hot, true);
        EdIcon(g, g_edPanelOpen ? IcoChevR : IcoChevL, *r, EdC(t.text2), 1.7f);
    }
    if (!g_edPanelOpen) return;

    const int x = g_edRcPanel.left + EdPx(14);
    int y = g_edRcPanel.top + EdPx(14);
    RECT h = { x, y, g_edRcPanel.right - EdPx(44), y + EdPx(18) };
    EdDrawText(dc, h, S(Str::EdSecShot), g_edFontSmall, t.text2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y = h.bottom + EdPx(12);

    wchar_t buf[MAX_PATH + 64];
    wsprintfW(buf, S(Str::EdFmtSource), g_edImgW, g_edImgH);
    RECT l1 = { x, y, g_edRcPanel.right - EdPx(14), y + EdPx(20) };
    EdDrawText(dc, l1, buf, g_edFont, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y = l1.bottom + EdPx(6);

    if (g_edSource[0]) {
        RECT l2 = { x, y, g_edRcPanel.right - EdPx(14), y + EdPx(20) };
        EdDrawText(dc, l2, g_edSource, g_edFont, t.text2,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        y = l2.bottom + EdPx(6);
    }

    wsprintfW(buf, S(Str::EdFmtMarks), (int)g_edObjs.size());
    RECT l3 = { x, y, g_edRcPanel.right - EdPx(14), y + EdPx(20) };
    EdDrawText(dc, l3, buf, g_edFont, t.text2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void EdPaintStatus(HDC dc, Gdiplus::Graphics& g, const EdTheme& t)
{
    HBRUSH b = CreateSolidBrush(t.chrome);
    FillRect(dc, &g_edRcStatus, b);
    DeleteObject(b);
    RECT line = { g_edRcStatus.left, g_edRcStatus.top, g_edRcStatus.right, g_edRcStatus.top + 1 };
    b = CreateSolidBrush(t.border);
    FillRect(dc, &line, b);
    DeleteObject(b);

    wchar_t buf[128];
    int x = EdPx(14);
    wsprintfW(buf, L"%d × %d", g_edImgW, g_edImgH);
    RECT r1 = { x, g_edRcStatus.top, x + EdTextWidth(dc, L"8888 × 8888", g_edFont), g_edRcStatus.bottom };
    EdDrawText(dc, r1, buf, g_edFont, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    x = r1.right + EdPx(12);
    RECT d1 = { x, (g_edRcStatus.top + g_edRcStatus.bottom) / 2 - EdPx(9), x + 1,
                (g_edRcStatus.top + g_edRcStatus.bottom) / 2 + EdPx(9) };
    b = CreateSolidBrush(t.border);
    FillRect(dc, &d1, b);
    DeleteObject(b);
    x = d1.right + EdPx(12);

    if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size())
        wsprintfW(buf, S(Str::EdFmtSel), g_edObjs[g_edSel].w, g_edObjs[g_edSel].h);
    else
        lstrcpynW(buf, S(Str::EdNoSel), 128);
    RECT r2 = { x, g_edRcStatus.top, x + EdPx(190), g_edRcStatus.bottom };
    EdDrawText(dc, r2, buf, g_edFont, t.text2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    x = r2.right + EdPx(12);
    RECT d2 = { x, d1.top, x + 1, d1.bottom };
    b = CreateSolidBrush(t.border);
    FillRect(dc, &d2, b);
    DeleteObject(b);

    struct { EdHit what; int ico; } zb[2] = { { EdHit::ZoomOut, IcoMinus }, { EdHit::ZoomIn, IcoPlus } };
    for (int i = 0; i < 2; ++i) {
        const RECT* r = EdRegionRect(zb[i].what, 0);
        if (!r) continue;
        EdPaintButton(g, *r, t, false, g_edHotWhat == zb[i].what, false);
        EdIcon(g, zb[i].ico, *r, EdC(t.text), 1.6f);
    }
    if (const RECT* rm = EdRegionRect(EdHit::ZoomOut, 0)) {
        const int pc = (int)(EdScale() * 100.0 + 0.5);
        wsprintfW(buf, L"%d %%", pc);
        RECT rv = { rm->right + EdPx(4), g_edRcStatus.top, rm->right + EdPx(4) + EdPx(48), g_edRcStatus.bottom };
        EdDrawText(dc, rv, buf, g_edFont, t.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (const RECT* rf = EdRegionRect(EdHit::Fit, 0)) {
        EdPaintButton(g, *rf, t, false, g_edHotWhat == EdHit::Fit, false);
        EdDrawText(dc, *rf, S(Str::EdFit), g_edFont, t.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void EdPaint(HWND hwnd, HDC dc)
{
    const EdTheme t = EdColors(g_edDark);
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    EdPaintCanvas(dc, g, t);
    EdPaintRail(dc, g, t);
    EdPaintPanel(dc, g, t);
    EdPaintStrip(dc, g, t);
    EdPaintStatus(dc, g, t);
}

// ---- дії ---------------------------------------------------------------

void EdSetColor(COLORREF c)
{
    if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size()) {
        if (g_edObjs[g_edSel].color == c) return;
        EdPushUndo();
        g_edObjs[g_edSel].color = c;
    }
    g_edColor = c;
    InvalidateRect(g_edWnd, nullptr, FALSE);
}

void EdDeleteSel()
{
    if (g_edSel < 0 || g_edSel >= (int)g_edObjs.size()) return;
    EdPushUndo();
    g_edObjs.erase(g_edObjs.begin() + g_edSel);
    g_edSel = -1;
    InvalidateRect(g_edWnd, nullptr, FALSE);
}

void EdRaise(bool front)
{
    if (g_edSel < 0 || g_edSel >= (int)g_edObjs.size()) return;
    const int last = (int)g_edObjs.size() - 1;
    if ((front && g_edSel == last) || (!front && g_edSel == 0)) return;
    EdPushUndo();
    EdObj o = g_edObjs[g_edSel];
    g_edObjs.erase(g_edObjs.begin() + g_edSel);
    if (front) { g_edObjs.push_back(o); g_edSel = (int)g_edObjs.size() - 1; }
    else       { g_edObjs.insert(g_edObjs.begin(), o); g_edSel = 0; }
    InvalidateRect(g_edWnd, nullptr, FALSE);
}

void EdSetAlphaAt(int mouseX)
{
    const RECT* sl = EdRegionRect(EdHit::Opacity, 0);
    if (!sl) return;
    const int w = sl->right - sl->left;
    if (w <= 0) return;
    int p = 10 + (int)((mouseX - sl->left) * 90.0 / w + 0.5);
    if (p < 10) p = 10;
    if (p > 100) p = 100;
    if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size()) g_edObjs[g_edSel].alpha = p;
    else g_edAlpha = p;
    InvalidateRect(g_edWnd, nullptr, FALSE);
}

int EdPick(POINT pt)
{
    for (int i = (int)g_edObjs.size() - 1; i >= 0; --i) {
        RECT r = EdObjScreen(g_edObjs[i]);
        const int tol = EdPx(3);
        InflateRect(&r, tol, tol);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

void EdNormalize(EdObj& o)
{
    if (o.w < 0) { o.x += o.w; o.w = -o.w; }
    if (o.h < 0) { o.y += o.h; o.h = -o.h; }
}

void EdResizeSel(int handle, POINT img)
{
    EdObj o = g_edDragOrig;
    int l = o.x, tp = o.y, r = o.x + o.w, bt = o.y + o.h;
    switch (handle) {
    case 0: l = img.x; tp = img.y; break;
    case 1: tp = img.y; break;
    case 2: r = img.x; tp = img.y; break;
    case 3: r = img.x; break;
    case 4: r = img.x; bt = img.y; break;
    case 5: bt = img.y; break;
    case 6: l = img.x; bt = img.y; break;
    case 7: l = img.x; break;
    default: break;
    }
    EdObj n = o;
    n.x = l; n.y = tp; n.w = r - l; n.h = bt - tp;
    EdNormalize(n);
    if (n.w < 2) n.w = 2;
    if (n.h < 2) n.h = 2;
    g_edObjs[g_edSel] = n;
}

// ---- завантаження зображення -------------------------------------------

// Читаємо в пам'ять, а не Bitmap::FromFile: інакше редактор тримав би файл
// відкритим усю сесію, і його не можна було б ні перейменувати, ні видалити.
Gdiplus::Bitmap* EdBitmapFromFile(const wchar_t* path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 256LL * 1024 * 1024) {
        CloseHandle(f);
        return nullptr;
    }
    std::vector<BYTE> data((size_t)sz.QuadPart);
    DWORD got = 0;
    const BOOL ok = ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr);
    CloseHandle(f);
    if (!ok || got != data.size()) return nullptr;

    IStream* st = SHCreateMemStream(data.data(), (UINT)data.size());
    if (!st) return nullptr;
    Gdiplus::Bitmap* src = Gdiplus::Bitmap::FromStream(st);
    Gdiplus::Bitmap* out = nullptr;
    if (src && src->GetLastStatus() == Gdiplus::Ok && src->GetWidth() && src->GetHeight()) {
        // Копія у власну пам'ять: далі потік можна відпустити.
        out = src->Clone(0, 0, (INT)src->GetWidth(), (INT)src->GetHeight(), PixelFormat32bppPARGB);
        if (out && out->GetLastStatus() != Gdiplus::Ok) { delete out; out = nullptr; }
    }
    delete src;
    st->Release();
    return out;
}

// GUID-и своєю копією — як і решта COM у цьому файлі: MinGW тримає їх в uuid.lib,
// MSVC в іншій, і сходяться вони лише так.
const GUID kCLSID_FileOpenDialog = { 0xdc1c5a9c, 0xe88a, 0x4dde, { 0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7 } };
const GUID kIID_IFileOpenDialog  = { 0xd57c7288, 0xd4ad, 0x4768, { 0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60 } };

bool EdPickFile(HWND owner, wchar_t* out, size_t cch)
{
    bool ok = false;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                kIID_IFileOpenDialog, (void**)&dlg)) || !dlg)
        return false;
    COMDLG_FILTERSPEC fs[1];
    fs[0].pszName = S(Str::EdOpenFilter);
    fs[0].pszSpec = L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp";
    dlg->SetFileTypes(1, fs);
    dlg->SetTitle(S(Str::EdOpenTitle));
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                lstrcpynW(out, p, (int)cch);
                CoTaskMemFree(p);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

void EdApplyTheme(HWND hwnd)
{
    g_edDark = ComputeDark();
    const BOOL dark = g_edDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
}

void EdFreeFonts()
{
    if (g_edFont)      { DeleteObject(g_edFont);      g_edFont = nullptr; }
    if (g_edFontBold)  { DeleteObject(g_edFontBold);  g_edFontBold = nullptr; }
    if (g_edFontSmall) { DeleteObject(g_edFontSmall); g_edFontSmall = nullptr; }
}

void EdMakeFonts()
{
    EdFreeFonts();
    g_edFont      = CreateUIFont(100, FW_NORMAL);
    g_edFontBold  = CreateUIFont(100, FW_SEMIBOLD);
    g_edFontSmall = CreateUIFont(90, FW_SEMIBOLD);
}

LRESULT CALLBACK EdWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_edDpi = (int)GetDpiForWindow(hwnd);
        EdMakeFonts();
        EdApplyTheme(hwnd);
        return 0;

    case WM_DPICHANGED: {
        g_edDpi = HIWORD(wp);
        EdMakeFonts();
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_SETTINGCHANGE:
        EdApplyTheme(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = EdPx(kEdMinW);
        mm->ptMinTrackSize.y = EdPx(kEdMinH);
        return 0;
    }

    case WM_SIZE:
        EdLayout(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        EdLayout(hwnd);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        EdPaint(hwnd, mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g_edDrag == EdDrag::Slider) { EdSetAlphaAt(pt.x); return 0; }
        if (g_edDrag == EdDrag::Pan) {
            g_edPanX += pt.x - g_edDragFrom.x;
            g_edPanY += pt.y - g_edDragFrom.y;
            g_edDragFrom = pt;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_edDrag == EdDrag::New) {
            const POINT img = EdToImage(pt);
            g_edNew.w = img.x - g_edNew.x;
            g_edNew.h = img.y - g_edNew.y;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_edDrag == EdDrag::Move && g_edSel >= 0) {
            const double s = EdScale();
            const int dx = (int)((pt.x - g_edDragFrom.x) / (s > 0 ? s : 1.0) + (pt.x >= g_edDragFrom.x ? 0.5 : -0.5));
            const int dy = (int)((pt.y - g_edDragFrom.y) / (s > 0 ? s : 1.0) + (pt.y >= g_edDragFrom.y ? 0.5 : -0.5));
            g_edObjs[g_edSel].x = g_edDragOrig.x + dx;
            g_edObjs[g_edSel].y = g_edDragOrig.y + dy;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_edDrag == EdDrag::Resize && g_edSel >= 0) {
            EdResizeSel(g_edHandle, EdToImage(pt));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        const EdRegion* r = EdFind(pt);
        const EdHit what = (r && r->what != EdHit::Canvas) ? r->what : EdHit::None;
        const int idx = r ? r->idx : -1;
        if (what != g_edHotWhat || idx != g_edHotIdx) {
            g_edHotWhat = what;
            g_edHotIdx = idx;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (!g_edTracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_edTracking = true;
        }
        // Курсор: над ручкою — стрілка розміру, над полотном з прямокутником — хрест.
        if (r && r->what == EdHit::Canvas) {
            LPCWSTR cur = IDC_ARROW;
            if (g_edTool == EdTool::Rect) cur = IDC_CROSS;
            if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size()) {
                RECT hs[8];
                EdHandles(g_edObjs[g_edSel], hs);
                static const LPCWSTR curs[8] = { IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE,
                                                 IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE };
                for (int i = 0; i < 8; ++i)
                    if (PtInRect(&hs[i], pt)) { cur = curs[i]; break; }
            }
            SetCursor(LoadCursorW(nullptr, cur));
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_edTracking = false;
        if (g_edHotWhat != EdHit::None) {
            g_edHotWhat = EdHit::None;
            g_edHotIdx = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        SetFocus(hwnd);
        const EdRegion* r = EdFind(pt);
        if (!r) return 0;
        switch (r->what) {
        case EdHit::Tool:
            g_edTool = (EdTool)r->idx;
            if (g_edTool != EdTool::Select) g_edSel = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case EdHit::Swatch:
            EdSetColor(kEdPalette[r->idx]);
            return 0;
        case EdHit::Opacity:
            if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size()) EdPushUndo();
            g_edDrag = EdDrag::Slider;
            SetCapture(hwnd);
            EdSetAlphaAt(pt.x);
            return 0;
        case EdHit::Undo:  EdUndoAction(); return 0;
        case EdHit::Redo:  EdRedoAction(); return 0;
        case EdHit::Help:  MessageBoxW(hwnd, S(Str::EdHelpBody), S(Str::EdHelpTitle),
                                       MB_OK | MB_ICONINFORMATION); return 0;
        case EdHit::Front: EdRaise(true);  return 0;
        case EdHit::Back:  EdRaise(false); return 0;
        case EdHit::Del:   EdDeleteSel();  return 0;
        case EdHit::ZoomOut: { POINT c = { (g_edRcCanvas.left + g_edRcCanvas.right) / 2,
                                           (g_edRcCanvas.top + g_edRcCanvas.bottom) / 2 };
                               EdZoomAt(c, false); return 0; }
        case EdHit::ZoomIn:  { POINT c = { (g_edRcCanvas.left + g_edRcCanvas.right) / 2,
                                           (g_edRcCanvas.top + g_edRcCanvas.bottom) / 2 };
                               EdZoomAt(c, true); return 0; }
        case EdHit::Fit:   EdFitView(); return 0;
        case EdHit::Panel:
            g_edPanelOpen = !g_edPanelOpen;
            EdLayout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case EdHit::Canvas: break;
        default: return 0;
        }

        if (!g_edImg) return 0;
        if (GetKeyState(VK_SPACE) < 0) {
            g_edDrag = EdDrag::Pan;
            g_edDragFrom = pt;
            SetCapture(hwnd);
            return 0;
        }
        if (g_edTool == EdTool::Rect) {
            const POINT img = EdToImage(pt);
            g_edNew.kind = EdKind::Rect;
            g_edNew.x = img.x; g_edNew.y = img.y; g_edNew.w = 0; g_edNew.h = 0;
            g_edNew.color = g_edColor;
            g_edNew.thick = g_edThick;
            g_edNew.alpha = g_edAlpha;
            g_edDrag = EdDrag::New;
            SetCapture(hwnd);
            return 0;
        }
        // Вибір: спершу ручки вибраного, потім самі об'єкти зверху вниз.
        if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size()) {
            RECT hs[8];
            EdHandles(g_edObjs[g_edSel], hs);
            for (int i = 0; i < 8; ++i) {
                if (PtInRect(&hs[i], pt)) {
                    EdPushUndo();
                    g_edDrag = EdDrag::Resize;
                    g_edHandle = i;
                    g_edDragOrig = g_edObjs[g_edSel];
                    SetCapture(hwnd);
                    return 0;
                }
            }
        }
        {
            const int hit = EdPick(pt);
            if (hit != g_edSel) { g_edSel = hit; InvalidateRect(hwnd, nullptr, FALSE); }
            if (hit >= 0) {
                EdPushUndo();
                g_edDrag = EdDrag::Move;
                g_edDragFrom = pt;
                g_edDragOrig = g_edObjs[hit];
                SetCapture(hwnd);
            }
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_edDrag == EdDrag::New) {
            EdObj o = g_edNew;
            EdNormalize(o);
            if (o.w >= 3 && o.h >= 3) {
                EdPushUndo();
                g_edObjs.push_back(o);
                g_edSel = (int)g_edObjs.size() - 1;
                g_edTool = EdTool::Select;   // намалював — одразу можна правити
            }
        } else if (g_edDrag == EdDrag::Move || g_edDrag == EdDrag::Resize) {
            // Порожній рух не має лишати сліду в скасуванні.
            if (!g_edUndo.empty() && g_edSel >= 0 && g_edSel < (int)g_edObjs.size()) {
                const EdSnap& prev = g_edUndo.back();
                if (g_edSel < (int)prev.objs.size()) {
                    const EdObj& a = prev.objs[g_edSel];
                    const EdObj& b = g_edObjs[g_edSel];
                    if (a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h) g_edUndo.pop_back();
                }
            }
        }
        if (g_edDrag != EdDrag::None) {
            g_edDrag = EdDrag::None;
            g_edHandle = -1;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MBUTTONDOWN:
        g_edDragFrom.x = GET_X_LPARAM(lp);
        g_edDragFrom.y = GET_Y_LPARAM(lp);
        g_edDrag = EdDrag::Pan;
        SetCapture(hwnd);
        return 0;

    case WM_MBUTTONUP:
        if (g_edDrag == EdDrag::Pan) { g_edDrag = EdDrag::None; ReleaseCapture(); }
        return 0;

    case WM_LBUTTONDBLCLK: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (PtInRect(&g_edRcCanvas, pt) && EdPick(pt) < 0) EdFitView();
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        if (!PtInRect(&g_edRcCanvas, pt)) return 0;
        EdZoomAt(pt, GET_WHEEL_DELTA_WPARAM(wp) > 0);
        return 0;
    }

    case WM_KEYDOWN: {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        switch (wp) {
        case 'Z': if (ctrl) EdUndoAction(); return 0;
        case 'Y': if (ctrl) EdRedoAction(); return 0;
        case 'V': if (!ctrl) { g_edTool = EdTool::Select; InvalidateRect(hwnd, nullptr, FALSE); } return 0;
        case 'R': if (!ctrl) { g_edTool = EdTool::Rect; g_edSel = -1; InvalidateRect(hwnd, nullptr, FALSE); } return 0;
        case VK_DELETE: EdDeleteSel(); return 0;
        case VK_ESCAPE:
            if (g_edSel >= 0) { g_edSel = -1; InvalidateRect(hwnd, nullptr, FALSE); }
            else if (g_edTool != EdTool::Select) { g_edTool = EdTool::Select; InvalidateRect(hwnd, nullptr, FALSE); }
            else DestroyWindow(hwnd);
            return 0;
        case VK_F1:
            MessageBoxW(hwnd, S(Str::EdHelpBody), S(Str::EdHelpTitle), MB_OK | MB_ICONINFORMATION);
            return 0;
        default: break;
        }
        return 0;
    }

    case WM_DESTROY:
        delete g_edImg;
        g_edImg = nullptr;
        g_edObjs.clear();
        g_edUndo.clear();
        g_edRedo.clear();
        g_edSel = -1;
        EdFreeFonts();
        g_edWnd = nullptr;
        return 0;

    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EdOpen(HINSTANCE hInst, HWND owner)
{
    if (g_edWnd) {
        ShowWindow(g_edWnd, SW_RESTORE);
        SetForegroundWindow(g_edWnd);
        return;
    }
    wchar_t path[MAX_PATH] = {};
    if (!EdPickFile(owner, path, MAX_PATH)) return;

    Gdiplus::Bitmap* bmp = EdBitmapFromFile(path);
    if (!bmp) {
        MessageBoxW(owner, S(Str::EdErrOpen), kAppName, MB_OK | MB_ICONWARNING);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = EdWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = L"lilhelpers_editor";
        wc.style         = CS_DBLCLKS;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(1));
        RegisterClassW(&wc);
        registered = true;
    }

    g_edImg  = bmp;
    g_edImgW = (int)bmp->GetWidth();
    g_edImgH = (int)bmp->GetHeight();
    lstrcpynW(g_edSource, PathFindFileNameW(path), MAX_PATH);
    g_edObjs.clear();
    g_edUndo.clear();
    g_edRedo.clear();
    g_edSel = -1;
    g_edTool = EdTool::Select;
    g_edZoom = 1.0f;
    g_edPanX = g_edPanY = 0;
    g_edPanelOpen = true;

    wchar_t caption[160];
    wsprintfW(caption, L"%s — %s", S(Str::EdTitle), kAppName);

    const int dpi = (int)GetDpiForSystem();
    const int want = MulDiv(1180, dpi, 96), wantH = MulDiv(760, dpi, 96);
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int maxW = (work.right - work.left) - MulDiv(80, dpi, 96);
    const int maxH = (work.bottom - work.top) - MulDiv(80, dpi, 96);
    const int w = EdMin(want, maxW > 0 ? maxW : want);
    const int h = EdMin(wantH, maxH > 0 ? maxH : wantH);

    g_edWnd = CreateWindowExW(0, L"lilhelpers_editor", caption,
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                              nullptr, nullptr, hInst, nullptr);
    if (!g_edWnd) {
        delete g_edImg;
        g_edImg = nullptr;
        return;
    }
    ShowWindow(g_edWnd, SW_SHOW);
    SetForegroundWindow(g_edWnd);
}

// =================== кінець редактора знімків (CAPS-20) ===================

void ShowTrayMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, S(Str::MenuSettings));
    AppendMenuW(menu, MF_STRING, IDM_EDITOR, S(Str::EdMenu));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, S(Str::MenuExit));
    SetForegroundWindow(hwnd); // інакше меню не закриється кліком повз
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg) {
        // Explorer перезапустився — повертаємо іконку в трей. Через TrayEnsure, бо
        // одразу після рестарту панель може ще не приймати іконок (CAPS-17).
        g_trayTries = 0;
        TrayEnsure(hwnd);
        return 0;
    }

    switch (msg) {
    case WMAPP_SWITCH:   // від хука
    case WM_HOTKEY:      // від системної реєстрації клавіші
        SwitchLayout();
        return 0;

    case WMAPP_SHOWSETTINGS:
        ShowSettings(hwnd);
        return 0;

    case WMAPP_SHAKE:    // від мишачого хука
        MagnifyStart();
        return 0;

    case WMAPP_PEEK:     // CAPS-16: від хука — пробіл або Esc у списку файлів
        if (wp == VK_ESCAPE) PeekClose();
        else                 PeekToggle((HWND)lp);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_MAG_HOLD)       MagnifyBeginShrink();
        else if (wp == TIMER_MAG_FRAME) OverlayFrameTick();
        else if (wp == TIMER_THEME)     ThemeTick();
        else if (wp == TIMER_TRAY)      TrayEnsure(hwnd);   // CAPS-17
        else if (wp == TIMER_UPDATE) {  // CAPS-10: хвилина після старту, далі кожні 30 хв
            SetTimer(hwnd, TIMER_UPDATE, 30 * 60 * 1000, nullptr);
            if (g_updDaily && NowUnix() - g_updLast > 86400) StartUpdate(false, false);
        }
        return 0;

    case WMAPP_UPDATE: {   // CAPS-10: потік оновлення завершився
        UpdResult* r = (UpdResult*)lp;
        g_updBusy = 0;
        if (!r->ok) {
            g_updState = UpdState::Error;
            g_updErr = r->err;                  // .new при збої прибирає сам потік
        } else if (!r->install) {
            g_updLast = NowUnix();
            RegSaveInt(kRegUpdLast, (int)(DWORD)g_updLast);
            wchar_t cur[32] = {};
            ExeVersionString(cur, 32);
            if (CompareVersion(r->tag, cur) > 0) {
                lstrcpynW(g_updTag, r->tag, 32);
                g_updState = UpdState::Available;
                if (!r->manual && lstrcmpW(g_updNotified, r->tag) != 0) {
                    wchar_t text[128] = {};
                    swprintf(text, 128, S(Str::UpdBalloonFmt),
                             r->tag[0] == L'v' ? r->tag + 1 : r->tag);
                    TrayBalloon(kAppName, text);
                    lstrcpynW(g_updNotified, r->tag, 32);
                    RegSaveStr(kRegUpdNotified, r->tag);
                }
            } else {
                g_updState = UpdState::UpToDate;
            }
        } else {
            g_updState = UpdState::Verified;
            UpdateUpdStatus();
            ApplyDownloadedUpdate();   // при успіху процес завершується
        }
        UpdateUpdStatus();
        delete r;
        return 0;
    }

    case WMAPP_THEMELOC: {   // CAPS-7: потік геолокації завершився
        LocResult* r = (LocResult*)lp;
        g_locBusy = 0;
        if (r->gen == g_locGen && g_th.src != LocSource::Manual) {
            if (r->ok) {
                g_fix.ok = true; g_fix.lat = r->lat; g_fix.lon = r->lon;
                g_fix.src = r->src; g_fix.at = NowUnix();
                g_locFailed = false;
                SaveFixCache();
            } else {
                g_locFailed = true;
                g_fix.at = NowUnix();   // не довбати сенсор/мережу щохвилини
            }
            ThemeTick();
            UpdateThemeStatus();
        } else if (g_locAgain) {
            g_locAgain = false;        // джерело змінили, поки тривало визначення
            StartLocate();
        }
        delete r;
        return 0;
    }

    case WM_POWERBROADCAST:   // CAPS-7: після сну тема має відповідати часу
        if (wp == PBT_APMRESUMEAUTOMATIC) ThemeTick();
        return TRUE;

    case WM_TIMECHANGE:       // CAPS-7: змінили час/пояс
        ThemeTick();
        return 0;

    case WMAPP_MAGDONE:   // системний розмір повернуто (lp = покоління анімації)
        if (g_magState == MagState::Shrinking && g_magGen == (LONG)lp) {
            RegDeleteInt(kRegCursorRestore);
            if (!g_overlay)          // при оверлеї стан закриє його ж таймер
                g_magState = MagState::Idle;
        }
        return 0;

    case WM_HSCROLL:
        if ((HWND)lp == g_curScale) {
            g_cur.scale = (int)SendMessageW(g_curScale, TBM_GETPOS, 0, 0);
            RegSaveInt(kRegCursorScale, g_cur.scale);
            SetCursorValueLabels();
        } else if ((HWND)lp == g_curHold) {
            g_cur.holdMs = (int)SendMessageW(g_curHold, TBM_GETPOS, 0, 0) * 100;
            RegSaveInt(kRegCursorHold, g_cur.holdMs);
            SetCursorValueLabels();
        }
        return 0;

    case WM_NOTIFY: {
        const NMHDR* nm = (const NMHDR*)lp;
        // CAPS-8: у темному режимі повзунки і чекбокси/радіо малюємо самі
        if (nm->code == NM_CUSTOMDRAW && g_dark) {
            NMCUSTOMDRAW* cd = (NMCUSTOMDRAW*)lp;
            wchar_t cls[32] = {};
            GetClassNameW(nm->hwndFrom, cls, 32);
            if (!lstrcmpiW(cls, TRACKBAR_CLASSW)) {
                if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                    if (cd->dwItemSpec == TBCD_CHANNEL) { FillRect(cd->hdc, &cd->rc, g_brDkBorder); return CDRF_SKIPDEFAULT; }
                    if (cd->dwItemSpec == TBCD_THUMB)   { FillRect(cd->hdc, &cd->rc, g_brDkThumb);  return CDRF_SKIPDEFAULT; }
                    if (cd->dwItemSpec == TBCD_TICS)    return CDRF_SKIPDEFAULT;
                }
                return CDRF_DODEFAULT;
            }
            if (!lstrcmpiW(cls, L"Button") && IsCheckOrRadio(nm->hwndFrom) && cd->dwDrawStage == CDDS_PREPAINT) {
                DrawCheckDark(nm->hwndFrom, cd->hdc, cd->rc);
                return CDRF_SKIPDEFAULT;
            }
            // CAPS-11: тема DarkMode_Explorer малює ВИМКНЕНУ кнопку як увімкнену —
            // «Оновити» без доступного оновлення виглядало натискабельним. Малюємо самі:
            // трохи світліша плашка, тьмяна рамка, сірий текст.
            if (!lstrcmpiW(cls, L"Button") && !IsCheckOrRadio(nm->hwndFrom) &&
                cd->dwDrawStage == CDDS_PREPAINT && !IsWindowEnabled(nm->hwndFrom)) {
                HBRUSH fill = CreateSolidBrush(RGB(50, 50, 50));
                FillRect(cd->hdc, &cd->rc, fill);
                DeleteObject(fill);
                FrameRect(cd->hdc, &cd->rc, g_brDkBorder);
                wchar_t label[64] = {};
                GetWindowTextW(nm->hwndFrom, label, 63);
                HGDIOBJ old = SelectObject(cd->hdc, (HFONT)SendMessageW(nm->hwndFrom, WM_GETFONT, 0, 0));
                SetBkMode(cd->hdc, TRANSPARENT);
                SetTextColor(cd->hdc, kDkGray);
                RECT rc = cd->rc;
                DrawTextW(cd->hdc, label, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(cd->hdc, old);
                return CDRF_SKIPDEFAULT;
            }
        }
        if (nm->hwndFrom == g_tabs && nm->code == TCN_SELCHANGE)
            SelectTab((int)SendMessageW(g_tabs, TCM_GETCURSEL, 0, 0));
        // CAPS-7: розклад дня/ночі
        if (nm->code == DTN_DATETIMECHANGE &&
            (nm->idFrom == IDC_TH_DARK_FROM || nm->idFrom == IDC_TH_LIGHT_FROM)) {
            g_th.darkFrom  = GetPickerMinutes(g_thDarkFrom,  g_th.darkFrom);
            g_th.lightFrom = GetPickerMinutes(g_thLightFrom, g_th.lightFrom);
            SaveThemeSettings();
            ThemeTick();
            UpdateThemeStatus();
        }
        // CAPS-7: посилання на GitHub у шапці. Через explorer, бо програма
        // елевейтований, а браузер має відкритись звичайним користувачем.
        if (nm->idFrom == IDC_COPYRIGHT && (nm->code == NM_CLICK || nm->code == NM_RETURN)) {
            const NMLINK* l = (const NMLINK*)lp;
            ShellExecuteW(nullptr, L"open", L"explorer.exe", l->item.szUrl, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }

    case WMAPP_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
            ShowSettings(hwnd);
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu(hwnd);
            break;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_AUTOSTART:
            if (HIWORD(wp) == BN_CLICKED) {
                bool want = SendMessageW(g_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (!SetAutostart(want))
                    MessageBoxW(hwnd, S(Str::MsgAutostartFailed), kAppName, MB_ICONERROR | MB_OK);
                SendMessageW(g_checkbox, BM_SETCHECK,
                             AutostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            return 0;
        case IDC_UPD_DAILY:       // CAPS-10
            if (HIWORD(wp) == BN_CLICKED) {
                g_updDaily = SendMessageW(g_updDailyCb, BM_GETCHECK, 0, 0) == BST_CHECKED;
                RegSaveInt(kRegUpdDaily, g_updDaily ? 1 : 0);
            }
            return 0;
        case IDC_UPD_CHECK:
            if (HIWORD(wp) == BN_CLICKED) StartUpdate(false, true);
            return 0;
        case IDC_UPD_INSTALL:
            if (HIWORD(wp) == BN_CLICKED && g_updState == UpdState::Available) StartUpdate(true, true);
            return 0;
        case IDC_UPD_ROLLBACK:
            if (HIWORD(wp) == BN_CLICKED &&
                MessageBoxW(hwnd, S(Str::MsgRollbackConfirm),
                            kAppName, MB_ICONQUESTION | MB_YESNO) == IDYES)
                RollbackUpdate();
            return 0;
        case IDC_LANG_SYSTEM:     // CAPS-12
        case IDC_LANG_UK:
        case IDC_LANG_EN:
            if (HIWORD(wp) == BN_CLICKED) {
                g_langPref = (LangPref)(LOWORD(wp) - IDC_LANG_SYSTEM);
                RegSaveInt(kRegLang, (int)g_langPref);
                const Lang want = ResolveLang(g_langPref);
                if (want != g_lang) { g_lang = want; ApplyLanguage(); }
            }
            return 0;
        case IDC_PEEK_ENABLE:     // CAPS-16
            if (HIWORD(wp) == BN_CLICKED) {
                g_peekOn = SendMessageW(g_peekEnableCb, BM_GETCHECK, 0, 0) == BST_CHECKED;
                RegSaveInt(kRegPeek, g_peekOn ? 1 : 0);
                ApplyPeekFeature();
            }
            return 0;
        case IDC_WT_AUTO:         // CAPS-8
        case IDC_WT_LIGHT:
        case IDC_WT_DARK:
            if (HIWORD(wp) == BN_CLICKED) {
                g_winTheme = (WinTheme)(LOWORD(wp) - IDC_WT_AUTO);
                RegSaveInt(kRegWindowTheme, (int)g_winTheme);
                ApplyWindowTheme(true);
            }
            return 0;
        case IDC_LAYOUT_ENABLE:   // CAPS-9
            if (HIWORD(wp) == BN_CLICKED)
                ApplyLayoutSwitch(hwnd, SendMessageW(g_layoutCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        case IDC_MODE_HOOK:
            if (HIWORD(wp) == BN_CLICKED && g_mode != Mode::Hook)
                ApplyMode(hwnd, Mode::Hook);
            return 0;
        case IDC_MODE_HOTKEY:
            if (HIWORD(wp) == BN_CLICKED && g_mode != Mode::Hotkey)
                ApplyMode(hwnd, Mode::Hotkey);
            return 0;
        case IDC_PASSTHROUGH:
            if (HIWORD(wp) == BN_CLICKED) {
                g_passthrough = SendMessageW(g_passthroughCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
                SavePassthrough(g_passthrough);
                ApplyRemoteContext();
            }
            return 0;
        case IDC_CUR_ENABLE:
            if (HIWORD(wp) == BN_CLICKED) {
                g_cur.enabled = SendMessageW(g_curEnable, BM_GETCHECK, 0, 0) == BST_CHECKED;
                RegSaveInt(kRegCursorEnable, g_cur.enabled ? 1 : 0);
                ApplyCursorFeature();
            }
            return 0;
        case IDC_CUR_OVERLAY:
            if (HIWORD(wp) == BN_CLICKED) {
                g_cur.overlay = SendMessageW(g_curOverlay, BM_GETCHECK, 0, 0) == BST_CHECKED;
                RegSaveInt(kRegCursorOverlay, g_cur.overlay ? 1 : 0);
            }
            return 0;
        case IDC_CUR_ADVANCED:
            if (HIWORD(wp) == BN_CLICKED)
                ToggleAdvanced();
            return 0;
        case IDC_CUR_WINDOWMS:
        case IDC_CUR_DIST:
        case IDC_CUR_FACTOR:
        case IDC_CUR_REVERSALS:
        case IDC_CUR_SHRINK:
            if (HIWORD(wp) == EN_KILLFOCUS)
                CommitAdvanced();
            return 0;
        // ---- CAPS-7: день/ніч ----
        case IDC_TH_ENABLE:
            if (HIWORD(wp) == BN_CLICKED) {
                g_th.enabled = SendMessageW(g_thEnable, BM_GETCHECK, 0, 0) == BST_CHECKED;
                ThemeApplySettings();
            }
            return 0;
        case IDC_TH_BY_SUN:
        case IDC_TH_BY_SCHED:
            if (HIWORD(wp) == BN_CLICKED) {
                g_th.bySchedule = (LOWORD(wp) == IDC_TH_BY_SCHED);
                ThemeApplySettings();
            }
            return 0;
        case IDC_TH_TOGGLE:
            if (HIWORD(wp) == BN_CLICKED) ThemeToggleNow();
            return 0;
        case IDC_TH_ADVANCED:
            if (HIWORD(wp) == BN_CLICKED) ToggleThemeAdvanced();
            return 0;
        case IDC_TH_SRC_AUTO:
        case IDC_TH_SRC_WIN:
        case IDC_TH_SRC_IP:
        case IDC_TH_SRC_MANUAL:
        case IDC_TH_SRC_TZ:
            if (HIWORD(wp) == BN_CLICKED) {
                g_th.src = (LocSource)(LOWORD(wp) - IDC_TH_SRC_AUTO);
                RegSaveInt(kRegThemeLocSrc, (int)g_th.src);
                EnableThemeControls();
                g_locFailed = false;
                if (g_th.src == LocSource::Manual) {
                    CommitManualCoords();          // сам зробить UseManualFix + ThemeTick
                } else if (g_locBusy) {
                    ++g_locGen;                    // відповідь, що летить, уже неактуальна
                    g_locAgain = true;
                } else {
                    StartLocate();
                }
                ThemeTick();
                UpdateThemeStatus();
            }
            return 0;
        case IDC_TH_LAT:
        case IDC_TH_LON:
            if (HIWORD(wp) == EN_KILLFOCUS) CommitManualCoords();
            return 0;
        case IDM_SETTINGS:
            ShowSettings(hwnd);
            return 0;
        case IDM_EDITOR:
            EdOpen(GetModuleHandleW(nullptr), hwnd);
            break;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_PAINT:
        PaintWindow(hwnd);
        return 0;

    case WM_ERASEBKGND:   // CAPS-8: у темному режимі фон вікна — наш
        if (g_dark) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, g_brDkBg);
            return 1;
        }
        break;

    case WM_SETTINGCHANGE:   // CAPS-8: «Автоматично» слідує за темою застосунків Windows
        if (lp && !lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet"))
            ApplyWindowTheme(false);
        return 0;

    case WM_CTLCOLORSTATIC: {
        const int id = GetDlgCtrlID((HWND)lp);
        const bool gray = (id == IDC_COPYRIGHT || id == IDC_PASSTHROUGH_HINT || id == IDC_HINT_GRAY ||
                           id == IDC_MODE_HINT);
        if (g_dark) {
            wchar_t cls[16] = {};
            GetClassNameW((HWND)lp, cls, 16);
            const bool isEdit = !lstrcmpiW(cls, L"Edit");   // вимкнене поле теж шле STATIC
            const COLORREF bg = isEdit ? kDkEdit : (IsPageControl((HWND)lp) ? kDkPage : kDkBg);
            SetBkMode((HDC)wp, TRANSPARENT);
            SetBkColor((HDC)wp, bg);
            SetTextColor((HDC)wp, (gray || (isEdit && !IsWindowEnabled((HWND)lp))) ? kDkGray : kDkText);
            return (LRESULT)(isEdit ? g_brDkEdit : (IsPageControl((HWND)lp) ? g_brDkPage : g_brDkBg));
        }
        const int color = IsPageControl((HWND)lp) ? COLOR_WINDOW : COLOR_BTNFACE;
        SetBkMode((HDC)wp, TRANSPARENT);
        SetBkColor((HDC)wp, GetSysColor(color));
        if (gray)
            SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
        return (LRESULT)GetSysColorBrush(color);
    }

    case WM_CTLCOLOREDIT:   // CAPS-8
        if (g_dark) {
            SetBkColor((HDC)wp, kDkEdit);
            SetTextColor((HDC)wp, kDkText);
            return (LRESULT)g_brDkEdit;
        }
        break;

    case WM_CTLCOLORBTN:    // CAPS-8: підкладка кнопок
        if (g_dark) {
            SetBkColor((HDC)wp, IsPageControl((HWND)lp) ? kDkPage : kDkBg);
            return (LRESULT)(IsPageControl((HWND)lp) ? g_brDkPage : g_brDkBg);
        }
        break;

    case WM_CLOSE:
        CommitAdvanced();          // підхопити те, що набрали й не зняли фокус
        CommitManualCoords();      // CAPS-7: те саме для координат
        ShowWindow(hwnd, SW_HIDE); // закриття вікна не завершує програму
        return 0;

    case WM_ENDSESSION:
        if (wp) MagnifyRestore();  // логаут/вимкнення — не лишати великий курсор
        return 0;

    case WM_DESTROY:
        PeekClose();        // CAPS-16
        MagnifyRestore();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Системний шрифт повідомлень (уже в пікселях системного DPI); CAPS-11: масштаб у
// відсотках і вага — для назви в шапці (165 %, напівжирний) і заголовків груп.
HFONT CreateUIFont(int percent, int weight)
{
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = MulDiv(lf.lfHeight, percent, 100);
    lf.lfWeight = weight;
    return CreateFontIndirectW(&lf);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    WaitForPreviousInstance();   // CAPS-10: після оновлення — дочекатись виходу старого
    CreateMutexW(nullptr, TRUE, L"lilhelpers_single_instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Другий запуск — показуємо вікно першого екземпляра
        if (HWND prev = FindWindowW(kWndClass, nullptr))
            PostMessageW(prev, WMAPP_SHOWSETTINGS, 0, 0);
        return 0;
    }
    // CAPS-12: мова — до будь-якого тексту (перша ж — опис задачі автозапуску нижче)
    g_langPref = (LangPref)RegLoadInt(kRegLang, 0, 0, 2);
    g_lang     = ResolveLang(g_langPref);

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icc = { sizeof(icc),
                                 ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES |
                                 ICC_DATE_CLASSES | ICC_LINK_CLASS };   // CAPS-7: time picker, SysLink
    InitCommonControlsEx(&icc);

    InitializeCriticalSection(&g_magLock);
    LoadCursorSettings();
    LoadThemeSettings();   // CAPS-7
    g_layoutOn = RegLoadInt(kRegLayoutSwitch, 1, 0, 1) != 0;   // CAPS-9
    g_winTheme = (WinTheme)RegLoadInt(kRegWindowTheme, 0, 0, 2); // CAPS-8
    g_updDaily = RegLoadInt(kRegUpdDaily, 1, 0, 1) != 0;          // CAPS-10
    g_peekOn   = RegLoadInt(kRegPeek, 1, 0, 1) != 0;              // CAPS-16
    g_updLast  = (DWORD)RegLoadInt(kRegUpdLast, 0, INT_MIN, INT_MAX);
    RegLoadStr(kRegUpdNotified, g_updNotified, 32);
    {   // недокачаний файл від обірваного оновлення — прибрати
        wchar_t exe[MAX_PATH] = {}, nw[MAX_PATH + 8] = {};
        ExePath(exe);
        swprintf(nw, MAX_PATH + 8, L"%s.new", exe);
        DeleteFileW(nw);
    }
    // Якщо попередній запуск обірвався із збільшеним курсором — повертаємо розмір
    // ДО того, як щось показуємо користувачу.
    RecoverCursorSize();

    Gdiplus::GdiplusStartupInput gdipInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdipInput, nullptr);
    LoadLogo(hInst);

    const UINT dpi = GetDpiForSystem();
    auto sc = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = kWndClass;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    WNDCLASSW ov = {};
    ov.lpfnWndProc   = DefWindowProcW;
    ov.hInstance     = hInst;
    ov.lpszClassName = L"lilhelpers_overlay";
    RegisterClassW(&ov);

    PeekCreateWindow(hInst);   // CAPS-16: вікно перегляду, поки приховане

    // ---- геометрія вікна (логічні px при 96 dpi, sc() масштабує) ----
    //
    // CAPS-11: шапка з логотипом, назвою і версією; сторінки на єдиній сітці —
    // 20 px від краю полотна, крок 8 px між елементами, підказка одразу під
    // своїм контролом, між групами 6–8 px повітря плюс заголовок групи.
    constexpr int W = 500, H = 634;   // 2.6.0: вкладка «Перегляд» переросла попередню висоту
    constexpr int TAB_X = 20, TAB_Y = 74, FOOT_H = 42;    // таб-контрол під шапкою, підвал під табом
    constexpr int PX = TAB_X + 20, PW = 420, PY = 116;    // сторінка: лівий край, ширина, перший рядок
    const int w = sc(W), h = sc(H);
    RECT rc = { 0, 0, w, h };
    AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU, FALSE);
    HWND hwnd = CreateWindowW(kWndClass, kAppName, WS_CAPTION | WS_SYSMENU,
        (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - h) / 2,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);

    HFONT font      = CreateUIFont(100, FW_NORMAL);
    HFONT fontSemi  = CreateUIFont(100, FW_SEMIBOLD);   // заголовки груп
    HFONT fontTitle = CreateUIFont(165, FW_SEMIBOLD);   // назва програми в шапці
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                  int x, int y, int cx, int cy, int id) {
        HWND c = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style,
                               sc(x), sc(y), sc(cx), sc(cy),
                               hwnd, (HMENU)(INT_PTR)id, hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };
    // CAPS-12: те саме, але підпис береться з таблиці й запам'ятовується — щоб
    // зміна мови переписала його без перестворення вікна.
    auto mkS = [&](const wchar_t* cls, Str s, DWORD style,
                   int x, int y, int cx, int cy, int id) {
        HWND c = mk(cls, S(s), style, x, y, cx, cy, id);
        RememberLoc(c, s);
        return c;
    };

    // ---- шапка: логотип, назва, гасло ----
    SetRect(&g_logoRect, sc(24), sc(16), sc(24 + 40), sc(16 + 40));
    SendMessageW(mk(L"STATIC", kAppName, 0, 76, 13, 380, 26, 0), WM_SETFONT, (WPARAM)fontTitle, TRUE);
    mkS(L"STATIC", Str::Tagline, 0, 76, 41, 400, 18, IDC_HINT_GRAY);

    // ---- підвал: авторство ліворуч, версія + посилання праворуч ----
    // Шапка — про продукт, підвал — про автора й випуск: так це читається як у
    // «Про програму», а не як підпис під заголовком.
    {
        wchar_t ver[32] = {}, about[160] = {};
        ExeVersionString(ver, 32);
        mkS(L"STATIC", Str::Copyright, 0, TAB_X + 2, H - 30, 280, 18, IDC_HINT_GRAY);
        swprintf(about, 160, L"v%s · <a href=\"https://github.com/V-Plum/lilhelpers\">GitHub</a>", ver);
        mk(L"SysLink", about, LWS_RIGHT, W - TAB_X - 202, H - 30, 200, 18, IDC_COPYRIGHT);   // WC_LINK
    }

    // Таб-контрол створюємо першим, але на порядок створення НЕ покладаємось:
    // після створення сторінок він явно опускається на низ z-порядку (див. нижче).
    // WS_CLIPSIBLINGS обов'язковий: контроли сторінок — сусіди таба вище за
    // z-order, і без нього будь-яке перемальовування самого таба (наведення на
    // заголовок) зафарбовує їх нашим полотном — «порожнє вікно» у v1.5.0.
    g_tabs = CreateWindowW(WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS,
                           sc(TAB_X), sc(TAB_Y), sc(W - 2 * TAB_X), sc(H - TAB_Y - FOOT_H),
                           hwnd, (HMENU)(INT_PTR)IDC_TABS, hInst, nullptr);
    SendMessageW(g_tabs, WM_SETFONT, (WPARAM)font, TRUE);
    SetWindowSubclass(g_tabs, TabSubclassProc, 1, 0);   // полотно сторінки — див. TabSubclassProc
    SendMessageW(g_tabs, TCM_SETPADDING, 0, MAKELPARAM(sc(10), sc(5)));   // повітря в заголовках
    {
        TCITEMW tab = {};
        tab.mask = TCIF_TEXT;
        for (int i = 0; i < kTabCount; ++i) {
            tab.pszText = (LPWSTR)S(kTabTitles[i]);
            SendMessageW(g_tabs, TCM_INSERTITEMW, i, (LPARAM)&tab);
        }
    }

    auto addL  = [&](HWND c) { return AddTo(g_pageLayout,   g_pageLayoutN,   c); };
    auto addC  = [&](HWND c) { return AddTo(g_pageCursor,   g_pageCursorN,   c); };
    auto addA  = [&](HWND c) { return AddTo(g_advCtrls,     g_advN,          c); };
    auto addT  = [&](HWND c) { return AddTo(g_pageTheme,    g_pageThemeN,    c); };
    auto addTA = [&](HWND c) { return AddTo(g_thAdv,        g_thAdvN,        c); };
    auto addS  = [&](HWND c) { return AddTo(g_pageSettings, g_pageSettingsN, c); };
    auto addP  = [&](HWND c) { return AddTo(g_pagePeek,     g_pagePeekN,     c); };   // CAPS-16

    // Сітка сторінки: y біжить згори вниз, кожен помічник сам відступає під себе.
    int y = PY;
    auto sec = [&](auto add, Str s) {                        // заголовок групи
        HWND c = add(mkS(L"STATIC", s, 0, PX, y, PW, 20, 0));
        SendMessageW(c, WM_SETFONT, (WPARAM)fontSemi, TRUE);
        y += 24;
        return c;
    };
    auto check = [&](auto add, Str s, int id, bool on, int lines = 1) {
        const int ch = lines > 1 ? 40 : 24;
        HWND c = add(mkS(L"BUTTON", s, BS_AUTOCHECKBOX | WS_TABSTOP | (lines > 1 ? BS_MULTILINE : 0),
                         PX, y, PW, ch, id));
        SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        y += ch + 4;
        return c;
    };
    auto text = [&](auto add, Str s, int lines, int id, int after) {   // звичайний текст
        HWND c = add(mkS(L"STATIC", s, 0, PX, y, PW, 18 * lines, id));
        y += 18 * lines + after;
        return c;
    };
    auto hint = [&](auto add, Str s, int lines = 1) {   // сірий, під контролом
        return text(add, s, lines, IDC_HINT_GRAY, 12);
    };
    auto radio = [&](auto add, Str s, int x, int cx, int id, bool first) {
        return add(mkS(L"BUTTON", s, BS_AUTORADIOBUTTON | (first ? (WS_GROUP | WS_TABSTOP) : 0),
                       x, y, cx, 22, id));
    };
    auto button = [&](auto add, Str s, int x, int cx, int id) {
        return add(mkS(L"BUTTON", s, BS_PUSHBUTTON | WS_TABSTOP, x, y, cx, 30, id));
    };

    // ---- вкладка «Розкладка» ----
    y = PY;
    g_layoutCheckbox = check(addL, Str::LayEnable, IDC_LAYOUT_ENABLE, g_layoutOn);
    hint(addL, Str::LayHint);
    y += 6;
    sec(addL, Str::LaySecMode);
    radio(addL, Str::LayModeHook,   PX,       140, IDC_MODE_HOOK,   true);
    radio(addL, Str::LayModeHotkey, PX + 150, 140, IDC_MODE_HOTKEY, false);
    y += 26;
    g_modeHint = text(addL, Str::Empty, 1, IDC_MODE_HINT, 12);
    y += 6;
    sec(addL, Str::LaySecRemote);
    g_passthrough = LoadPassthrough();
    g_passthroughCheckbox = check(addL, Str::LayPassthrough, IDC_PASSTHROUGH, g_passthrough, 2);
    text(addL, Str::LayRemoteList, 1, IDC_PASSTHROUGH_HINT, 12);

    // ---- вкладка «Курсор» ----
    y = PY;
    g_curEnable = check(addC, Str::CurEnable, IDC_CUR_ENABLE, g_cur.enabled);
    hint(addC, Str::CurEnableHint);
    y += 6;
    auto slider = [&](Str label, HWND& valueOut, int id, int lo, int hi, int page, int pos) {
        addC(mkS(L"STATIC", label, 0, PX, y, PW - 100, 20, 0));
        valueOut = addC(mk(L"STATIC", L"", SS_RIGHT, PX + PW - 90, y, 90, 20, 0));
        y += 22;
        HWND t = addC(mk(TRACKBAR_CLASSW, L"", TBS_AUTOTICKS | WS_TABSTOP, PX - 4, y, PW + 8, 30, id));
        SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
        if (page) SendMessageW(t, TBM_SETPAGESIZE, 0, page);
        SendMessageW(t, TBM_SETPOS, TRUE, pos);
        y += 36;
        return t;
    };
    g_curScale = slider(Str::CurScale, g_curScaleVal, IDC_CUR_SCALE, 2, 8, 0, g_cur.scale);
    g_curHold  = slider(Str::CurHold,  g_curHoldVal,  IDC_CUR_HOLD,  5, 50, 5, g_cur.holdMs / 100);
    y += 4;
    g_curOverlay = check(addC, Str::CurOverlay, IDC_CUR_OVERLAY, g_cur.overlay);
    hint(addC, Str::CurOverlayHint);
    y += 2;
    g_curAdvBtn = button(addC, Str::Details, PX, 140, IDC_CUR_ADVANCED);
    y += 30 + 14;

    // «Детально»: чутливість жесту. Значення приймаються при втраті фокуса й
    // притискаються до робочого діапазону, щоб не можна було вимкнути фічу
    // випадковим нулем.
    auto advRow = [&](Str label, int id, int value) {
        addA(mkS(L"STATIC", label, 0, PX, y + 3, PW - 100, 18, 0));
        HWND e = addA(mk(L"EDIT", L"", ES_NUMBER | ES_RIGHT | WS_BORDER | WS_TABSTOP,
                         PX + PW - 90, y, 90, 24, id));
        wchar_t buf[16];
        wsprintfW(buf, L"%d", value);
        SetWindowTextW(e, buf);
        y += 28;
        return e;
    };
    g_edWindow = advRow(Str::CurAdvWindow, IDC_CUR_WINDOWMS,  g_cur.windowMs);
    g_edDist   = advRow(Str::CurAdvDist,   IDC_CUR_DIST,      g_cur.distance);
    g_edFactor = advRow(Str::CurAdvFactor, IDC_CUR_FACTOR,    g_cur.factor);
    g_edRevers = advRow(Str::CurAdvRevers, IDC_CUR_REVERSALS, g_cur.reversals);
    g_edShrink = advRow(Str::CurAdvShrink, IDC_CUR_SHRINK,    g_cur.shrinkMs);

    SetCursorValueLabels();

    // ---- вкладка «День/ніч» (CAPS-7) ----
    // (таб-контрол опускається на низ z-порядку нижче, після створення всіх сторінок)
    y = PY;
    g_thEnable = check(addT, Str::ThEnable, IDC_TH_ENABLE, g_th.enabled);
    g_thBySun   = radio(addT, Str::ThBySun,   PX,       230, IDC_TH_BY_SUN,   true);
    g_thBySched = radio(addT, Str::ThBySched, PX + 240, 170, IDC_TH_BY_SCHED, false);
    CheckRadioButton(hwnd, IDC_TH_BY_SUN, IDC_TH_BY_SCHED,
                     g_th.bySchedule ? IDC_TH_BY_SCHED : IDC_TH_BY_SUN);
    y += 26;
    g_thStatus = text(addT, Str::Empty, 2, IDC_TH_STATUS, 4);

    addT(mkS(L"STATIC", Str::ThDarkFrom, 0, PX, y + 4, 100, 20, 0));
    g_thDarkFrom = addT(mk(DATETIMEPICK_CLASSW, L"", DTS_TIMEFORMAT | DTS_UPDOWN | WS_TABSTOP,
                           PX + 104, y, 90, 26, IDC_TH_DARK_FROM));
    addT(mkS(L"STATIC", Str::ThLightFrom, 0, PX + 220, y + 4, 70, 20, 0));
    g_thLightFrom = addT(mk(DATETIMEPICK_CLASSW, L"", DTS_TIMEFORMAT | DTS_UPDOWN | WS_TABSTOP,
                            PX + 294, y, 90, 26, IDC_TH_LIGHT_FROM));
    y += 26 + 14;
    SetPickerMinutes(g_thDarkFrom,  g_th.darkFrom);
    SetPickerMinutes(g_thLightFrom, g_th.lightFrom);
    SetWindowSubclass(g_thDarkFrom,  DtpSubclassProc, 1, 0);   // CAPS-8: темний режим
    SetWindowSubclass(g_thLightFrom, DtpSubclassProc, 1, 0);

    g_thToggle = button(addT, Str::ThToggle, PX, 170, IDC_TH_TOGGLE);
    y += 30 + 8;
    g_thNow = text(addT, Str::Empty, 2, IDC_TH_NOW, 4);
    hint(addT, Str::ThFullscreenHint, 2);
    g_thAdvBtn = button(addT, Str::Details, PX, 140, IDC_TH_ADVANCED);
    y += 30 + 10;

    // «Детально»: звідки брати розташування для сходу/заходу
    text(addTA, Str::ThLocTitle, 1, 0, 4);
    {
        const Str names[5] = { Str::ThSrcAuto, Str::ThSrcWin, Str::ThSrcIp,
                               Str::ThSrcManual, Str::ThSrcTz };
        const int xs[5] = { PX, PX + 126, PX + 272, PX, PX + 126 };
        const int ys[5] = { 0, 0, 0, 24, 24 };
        const int ws[5] = { 120, 140, 148, 120, 200 };
        for (int i = 0; i < 5; ++i)
            g_thSrc[i] = addTA(mkS(L"BUTTON", names[i],
                BS_AUTORADIOBUTTON | (i == 0 ? (WS_GROUP | WS_TABSTOP) : 0),
                xs[i], y + ys[i], ws[i], 22, IDC_TH_SRC_AUTO + i));
        CheckRadioButton(hwnd, IDC_TH_SRC_AUTO, IDC_TH_SRC_TZ, IDC_TH_SRC_AUTO + (int)g_th.src);
        y += 24 + 28;
    }
    addTA(mkS(L"STATIC", Str::ThLat, 0, PX, y + 4, 60, 18, 0));
    g_thLat = addTA(mk(L"EDIT", L"", ES_RIGHT | WS_BORDER | WS_TABSTOP, PX + 64, y, 90, 24, IDC_TH_LAT));
    addTA(mkS(L"STATIC", Str::ThLon, 0, PX + 176, y + 4, 64, 18, 0));
    g_thLon = addTA(mk(L"EDIT", L"", ES_RIGHT | WS_BORDER | WS_TABSTOP, PX + 244, y, 90, 24, IDC_TH_LON));
    y += 24 + 8;
    hint(addTA, Str::ThVpnHint);
    if (g_th.hasManual) {
        wchar_t b[32];
        swprintf(b, 32, L"%.4f", g_th.lat); SetWindowTextW(g_thLat, b);
        swprintf(b, 32, L"%.4f", g_th.lon); SetWindowTextW(g_thLon, b);
    }

    // ---- вкладка «Перегляд» (CAPS-16) ----
    y = PY;
    // Помітка «експериментальна» — найперша на сторінці й звичайним кольором, а не
    // сірим, як підказки: її треба прочитати ДО того, як вирішувати щодо чекбокса.
    text(addP, Str::PeekExperimental, 2, 0, 10);
    g_peekEnableCb = check(addP, Str::PeekEnable, IDC_PEEK_ENABLE, g_peekOn, 2);
    hint(addP, Str::PeekHint, 2);
    y += 6;
    sec(addP, Str::PeekSecTypes);
    text(addP, Str::PeekTypesImages, 2, 0, 4);
    text(addP, Str::PeekTypesText,   2, 0, 4);
    text(addP, Str::PeekTypesMedia,  2, 0, 4);
    text(addP, Str::PeekTypesOther,  2, 0, 4);
    text(addP, Str::PeekZoomHint,    2, IDC_HINT_GRAY, 10);
    sec(addP, Str::PeekSecKeeps);
    text(addP, Str::PeekKeeps, 3, 0, 8);

    // ---- вкладка «Налаштування» (CAPS-9) ----
    y = PY;
    g_checkbox = check(addS, Str::SetAutostart, IDC_AUTOSTART, false);
    hint(addS, Str::SetAutostartHint, 2);
    y += 6;
    sec(addS, Str::SetSecLang);   // CAPS-12
    radio(addS, Str::SetLangSystem, PX,       130, IDC_LANG_SYSTEM, true);
    radio(addS, Str::SetLangUk,     PX + 140, 130, IDC_LANG_UK,     false);
    radio(addS, Str::SetLangEn,     PX + 280, 130, IDC_LANG_EN,     false);
    CheckRadioButton(hwnd, IDC_LANG_SYSTEM, IDC_LANG_EN, IDC_LANG_SYSTEM + (int)g_langPref);
    y += 26;
    hint(addS, Str::SetLangHint);
    y += 6;
    sec(addS, Str::SetSecTheme);   // CAPS-8
    radio(addS, Str::SetThAuto,  PX,       130, IDC_WT_AUTO,  true);
    radio(addS, Str::SetThLight, PX + 140, 130, IDC_WT_LIGHT, false);
    radio(addS, Str::SetThDark,  PX + 280, 130, IDC_WT_DARK,  false);
    CheckRadioButton(hwnd, IDC_WT_AUTO, IDC_WT_DARK, IDC_WT_AUTO + (int)g_winTheme);
    y += 26;
    hint(addS, Str::SetThHint);
    y += 6;
    sec(addS, Str::SetSecUpd);    // CAPS-10
    g_updDailyCb = check(addS, Str::UpdDaily, IDC_UPD_DAILY, g_updDaily);
    g_updStatus  = text(addS, Str::Empty, 2, IDC_UPD_STATUS, 8);
    g_updCheckBtn    = button(addS, Str::UpdCheck,    PX,       150, IDC_UPD_CHECK);
    g_updInstallBtn  = button(addS, Str::UpdInstall,  PX + 160, 110, IDC_UPD_INSTALL);
    g_updRollbackBtn = button(addS, Str::UpdRollback, PX + 280, 140, IDC_UPD_ROLLBACK);
    y += 30 + 12;
    hint(addS, Str::UpdHint, 2);
    UpdateUpdStatus();

    // Таб-контрол — НА САМИЙ НИЗ z-порядку. Попри те, що він створений першим,
    // дамп z-порядку (15.09.2026) показав його НАД усіма сторінками — тому кожне
    // його перемальовування (наведення на заголовок) зафарбовувало контроли
    // полотном, а WS_CLIPSIBLINGS не рятував: він вирізає лише сусідів ВИЩЕ.
    // Тепер сторінки завжди вище таба, і разом із WS_CLIPSIBLINGS полотно
    // ніколи не лягає поверх них.
    SetWindowPos(g_tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    SelectTab(0);

    // Контрол, що не вліз у масив сторінки, ніколи не сховається при перемиканні
    // вкладок — саме так у 2.1.0 «Оновити» лишалась поверх усіх вкладок. Повідомлення
    // для розробника (не локалізоване): користувач його не побачить, бо запас великий.
    if (g_pageOverflow)
        MessageBoxW(hwnd, L"Page control array overflow - raise the capacity.",
                    kAppName, MB_ICONERROR | MB_OK);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd   = hwnd;
    g_nid.uID    = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WMAPP_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), 0);
    lstrcpyW(g_nid.szTip, L"Little Helpers");
    // CAPS-17: спершу дозвіл на broadcast (інакше втрачену іконку вже нічим не повернути),
    // і лише потім перша спроба — щоб не проґавити TaskbarCreated у проміжку.
    ChangeWindowMessageFilterEx(hwnd, g_taskbarCreatedMsg, MSGFLT_ALLOW, nullptr);
    TrayEnsure(hwnd);

    g_mainWnd = hwnd;
    ApplyWindowTheme(true);   // CAPS-8: тема вікна до першого показу
    SetTimer(hwnd, TIMER_UPDATE, 60 * 1000, nullptr);   // CAPS-10: перша перевірка за хвилину

    // CAPS-7: одразу привести тему до часу доби; координати — з кешу, свіжі у фоні.
    EnableThemeControls();
    if (g_th.enabled) {
        SetTimer(hwnd, TIMER_THEME, 60 * 1000, nullptr);
        if (!g_th.bySchedule && (!g_fix.ok || NowUnix() - g_fix.at > 6 * 3600))
            StartLocate();
        ThemeTick();
    } else {
        UpdateThemeStatus();
    }

    // CAPS-1: стежимо за зміною активного вікна, щоб знати, коли ми в remote/VM.
    g_inRemote = IsRemoteWindow(GetForegroundWindow());
    g_winEvent = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                 nullptr, WinEventProc, 0, 0,
                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    StartHookThread();  // має бути до StartInterception у режимі Hook
    ApplyPeekFeature(); // CAPS-16: хук потрібен і перегляду, навіть без розкладки
    ApplyCursorFeature();  // CAPS-2: мишачий хук на тому ж потоці
    g_mode = LoadMode();
    // CAPS-9: перехоплення лише якщо перемикання ввімкнено; інакше програма живе
    // заради курсора/дня-ночі, а Caps Lock лишається звичайним.
    if (g_layoutOn && !StartInterception(g_mode)) {
        // збережений режим не піднявся — пробуємо інший, щоб утиліта не була мертвою
        Mode other = (g_mode == Mode::Hook) ? Mode::Hotkey : Mode::Hook;
        if (!StartInterception(other)) {
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            MessageBoxW(nullptr, S(Str::MsgHookFailed), kAppName, MB_ICONERROR | MB_OK);
            return 1;
        }
        g_mode = other;
    }
    CheckRadioButton(hwnd, IDC_MODE_HOOK, IDC_MODE_HOTKEY,
                     g_mode == Mode::Hook ? IDC_MODE_HOOK : IDC_MODE_HOTKEY);
    SetLayoutControlsEnabled(hwnd);
    UpdateModeHint();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) { // Tab/Space у вікні налаштувань
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    MagnifyRestore();   // страховка, якщо цикл завершився повз WM_DESTROY
    DeleteCriticalSection(&g_magLock);
    StopInterception();
    StopHookThread();
    if (g_winEvent) UnhookWinEvent(g_winEvent);
    delete g_logo;
    if (g_mfStarted) MFShutdown();   // CAPS-16
    if (g_d2d) g_d2d->Release();
    if (g_wic) g_wic->Release();
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    CoUninitialize();
    return 0;
}
