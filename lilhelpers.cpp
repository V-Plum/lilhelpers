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
constexpr UINT TIMER_MAG_HOLD   = 1;
constexpr UINT TIMER_MAG_FRAME  = 2;   // кадр оверлейної анімації
constexpr UINT TIMER_THEME      = 3;   // CAPS-7: перевірка теми раз на хвилину
constexpr UINT TIMER_UPDATE     = 4;   // CAPS-10: хвилина після старту, далі кожні 30 хв

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
X(PeekTypesImages,    L"Зображення: JPEG, PNG, GIF, BMP, TIFF, ICO — з поворотом за EXIF.",             \
                      L"Images: JPEG, PNG, GIF, BMP, TIFF, ICO — rotated by EXIF.")                     \
X(PeekTypesText,      L"Текст і код: txt, md, json, xml, yaml, csv, ini, log, скрипти й вихідний код "  \
                      L"— до 1 МБ; кодування UTF-8, UTF-16 або системне ANSI.",                         \
                      L"Text and code: txt, md, json, xml, yaml, csv, ini, log, scripts and source "    \
                      L"— up to 1 MB; UTF-8, UTF-16 or the system ANSI page.")                          \
X(PeekTypesOther,     L"Решта файлів і папки — картка: значок, тип, розмір, дати.",                     \
                      L"Everything else — a card: icon, type, size, dates.")                            \
X(PeekSecKeeps,       L"Що лишається за Провідником",   L"What stays with Explorer")                    \
X(PeekKeeps,          L"Ctrl + пробіл і Shift + пробіл, пошук набором літер, пробіл у полях адреси, "   \
                      L"пошуку й перейменування, а також у діалогах відкриття та збереження файлів.",   \
                      L"Ctrl + Space and Shift + Space, type-to-search, Space in the address, search "  \
                      L"and rename boxes, and the Open/Save file dialogs.")                             \
/* вікно перегляду */                                                                                  \
X(PeekFmtImage,       L"%d × %d · %s",                  L"%d × %d · %s")                                \
X(PeekFmtTwo,         L"%s · %s",                       L"%s · %s")                                     \
X(PeekFmtItems,       L"елементів: %d%s",               L"items: %d%s")                                 \
X(PeekLblType,        L"Тип",                           L"Type")                                        \
X(PeekLblSize,        L"Розмір",                        L"Size")                                        \
X(PeekLblItems,       L"Елементів",                     L"Items")                                       \
X(PeekLblTarget,      L"Веде до",                                                                    \
                      L"Points to")                                                                  \
X(PeekFmtImageAnim,   L"%d × %d · кадрів: %d · %s",                                                  \
                      L"%d × %d · frames: %d · %s")                                                  \
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
bool     g_peekTracking = false;
bool     g_peekDark     = false;
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
                                        L".bmp", L".dib", L".tif", L".tiff", L".ico", L".emf", L".wmf" };
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
    if (trunc) {
        const wchar_t* note = S(Str::PeekTruncated);
        text.push_back(L'\n'); text.push_back(L'\n');
        text.insert(text.end(), note, note + lstrlenW(note));
    }
    std::vector<wchar_t> crlf;
    NormalizeNewlines(text, crlf);
    SendMessageW(g_peekEdit, WM_SETFONT, (WPARAM)(IsProseExt(ext) ? g_peekFont : g_peekFontMono), FALSE);
    // WM_SETFONT скидає поля EDIT до типових — повертаємо після кожної зміни шрифту
    SendMessageW(g_peekEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(PeekPx(6), PeekPx(12)));
    SetWindowTextW(g_peekEdit, crlf.data());
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

// ---- завантаження елемента ----

void PeekReset()
{
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
        swprintf(I.subtitle, 320, S(Str::PeekFmtTwo), I.type, I.size);
        g_peekKind = PeekKind::Text;
        return;
    }
    swprintf(I.subtitle, 320, S(Str::PeekFmtTwo), I.type, I.size);
    g_peekKind = PeekKind::Card;
}

// ---- вікно перегляду: геометрія, тема, малювання ----

RECT PeekCloseRect(const RECT& rc)
{
    const int s = PeekPx(kPeekHead);
    return { rc.right - s, rc.top, rc.right, rc.top + s };
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
    InvalidateRect(g_peekWnd, nullptr, TRUE);
    RedrawWindow(g_peekEdit, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
}

void PeekLayout(HWND hwnd)
{
    RECT c = PeekContentRect(hwnd);
    if (g_peekKind == PeekKind::Text) {
        // Поле трохи менше за область вмісту: текст не притискається до країв
        c.left += PeekPx(12); c.right -= PeekPx(4); c.top += PeekPx(10); c.bottom -= PeekPx(8);
        SetWindowPos(g_peekEdit, nullptr, c.left, c.top, c.right - c.left, c.bottom - c.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
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
        w = PeekPx(560);
        h = head + PeekPx(232);
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
    RECT nameR = { x, rc.top + PeekPx(7), closeR.left - PeekPx(8), rc.top + PeekPx(7) + PeekPx(20) };
    HGDIOBJ old = SelectObject(dc, g_peekFontBold);
    SetTextColor(dc, text);
    DrawTextW(dc, g_peekInfo.name, -1, &nameR, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT subR = { x, nameR.bottom, closeR.left - PeekPx(8), rc.top + head - PeekPx(4) };
    SelectObject(dc, g_peekFont);
    SetTextColor(dc, gray);
    DrawTextW(dc, g_peekInfo.subtitle, -1, &subR, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
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
        double scale = 1.0;
        if (g_peekInfo.imgW > cw) scale = (double)cw / g_peekInfo.imgW;
        if (g_peekInfo.imgH * scale > ch) scale = (double)ch / g_peekInfo.imgH;
        const int dw = (int)(g_peekInfo.imgW * scale + 0.5), dh = (int)(g_peekInfo.imgH * scale + 0.5);
        const int dx = c.left + (cw - dw) / 2, dy = c.top + (ch - dh) / 2;
        Gdiplus::Graphics g(dc);
        if (dw == g_peekInfo.imgW && dh == g_peekInfo.imgH) {
            g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.DrawImage(g_peekImg, dx, dy, dw, dh);
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
            g.DrawImage(g_peekScaled, dx, dy, dw, dh);
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
        row(Str::PeekLblType, g_peekInfo.type);
        if (g_peekInfo.isDir) row(Str::PeekLblItems, items);
        else                  row(Str::PeekLblSize,  g_peekInfo.size);
        row(Str::PeekLblTarget, g_peekInfo.target);
        row(Str::PeekLblModified, g_peekInfo.modified);
        row(Str::PeekLblCreated,  g_peekInfo.created);
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
        const RECT closeR = PeekCloseRect(rc);
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const bool hot = PtInRect(&closeR, pt) != FALSE;
        if (hot != g_peekCloseHot) { g_peekCloseHot = hot; InvalidateRect(hwnd, &closeR, FALSE); }
        if (!g_peekTracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_peekTracking = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_peekTracking = false;
        if (g_peekCloseHot) { g_peekCloseHot = false; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;

    case WM_LBUTTONUP: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        const RECT closeR = PeekCloseRect(rc);
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (PtInRect(&closeR, pt)) PeekClose();
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

    g_peekEdit = CreateWindowExW(0, L"EDIT", L"",
                                 WS_CHILD | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                                 0, 0, 10, 10, g_peekWnd, nullptr, hInst, nullptr);
    SendMessageW(g_peekEdit, EM_SETLIMITTEXT, 0, 0);
    SendMessageW(g_peekEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(PeekPx(14), PeekPx(14)));
    {
        int tab = 16;   // діалогові одиниці ≈ 4 символи
        SendMessageW(g_peekEdit, EM_SETTABSTOPS, 1, (LPARAM)&tab);
    }
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

void ShowTrayMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, S(Str::MenuSettings));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, S(Str::MenuExit));
    SetForegroundWindow(hwnd); // інакше меню не закриється кліком повз
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg) {
        // Explorer перезапустився — повертаємо іконку в трей
        Shell_NotifyIconW(NIM_ADD, &g_nid);
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
    constexpr int W = 500, H = 606;
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
    text(addP, Str::PeekExperimental, 2, 0, 14);
    g_peekEnableCb = check(addP, Str::PeekEnable, IDC_PEEK_ENABLE, g_peekOn, 2);
    hint(addP, Str::PeekHint, 2);
    y += 6;
    sec(addP, Str::PeekSecTypes);
    text(addP, Str::PeekTypesImages, 1, 0, 6);
    text(addP, Str::PeekTypesText,   2, 0, 6);
    text(addP, Str::PeekTypesOther,  1, 0, 12);
    y += 6;
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
    Shell_NotifyIconW(NIM_ADD, &g_nid);

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
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    CoUninitialize();
    return 0;
}
