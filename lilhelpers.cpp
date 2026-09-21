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
// ⚠ Заголовки WinRT у MinGW мають зіткнення: IReference<boolean> і
// IReference<BYTE> там — та сама спеціалізація (boolean = unsigned char), і
// друга з них не компілюється. Глушимо перший блок його ж вартовим макросом:
// нам потрібні зовсім інші інтерфейси, а MSVC цей рядок просто не помітить.
#define ____FIReference_1_boolean_INTERFACE_DEFINED__
#include <windows.foundation.h>
#include <windows.storage.streams.h>
#include <windows.storage.h>
#include <windows.applicationmodel.datatransfer.h>
#include <shobjidl.h>            // CAPS-36: IDataTransferManagerInterop
#include <d2d1_3.h>
#include <d2d1svg.h>
// CAPS-24: текст і кольорові емодзі — DirectWrite. GDI+ не знає ні COLR/CBDT,
// ні сучасного розкладання гліфів, тому весь текст редактора йде через нього.
#include <dwrite.h>
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
#include <sddl.h>
// CAPS-21: захоплення екрана через Desktop Duplication — системні DXGI і D3D11.
#include <dxgi1_6.h>
#include <d3d11.h>
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
constexpr UINT WMAPP_EDTEXT       = WM_APP + 9;   // CAPS-24: поле вводу напису; wp = 1 зафіксувати, 0 скасувати
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

constexpr int  IDC_CAP_HK1       = 190;   // CAPS-21: три поля перехоплення
constexpr int  IDC_CAP_HKRESET   = 193;
constexpr int  IDC_CAP_KEEPTOOL  = 194;   // CAPS-23
constexpr int  IDC_CAP_SHAREUNLOCK = 195; // CAPS-52
constexpr int  IDR_LOGO_PNG    = 100;  // RCDATA з lilhelpers.png
constexpr int  HOTKEY_ID       = 1;
constexpr UINT IDM_SETTINGS    = 1;
constexpr UINT IDM_EXIT        = 2;
constexpr UINT IDM_EDITOR      = 3;   // CAPS-20: редактор знімків
constexpr UINT IDM_CAPSCREEN   = 4;   // CAPS-21: знімок екрана
constexpr UINT IDM_CAPWINDOW   = 5;   // CAPS-21: знімок активного вікна
constexpr UINT IDM_CAPREGION   = 6;   // CAPS-21: знімок ділянки
constexpr UINT IDM_CAPCLIP     = 7;   // CAPS-21: з буфера обміну
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
// CAPS-52: 1 = дозволити непривілейованим процесам викликати наші COM-обʼєкти.
// Типово 0: це послаблення захисту елевейтованого процесу, і вмикати його має
// сенс лише тому, кому справді потрібне системне меню поширення.
const wchar_t* kRegShareUnlock  = L"ShareUnlock";
bool g_shareUnlock = false;
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
X(EdCapRegion,        L"Знімок ділянки",                L"Region shot")                                \
X(EdCapClip,          L"З буфера обміну",               L"From clipboard")                             \
X(EdErrClip,          L"У буфері обміну немає зображення.", L"No image in the clipboard.")             \
X(CapHkNone,          L"не задано",                     L"not set")                                    \
X(CapHkBusy,          L"Частину гарячих клавіш тримає інша програма — знімки по них не працюватимуть.", \
                      L"Another program holds some hotkeys; those shortcuts will not work.")           \
X(TabShots,           L"Знімки",                        L"Shots")                                      \
X(CapSecHotkeys,      L"Гарячі клавіші",                L"Hotkeys")                                    \
X(CapHkClipL,         L"Зображення з буфера",           L"From clipboard")                             \
X(CapHkPress,         L"натисніть комбінацію…",         L"press a combination…")                       \
X(CapHkTaken,         L"зайнято іншою програмою",       L"held by another app")                        \
X(CapHkOff,           L"вимкнено",                      L"off")                                        \
X(CapHkDefaults,      L"Повернути типові",              L"Restore defaults")                           \
X(CapHkHint,          L"Клацніть поле і натисніть комбінацію з Ctrl, Alt або Shift. Якщо її вже "      \
                      L"тримає інша програма, поле лишиться як було й скаже про це. Backspace "        \
                      L"вимикає клавішу зовсім.",                                                       \
                      L"Click a field and press a combination with Ctrl, Alt or Shift. If another "    \
                      L"app already holds it, the field stays as it was and says so. Backspace "       \
                      L"turns the shortcut off.")                                                       \
X(EdCopy,             L"Копіювати",                     L"Copy")                                       \
X(EdSaveAs,           L"Зберегти як",                   L"Save as")                                    \
X(EdSaveName,         L"Знімок",                        L"Shot")                                       \
X(EdSaveTitle,        L"Зберегти знімок",               L"Save screenshot")                            \
X(EdFmtPng,           L"Зображення PNG",                L"PNG image")                                  \
X(EdFmtJpg,           L"Зображення JPEG",               L"JPEG image")                                 \
X(EdCopied,           L"Скопійовано",                   L"Copied")                                     \
X(EdSaved,            L"Збережено",                     L"Saved")                                      \
X(EdErrSave,          L"Не вдалося зберегти файл.",     L"Could not save the file.")                   \
X(EdErrCopy,          L"Не вдалося покласти в буфер обміну.", L"Could not copy to the clipboard.")     \
X(EdAskDiscard,       L"Закрити редактор? Позначки не збережено.",                                     \
                      L"Close the editor? Marks are not saved.")                                       \
X(CapSecOutput,       L"Малювання",                     L"Drawing")                                    \
X(CapCloseAfterCopy,  L"Закривати редактор після копіювання",                                          \
                      L"Close the editor after copying")                                               \
X(EdCapScreen,        L"Знімок екрана",                 L"Screenshot")                                 \
X(EdCapWindow,        L"Знімок вікна",                  L"Window shot")                                \
X(EdFmtHdr,           L"HDR · біле SDR %d ніт · зведено",                                              \
                      L"HDR · SDR white %d nits · mapped")                                            \
X(EdHdrNote,          L"HDR · тон-мапінг застосовано",  L"HDR · tone-mapped")                          \
X(EdErrCapture,       L"Не вдалося зробити знімок екрана.", L"Could not capture the screen.")          \
X(EdMenu,             L"Редактор знімків…",             L"Screenshot editor…")                         \
X(EdTitle,            L"Редактор знімків",              L"Screenshot editor")                          \
X(EdToolEllipse,      L"Еліпс",                         L"Ellipse")                                    \
X(EdToolLine,         L"Лінія",                         L"Line")                                       \
X(EdToolPen,          L"Олівець",                       L"Pencil")                                     \
X(EdToolText,         L"Текст",                         L"Text")                                       \
X(EdToolHide,         L"Приховати",                     L"Hide")                                       \
X(EdToolMark,         L"Маркер",                        L"Marker")                                     \
X(EdToolCounter,      L"Лічильник",                     L"Counter")                                    \
X(EdToolStamp,        L"Штамп",                         L"Stamp")                                      \
X(EdKindImage,        L"Зображення",                    L"Image")                                      \
X(EdFmtPicked,        L"Вибрано: %d",                   L"Selected: %d")                               \
X(EdSizeImgTitle,     L"Розмір зображення",             L"Image size")                                 \
X(EdSizeCanTitle,     L"Розмір полотна",                L"Canvas size")                                \
X(EdSizeW,            L"Ширина",                        L"Width")                                      \
X(EdSizeH,            L"Висота",                        L"Height")                                     \
X(EdSizePct,          L"Відсоток",                      L"Percent")                                    \
X(EdSizeKeep,         L"Тримати пропорції",             L"Keep proportions")                           \
X(EdSizeText,         L"Масштабувати текст разом із зображенням",                                      \
                                                        L"Scale text along with the image")            \
X(EdSizeSharp,        L"Різко (без згладжування)",      L"Sharp (no smoothing)")                       \
X(EdSizeApply,        L"Змінити",                       L"Resize")                                     \
X(EdSizeCancel,       L"Скасувати",                     L"Cancel")                                     \
X(EdFmtSizeNow,       L"Зараз: %d × %d",                L"Now: %d × %d")                               \
X(EdSizeNoteImg,      L"Товщина ліній, кружечки й штампи лишаться свого розміру.\n"                     \
                      L"Зменшення втрачає пікселі назавжди — повернути їх зможе лише скасування.",     \
                      L"Line widths, counters and stamps keep their size.\n"                           \
                      L"Shrinking loses pixels for good — only undo brings them back.")                \
X(EdSizeNoteCan,      L"Знімок стане окремим об\x2019єктом, а тло навколо — прозорим.",                  \
                      L"The shot becomes a separate object and the space around it stays clear.")      \
X(EdBtnSizeImg,       L"Розмір зображення…",            L"Image size…")                                \
X(EdShare,            L"Поділитися",                    L"Share")                                      \
X(EdTipShare,         L"Системне меню поширення Windows", L"The Windows share menu")                   \
X(EdErrShare,         L"Системне меню поширення не відкрилось.",                                       \
                                                        L"The system share menu did not open.")        \
X(CapShareUnlock,     L"Дозволити меню поширення діставати до програми",                               \
                      L"Let the share menu reach into this program")                                   \
X(CapShareUnlockHint, L"Потрібно для «Поділитися». Відкриває виклики до програми процесам зі "           \
                      L"звичайними правами — вмикайте лише за потреби. Діє після перезапуску.",         \
                      L"Needed for Share. It opens calls into this program to normal-privilege "        \
                      L"processes — turn it on only if you need it. Takes effect after a restart.")     \
X(EdErrShareQuiet,    L"Windows відкрила меню поширення, але даних у програми так і не спитала — "      \
                      L"приймач отримав би порожнечу.\n\nПричина: програма працює з правами "          \
                      L"адміністратора (це потрібно для перехоплення CapsLock), а меню поширення й "    \
                      L"самі приймачі — ні, і без окремого дозволу дістати до неї не можуть.\n\n"      \
                      L"Увімкніть «Дозволити меню поширення діставати до програми» в налаштуваннях, "   \
                      L"на вкладці «Знімки», і перезапустіть програму. Прочитайте там опис: дозвіл "    \
                      L"послаблює захист, тож вмикайте його, лише якщо потрібне саме поширення.\n\n"   \
                      L"Без нього робочі шляхи ті самі: «Копіювати» (Ctrl+C) або «Експорт».",           \
                      L"Windows opened the share menu but never asked this program for the data, "      \
                      L"so the target would have received nothing.\n\nThe cause: this program runs "   \
                      L"elevated (needed for the CapsLock hook) while the share menu and the targets "  \
                      L"do not, and without an explicit permission they cannot reach it.\n\n"          \
                      L"Turn on \"Let the share menu reach into this program\" in Settings, on the "     \
                      L"Snapshots tab, then restart. Read the note there: it weakens protection, so "   \
                      L"enable it only if you need sharing.\n\n"                                       \
                      L"Without it, use Copy (Ctrl+C) or Export.")                                     \
X(EdErrShareStill,    L"Windows відкрила меню поширення, але даних у програми так і не спитала.\n\n"   \
                      L"Дозвіл для меню поширення вже ввімкнено, отже справа не в ньому — причина "     \
                      L"інша. Скажіть про це, будь ласка: це потрібно розбирати окремо.\n\n"           \
                      L"Поки що: «Копіювати» (Ctrl+C) або «Експорт».",                                 \
                      L"Windows opened the share menu but never asked this program for the data.\n\n"  \
                      L"The share permission is already on, so that is not the cause — something "      \
                      L"else is. Please report it; this needs a separate look.\n\n"                    \
                      L"For now: Copy (Ctrl+C) or Export.")                                            \
X(EdBtnSizeCan,       L"Розмір полотна…",               L"Canvas size…")                               \
X(EdTipSizeImg,       L"Змінити розмір самого зображення", L"Resize the image itself")                 \
X(EdTipSizeCan,       L"Змінити розмір полотна",        L"Resize the canvas")                          \
X(EdTipAlL,           L"Вирівняти за лівим краєм",      L"Align left edges")                           \
X(EdTipAlCx,          L"Вирівняти по центру вертикалі", L"Align vertical centres")                     \
X(EdTipAlR,           L"Вирівняти за правим краєм",     L"Align right edges")                          \
X(EdTipAlT,           L"Вирівняти за верхнім краєм",    L"Align top edges")                            \
X(EdTipAlCy,          L"Вирівняти по центру горизонталі", L"Align horizontal centres")                 \
X(EdTipAlB,           L"Вирівняти за нижнім краєм",     L"Align bottom edges")                         \
X(EdTipDistX,         L"Рівні проміжки по горизонталі", L"Even gaps across")                           \
X(EdTipDistY,         L"Рівні проміжки по вертикалі",   L"Even gaps down")                             \
X(EdTipMakeGroup,     L"Згрупувати",                    L"Group")                                      \
X(EdTipUngroup,       L"Розгрупувати",                  L"Ungroup")                                    \
X(EdToolCrop,         L"Кадр",                          L"Crop")                                       \
X(EdCropApply,        L"Застосувати",                   L"Apply")                                      \
X(EdCropCancel,       L"Скасувати",                     L"Cancel")                                     \
X(EdCropReset,        L"Скинути кадр",                  L"Reset the frame")                            \
X(EdAspectFree,       L"Вільно",                        L"Free")                                       \
X(EdFmtOutside,       L"Поза кадром: %d",               L"Outside the frame: %d")                      \
X(EdTipAspect,        L"Пропорції кадру",               L"Frame proportions")                          \
X(EdOutline,          L"Контур",                        L"Outline")                                    \
X(EdFilled,           L"Заливка",                       L"Filled")                                     \
X(CapKeepTool,        L"Лишати інструмент активним після малювання",                                   \
                      L"Keep the tool active after drawing")                                           \
X(EdToolRect,         L"Прямокутник",                   L"Rectangle")                                  \
X(EdSelHint,          L"Виберіть позначку, щоб змінити її колір, прозорість або розмір.",              \
                      L"Select a mark to change its colour, opacity or size.")                         \
X(EdNoSel,            L"Нічого не вибрано",             L"Nothing selected")                           \
X(EdFmtSel,           L"Вибране %d × %d",               L"Picked %d × %d")                             \
X(EdFit,              L"Вписати",                       L"Fit")                                        \
X(EdSecShot,          L"ЗНІМОК",                        L"IMAGE")                                      \
X(EdFmtSource,        L"Зображення · %d × %d",          L"Image · %d × %d")                            \
X(EdFmtMarks,         L"Позначок: %d",                  L"Marks: %d")                                  \
X(EdSecTone,          L"ТОН",                           L"TONE")                                       \
X(EdExposure,         L"Експозиція",                    L"Exposure")                                   \
X(EdGamma,            L"Гама",                          L"Gamma")                                      \
X(EdContrast,         L"Контраст",                      L"Contrast")                                   \
X(EdToneReset,        L"Скинути",                       L"Reset")                                      \
X(EdCompare,          L"Порівняти",                     L"Compare")                                    \
X(EdOriginal,         L"вихідний кадр",                 L"original frame")                             \
X(EdTipRotL,          L"Повернути ліворуч",             L"Rotate left")                                \
X(EdTipRotR,          L"Повернути праворуч",            L"Rotate right")                               \
X(EdTipFlipH,         L"Дзеркало по горизонталі",       L"Mirror horizontally")                        \
X(EdTipFlipV,         L"Дзеркало по вертикалі",         L"Mirror vertically")                          \
X(EdTipExposure,      L"Експозиція, EV",                L"Exposure, EV")                               \
X(EdTipGamma,         L"Гама: середні тони",            L"Gamma: midtones")                            \
X(EdTipContrast,      L"Контраст",                      L"Contrast")                                   \
X(EdTipToneReset,     L"Повернути тон, як було при захопленні",                                        \
                                                        L"Back to the tone as captured")               \
X(EdTipCompare,       L"Тримайте, щоб побачити вихідний кадр",                                         \
                                                        L"Hold to see the original frame")             \
X(EdOpenTitle,        L"Відкрити зображення",           L"Open image")                                 \
X(EdOpenBtn,          L"Відкрити",                      L"Open")                                       \
X(EdToolSelect,       L"Вибір",                         L"Select")                                     \
X(EdTipColor,         L"Колір позначки",                L"Mark colour")                                \
X(EdTipColorGroup,    L"Колір кружечка · подвійний клік — на всю групу",                                \
                                                        L"Circle colour · double-click applies to the whole group") \
X(EdTipSizeGroup,     L"Розмір кружечка · подвійний клік — на всю групу",                               \
                                                        L"Circle size · double-click applies to the whole group")   \
X(EdTipGroupEdit,     L"Редагування групи: колір, розмір і прозорість — усім кружечкам",                \
                                                        L"Group editing: colour, size and opacity go to every circle") \
X(EdTipGroupDel,      L"Видалити всю групу",            L"Delete the whole group")                     \
X(EdTipThick,         L"Товщина лінії",                 L"Line width")                                 \
X(EdTipDash,          L"Стиль лінії",                   L"Line style")                                 \
X(EdTipHeadFront,     L"Передній наконечник",           L"Head at the end")                            \
X(EdTipHeadBack,      L"Задній наконечник",             L"Head at the start")                          \
X(EdTipHeadSize,      L"Розмір наконечників",           L"Head size")                                  \
X(EdTipSizeDn,        L"Менший кегль",                  L"Smaller type")                               \
X(EdTipSizeUp,        L"Більший кегль",                 L"Larger type")                                \
X(EdTipBold,          L"Жирний",                        L"Bold")                                       \
X(EdTipItalic,        L"Курсив",                        L"Italic")                                     \
X(EdTipAlignL,        L"Рядки ліворуч",                 L"Align left")                                 \
X(EdTipAlignC,        L"Рядки по центру",               L"Align centre")                               \
X(EdTipAlignR,        L"Рядки праворуч",                L"Align right")                                \
X(EdTipStroke0,       L"Без обводки",                   L"No outline")                                 \
X(EdTipStroke1,       L"Світла обводка",                L"Light outline")                              \
X(EdTipStroke2,       L"Темна обводка",                 L"Dark outline")                               \
X(EdTipAlpha,         L"Прозорість",                    L"Opacity")                                    \
X(EdTipFront,         L"На передній план",              L"Bring to front")                             \
X(EdTipBack,          L"На задній план",                L"Send to back")                               \
X(EdTipDup,           L"Дублювати (Ctrl+D)",            L"Duplicate (Ctrl+D)")                         \
X(EdTipDel,           L"Видалити (Delete)",             L"Delete (Delete)")                            \
X(EdTipUndo,          L"Скасувати (Ctrl+Z)",            L"Undo (Ctrl+Z)")                              \
X(EdTipRedo,          L"Повторити (Ctrl+Y)",            L"Redo (Ctrl+Y)")                              \
X(EdTipHelp,          L"Довідка (F1)",                  L"Help (F1)")                                  \
X(EdZoom100,          L"100 %",                         L"100 %")                                      \
X(EdTipZoom,          L"Масштаб перегляду",             L"View scale")                                 \
X(EdTipZoom100,       L"Піксель у піксель (100 %)",     L"Pixel for pixel (100 %)")                    \
X(EdTipFit,           L"Вписати у вікно",               L"Fit to window")                              \
X(EdTipPanelHide,     L"Згорнути панель",               L"Collapse the panel")                         \
X(EdTipPanelShow,     L"Розгорнути панель",             L"Expand the panel")                           \
X(EdTipOpenMore,      L"Інші джерела зображення",       L"Other image sources")                        \
X(EdTipMin,           L"Згорнути",                      L"Minimise")                                   \
X(EdTipMax,           L"Розгорнути",                    L"Maximise")                                   \
X(EdTipRestore,       L"Відновити розмір",              L"Restore down")                               \
X(EdTipClose,         L"Закрити",                       L"Close")                                      \
X(EdTipBlur,          L"Розмиття",                      L"Blur")                                       \
X(EdTipPixels,        L"Пікселі",                       L"Pixels")                                     \
X(EdTipPlate,         L"Суцільна плашка",               L"Solid plate")                                \
X(EdTipStrength,      L"Сила приховування",             L"Hiding strength")                            \
X(EdTipMarkH,         L"Висота смуги",                  L"Band height")                                \
X(EdTipSize,          L"Розмір",                        L"Size")                                       \
X(EdFmtGroupOnly,     L"Група %d",                      L"Group %d")                                   \
X(EdNumStartLabel,    L"Початок",                       L"Start")                                      \
X(EdFmtNext,          L"Наступний: %d",                 L"Next: %d")                                   \
X(EdTipGroup,         L"Виберіть вже вставлений елемент для вибору іншої групи",                       \
                      L"Pick an already placed circle to switch to its group")                        \
X(EdTipNumStart,      L"Початковий номер",              L"Starting number")                            \
X(EdTipNumReset,      L"Нова група: нумерація знову з початку",                                        \
                      L"New group: numbering starts over")                                             \
X(EdTipStampMore,     L"Більше емодзі",                 L"More emoji")                                 \
X(EdTipTextBox,       L"Ручками з боків — ширина блока; кегль — степером",                              \
                      L"Side handles set the block width; type size has a stepper")                    \
X(EdAskReplace,       L"Відкрити інше зображення? Позначки не збережено.",                             \
                      L"Open another image? Marks are not saved.")                                     \
X(EdOpenFilter,       L"Зображення",                    L"Images")                                     \
X(EdErrOpen,          L"Не вдалося відкрити зображення.", L"Could not open the image.")                \
X(EdHelpTitle,        L"Редактор знімків",              L"Screenshot editor")                          \
X(EdHelpBody,         L"Інструменти: V вибір, R прямокутник, E еліпс, L лінія,\n"                   \
                      L"P олівець, T текст, B приховати, H маркер,\nN лічильник, S штамп, C кадр.\n\n"   \
                      L"Shift під час малювання — квадрат, коло, кут через 45°\n"                       \
                      L"Текст: Enter — готово, Shift+Enter — новий рядок,\n"                            \
                      L"подвійний клік по напису — відкрити на правку\n"                                \
                      L"Стрілки — посунути вибране, з Shift — на 10 точок\n"                                  \
                      L"Delete — видалити вибране\nCtrl+Z, Ctrl+Y — скасувати й повторити\n"           \
                      L"Ctrl+O — відкрити, Ctrl+C — копіювати, Ctrl+S — зберегти\n"                                        \
                      L"Коліщатко — прокрутка, Ctrl — убік, Alt — масштаб\n"                                 \
                      L"Подвійний клік — вписати у вікно\n"                                \
                      L"Пробіл або середня кнопка — рухати полотно",                                    \
                      L"Tools: V select, R rectangle, E ellipse, L line,\n"                          \
                      L"P pencil, T text, B hide, H marker,\nN counter, S stamp, C crop.\n\n"            \
                      L"Shift while drawing — square, circle, 45° steps\n"                              \
                      L"Text: Enter finishes, Shift+Enter adds a line,\n"                               \
                      L"double click a caption to edit it again\n"                                      \
                      L"Arrows nudge the selection, with Shift by 10 points\n"                                \
                      L"Delete — remove the selection\nCtrl+Z, Ctrl+Y — undo and redo\n"               \
                      L"Ctrl+C — copy, Ctrl+S — save\n"                                                 \
                      L"Wheel scrolls, Ctrl sideways, Alt zooms\n"                                           \
                      L"Double click — fit to window\n"                                             \
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

constexpr int kTabCount = 6;
const Str kTabTitles[kTabCount] = { Str::TabLayout, Str::TabCursor, Str::TabTheme,
                                    Str::TabPeek, Str::TabShots, Str::TabSettings };

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
HWND  g_pageShots[24]    = {};  int g_pageShotsN = 0;  // CAPS-21

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
    for (int i = 0; i < g_pageShotsN; ++i)    if (g_pageShots[i]    == c) return true;
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
    ShowGroup(g_pageShots, g_pageShotsN, index == 4);        // CAPS-21
    ShowGroup(g_pageSettings, g_pageSettingsN, index == 5);
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
void CapHkRefresh();   // CAPS-21: підписи клавіш теж залежать від мови

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

// ===================== CAPS-21: захоплення екрана =====================
// Чому не BitBlt. У режимі HDR робочий стіл композиційно лежить у scRGB
// (FP16, лінійний) або PQ, а BitBlt віддає буфер БЕЗ тон-мапінгу в SDR — звідси
// вицвілі або темні знімки. Лікувати це «потім повзунком» не можна: оригінал
// уже втрачено. Тому захват іде через Desktop Duplication, яка разом із
// пікселями віддає й колірний простір виходу.
//
// Три речі, виміряні пробами (dupprobe1..3 у scratchpad), а не вгадані:
//
//  1. ПЕРШИЙ кадр після DuplicateOutput порожній: AccumulatedFrames = 0 і
//     суцільний чорний. Справжній приходить наступним. Пропускати кадри, доки
//     AccumulatedFrames == 0 && LastPresentTime == 0.
//  2. На НЕРУХОМОМУ екрані другий кадр усе одно приходить за 0..16 мс, тобто за
//     один інтервал оновлення. Штовхати екран власним вікном не треба — це
//     перевірено окремим прогоном, де проба мовчала, щоб не міняти екран сама.
//  3. RowPitch НЕ дорівнює width*4: на цій машині 6272 проти 6256. Той самий
//     клас помилки, що зрізав кадри відео в CAPS-16.
//
// HDR-гілку на цій машині перевірити нічим — тут SDR-вихід (ColorSpace 0,
// 8 біт). Вона написана за специфікаціями і чекає на живу перевірку власником.

constexpr int   kCapBudgetMs   = 1200;   // скільки чекаємо непорожній кадр
constexpr float kCapHdrFallback = 200.0f; // типове біле SDR у Windows при HDR
constexpr float kCapScrgbWhite = 80.0f;  // scRGB 1.0 = 80 ніт за визначенням

struct CapShot {
    Gdiplus::Bitmap* bmp;
    int   fmt;          // DXGI_FORMAT кадру — для рядка діагностики
    int   cs;           // колірний простір виходу
    bool  hdr;          // джерело було HDR
    bool  toneMapped;   // і ми його звели в SDR
    float sdrWhite;     // ніт; -1 якщо система не сказала
    int   w, h;
};

// Рівень білого SDR: скільки ніт коштує звичайна біла кнопка при ввімкненому
// HDR. Без нього немає від чого нормалізувати. Система інколи не відповідає
// (на цій машині саме так) — тоді беремо 80 ніт, тобто множник 1.
float CapSdrWhiteNits(const wchar_t* gdiDeviceName)
{
    UINT32 npath = 0, nmode = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &npath, &nmode) != ERROR_SUCCESS)
        return -1.0f;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(npath);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nmode);
    float out = -1.0f;
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &npath, paths.data(), &nmode, modes.data(),
                           nullptr) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < npath; ++i) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
            src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = sizeof(src);
            src.header.adapterId = paths[i].sourceInfo.adapterId;
            src.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
            if (lstrcmpiW(src.viewGdiDeviceName, gdiDeviceName)) continue;
            // ⚠ Тип запиту — 11 (DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL).
            // Тут довго стояло 26, запит мовчки падав, і тон-мапінг брав
            // запасні 80 ніт замість справжніх — звідси бліді HDR-знімки.
            // Структури немає в старих SDK, тому оголошено на місці.
            struct {
                DISPLAYCONFIG_DEVICE_INFO_HEADER header;
                ULONG SDRWhiteLevel;
            } wl = {};
            wl.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)11;
            wl.header.size = sizeof(wl);
            wl.header.adapterId = paths[i].targetInfo.adapterId;
            wl.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS)
                out = wl.SDRWhiteLevel / 1000.0f * kCapScrgbWhite;
            break;
        }
    }
    return out;
}

float CapHalfToFloat(unsigned short h)
{
    const unsigned s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v;
    if (e == 0)       v = m / 1024.0f * 6.103515625e-5f;          // субнормальні
    else if (e == 31) v = m ? 0.0f : 65504.0f;                    // NaN міняємо на 0, inf підрізаємо
    else              v = (1.0f + m / 1024.0f) * (float)pow(2.0, (int)e - 15);
    return s ? -v : v;
}

// Усе до білого SDR проходить БЕЗ ЗМІН: знімок інтерфейсу на HDR-екрані має
// виглядати рівно так, як він виглядає на екрані. Яскравіше за біле —
// зрізається. Мати водночас «біле лишається білим» і «яскравіше за біле ще
// розрізняється» у восьми бітах неможливо, і для знімків інтерфейсу вибір саме
// такий; відблиски HDR-відео поверне ручний повзунок експозиції.
float CapKnee(float n)
{
    if (n <= 0.0f) return 0.0f;
    return n < 1.0f ? n : 1.0f;
}

BYTE CapToSrgb8(float lin)
{
    if (lin <= 0.0f) return 0;
    if (lin >= 1.0f) return 255;
    const float s = (lin <= 0.0031308f) ? (12.92f * lin)
                                        : (1.055f * (float)pow(lin, 1.0 / 2.4) - 0.055f);
    int v = (int)(s * 255.0f + 0.5f);
    return (BYTE)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// PQ (SMPTE ST 2084) -> ніти.
float CapPqToNits(float e)
{
    const double m1 = 0.1593017578125, m2 = 78.84375;
    const double c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    if (e <= 0.0f) return 0.0f;
    const double p = pow((double)e, 1.0 / m2);
    double num = p - c1;
    if (num < 0.0) num = 0.0;
    const double den = c2 - c3 * p;
    if (den <= 0.0) return 10000.0f;
    return (float)(10000.0 * pow(num / den, 1.0 / m1));
}

struct CapOutput {
    IDXGIAdapter1* adapter;
    IDXGIOutput6*  output;
    RECT           rc;
    DXGI_COLOR_SPACE_TYPE cs;
    wchar_t        device[32];
};

// Знаходимо вихід, якому належить монітор. Перебирати треба ВСІ адаптери:
// на цій машині та сама відеокарта перелічується двічі, і виходи має лише одна
// з копій, а третім іде Microsoft Basic Render Driver узагалі без виходів.
bool CapFindOutput(HMONITOR mon, CapOutput* out)
{
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f)) || !f) return false;
    bool found = false;
    for (UINT ai = 0; !found; ++ai) {
        IDXGIAdapter1* a = nullptr;
        if (f->EnumAdapters1(ai, &a) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT oi = 0; !found; ++oi) {
            IDXGIOutput* o = nullptr;
            if (a->EnumOutputs(oi, &o) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od = {};
            o->GetDesc(&od);
            if (od.AttachedToDesktop && od.Monitor == mon) {
                IDXGIOutput6* o6 = nullptr;
                if (SUCCEEDED(o->QueryInterface(__uuidof(IDXGIOutput6), (void**)&o6)) && o6) {
                    out->adapter = a;
                    out->output  = o6;
                    out->rc      = od.DesktopCoordinates;
                    lstrcpynW(out->device, od.DeviceName, 32);
                    DXGI_OUTPUT_DESC1 d1 = {};
                    out->cs = SUCCEEDED(o6->GetDesc1(&d1)) ? d1.ColorSpace
                                                           : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
                    a->AddRef();
                    found = true;
                }
            }
            o->Release();
        }
        a->Release();
    }
    f->Release();
    return found;
}

// Запасний шлях: звичайний BitBlt. Для SDR він дає той самий результат, а
// потрібен там, де дублювання недоступне — сеанс RDP, деякі віртуалки,
// захищений робочий стіл.
Gdiplus::Bitmap* CapBitBlt(const RECT& rc)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, rc.left, rc.top, SRCCOPY);
    SelectObject(mem, old);
    Gdiplus::Bitmap* out = Gdiplus::Bitmap::FromHBITMAP(bmp, nullptr);
    Gdiplus::Bitmap* cloned = nullptr;
    if (out && out->GetLastStatus() == Gdiplus::Ok)
        cloned = out->Clone(0, 0, w, h, PixelFormat32bppPARGB);
    delete out;
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (cloned) {
        // BitBlt не дає альфи; без цього знімок вийде прозорим.
        Gdiplus::BitmapData bd;
        Gdiplus::Rect all(0, 0, w, h);
        if (cloned->LockBits(&all, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &bd) == Gdiplus::Ok) {
            for (int y = 0; y < h; ++y) {
                BYTE* row = (BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
                for (int x = 0; x < w; ++x) row[x * 4 + 3] = 255;
            }
            cloned->UnlockBits(&bd);
        }
    }
    return cloned;
}

// Перетворення кадру в 32-бітний бітмап редактора. Тут і живе весь тон-мапінг.
Gdiplus::Bitmap* CapConvert(const BYTE* src, UINT srcPitch, UINT w, UINT h,
                            DXGI_FORMAT fmt, DXGI_COLOR_SPACE_TYPE cs, float sdrWhite,
                            bool* toneMapped)
{
    *toneMapped = false;
    Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap((INT)w, (INT)h, PixelFormat32bppPARGB);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) { delete bmp; return nullptr; }
    Gdiplus::BitmapData bd;
    Gdiplus::Rect all(0, 0, (INT)w, (INT)h);
    if (bmp->LockBits(&all, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &bd) != Gdiplus::Ok) {
        delete bmp;
        return nullptr;
    }

    // Якщо система не сказала рівень білого, для HDR брати 80 ніт не можна:
    // це зробить картинку блідою рівно так само, як робив хибний код запиту.
    const bool hdrSrc = (cs != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    const float white = (sdrWhite > 1.0f) ? sdrWhite
                                          : (hdrSrc ? kCapHdrFallback : kCapScrgbWhite);

    if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        // scRGB: лінійний, 1.0 = 80 ніт. Таблиця на всі 65536 півзначень одразу —
        // інакше на кадрі 4K вийшло б 24 мільйони викликів pow.
        *toneMapped = true;
        const float scale = white / kCapScrgbWhite;
        std::vector<BYTE> lut(65536);
        for (int i = 0; i < 65536; ++i)
            lut[(size_t)i] = CapToSrgb8(CapKnee(CapHalfToFloat((unsigned short)i) / scale));
        for (UINT y = 0; y < h; ++y) {
            const unsigned short* s = (const unsigned short*)(src + (size_t)y * srcPitch);
            BYTE* d = (BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
            for (UINT x = 0; x < w; ++x) {
                d[x * 4 + 2] = lut[s[x * 4 + 0]];   // R
                d[x * 4 + 1] = lut[s[x * 4 + 1]];   // G
                d[x * 4 + 0] = lut[s[x * 4 + 2]];   // B
                d[x * 4 + 3] = 255;
            }
        }
    } else if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM) {
        const bool pq = (cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                         cs == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020);
        if (pq) {
            *toneMapped = true;
            float lin[1024];
            for (int i = 0; i < 1024; ++i) lin[i] = CapPqToNits(i / 1023.0f) / white;
            for (UINT y = 0; y < h; ++y) {
                const UINT32* s = (const UINT32*)(src + (size_t)y * srcPitch);
                BYTE* d = (BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
                for (UINT x = 0; x < w; ++x) {
                    const UINT32 v = s[x];
                    const float r2020 = lin[v & 0x3FF];
                    const float g2020 = lin[(v >> 10) & 0x3FF];
                    const float b2020 = lin[(v >> 20) & 0x3FF];
                    // BT.2020 -> BT.709 у лінійному світлі, інакше кольори попливуть
                    const float r = 1.6605f * r2020 - 0.5876f * g2020 - 0.0728f * b2020;
                    const float g = -0.1246f * r2020 + 1.1329f * g2020 - 0.0083f * b2020;
                    const float b = -0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020;
                    d[x * 4 + 2] = CapToSrgb8(CapKnee(r));
                    d[x * 4 + 1] = CapToSrgb8(CapKnee(g));
                    d[x * 4 + 0] = CapToSrgb8(CapKnee(b));
                    d[x * 4 + 3] = 255;
                }
            }
        } else {
            for (UINT y = 0; y < h; ++y) {
                const UINT32* s = (const UINT32*)(src + (size_t)y * srcPitch);
                BYTE* d = (BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
                for (UINT x = 0; x < w; ++x) {
                    const UINT32 v = s[x];
                    d[x * 4 + 2] = (BYTE)(((v) & 0x3FF) >> 2);
                    d[x * 4 + 1] = (BYTE)(((v >> 10) & 0x3FF) >> 2);
                    d[x * 4 + 0] = (BYTE)(((v >> 20) & 0x3FF) >> 2);
                    d[x * 4 + 3] = 255;
                }
            }
        }
    } else {
        // B8G8R8A8: звичайний SDR, копія рядками з урахуванням RowPitch
        for (UINT y = 0; y < h; ++y) {
            const BYTE* s = src + (size_t)y * srcPitch;
            BYTE* d = (BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
            for (UINT x = 0; x < w; ++x) {
                d[x * 4 + 0] = s[x * 4 + 0];
                d[x * 4 + 1] = s[x * 4 + 1];
                d[x * 4 + 2] = s[x * 4 + 2];
                d[x * 4 + 3] = 255;   // у кадрі альфа буває нульова
            }
        }
    }

    bmp->UnlockBits(&bd);
    return bmp;
}

bool CapGrabMonitor(HMONITOR mon, CapShot* out)
{
    *out = CapShot{};
    CapOutput co = {};
    if (!CapFindOutput(mon, &co)) return false;

    out->sdrWhite = CapSdrWhiteNits(co.device);
    out->hdr = (co.cs != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    out->cs  = (int)co.cs;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(co.adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) { co.output->Release(); co.adapter->Release(); return false; }

    const DXGI_FORMAT want[3] = { DXGI_FORMAT_R16G16B16A16_FLOAT,
                                  DXGI_FORMAT_R10G10B10A2_UNORM,
                                  DXGI_FORMAT_B8G8R8A8_UNORM };
    IDXGIOutputDuplication* dup = nullptr;
    hr = co.output->DuplicateOutput1(dev, 0, 3, want, &dup);
    if (FAILED(hr) || !dup) {
        ctx->Release(); dev->Release();
        co.output->Release(); co.adapter->Release();
        return false;
    }

    Gdiplus::Bitmap* bmp = nullptr;
    const DWORD t0 = GetTickCount();
    while ((int)(GetTickCount() - t0) < kCapBudgetMs) {
        IDXGIResource* res = nullptr;
        DXGI_OUTDUPL_FRAME_INFO fi = {};
        hr = dup->AcquireNextFrame(60, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) break;
        const bool real = (fi.AccumulatedFrames > 0) || (fi.LastPresentTime.QuadPart != 0);
        if (!real) {                      // перший кадр порожній — це норма, не помилка
            if (res) res->Release();
            dup->ReleaseFrame();
            continue;
        }
        ID3D11Texture2D* tex = nullptr;
        if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex) {
            D3D11_TEXTURE2D_DESC td = {};
            tex->GetDesc(&td);
            out->fmt = (int)td.Format;
            D3D11_TEXTURE2D_DESC sd = td;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            ID3D11Texture2D* stage = nullptr;
            if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &stage)) && stage) {
                ctx->CopyResource(stage, tex);
                D3D11_MAPPED_SUBRESOURCE m = {};
                if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
                    bmp = CapConvert((const BYTE*)m.pData, m.RowPitch, td.Width, td.Height,
                                     td.Format, co.cs, out->sdrWhite, &out->toneMapped);
                    ctx->Unmap(stage, 0);
                }
                stage->Release();
            }
            tex->Release();
        }
        if (res) res->Release();
        dup->ReleaseFrame();
        break;
    }

    dup->Release();
    ctx->Release();
    dev->Release();
    co.output->Release();
    co.adapter->Release();

    if (!bmp) return false;
    out->bmp = bmp;
    out->w = (int)bmp->GetWidth();
    out->h = (int)bmp->GetHeight();
    return true;
}

// Обрізає знімок монітора до прямокутника в координатах робочого стола.
Gdiplus::Bitmap* CapCrop(Gdiplus::Bitmap* whole, const RECT& monRc, const RECT& want)
{
    RECT r = want;
    if (r.left < monRc.left) r.left = monRc.left;
    if (r.top < monRc.top) r.top = monRc.top;
    if (r.right > monRc.right) r.right = monRc.right;
    if (r.bottom > monRc.bottom) r.bottom = monRc.bottom;
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return nullptr;
    return whole->Clone(r.left - monRc.left, r.top - monRc.top, w, h, PixelFormat32bppPARGB);
}

// Знімок монітора, на якому зараз курсор. Увесь віртуальний робочий стіл
// свідомо не зшиваємо: у сусідніх моніторів можуть бути різні колірні простори
// й різний рівень білого, і «один знімок» із них був би склейкою двох різних
// експозицій.
bool CapScreen(CapShot* out)
{
    POINT pt = {};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    if (CapGrabMonitor(mon, out)) return true;

    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return false;
    Gdiplus::Bitmap* bmp = CapBitBlt(mi.rcMonitor);     // дублювання недоступне
    if (!bmp) return false;
    *out = CapShot{};
    out->bmp = bmp;
    out->w = (int)bmp->GetWidth();
    out->h = (int)bmp->GetHeight();
    return true;
}

// Знімок вікна. Беремо межі, які малює DWM: GetWindowRect у Windows 11 віддає
// ще й невидиме поле для тіні, і знімок вийшов би з прозорими полями.
bool CapWindow(HWND target, CapShot* out)
{
    if (!target || !IsWindow(target)) return false;
    RECT rc = {};
    if (FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc))) ||
        rc.right <= rc.left)
        GetWindowRect(target, &rc);

    HMONITOR mon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return false;

    CapShot whole = {};
    if (CapGrabMonitor(mon, &whole)) {
        Gdiplus::Bitmap* part = CapCrop(whole.bmp, mi.rcMonitor, rc);
        delete whole.bmp;
        if (!part) return false;
        *out = whole;
        out->bmp = part;
        out->w = (int)part->GetWidth();
        out->h = (int)part->GetHeight();
        return true;
    }
    Gdiplus::Bitmap* bmp = CapBitBlt(rc);
    if (!bmp) return false;
    *out = CapShot{};
    out->bmp = bmp;
    out->w = (int)bmp->GetWidth();
    out->h = (int)bmp->GetHeight();
    return true;
}

Gdiplus::Bitmap* EdBitmapFromFile(const wchar_t* path);   // визначено в блоці редактора

bool g_capCancelled = false;   // вибір ділянки скасовано — це не помилка

// ---- CAPS-21: вхід із буфера обміну ------------------------------------
// PNG читаємо ПЕРШИМ і лише потім DIB. Причина в CF_DIBV5: від'ємна висота
// означає порядок рядків згори вниз, а альфа буває премультиплікованою — це
// класичне джерело перевернутих і чорних картинок. PNG такої двозначності не
// має взагалі.

UINT CapClipboardPngFormat()
{
    static UINT fmt = 0;
    if (!fmt) fmt = RegisterClipboardFormatW(L"PNG");
    return fmt;
}

// Якщо в бітмапі геть уся альфа нульова, це не «повністю прозоре зображення»,
// а джерело, яке про альфу не думало. Робимо непрозорим, інакше редактор
// показуватиме порожнечу.
void CapFixOpacity(Gdiplus::Bitmap* bmp)
{
    if (!bmp) return;
    const int w = (int)bmp->GetWidth(), h = (int)bmp->GetHeight();
    Gdiplus::BitmapData bd;
    Gdiplus::Rect all(0, 0, w, h);
    if (bmp->LockBits(&all, Gdiplus::ImageLockModeRead | Gdiplus::ImageLockModeWrite,
                      PixelFormat32bppPARGB, &bd) != Gdiplus::Ok)
        return;
    bool any = false;
    for (int y = 0; y < h && !any; ++y) {
        const BYTE* row = (const BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
        for (int x = 0; x < w; ++x)
            if (row[x * 4 + 3]) { any = true; break; }
    }
    if (!any) {
        for (int y = 0; y < h; ++y) {
            BYTE* row = (BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
            for (int x = 0; x < w; ++x) row[x * 4 + 3] = 255;
        }
    }
    bmp->UnlockBits(&bd);
}

Gdiplus::Bitmap* CapCloneParg(Gdiplus::Bitmap* src)
{
    if (!src || src->GetLastStatus() != Gdiplus::Ok) return nullptr;
    const int w = (int)src->GetWidth(), h = (int)src->GetHeight();
    if (w <= 0 || h <= 0) return nullptr;
    Gdiplus::Bitmap* out = src->Clone(0, 0, w, h, PixelFormat32bppPARGB);
    if (out && out->GetLastStatus() != Gdiplus::Ok) { delete out; out = nullptr; }
    return out;
}

Gdiplus::Bitmap* CapFromClipboard()
{
    if (!OpenClipboard(nullptr)) return nullptr;
    Gdiplus::Bitmap* out = nullptr;

    const UINT png = CapClipboardPngFormat();
    if (png && IsClipboardFormatAvailable(png)) {
        if (HANDLE h = GetClipboardData(png)) {
            const SIZE_T n = GlobalSize(h);
            if (const void* p = GlobalLock(h)) {
                if (IStream* st = SHCreateMemStream((const BYTE*)p, (UINT)n)) {
                    Gdiplus::Bitmap* src = Gdiplus::Bitmap::FromStream(st);
                    out = CapCloneParg(src);
                    delete src;
                    st->Release();
                }
                GlobalUnlock(h);
            }
        }
    }

    if (!out) {
        for (UINT fmt : { (UINT)CF_DIBV5, (UINT)CF_DIB }) {
            if (!IsClipboardFormatAvailable(fmt)) continue;
            HANDLE h = GetClipboardData(fmt);
            if (!h) continue;
            if (const void* p = GlobalLock(h)) {
                const BITMAPINFO* bi = (const BITMAPINFO*)p;
                const DWORD hdr = bi->bmiHeader.biSize;
                DWORD palette = 0;
                if (bi->bmiHeader.biBitCount <= 8)
                    palette = (bi->bmiHeader.biClrUsed ? bi->bmiHeader.biClrUsed
                                                       : (1u << bi->bmiHeader.biBitCount)) * sizeof(RGBQUAD);
                else if (bi->bmiHeader.biCompression == BI_BITFIELDS && hdr == sizeof(BITMAPINFOHEADER))
                    palette = 3 * sizeof(DWORD);
                const BYTE* bits = (const BYTE*)p + hdr + palette;
                Gdiplus::Bitmap src(bi, (void*)bits);
                out = CapCloneParg(&src);
                GlobalUnlock(h);
            }
            if (out) break;
        }
    }

    if (!out && IsClipboardFormatAvailable(CF_BITMAP)) {
        if (HBITMAP hb = (HBITMAP)GetClipboardData(CF_BITMAP)) {
            Gdiplus::Bitmap* src = Gdiplus::Bitmap::FromHBITMAP(hb, nullptr);
            out = CapCloneParg(src);
            delete src;
        }
    }

    wchar_t dropped[MAX_PATH] = {};
    if (!out && IsClipboardFormatAvailable(CF_HDROP)) {
        if (HDROP drop = (HDROP)GetClipboardData(CF_HDROP))
            DragQueryFileW(drop, 0, dropped, MAX_PATH);
    }
    CloseClipboard();

    if (!out && dropped[0]) out = EdBitmapFromFile(dropped);   // визначено нижче за текстом
    CapFixOpacity(out);
    return out;
}

// ---- CAPS-21: вибір ділянки рамкою -------------------------------------
// Екран спершу ЗАМОРОЖУЄМО, а вже потім показуємо поверх нього вікно вибору.
// Так рамка й притемнення не потрапляють у результат, вибір виходить точний до
// пікселя, і ніщо на екрані не встигне змінитись між вибором і знімком.

HWND  g_rgnWnd = nullptr;
Gdiplus::Bitmap* g_rgnImg = nullptr;   // заморожений кадр; НЕ власність цього коду
RECT  g_rgnMon = {};
POINT g_rgnFrom = {}, g_rgnTo = {};
bool  g_rgnDragging = false, g_rgnDone = false, g_rgnOk = false;
bool  g_rgnHadFocus = false;   // фокус справді був, а не «ніколи не приходив»
POINT g_rgnCur = {};           // курсор у клієнтських координатах — для напрямних
HFONT g_rgnFont = nullptr;

RECT RgnSelRect()
{
    RECT r;
    r.left   = g_rgnFrom.x < g_rgnTo.x ? g_rgnFrom.x : g_rgnTo.x;
    r.top    = g_rgnFrom.y < g_rgnTo.y ? g_rgnFrom.y : g_rgnTo.y;
    r.right  = g_rgnFrom.x > g_rgnTo.x ? g_rgnFrom.x : g_rgnTo.x;
    r.bottom = g_rgnFrom.y > g_rgnTo.y ? g_rgnFrom.y : g_rgnTo.y;
    return r;
}

// Підпис у темній плашці біля точки. Використовується і для розміру рамки,
// і для координат першого кута.
void RgnLabel(HDC dc, Gdiplus::Graphics& g, int w, int h, int ax, int ay, const wchar_t* text)
{
    HGDIOBJ oldF = SelectObject(dc, g_rgnFont);
    RECT m = { 0, 0, 0, 0 };
    DrawTextW(dc, text, -1, &m, DT_CALCRECT | DT_SINGLELINE);
    const int bw = (m.right - m.left) + 16, bh = (m.bottom - m.top) + 10;
    int bx = ax, by = ay - bh - 6;
    if (by < 0) by = ay + 6;
    if (bx + bw > w) bx = w - bw;
    if (bx < 0) bx = 0;
    Gdiplus::SolidBrush back(Gdiplus::Color(220, 20, 20, 24));
    g.FillRectangle(&back, bx, by, bw, bh);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT tr = { bx, by, bx + bw, by + bh };
    DrawTextW(dc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldF);
}

void RgnPaint(HDC dc, int w, int h)
{
    Gdiplus::Graphics g(dc);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    if (g_rgnImg) g.DrawImage(g_rgnImg, 0, 0, w, h);

    const RECT s = g_rgnDragging ? RgnSelRect() : RECT{ 0, 0, 0, 0 };
    Gdiplus::SolidBrush scrim(Gdiplus::Color(120, 8, 8, 12));
    if (!g_rgnDragging || s.right <= s.left || s.bottom <= s.top) {
        g.FillRectangle(&scrim, 0, 0, w, h);
    } else {
        // Притемнюємо все, крім вибраного, чотирма прямокутниками: так вибрана
        // ділянка лишається саме такою, якою піде в редактор.
        g.FillRectangle(&scrim, 0, 0, w, (INT)s.top);
        g.FillRectangle(&scrim, 0, (INT)s.bottom, w, h - (INT)s.bottom);
        g.FillRectangle(&scrim, 0, (INT)s.top, (INT)s.left, (INT)(s.bottom - s.top));
        g.FillRectangle(&scrim, (INT)s.right, (INT)s.top, w - (INT)s.right, (INT)(s.bottom - s.top));

        Gdiplus::Pen white(Gdiplus::Color(235, 255, 255, 255), 1.0f);
        g.DrawRectangle(&white, (INT)s.left, (INT)s.top,
                        (INT)(s.right - s.left) - 1, (INT)(s.bottom - s.top) - 1);

        wchar_t buf[64];
        wsprintfW(buf, L"%d × %d", (int)(s.right - s.left), (int)(s.bottom - s.top));
        RgnLabel(dc, g, w, h, (int)s.left, (int)s.top, buf);
    }

    // Напрямні від курсора через увесь екран: ще до першого натискання видно,
    // де саме ляже кут майбутньої рамки. Малюються поверх притемнення й
    // лишаються під час тягання — тоді вони показують другий кут.
    {
        Gdiplus::Pen guide(Gdiplus::Color(150, 255, 255, 255), 1.0f);
        g.DrawLine(&guide, 0, (INT)g_rgnCur.y, w, (INT)g_rgnCur.y);
        g.DrawLine(&guide, (INT)g_rgnCur.x, 0, (INT)g_rgnCur.x, h);
    }
    if (!g_rgnDragging) {
        wchar_t c[64];
        wsprintfW(c, L"%d, %d", (int)g_rgnCur.x, (int)g_rgnCur.y);
        RgnLabel(dc, g, w, h, (int)g_rgnCur.x + 12, (int)g_rgnCur.y + 34, c);
    }
}

LRESULT CALLBACK RgnWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
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
        RgnPaint(mem, rc.right, rc.bottom);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        g_rgnFrom.x = GET_X_LPARAM(lp);
        g_rgnFrom.y = GET_Y_LPARAM(lp);
        g_rgnTo = g_rgnFrom;
        g_rgnDragging = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        g_rgnCur.x = GET_X_LPARAM(lp);
        g_rgnCur.y = GET_Y_LPARAM(lp);
        if (g_rgnDragging) g_rgnTo = g_rgnCur;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        if (!g_rgnDragging) return 0;
        g_rgnTo.x = GET_X_LPARAM(lp);
        g_rgnTo.y = GET_Y_LPARAM(lp);
        ReleaseCapture();
        const RECT s = RgnSelRect();
        g_rgnOk = (s.right - s.left >= 4 && s.bottom - s.top >= 4);   // клік без тягання = скасування
        g_rgnDone = true;
        return 0;
    }
    case WM_RBUTTONDOWN:
        g_rgnOk = false;
        g_rgnDone = true;
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { g_rgnOk = false; g_rgnDone = true; }
        return 0;
    case WM_SETFOCUS:
        g_rgnHadFocus = true;
        return 0;

    // Фокус забрали — вибір скасовано, щоб притемнене вікно не висіло поверх
    // усього. Але лише якщо фокус справді був: SetForegroundWindow інколи не
    // спрацьовує, і тоді KILLFOCUS прилітає одразу після показу.
    case WM_KILLFOCUS:
        if (g_rgnHadFocus && !g_rgnDone) { g_rgnOk = false; g_rgnDone = true; }
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool CapRegionPick(Gdiplus::Bitmap* frozen, const RECT& monRc, RECT* out)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = RgnWndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"lilhelpers_region";
        wc.hCursor       = LoadCursorW(nullptr, IDC_CROSS);
        RegisterClassW(&wc);
        registered = true;
    }
    if (!g_rgnFont) g_rgnFont = CreateUIFont(105, FW_SEMIBOLD);

    g_rgnImg = frozen;
    g_rgnMon = monRc;
    g_rgnDragging = g_rgnDone = g_rgnOk = g_rgnHadFocus = false;
    g_rgnFrom = g_rgnTo = POINT{ 0, 0 };
    GetCursorPos(&g_rgnCur);                     // напрямні одразу під курсором
    g_rgnCur.x -= monRc.left;
    g_rgnCur.y -= monRc.top;

    g_rgnWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"lilhelpers_region", L"",
                               WS_POPUP, monRc.left, monRc.top,
                               monRc.right - monRc.left, monRc.bottom - monRc.top,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_rgnWnd) { g_rgnImg = nullptr; return false; }
    ShowWindow(g_rgnWnd, SW_SHOW);
    SetForegroundWindow(g_rgnWnd);
    SetFocus(g_rgnWnd);

    // Власний цикл повідомлень: вибір ділянки модальний за суттю.
    MSG msg;
    while (!g_rgnDone) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) { PostQuitMessage(0); break; }   // WM_QUIT віддаємо назад головному циклу
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Результат забираємо ДО руйнування вікна: DestroyWindow шле сфокусованому
    // вікну WM_KILLFOCUS, і обробник «фокус забрали — скасовано» інакше затирає
    // щойно зроблений вибір.
    const RECT s = RgnSelRect();
    const bool okLocal = g_rgnOk;
    DestroyWindow(g_rgnWnd);
    g_rgnWnd = nullptr;
    g_rgnImg = nullptr;
    if (!okLocal) return false;
    out->left   = monRc.left + s.left;
    out->top    = monRc.top + s.top;
    out->right  = monRc.left + s.right;
    out->bottom = monRc.top + s.bottom;
    return true;
}

bool CapRegion(CapShot* out)
{
    POINT pt = {};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return false;

    CapShot whole = {};
    if (!CapGrabMonitor(mon, &whole)) {
        whole = CapShot{};
        whole.bmp = CapBitBlt(mi.rcMonitor);
        if (!whole.bmp) return false;
    }
    RECT sel = {};
    const bool picked = CapRegionPick(whole.bmp, mi.rcMonitor, &sel);
    if (!picked) { delete whole.bmp; g_capCancelled = true; return false; }

    Gdiplus::Bitmap* part = CapCrop(whole.bmp, mi.rcMonitor, sel);
    delete whole.bmp;
    if (!part) return false;
    *out = whole;
    out->bmp = part;
    out->w = (int)part->GetWidth();
    out->h = (int)part->GetHeight();
    return true;
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

// Власний заголовок. Системний підпис — мертве місце заввишки в кнопку, і
// скасування з довідкою живуть там природніше, ніж у смузі властивостей, якій
// місця бракує. Ціна — свої кнопки згорнути/розгорнути/закрити й власний
// розбір WM_NCCALCSIZE та WM_NCHITTEST.
constexpr int kEdCaption = 40;    // заголовок
constexpr int kEdStrip   = 48;    // смуга властивостей
constexpr int kEdRail    = 52;    // панель інструментів
constexpr int kEdPanel   = 260;   // права панель
constexpr int kEdPanelLo = 30;    // вона ж згорнута
constexpr int kEdStatus  = 48;
// Смуга лічильника — найдовша з усіх: чіп, група, початок зі степером,
// наступний номер, кольори, розміри, прозорість і кнопка нової групи. Разом із
// діями над вибраним це близько 1170 точок — короткі підписи «Початок» і
// «Наступний» (зауваження власника) звільнили тут майже сотню.
constexpr int kEdMinW    = 1200;
// 11 інструментів займають 8 + 7 + 11*(40+4) = 499 точок. Плюс заголовок,
// смуга властивостей і рядок стану — ось звідки цей мінімум: у нижчому вікні
// останній інструмент просто не вміщався б у панель.
constexpr int kEdMinH    = 660;
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

enum class EdTool { Select, Rect, Ellipse, Line, Pen, Text, Hide, Mark, Counter, Stamp, Crop };
// ⚠ Окремої стрілки немає з 21.09: лінія сама носить наконечники.
enum class EdKind { Rect, Ellipse, Line, Pen, Text, Hide, Mark, Counter, Stamp,
                    Image };

struct EdObj {
    EdKind   kind;
    // Rect і Ellipse: рамка, w і h завжди додатні. Line: відрізок від
    // (x, y) до (x + w, y + h), тож знак w і h НЕСЕ напрямок і нормалізації не
    // підлягає — інакше стрілка почала б дивитись не туди.
    int      x, y, w, h;      // у координатах ЗОБРАЖЕННЯ, не екрана
    COLORREF color;
    int      thick;           // товщина в пікселях ЗНІМКА, не екрана
    int      alpha;           // 10..100 %
    bool     filled;          // лише Rect і Ellipse
    std::vector<POINT> pts;   // лише Pen: сам слід, у координатах зображення
    // Далі — лише Text. Кегль, як і товщина, у пікселях ЗНІМКА: обраний розмір
    // завжди дає однаковий напис у файлі, байдуже, який масштаб на екрані.
    std::wstring text;
    int      size;
    bool     bold, italic;
    int      align;           // 0 ліворуч, 1 по центру, 2 праворуч
    int      outline;         // 0 без обводки, 1 світла, 2 темна
    // Ширина блока в пікселях знімка. 0 = блок сам по тексту, і тоді
    // вирівнювати немає відносно чого: рядок дорівнює блоку. Розтягнута
    // ручкою ширина вмикає переноси, і аж тоді вирівнювання щось означає.
    int      boxw;
    // Далі — лише Hide. Режим: 0 розмиття, 1 пікселі, 2 плашка. Сила — 10..100 %,
    // і від неї залежить радіус розмиття чи розмір блока пікселізації.
    int      mode;
    int      strength;
    // Лічильник і штамп. num — число в кружечку; stamp — що саме за штамп:
    // 0..5 власні контури, від kEdEmojiBase — емодзі за номером у таблиці.
    // Лічильник НЕ зберігає свого номера. Номер = початок групи + скільки в
    // цій групі кружечків, створених раніше. Тому видалення першого зсуває
    // решту, зміна початку перенумеровує всіх, а новий знімок починає з
    // початку сам собою — усе це не окремі правила, а одна формула.
    int      seq;     // порядок створення серед лічильників
    int      group;   // група: у кожної своя незалежна нумерація
    int      start;   // з якого числа починає група (однакове в усіх її кружечках)
    int      stamp;
    // CAPS-34. Стиль лінії — для всіх контурних; наконечник — лише для стрілки.
    // dash: 0 суцільна, 1 пунктир, 2 штрихпунктир.
    // headFront — наконечник у КІНЦІ (там, де відпустили мишу), headBack — на
    // початку. 0 немає, 1 трикутник, 2 «пташка», 3 кружечок; вони НЕЗАЛЕЖНІ,
    // тож лінія буває і без наконечників, і з різними на кінцях (вимога
    // власника: окремої «стрілки» більше немає, є лінія з властивостями).
    int      dash;
    int      headFront, headBack, headSize;
    // CAPS-42. Лише Image: НОМЕР бітмапа в реєстрі, а не сам вказівник.
    // EdObj копіюється у кожен знімок скасування — вказівник там означав би
    // подвійне звільнення, а номер переживає будь-яку кількість копій.
    int      img;
    // CAPS-43. Кут у градусах, 0..359, навколо ЦЕНТРА позначки. Приховування
    // й маркер не повертаються: вони беруть пікселі знімка під собою, і
    // повернута плашка мусила б брати повернуту ділянку — окрема робота.
    int      rot;
    // CAPS-35. Номер групи, 0 — поза групою. Клік по будь-якому учаснику
    // вибирає всю групу; сама група — це просто спільний номер, а не окремий
    // об'єкт-контейнер: контейнер довелося б проводити крізь порядок, кадр,
    // поворот знімка й скасування.
    int      grp;
};

// Чіп називає вид однією назвою і для інструмента, і для вибраного. Префікс
// «Вибрано: » з'їдав до шістдесяти точок смуги й нічого не додавав: що саме
// вибрано, видно по рамці на полотні.
// ⚠ Чіп рахують ДВА місця — розкладка й малювання, — тож назву віддає одна
// функція. У лічильника чіпа немає навмисно: одразу за ним іде жирне «Група N»,
// яке каже те саме, а смузі лічильника бракує сотні точок на мінімальній
// ширині вікна — рівно стільки чіп і займав.
const wchar_t* EdChipLabel();

Str EdKindName(EdKind k)
{
    switch (k) {
    case EdKind::Image:   return Str::EdKindImage;
    case EdKind::Ellipse: return Str::EdToolEllipse;
    case EdKind::Line:    return Str::EdToolLine;
    case EdKind::Pen:     return Str::EdToolPen;
    case EdKind::Text:    return Str::EdToolText;
    case EdKind::Hide:    return Str::EdToolHide;
    case EdKind::Mark:    return Str::EdToolMark;
    case EdKind::Counter: return Str::EdToolCounter;
    case EdKind::Stamp:   return Str::EdToolStamp;
    default:              return Str::EdToolRect;
    }
}

Str EdToolName(EdTool t)
{
    switch (t) {
    case EdTool::Ellipse: return Str::EdToolEllipse;
    case EdTool::Line:    return Str::EdToolLine;
    case EdTool::Pen:     return Str::EdToolPen;
    case EdTool::Text:    return Str::EdToolText;
    case EdTool::Hide:    return Str::EdToolHide;
    case EdTool::Mark:    return Str::EdToolMark;
    case EdTool::Counter: return Str::EdToolCounter;
    case EdTool::Stamp:   return Str::EdToolStamp;
    case EdTool::Crop:    return Str::EdToolCrop;
    case EdTool::Rect:    return Str::EdToolRect;
    default:              return Str::Empty;
    }
}

EdKind EdToolKind(EdTool t)
{
    switch (t) {
    case EdTool::Ellipse: return EdKind::Ellipse;
    case EdTool::Line:    return EdKind::Line;
    case EdTool::Pen:     return EdKind::Pen;
    case EdTool::Text:    return EdKind::Text;
    case EdTool::Hide:    return EdKind::Hide;
    case EdTool::Mark:    return EdKind::Mark;
    case EdTool::Counter: return EdKind::Counter;
    case EdTool::Stamp:   return EdKind::Stamp;
    default:              return EdKind::Rect;
    }
}

bool EdIsSegment(EdKind k) { return k == EdKind::Line; }
// Стиль лінії має сенс лише там, де є сама лінія: у напису, приховування,
// маркера, лічильника й штампа контуру немає.
bool EdCanRotate(EdKind k) { return k != EdKind::Hide && k != EdKind::Mark; }

bool EdHasDash(EdKind k)
{
    return k == EdKind::Rect || k == EdKind::Ellipse || k == EdKind::Line ||
           k == EdKind::Pen;
}
bool EdCanFill(EdKind k)   { return k == EdKind::Rect || k == EdKind::Ellipse; }
// Приховування й маркер — не мазки пером, а дії НАД ПІКСЕЛЯМИ знімка: обидва
// беруть ділянку базового бітмапа й повертають її зміненою.
bool EdIsEffect(EdKind k)  { return k == EdKind::Hide || k == EdKind::Mark; }
// У напису товщини немає: її роль грає кегль. У приховування — теж: там сила.
// У маркера ті самі три кнопки означають висоту смуги.
bool EdHasThick(EdKind k)  { return k != EdKind::Text && k != EdKind::Hide &&
                                    k != EdKind::Image; }
// Лічильник і штамп ставляться одним кліком, а не тягненням: у них немає
// «намалюй рамку», є лише розмір із трьох значень.
bool EdIsStamped(EdKind k) { return k == EdKind::Counter || k == EdKind::Stamp; }

// Розміри кружечка лічильника й штампа — діаметр у пікселях знімка.
const int kEdStampSizes[3] = { 28, 40, 56 };

// Шість власних контурів. Далі — емодзі з таблиці нижче.
constexpr int kEdEmojiBase = 100;
const int kEdVectorStamps = 6;

// Чотири швидкі емодзі в смузі; решта — у сітці за кнопкою «…». Тримаємо один
// список: перші чотири просто показуються одразу.
const wchar_t* const kEdEmoji[] = {
    L"\U0001F44D", L"\U0001F44E", L"\U0001F512", L"\U0001F4A1",
    L"\u2705",     L"\u274C",     L"\u2757",     L"\u2753",
    L"\u26A0",     L"\U0001F525", L"\u2B50",     L"\U0001F4CC",
    L"\U0001F440", L"\U0001F3AF", L"\U0001F41B", L"\U0001F389",
    L"\u2B06",     L"\u2B07",     L"\u27A1",     L"\u2B05",
    L"\U0001F535", L"\U0001F534", L"\U0001F7E2", L"\U0001F7E1",
    L"\U0001F4C1", L"\U0001F5D1", L"\U0001F50D", L"\u2699",
    L"\U0001F464", L"\U0001F4C5", L"\u23F0",     L"\U0001F4AC",
    L"\u2764",     L"\U0001F44C", L"\U0001F91D", L"\U0001F680",
    L"\U0001F6D1", L"\u267B",     L"\U0001F4CA", L"\U0001F4B0",
    L"\u2753",     L"\u2139",     L"\U0001F4DD", L"\U0001F3C1",
    L"\U0001F513", L"\U0001F4E7", L"\U0001F5A5", L"\U0001F4F1"
};
const int kEdEmojiCount = (int)(sizeof(kEdEmoji) / sizeof(*kEdEmoji));
const int kEdEmojiQuick = 4;      // скільки з них стоїть просто в смузі

// Три висоти смуги маркера — у пікселях знімка, як і решта розмірів.
const int kEdMarkH[3] = { 16, 26, 40 };

// ⚠ Чотири кольори маркера — з чистими каналами (0 або 255) НАВМИСНО. Множення
// на фон у нас робиться через порозрядне І, і лише для таких кольорів воно
// точно збігається зі справжнім множенням: 255 лишає канал як був, 0 гасить.
const COLORREF kEdMarkPalette[4] = {
    RGB(255, 255, 0), RGB(0, 255, 0), RGB(255, 0, 255), RGB(0, 255, 255)
};

const COLORREF* EdPaletteFor(EdKind k, int& n)
{
    if (k == EdKind::Mark) { n = 4; return kEdMarkPalette; }
    n = 8;
    return kEdPalette;
}

// Товщини: тонка, середня, товста. У пікселях знімка (рішення власника).
const int kEdThicks[3] = { 2, 4, 7 };

const int* EdThickSet(EdKind k)
{
    if (k == EdKind::Mark) return kEdMarkH;
    if (EdIsStamped(k))    return kEdStampSizes;
    return kEdThicks;
}

int EdThickIndex(EdKind k, int t)
{
    const int* set = EdThickSet(k);
    for (int i = 0; i < 3; ++i) if (set[i] == t) return i;
    return 1;
}

// Кеглі — драбинкою, а не кроком в один піксель: на знімку різниця між 31 і 32
// непомітна, зате гортати степером довелося б удесятеро довше.
const int kEdSizes[13] = { 12, 14, 16, 18, 20, 24, 28, 32, 40, 48, 64, 80, 96 };

int EdSizeStep(int cur, int dir)
{
    int best = 0;
    for (int i = 0; i < 13; ++i) if (kEdSizes[i] <= cur) best = i;
    int n = best + dir;
    if (n < 0)  n = 0;
    if (n > 12) n = 12;
    return kEdSizes[n];
}

// Визначені нижче, поруч із нормалізацією, а потрібні вже тут — у перевірці попадання.
void   EdPenBounds(EdObj& o);
double EdDistToSeg(double px, double py, double ax, double ay, double bx, double by);

struct EdSnap {
    std::vector<EdObj> objs;
    int sel;
    std::vector<int> selMore;   // CAPS-35: скасування має повертати ВЕСЬ вибір
    RECT crop;        // кадр — теж частина стану, інакше Ctrl+Z повертав би не все
    // Тон і геометрія — так само стан, а не «налаштування»: поворот зсуває
    // кожну позначку, і повернути самі позначки без нього означало б покласти
    // їх не туди.
    int exposure, gamma, contrast, rot;
    bool mirror;
    int srcId;        // який саме оригінал був у роботі
};

struct EdTile {
    std::wstring     key;
    Gdiplus::Bitmap* bmp;
    int              pad;    // на скільки плитка більша за сам напис із кожного боку
};

// Плитки малюються далеко нижче, а смуга властивостей показує ними емодзі
// на кнопках штампів — тому оголошуємо наперед.
const EdTile* EdTextTile(const EdObj& o, double s);

enum class EdHit { None, Canvas, Tool, Swatch, Opacity, Undo, Redo, Help,
                   Front, Back, Del, Zoom, Zoom100, Fit, Panel, Copy, Save,
                   Thick, Fill, Size, Bold, Italic, Align, Stroke, Dup, Open,
                   OpenMenu, Min, Max, Close, HideMode, Strength,
                   NumStart, NumReset, StampPick, StampMore, NumGroup, NumNext,
                   Aspect, CropReset, CropOk, CropNo,
                   RotL, RotR, FlipH, FlipV, Exposure, Gamma, Contrast,
                   ToneReset, Compare, GroupEdit, GroupDel, Pick, PickItem,
                   SelAlign, SelGroup, SizeImg, SizeCan, Share };

struct EdRegion { RECT r; EdHit what; int idx; };

EdHit g_edToneWhat = EdHit::Exposure;   // який саме повзунок тону зараз тягнуть

enum EdIco { IcoSelect, IcoRect, IcoUndo, IcoRedo, IcoHelp, IcoFront, IcoBack,
             IcoDel, IcoOpacity, IcoChevR, IcoChevL, IcoMinus, IcoPlus,
             IcoEllipse, IcoLine, IcoPen, IcoText, IcoBold, IcoItalic,
             IcoAlignL, IcoAlignC, IcoAlignR, IcoStroke0, IcoStroke1, IcoStroke2,
             IcoDup, IcoOpen, IcoChevD, IcoSave, IcoCopy,
             IcoWinMin, IcoWinMax, IcoWinRestore, IcoWinClose,
             IcoHide, IcoMark, IcoBlur, IcoPixels, IcoPlate, IcoStrength,
             IcoCounter, IcoStamp, IcoNewGroup, IcoMore, IcoCrop,
             IcoRotL, IcoRotR, IcoFlipH, IcoFlipV, IcoCompare,
             IcoGroupEdit, IcoGroupDel,
             IcoAlL, IcoAlCx, IcoAlR, IcoAlT, IcoAlCy, IcoAlB, IcoDistX, IcoDistY,
             IcoGroup, IcoUngroup,
             IcoStCheck, IcoStCross, IcoStQuestion, IcoStBang, IcoStStar, IcoStWarn };

enum class EdDrag { None, New, Move, Resize, Pan, Slider, Strength, Crop, CropMove,
                    Tone, Compare, Zoom, Rotate, ManyResize, ManyRotate };

HWND  g_edWnd = nullptr;
HFONT g_edFont = nullptr, g_edFontBold = nullptr, g_edFontSmall = nullptr;
HICON g_edIcon = nullptr;         // значок у власному заголовку
int   g_edDpi = 96;
bool  g_edDark = false;
bool  g_edPanelOpen = true;

Gdiplus::Bitmap* g_edImg = nullptr;
int      g_edImgW = 0, g_edImgH = 0;
wchar_t  g_edSource[MAX_PATH] = {};
bool     g_edHdr = false, g_edToneMapped = false;   // CAPS-21: звідки прийшов кадр
float    g_edSdrWhite = -1.0f;                      // ніт; -1 = система не сказала

// CAPS-28. Оригінал лишається недоторканим, а все, що робить права панель, —
// РЕЦЕПТ поверх нього: поворот, дзеркало, три повзунки тону. g_edImg — результат
// цього рецепта, і саме він іде в буфер, у файл і в плитки ефектів. Через це
// «Порівняти» нічого не коштує, а двічі застосований тон не накопичується.
Gdiplus::Bitmap* g_edSrc = nullptr;   // як прийшло; ніколи не змінюється
// CAPS-44. Зміна розміру робить НОВИЙ оригінал, а скасування має повернути
// старий — разом із пікселями. Тому джерела живуть у банку, а знімок стану
// тримає лише НОМЕР: той самий прийом, що й для вкладених зображень.
std::vector<Gdiplus::Bitmap*> g_edSrcBank;
int      g_edSrcId = -1;
Gdiplus::Bitmap* g_edCmp = nullptr;   // той самий рецепт БЕЗ тону, лише поки порівнюють
int      g_edExposure = 0;            // -20..+20 = -2,0…+2,0 EV кроком 0,1
int      g_edGamma    = 100;          // 50..200 = 0,50…2,00
int      g_edContrast = 0;            // -50..+50
int      g_edRot      = 0;            // чверті оберту за годинниковою
bool     g_edMirror   = false;        // дзеркало по горизонталі ДО повороту
bool     g_edCompare  = false;        // кнопку «Порівняти» тримають
bool     g_edTonePushed = false;      // чи вже поклали знімок для Ctrl+Z
// CAPS-40: поки ввімкнено, колір, розмір і прозорість ідуть усій групі
// лічильника, а не одному кружечку. Подвійний клік робить те саме разово.
bool     g_edGroupEdit  = false;
// CAPS-47: серія натискань стрілки — ОДИН крок скасування. Прапорець тримає
// серію відкритою, доки клавішу не відпустили.
bool     g_edNudging    = false;
// CAPS-34: який випадний селект зараз розкрито (-1 — жоден). Селект — це не
// вікно, а ділянки поверх полотна: власне вікно заради трьох рядків коштувало б
// класу, фокуса й окремого шляху закриття.
int      g_edPickOpen   = -1;

void EdRebuildImage();                // тіло далеко нижче: йому потрібні плитки
void EdFitView();                     // поворот міняє сторони — вид доводиться вписувати
bool EdSelBounds(RECT* out);          // спільні габарити вибраного
bool EdManySel();                     // вибрано двоє й більше
std::vector<int> EdSelAll();          // головний вибраний плюс решта
void EdKeepAspect(int handle, int ow, int oh, int& l, int& t, int& r, int& b);
void EdLayout(HWND hwnd);
// CAPS-22: стан виходів. Остання використана дія підсвічується як дія для Enter.
int      g_edLastAction  = 0;      // 0 буфер, 1 файл
bool     g_edSaved       = false;  // уже кудись пішло — Esc не питає
// Після малювання інструмент лишається активним: зазвичай далі малюють ще одну
// позначку, а не правлять щойно поставлену. Вибір власника, з перемикачем.
bool     g_edKeepTool    = true;
DWORD    g_edToastUntil  = 0;
Str      g_edToast       = Str::Empty;
void EdDoCopy();
void EdDoSave();
bool EdConfirmClose();
void EdDuplicateSel();

// Бітмапи вкинутих зображень. Звідси нічого не видаляється: видалений об'єкт
// має повертатися по Ctrl+Z, а отже, його пікселі мусять дожити до кінця
// роботи над знімком. Чиститься разом із новим знімком.
std::vector<Gdiplus::Bitmap*> g_edImgBank;

int EdAddImage(Gdiplus::Bitmap* b)
{
    if (!b) return -1;
    g_edImgBank.push_back(b);
    return (int)g_edImgBank.size() - 1;
}

Gdiplus::Bitmap* EdImageOf(const EdObj& o)
{
    if (o.kind != EdKind::Image) return nullptr;
    if (o.img < 0 || o.img >= (int)g_edImgBank.size()) return nullptr;
    return g_edImgBank[o.img];
}

void EdImageBankClear()
{
    for (size_t i = 0; i < g_edImgBank.size(); ++i) delete g_edImgBank[i];
    g_edImgBank.clear();
}

std::vector<EdObj> g_edObjs;
// g_edSel лишається ГОЛОВНИМ вибраним: його властивості показує смуга, його
// ручки видно. Решта вибраних живуть окремо — так увесь наявний код, який
// знає про один вибраний, працює далі без переписування.
int      g_edSel = -1;
std::vector<int> g_edSelMore;
int      g_edNextGrp = 1;

// ---- CAPS-27: кадр ------------------------------------------------------
// Кроп — ВЛАСТИВІСТЬ кадру, а не дія над пікселями. Знімок лишається цілим,
// позначки за межами кадру нікуди не діваються, і «Скинути кадр» повертає все
// на місце навіть через десяток інших дій. Ціна — всі координати екрана тепер
// рахуються від видимої ділянки, а не від нуля зображення.
RECT     g_edCrop = {};           // у координатах ЗОБРАЖЕННЯ; порожній = без кадру
bool     g_edCropping = false;    // режим редагування кадру
RECT     g_edCropEdit = {};       // кадр, який зараз тягнуть
int      g_edCropAspect = 0;      // 0 вільно, 1 — 16:9, 2 — 4:3, 3 — 1:1
// Геометрія кадру визначена нижче, а розкладка потребує її вже тут.
bool EdHasCrop();
RECT EdCropScreen(const RECT& c);
int  EdCropHandles(RECT out[8]);
void EdCropClamp(RECT& r);
void EdCropAspect(RECT& r, int anchorRight, int anchorBottom);
int  EdOutsideCount();
void EdCropBegin();
void EdCropFinish(bool apply);
void EdCropReset();
std::vector<EdSnap> g_edUndo, g_edRedo;

EdTool   g_edTool  = EdTool::Select;
COLORREF g_edColor = RGB(232, 17, 35);   // типовий колір нових позначок
int      g_edThick = 4;
bool     g_edFill  = false;   // типово контур: заливка ховає те, що позначають
int      g_edAlpha = 100;
// Типово напис із світлою обводкою: критерій готовності етапу — щоб текст
// читався і на світлому, і на темному тлі, а червоне без обводки на темному
// тлі не читається зовсім.
int      g_edSize    = 32;
bool     g_edBold    = true;
bool     g_edItalic  = false;
int      g_edAlign   = 0;
int      g_edOutline = 1;
// Типова сила приховування навмисно висока: інструмент існує, щоб під ним
// нічого не читалося. Ослабити її користувач може свідомо, посиливши — ні.
int      g_edHideMode = 1;        // типово пікселі: вони чесніші за розмиття
int      g_edStrength = 70;
int      g_edMarkH    = kEdMarkH[1];
COLORREF g_edMarkColor = RGB(255, 255, 0);
// Лічильник нумерує сам, а «початок» — те число, до якого його скидає кнопка
// «Почати». Видалення кружечка з середини решту НЕ перенумеровує: підпис у чаті
// поруч зі знімком інакше перестав би збігатися.
int      g_edStartNum = 1;        // початок для групи, в якій ще нема кружечків
int      g_edSeq      = 0;        // лічильник створень, лише зростає
int      g_edCounterGroup = 0;    // куди піде наступний кружечок
// Формули нумерації визначені разом із малюванням, а потрібні вже розкладці.
int EdCurGroup();
int EdGroupStart(int grp);
int EdGroupCount(int grp);
int      g_edDash = 0;                   // CAPS-34: стиль лінії за замовчуванням
int      g_edHeadFront = 0, g_edHeadBack = 0, g_edHeadSize = 1;
int      g_edStampSize = kEdStampSizes[1];
int      g_edStamp    = 0;

float g_edZoom = 1.0f;                   // множник до «вписаного», як у перегляді
int   g_edPanX = 0, g_edPanY = 0;

std::vector<EdRegion> g_edRegions;
// Ліва частина смуги росте зліва направо, права притиснута до краю. Тут —
// межа між ними: усе ліве малюється з клипом по ній, тож переповнення видно
// зрізаною кнопкою, а не двома іконками одна на одній.
int g_edStripLimit = 0;
int g_edCapLimit = 0;      // те саме для заголовка: доки тягнеться підпис
RECT  g_edRcCaption = {};
RECT  g_edRcStrip = {}, g_edRcRail = {}, g_edRcCanvas = {}, g_edRcPanel = {}, g_edRcStatus = {};
bool  g_edActive = true;          // вікно активне: неактивний підпис приглушуємо
EdHit g_edHotWhat = EdHit::None;
int   g_edHotIdx  = -1;
bool  g_edTracking = false;

// CAPS-24: поле введення напису. Живе тут, а не в своїй секції нижче, бо
// малювання полотна має знати, який об'єкт зараз показує саме поле.
HWND   g_edEdit    = nullptr;
int    g_edEditIdx = -1;          // -1 = новий напис, інакше індекс наявного

EdDrag g_edDrag = EdDrag::None;
int    g_edHandle = -1;
POINT  g_edDragFrom = {};
EdObj  g_edDragOrig = {};
// Спільне тягнення кількох позначок рахується ЩОРАЗУ ВІД ПОЧАТКОВИХ копій, а не
// від попереднього кадру: інакше округлення на кожному русі миші накопичувалось
// би, і група повільно «спливала» б убік від курсора.
std::vector<EdObj> g_edManyOrig;
std::vector<int>   g_edManyIdx;
RECT   g_edManyBox = {};          // габарити на початку тягнення, у точках знімка
double g_edManyAng0 = 0.0;        // кут на курсор у мить натискання
EdObj  g_edNew = {};
RECT   g_edCropOrig = {};    // кадр на початку тягнення
POINT  g_edNewOrigin = {};   // точка натиску в координатах знімка: маркер тягнеться від неї

int EdPx(int v) { return MulDiv(v, g_edDpi, 96); }

inline Gdiplus::Color EdC(COLORREF c, int a = 255)
{
    return Gdiplus::Color((BYTE)a, GetRValue(c), GetGValue(c), GetBValue(c));
}

inline int EdMin(int a, int b) { return a < b ? a : b; }
inline double EdMinD(double a, double b) { return a < b ? a : b; }

// Точка, повернута навколо центра на заданий кут. Одна формула на все:
// малювання, влучання, ручки.
void EdRotatePt(double cx, double cy, double deg, double& x, double& y)
{
    if (deg == 0.0) return;
    const double r = deg * 3.14159265358979 / 180.0;
    const double cs = cos(r), sn = sin(r);
    const double dx = x - cx, dy = y - cy;
    x = cx + dx * cs - dy * sn;
    y = cy + dx * sn + dy * cs;
}

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
// Контур штампа малюється нижче, а потрібен уже тут: кнопка штампа в смузі
// показує РІВНО той самий контур, що й ставить, щоб вони не розійшлися.
void EdStampShape(Gdiplus::Graphics& g, int id, float ox, float oy, float side,
                  Gdiplus::Color c);

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
    case IcoEllipse:
        g.DrawEllipse(&pen, 2.6f, 4.6f, 14.8f, 10.8f);
        break;
    case IcoLine:
        g.DrawLine(&pen, 3.8f, 15.6f, 16.2f, 4.4f);
        break;
    // Олівець показуємо його СЛІДОМ, а не самим олівцем: на 20 точках корпус
    // із гострим кінчиком і гумкою читався як щось незрозуміле, а крива —
    // саме те, що інструмент робить.
    case IcoPen:
        g.DrawBezier(&pen, 2.8f, 13.6f, 6.4f, 3.2f, 9.6f, 16.8f, 13.2f, 8.4f);
        g.DrawBezier(&pen, 13.2f, 8.4f, 15.0f, 3.8f, 16.4f, 4.6f, 17.2f, 7.2f);
        break;
    // Держак, гачок і наконечник-шеврон, вершина якого збігається з початком
    // держака: інакше наконечник «наїжджає» на лінію і зливається з нею.
    case IcoUndo:
        g.DrawLine(&pen, 6.4f, 7.8f, 12.4f, 7.8f);
        g.DrawArc(&pen, 9.2f, 7.8f, 6.4f, 6.4f, -90.0f, 180.0f);
        g.DrawLine(&pen, 12.4f, 14.2f, 10.2f, 14.2f);
        g.DrawLine(&pen, 9.1f, 5.2f, 6.4f, 7.8f);
        g.DrawLine(&pen, 6.4f, 7.8f, 9.1f, 10.4f);
        break;
    case IcoRedo:
        g.DrawLine(&pen, 13.6f, 7.8f, 7.6f, 7.8f);
        g.DrawArc(&pen, 4.4f, 7.8f, 6.4f, 6.4f, 90.0f, 180.0f);
        g.DrawLine(&pen, 7.6f, 14.2f, 9.8f, 14.2f);
        g.DrawLine(&pen, 10.9f, 5.2f, 13.6f, 7.8f);
        g.DrawLine(&pen, 13.6f, 7.8f, 10.9f, 10.4f);
        break;
    // Знак питання: дуга верхнього гачка, плавний перехід у ніжку кривою Безьє
    // і крапка окремо. Одна дуга на 230° читалась як кільце, а не як «?».
    case IcoHelp:
        g.DrawEllipse(&pen, 2.6f, 2.6f, 14.8f, 14.8f);
        g.DrawArc(&pen, 7.0f, 4.9f, 6.0f, 6.0f, 180.0f, 200.0f);
        g.DrawBezier(&pen, 12.82f, 8.93f, 12.2f, 10.7f, 10.0f, 10.6f, 10.0f, 12.3f);
        g.FillEllipse(&br, 9.15f, 13.9f, 1.7f, 1.7f);
        break;
    // IcoFront: прямокутник і стрілка вгору. IcoBack — той самий прямокутник,
    // але вище, і стрілка вниз. Дві заливки різного квадрата, як було раніше,
    // на цьому розмірі не розрізнялись узагалі.
    case IcoFront:
        g.DrawRectangle(&pen, 4.0f, 9.0f, 12.0f, 8.0f);
        g.DrawLine(&pen, 10.0f, 7.2f, 10.0f, 1.8f);
        g.DrawLine(&pen, 7.3f, 4.5f, 10.0f, 1.8f);
        g.DrawLine(&pen, 10.0f, 1.8f, 12.7f, 4.5f);
        break;
    case IcoBack:
        g.DrawRectangle(&pen, 4.0f, 3.0f, 12.0f, 8.0f);
        g.DrawLine(&pen, 10.0f, 12.8f, 10.0f, 18.2f);
        g.DrawLine(&pen, 7.3f, 15.5f, 10.0f, 18.2f);
        g.DrawLine(&pen, 10.0f, 18.2f, 12.7f, 15.5f);
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
    // Дублювання: два однакові аркуші зі зсувом. Задній навмисно не суцільний —
    // інакше на 20 точках пара зливається в одну товсту рамку.
    // Зберегти — стрілка вниз у лоток. Копіювати — планшет із затискачем:
    // два аркуші зі зсувом тут уже зайняті дублюванням.
    // Кнопки вікна малюємо тонкою лінією в один піксель, як це робить сама
    // Windows: товстий штрих поруч із системними вікнами читається як чужий.
    // Приховати — око з рискою: те саме, чим позначають «не показувати».
    // Лічильник — кружечок із одиницею всередині; малюємо саму цифру шляхами,
    // бо тексту в піктограмах у нас немає.
    // Поворот — три чверті кола і СУЦІЛЬНИЙ наконечник на кінці дуги.
    // ⚠ Наконечник із двох тонких ліній на 24 точках читався як обламаний хвіст,
    // а не як стрілка (зауваження власника по 3.8.0). Дрібні деталі на цьому
    // розмірі не виживають: або суцільна фігура, або нічого.
    case IcoRotR: {
        g.DrawArc(&pen, 3.0f, 3.4f, 14.0f, 14.0f, 290.0f, 320.0f);
        Gdiplus::PointF a[3] = { { 10.8f, 2.7f }, { 7.4f, 6.9f }, { 5.5f, 1.6f } };
        Gdiplus::SolidBrush b(c);
        g.FillPolygon(&b, a, 3);
        break;
    }
    case IcoRotL: {
        g.DrawArc(&pen, 3.0f, 3.4f, 14.0f, 14.0f, 250.0f, -320.0f);
        Gdiplus::PointF a[3] = { { 9.2f, 2.7f }, { 12.6f, 6.9f }, { 14.5f, 1.6f } };
        Gdiplus::SolidBrush b(c);
        g.FillPolygon(&b, a, 3);
        break;
    }
    // Дзеркало — плита і її відображення через вісь. Трикутники читались як
    // «назад/вперед», а пунктир на 24 точках однаково зливався в суцільну лінію,
    // тож вісь тепер чесно суцільна.
    case IcoFlipH: {
        // Вісь — рукотворний пунктир чотирма рисками: DashStyle на 24 точках
        // зливається в суцільну лінію, а суцільна лінія — це вже не вісь дзеркала.
        for (int i = 0; i < 4; ++i) {
            const float y0 = 2.4f + i * 4.0f;
            g.DrawLine(&pen, 10.0f, y0, 10.0f, y0 + 2.4f);
        }
        // ⚠ Прямокутні трикутники, а не рівнобедрені: у рівнобедрених немає
        // «верху й низу», і пара читається як «назад/вперед». Тут же кожен має
        // свій прямий кут, тож видно, що це та сама фігура, відображена.
        Gdiplus::PointF l[3] = { { 1.8f, 3.0f }, { 7.6f, 3.0f }, { 7.6f, 17.0f } };
        Gdiplus::PointF r[3] = { { 18.2f, 3.0f }, { 12.4f, 3.0f }, { 12.4f, 17.0f } };
        Gdiplus::SolidBrush b(c);
        g.FillPolygon(&b, l, 3);
        g.DrawPolygon(&pen, r, 3);
        break;
    }
    case IcoFlipV: {
        for (int i = 0; i < 4; ++i) {
            const float x0 = 2.4f + i * 4.0f;
            g.DrawLine(&pen, x0, 10.0f, x0 + 2.4f, 10.0f);
        }
        Gdiplus::PointF u[3] = { { 3.0f, 1.8f }, { 3.0f, 7.6f }, { 17.0f, 7.6f } };
        Gdiplus::PointF d[3] = { { 3.0f, 18.2f }, { 3.0f, 12.4f }, { 17.0f, 12.4f } };
        Gdiplus::SolidBrush b(c);
        g.FillPolygon(&b, u, 3);
        g.DrawPolygon(&pen, d, 3);
        break;
    }
    // Вирівнювання: вісь і дві плитки, притиснуті до неї. Плитки різної
    // довжини — інакше не видно, ЩО саме вирівнялось.
    case IcoAlL: case IcoAlCx: case IcoAlR: {
        Gdiplus::SolidBrush b(c);
        const float ax = (id == IcoAlL) ? 2.6f : (id == IcoAlR) ? 17.4f : 10.0f;
        g.DrawLine(&pen, ax, 2.0f, ax, 18.0f);
        const float w1 = 10.0f, w2 = 6.0f;
        const float x1 = (id == IcoAlL) ? ax + 1.4f : (id == IcoAlR) ? ax - 1.4f - w1 : ax - w1 / 2;
        const float x2 = (id == IcoAlL) ? ax + 1.4f : (id == IcoAlR) ? ax - 1.4f - w2 : ax - w2 / 2;
        g.FillRectangle(&b, x1, 4.6f, w1, 4.4f);
        g.FillRectangle(&b, x2, 11.0f, w2, 4.4f);
        break;
    }
    case IcoAlT: case IcoAlCy: case IcoAlB: {
        Gdiplus::SolidBrush b(c);
        const float ay = (id == IcoAlT) ? 2.6f : (id == IcoAlB) ? 17.4f : 10.0f;
        g.DrawLine(&pen, 2.0f, ay, 18.0f, ay);
        const float h1 = 10.0f, h2 = 6.0f;
        const float y1 = (id == IcoAlT) ? ay + 1.4f : (id == IcoAlB) ? ay - 1.4f - h1 : ay - h1 / 2;
        const float y2 = (id == IcoAlT) ? ay + 1.4f : (id == IcoAlB) ? ay - 1.4f - h2 : ay - h2 / 2;
        g.FillRectangle(&b, 4.6f, y1, 4.4f, h1);
        g.FillRectangle(&b, 11.0f, y2, 4.4f, h2);
        break;
    }
    case IcoDistX: {
        Gdiplus::SolidBrush b(c);
        g.FillRectangle(&b, 2.0f, 4.0f, 3.2f, 12.0f);
        g.FillRectangle(&b, 8.4f, 4.0f, 3.2f, 12.0f);
        g.FillRectangle(&b, 14.8f, 4.0f, 3.2f, 12.0f);
        break;
    }
    case IcoDistY: {
        Gdiplus::SolidBrush b(c);
        g.FillRectangle(&b, 4.0f, 2.0f, 12.0f, 3.2f);
        g.FillRectangle(&b, 4.0f, 8.4f, 12.0f, 3.2f);
        g.FillRectangle(&b, 4.0f, 14.8f, 12.0f, 3.2f);
        break;
    }
    // Група — рамка навколо двох плиток; розгрупування — ті самі плитки без неї.
    case IcoGroup: case IcoUngroup: {
        Gdiplus::SolidBrush b(c);
        g.FillRectangle(&b, 4.6f, 4.6f, 5.2f, 5.2f);
        g.FillRectangle(&b, 10.2f, 10.2f, 5.2f, 5.2f);
        if (id == IcoGroup) {
            Gdiplus::Pen fr(c, 1.2f);
            fr.SetDashStyle(Gdiplus::DashStyleDot);
            g.DrawRectangle(&fr, 2.0f, 2.0f, 16.0f, 16.0f);
        }
        break;
    }
    // Група лічильників: два кружечки — те саме, що ставить інструмент, але не
    // один. Перекреслені — та сама група, але видалена.
    // ⚠ Між кружечками потрібен зазор: дотичні читаються як знак нескінченності.
    case IcoGroupEdit:
        g.DrawEllipse(&pen, 1.2f, 6.0f, 7.6f, 7.6f);
        g.DrawEllipse(&pen, 11.2f, 6.0f, 7.6f, 7.6f);
        break;
    // Видалення групи — мінус у кружечку, рівно пара до плюса «нової групи»
    // (вимога власника). Дві дії над тим самим — двома дзеркальними знаками.
    case IcoGroupDel:
        g.DrawEllipse(&pen, 2.8f, 2.8f, 14.4f, 14.4f);
        g.DrawLine(&pen, 6.2f, 10.0f, 13.8f, 10.0f);
        break;
    // Порівняння — квадрат, розділений по діагоналі: до і після.
    case IcoCompare: {
        Gdiplus::PointF tri[3] = { { 2.6f, 17.4f }, { 17.4f, 17.4f }, { 17.4f, 2.6f } };
        Gdiplus::SolidBrush b(c);
        g.FillPolygon(&b, tri, 3);
        g.DrawRectangle(&pen, 2.6f, 2.6f, 14.8f, 14.8f);
        break;
    }
    // Кадр — дві кутові дужки, як позначають обрізання у фоторедакторах.
    case IcoCrop:
        g.DrawLine(&pen, 5.6f, 1.8f, 5.6f, 14.4f);
        g.DrawLine(&pen, 5.6f, 14.4f, 18.2f, 14.4f);
        g.DrawLine(&pen, 1.8f, 5.6f, 14.4f, 5.6f);
        g.DrawLine(&pen, 14.4f, 5.6f, 14.4f, 18.2f);
        break;
    case IcoCounter:
        g.DrawEllipse(&pen, 2.6f, 2.6f, 14.8f, 14.8f);
        g.DrawLine(&pen, 8.4f, 7.6f, 10.2f, 6.0f);
        g.DrawLine(&pen, 10.2f, 6.0f, 10.2f, 14.0f);
        g.DrawLine(&pen, 8.2f, 14.0f, 12.2f, 14.0f);
        break;
    case IcoStamp:
        g.DrawLine(&pen, 5.4f, 16.6f, 14.6f, 16.6f);
        g.DrawLine(&pen, 6.4f, 13.4f, 13.6f, 13.4f);
        g.DrawLine(&pen, 6.4f, 13.4f, 7.6f, 9.0f);
        g.DrawLine(&pen, 13.6f, 13.4f, 12.4f, 9.0f);
        g.DrawArc(&pen, 6.6f, 2.8f, 6.8f, 6.8f, 0.0f, -180.0f);
        g.DrawLine(&pen, 6.6f, 6.2f, 7.6f, 9.0f);
        g.DrawLine(&pen, 13.4f, 6.2f, 12.4f, 9.0f);
        break;
    // Нова група — коло з плюсом: те саме коло, що й сам лічильник, плюс
    // означає «ще одне». Кругова стрілка, яка тут стояла раніше, на двадцяти
    // точках зліплювалась у кільце й читалась як «оновити», а не «нова».
    case IcoNewGroup:
        g.DrawEllipse(&pen, 2.8f, 2.8f, 14.4f, 14.4f);
        g.DrawLine(&pen, 10.0f, 6.2f, 10.0f, 13.8f);
        g.DrawLine(&pen, 6.2f, 10.0f, 13.8f, 10.0f);
        break;
    case IcoMore:
        g.FillEllipse(&br, 3.0f, 8.6f, 2.8f, 2.8f);
        g.FillEllipse(&br, 8.6f, 8.6f, 2.8f, 2.8f);
        g.FillEllipse(&br, 14.2f, 8.6f, 2.8f, 2.8f);
        break;
    case IcoStCheck: case IcoStCross: case IcoStQuestion:
    case IcoStBang:  case IcoStStar:  case IcoStWarn: {
        // Піктограма штампа — той самий контур, що й сам штамп: одне джерело,
        // тож кнопка не може розійтися з тим, що вона ставить.
        const int side = EdMin(box.right - box.left, box.bottom - box.top);
        g.Restore(st);
        EdStampShape(g, id - IcoStCheck,
                     box.left + (box.right - box.left - side) / 2.0f,
                     box.top + (box.bottom - box.top - side) / 2.0f, (float)side, c);
        return;
    }
    case IcoHide:
        g.DrawBezier(&pen, 2.4f, 10.0f, 6.0f, 4.6f, 14.0f, 4.6f, 17.6f, 10.0f);
        g.DrawBezier(&pen, 17.6f, 10.0f, 14.0f, 15.4f, 6.0f, 15.4f, 2.4f, 10.0f);
        g.DrawEllipse(&pen, 7.4f, 7.0f, 5.2f, 6.0f);
        g.DrawLine(&pen, 3.6f, 16.8f, 16.4f, 3.2f);
        break;
    // Маркер — скошене перо зі слідом під ним.
    case IcoMark:
        g.DrawLine(&pen, 5.0f, 11.2f, 12.6f, 3.6f);
        g.DrawLine(&pen, 12.6f, 3.6f, 16.0f, 7.0f);
        g.DrawLine(&pen, 16.0f, 7.0f, 8.4f, 14.6f);
        g.DrawLine(&pen, 8.4f, 14.6f, 5.0f, 11.2f);
        g.DrawLine(&pen, 3.0f, 17.4f, 17.0f, 17.4f);
        break;
    case IcoBlur: {
        Gdiplus::SolidBrush b1(Gdiplus::Color(70, c.GetR(), c.GetG(), c.GetB()));
        Gdiplus::SolidBrush b2(Gdiplus::Color(130, c.GetR(), c.GetG(), c.GetB()));
        g.FillEllipse(&b1, 1.6f, 1.6f, 16.8f, 16.8f);
        g.FillEllipse(&b2, 4.8f, 4.8f, 10.4f, 10.4f);
        g.FillEllipse(&br, 7.8f, 7.8f, 4.4f, 4.4f);
        break;
    }
    case IcoPixels: {
        const float q = 4.2f;
        for (int gy = 0; gy < 3; ++gy)
            for (int gx = 0; gx < 3; ++gx) {
                Gdiplus::SolidBrush qb(Gdiplus::Color((BYTE)(70 + 60 * ((gx + gy) % 3)),
                                                      c.GetR(), c.GetG(), c.GetB()));
                g.FillRectangle(&qb, 3.4f + gx * (q + 0.6f), 3.4f + gy * (q + 0.6f), q, q);
            }
        break;
    }
    case IcoPlate:
        g.FillRectangle(&br, 3.0f, 5.0f, 14.0f, 10.0f);
        break;
    case IcoStrength:
        g.DrawLine(&pen, 3.4f, 14.6f, 3.4f, 12.2f);
        g.DrawLine(&pen, 7.6f, 14.6f, 7.6f, 9.4f);
        g.DrawLine(&pen, 11.8f, 14.6f, 11.8f, 6.6f);
        g.DrawLine(&pen, 16.0f, 14.6f, 16.0f, 3.8f);
        break;
    case IcoWinMin:
        g.DrawLine(&pen, 4.0f, 10.0f, 16.0f, 10.0f);
        break;
    case IcoWinMax:
        g.DrawRectangle(&pen, 4.5f, 4.5f, 11.0f, 11.0f);
        break;
    case IcoWinRestore:
        g.DrawRectangle(&pen, 3.5f, 6.5f, 10.0f, 10.0f);
        g.DrawLine(&pen, 6.5f, 6.5f, 6.5f, 3.5f);
        g.DrawLine(&pen, 6.5f, 3.5f, 16.5f, 3.5f);
        g.DrawLine(&pen, 16.5f, 3.5f, 16.5f, 13.5f);
        g.DrawLine(&pen, 16.5f, 13.5f, 13.5f, 13.5f);
        break;
    case IcoWinClose:
        g.DrawLine(&pen, 4.5f, 4.5f, 15.5f, 15.5f);
        g.DrawLine(&pen, 15.5f, 4.5f, 4.5f, 15.5f);
        break;
    case IcoSave:
        g.DrawLine(&pen, 10.0f, 2.8f, 10.0f, 11.8f);
        g.DrawLine(&pen, 6.4f, 8.4f, 10.0f, 12.0f);
        g.DrawLine(&pen, 10.0f, 12.0f, 13.6f, 8.4f);
        g.DrawLine(&pen, 3.2f, 12.6f, 3.2f, 16.6f);
        g.DrawLine(&pen, 3.2f, 16.6f, 16.8f, 16.6f);
        g.DrawLine(&pen, 16.8f, 16.6f, 16.8f, 12.6f);
        break;
    case IcoCopy:
        g.DrawRectangle(&pen, 4.2f, 3.8f, 11.6f, 13.6f);
        g.DrawLine(&pen, 7.6f, 3.8f, 7.6f, 2.4f);
        g.DrawLine(&pen, 7.6f, 2.4f, 12.4f, 2.4f);
        g.DrawLine(&pen, 12.4f, 2.4f, 12.4f, 3.8f);
        g.DrawLine(&pen, 7.2f, 9.0f, 12.8f, 9.0f);
        g.DrawLine(&pen, 7.2f, 12.4f, 12.8f, 12.4f);
        break;
    case IcoDup:
        g.DrawRectangle(&pen, 2.8f, 2.8f, 10.4f, 10.4f);
        g.DrawRectangle(&pen, 6.8f, 6.8f, 10.4f, 10.4f);
        break;
    case IcoOpen:
        g.DrawLine(&pen, 2.6f, 15.8f, 2.6f, 5.0f);
        g.DrawLine(&pen, 2.6f, 5.0f, 8.0f, 5.0f);
        g.DrawLine(&pen, 8.0f, 5.0f, 9.6f, 7.0f);
        g.DrawLine(&pen, 9.6f, 7.0f, 15.6f, 7.0f);
        g.DrawLine(&pen, 15.6f, 7.0f, 15.6f, 9.0f);
        g.DrawLine(&pen, 2.6f, 15.8f, 17.4f, 15.8f);
        g.DrawLine(&pen, 17.4f, 15.8f, 19.0f, 9.0f);
        g.DrawLine(&pen, 19.0f, 9.0f, 5.0f, 9.0f);
        g.DrawLine(&pen, 5.0f, 9.0f, 2.6f, 15.8f);
        break;
    case IcoChevD:
        g.DrawLine(&pen, 5.6f, 8.0f, 10.0f, 12.4f);
        g.DrawLine(&pen, 10.0f, 12.4f, 14.4f, 8.0f);
        break;
    case IcoText:
        g.DrawLine(&pen, 4.0f, 4.8f, 16.0f, 4.8f);
        g.DrawLine(&pen, 10.0f, 4.8f, 10.0f, 15.6f);
        g.DrawLine(&pen, 7.2f, 15.6f, 12.8f, 15.6f);
        break;
    // Жирний і курсив малюємо ТОВСТІШИМ пером за решту: на 20 точках різницю
    // накреслення видно лише масою штриха, а не формою літери.
    case IcoBold: {
        Gdiplus::Pen bp(c, sw * 1.9f);
        bp.SetLineJoin(Gdiplus::LineJoinRound);
        bp.SetStartCap(Gdiplus::LineCapRound);
        bp.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&bp, 6.6f, 4.6f, 6.6f, 15.4f);
        g.DrawLine(&bp, 6.6f, 4.6f, 10.6f, 4.6f);
        g.DrawArc(&bp, 7.4f, 4.6f, 6.4f, 5.4f, -90.0f, 180.0f);
        g.DrawLine(&bp, 6.6f, 10.0f, 11.0f, 10.0f);
        g.DrawArc(&bp, 7.4f, 10.0f, 7.2f, 5.4f, -90.0f, 180.0f);
        g.DrawLine(&bp, 6.6f, 15.4f, 11.0f, 15.4f);
        break;
    }
    case IcoItalic: {
        Gdiplus::Pen ip(c, sw * 1.5f);
        ip.SetStartCap(Gdiplus::LineCapRound);
        ip.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&ip, 8.2f, 4.6f, 14.6f, 4.6f);
        g.DrawLine(&ip, 5.4f, 15.4f, 11.8f, 15.4f);
        g.DrawLine(&ip, 11.8f, 4.6f, 8.2f, 15.4f);
        break;
    }
    case IcoAlignL: case IcoAlignC: case IcoAlignR: {
        const float ys[4] = { 5.2f, 8.4f, 11.6f, 14.8f };
        for (int i = 0; i < 4; ++i) {
            const float full = 12.8f;
            const float len = (i % 2) ? full * 0.62f : full;
            float x0 = 3.6f;
            if (id == IcoAlignC) x0 = 3.6f + (full - len) / 2.0f;
            if (id == IcoAlignR) x0 = 3.6f + (full - len);
            g.DrawLine(&pen, x0, ys[i], x0 + len, ys[i]);
        }
        break;
    }
    // Обводка: літера на плашці того тла, проти якого обводка й потрібна.
    // Без плашки світла обводка на світлій темі була б просто невидимою.
    case IcoStroke0: case IcoStroke1: case IcoStroke2: {
        const bool none  = (id == IcoStroke0);
        const bool light = (id == IcoStroke1);
        Gdiplus::PointF a[3] = { { 4.8f, 15.6f }, { 10.0f, 4.8f }, { 15.2f, 15.6f } };
        if (!none) {
            Gdiplus::SolidBrush chip(light ? Gdiplus::Color(255, 40, 40, 46)
                                           : Gdiplus::Color(255, 240, 240, 242));
            g.FillRectangle(&chip, 1.2f, 1.2f, 17.6f, 17.6f);
            Gdiplus::Pen op(light ? Gdiplus::Color(255, 255, 255, 255)
                                  : Gdiplus::Color(255, 22, 22, 26), sw * 3.2f);
            op.SetLineJoin(Gdiplus::LineJoinRound);
            op.SetStartCap(Gdiplus::LineCapRound);
            op.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLines(&op, a, 3);
            g.DrawLine(&op, 7.1f, 11.4f, 12.9f, 11.4f);
        }
        Gdiplus::Pen lp(none ? c : Gdiplus::Color(255, 232, 17, 35), sw * 1.3f);
        lp.SetLineJoin(Gdiplus::LineJoinRound);
        lp.SetStartCap(Gdiplus::LineCapRound);
        lp.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLines(&lp, a, 3);
        g.DrawLine(&lp, 7.1f, 11.4f, 12.9f, 11.4f);
        break;
    }
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

// Гліф займає не всю кнопку, а 58 % її сторони. Без цього поля іконка тисне
// на межі кнопки, а штрих, який масштабується разом із сіткою, виходить удвічі
// товщим за системні іконки поруч.
RECT EdIconBox(const RECT& r)
{
    const int side = (int)(EdMin(r.right - r.left, r.bottom - r.top) * 0.58f);
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    RECT b = { cx - side / 2, cy - side / 2, cx - side / 2 + side, cy - side / 2 + side };
    return b;
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

// Одне місце, де знімок дізнається про тон і геометрію: три копії цього
// присвоєння вже встигли б розійтися.
void EdSelMoreSnap(EdSnap& s);   // нижче: список вибраних живе далі за знімком

void EdSnapTone(EdSnap& s)
{
    s.exposure = g_edExposure; s.gamma = g_edGamma; s.contrast = g_edContrast;
    s.rot = g_edRot;           s.mirror = g_edMirror;
    s.srcId = g_edSrcId;
}

void EdPushUndo()
{
    EdSnap s;
    s.objs = g_edObjs;
    s.sel  = g_edSel;
    s.crop = g_edCrop;
    EdSnapTone(s);
    EdSelMoreSnap(s);
    g_edUndo.push_back(s);
    if ((int)g_edUndo.size() > kEdUndoMax) g_edUndo.erase(g_edUndo.begin());
    g_edRedo.clear();
}

void EdApply(const EdSnap& s)
{
    g_edObjs = s.objs;
    g_edSel  = s.sel;
    g_edSelMore = s.selMore;
    g_edCrop = s.crop;
    if (g_edSel >= (int)g_edObjs.size()) g_edSel = -1;

    // Повернення до іншого оригіналу — теж «геометрія»: міняється розмір.
    bool srcBack = false;
    if (s.srcId != g_edSrcId && s.srcId >= 0 && s.srcId < (int)g_edSrcBank.size()) {
        g_edSrcId = s.srcId;
        g_edSrc = g_edSrcBank[s.srcId];
        srcBack = true;
    }
    const bool geom = srcBack || (s.rot != g_edRot || s.mirror != g_edMirror);
    const bool tone = (s.exposure != g_edExposure || s.gamma != g_edGamma ||
                       s.contrast != g_edContrast);
    g_edExposure = s.exposure; g_edGamma = s.gamma; g_edContrast = s.contrast;
    g_edRot = s.rot; g_edMirror = s.mirror;
    if (geom || tone) EdRebuildImage();
    if (geom && g_edWnd) { EdFitView(); EdLayout(g_edWnd); }
}

void EdUndoAction()
{
    if (g_edUndo.empty()) return;
    EdSnap cur;
    cur.objs = g_edObjs;
    cur.sel  = g_edSel;
    cur.crop = g_edCrop;
    EdSnapTone(cur);
    EdSelMoreSnap(cur);
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
    cur.crop = g_edCrop;
    EdSnapTone(cur);
    EdSelMoreSnap(cur);
    g_edUndo.push_back(cur);
    EdApply(g_edRedo.back());
    g_edRedo.pop_back();
    InvalidateRect(g_edWnd, nullptr, FALSE);
}

// ---- геометрія полотна --------------------------------------------------

bool EdHasCrop()
{
    return g_edCrop.right > g_edCrop.left && g_edCrop.bottom > g_edCrop.top;
}

// Видима ділянка: кадр, якщо він є, інакше весь знімок. Через ці чотири
// функції проходить уся геометрія полотна — тому кадр і не довелося
// розмазувати по всьому редактору.
int EdViewX() { return EdHasCrop() ? g_edCrop.left : 0; }
int EdViewY() { return EdHasCrop() ? g_edCrop.top  : 0; }
int EdViewW() { return EdHasCrop() ? g_edCrop.right - g_edCrop.left : g_edImgW; }
int EdViewH() { return EdHasCrop() ? g_edCrop.bottom - g_edCrop.top : g_edImgH; }

double EdFitScale()
{
    const int cw = g_edRcCanvas.right - g_edRcCanvas.left - EdPx(24);
    const int ch = g_edRcCanvas.bottom - g_edRcCanvas.top - EdPx(24);
    const int vw = EdViewW(), vh = EdViewH();
    if (!vw || !vh || cw <= 0 || ch <= 0) return 1.0;
    double fit = 1.0;
    if (vw > cw) fit = (double)cw / vw;
    if (vh * fit > ch) fit = (double)ch / vh;
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
    int dw = (int)(EdViewW() * s + 0.5), dh = (int)(EdViewH() * s + 0.5);
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
    // Повертаємо АБСОЛЮТНУ координату знімка: позначки живуть у ній, тож кадр
    // їх не зачіпає — ні тих, що вже стоять, ні тих, які ставлять у кадрі.
    POINT p;
    p.x = (s > 0) ? EdViewX() + (int)((scr.x - ir.left) / s + 0.5) : 0;
    p.y = (s > 0) ? EdViewY() + (int)((scr.y - ir.top) / s + 0.5) : 0;
    return p;
}

RECT EdObjScreen(const EdObj& o)
{
    const RECT ir = EdImageRect();
    const double s = EdScale();
    RECT r;
    r.left   = ir.left + (int)((o.x - EdViewX()) * s + 0.5);
    r.top    = ir.top + (int)((o.y - EdViewY()) * s + 0.5);
    r.right  = ir.left + (int)((o.x + o.w - EdViewX()) * s + 0.5);
    r.bottom = ir.top + (int)((o.y + o.h - EdViewY()) * s + 0.5);
    return r;
}

// Ручки — сталого розміру на екрані. Якби вони масштабувалися разом із
// зображенням, на 8× вони перекрили б сам об'єкт.
// Кількість ручок залежить від виду: у відрізка їх дві, по кінцях, бо тягнути
// «середину лівої сторони» в лінії нема сенсу.
// У напису ручок дві, по боках, і вони міняють ШИРИНУ БЛОКА, а не кегль.
// Ручки, що розтягують самі літери, дали б неоднакову висоту рядків у сусідніх
// написах; висота блока завжди виходить із тексту, тож верхніх і нижніх ручок
// у нього теж немає.
int EdHandleCount(const EdObj& o)
{
    if (EdIsStamped(o.kind)) return 0;
    // Маркер, як і напис, тягнеться лише вшир: висота смуги — це набір із трьох
    // значень, і ручка, що її розтягує, зробила б сусідні смуги різними.
    if (o.kind == EdKind::Text || o.kind == EdKind::Mark) return 2;
    return EdIsSegment(o.kind) ? 2 : 8;
}

// Ручка повороту — окремо від восьми ручок розміру, над верхнім краєм.
// Окремо саме тому, що дія в неї інша: сплутати її з розміром дорого.
bool EdRotHandle(const EdObj& o, RECT* out)
{
    if (!EdCanRotate(o.kind)) return false;
    const RECT r = EdObjScreen(o);
    const int h = EdPx(11), k = h / 2;
    double hx = (r.left + r.right) / 2.0, hy = r.top - EdPx(22);
    EdRotatePt((r.left + r.right) / 2.0, (r.top + r.bottom) / 2.0, o.rot, hx, hy);
    out->left = (LONG)(hx + 0.5) - k;
    out->top  = (LONG)(hy + 0.5) - k;
    out->right = out->left + h;
    out->bottom = out->top + h;
    return true;
}

// Габарити вибраного НА ЕКРАНІ. Окрема функція, бо EdSelBounds рахує точки
// знімка, а ручки живуть у пікселях вікна й не масштабуються.
bool EdSelScreenBox(RECT* out)
{
    RECT b;
    if (!EdSelBounds(&b)) return false;
    const RECT ir = EdImageRect();
    const double sc = EdScale();
    out->left   = ir.left + (int)((b.left  - EdViewX()) * sc + 0.5);
    out->top    = ir.top  + (int)((b.top   - EdViewY()) * sc + 0.5);
    out->right  = ir.left + (int)((b.right - EdViewX()) * sc + 0.5);
    out->bottom = ir.top  + (int)((b.bottom - EdViewY()) * sc + 0.5);
    return true;
}

// Вісім ручок спільної рамки. Порядок той самий, що в EdHandles, щоб курсор і
// логіка «протилежний кут нерухомий» працювали без окремої таблиці.
int EdManyHandles(RECT out[8])
{
    RECT r;
    if (!EdManySel() || !EdSelScreenBox(&r)) return 0;
    const int h = EdPx(9), k = h / 2;
    const int xs[3] = { (int)r.left, (int)(r.left + r.right) / 2, (int)r.right };
    const int ys[3] = { (int)r.top, (int)(r.top + r.bottom) / 2, (int)r.bottom };
    const int ix[8] = { 0, 1, 2, 2, 2, 1, 0, 0 };
    const int iy[8] = { 0, 0, 0, 1, 2, 2, 2, 1 };
    for (int i = 0; i < 8; ++i) {
        out[i].left   = xs[ix[i]] - k;
        out[i].top    = ys[iy[i]] - k;
        out[i].right  = out[i].left + h;
        out[i].bottom = out[i].top + h;
    }
    return 8;
}

bool EdManyRotHandle(RECT* out)
{
    RECT r;
    if (!EdManySel() || !EdSelScreenBox(&r)) return false;
    const int h = EdPx(11), k = h / 2;
    out->left = (r.left + r.right) / 2 - k;
    out->top  = r.top - EdPx(22) - k;
    out->right = out->left + h;
    out->bottom = out->top + h;
    return true;
}

int EdHandles(const EdObj& o, RECT out[8])
{
    const int h = EdPx(9), k = h / 2;
    const int n = EdHandleCount(o);
    // Прямокутники ручок рахуються на НЕПОВЕРНУТІЙ рамці, а наприкінці
    // повертаються навколо центра — інакше вони лишились би стояти рівно,
    // поки сама позначка нахилена.
    struct RotAll {
        const EdObj& o;
        RECT* out;
        int n;
        ~RotAll() {
            if (o.rot == 0 || !EdCanRotate(o.kind)) return;
            const RECT r = EdObjScreen(o);
            const double cx = (r.left + r.right) / 2.0, cy = (r.top + r.bottom) / 2.0;
            for (int i = 0; i < n; ++i) {
                double hx = (out[i].left + out[i].right) / 2.0;
                double hy = (out[i].top + out[i].bottom) / 2.0;
                EdRotatePt(cx, cy, o.rot, hx, hy);
                const int side = out[i].right - out[i].left;
                out[i].left = (LONG)(hx + 0.5) - side / 2;
                out[i].top  = (LONG)(hy + 0.5) - side / 2;
                out[i].right = out[i].left + side;
                out[i].bottom = out[i].top + side;
            }
        }
    } rotAll{ o, out, n };
    if (o.kind == EdKind::Text || o.kind == EdKind::Mark) {
        const RECT r = EdObjScreen(o);
        const int cy = (r.top + r.bottom) / 2;
        const int cx[2] = { r.left, r.right };
        for (int i = 0; i < 2; ++i) {
            out[i].left = cx[i] - k;  out[i].top = cy - k;
            out[i].right = out[i].left + h;  out[i].bottom = out[i].top + h;
        }
        return n;
    }
    if (EdIsSegment(o.kind)) {
        const RECT ir = EdImageRect();
        const double sc = EdScale();
        const int cx[2] = { ir.left + (int)(o.x * sc + 0.5),
                            ir.left + (int)((o.x + o.w) * sc + 0.5) };
        const int cy[2] = { ir.top + (int)(o.y * sc + 0.5),
                            ir.top + (int)((o.y + o.h) * sc + 0.5) };
        for (int i = 0; i < 2; ++i) {
            out[i].left = cx[i] - k;  out[i].top = cy[i] - k;
            out[i].right = out[i].left + h;  out[i].bottom = out[i].top + h;
        }
        return n;
    }
    const RECT r = EdObjScreen(o);
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
    return n;
}

// Стеля масштабу подвійна: 800 % на екрані і 20 000 точок на довгій стороні.
// Друга важливіша — за нею GDI+ починає їсти пам'ять горстями.
float EdZoomMax()
{
    const double fit = EdFitScale();
    const double longSide = (EdViewW() > EdViewH() ? EdViewW() : EdViewH()) * fit;
    double z = (fit > 0.0) ? 8.0 / fit : 8.0;
    if (longSide > 0 && longSide * z > 20000.0) z = 20000.0 / longSide;
    if (z < 1.0) z = 1.0;
    return (float)z;
}

// Повзунок лінійний за ЛОГАРИФМОМ масштабу: при лінійній шкалі перша чверть
// ходу з'їдала б увесь корисний діапазон, а решта тягнула б одні й ті самі
// величезні збільшення.
int EdZoomToSlider(float z)
{
    const double zmax = EdZoomMax();
    if (zmax <= 1.0) return 0;
    double p = log((double)z) / log(zmax);
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;
    return (int)(p * 100.0 + 0.5);
}

float EdSliderToZoom(int pos)
{
    const double zmax = EdZoomMax();
    if (zmax <= 1.0) return 1.0f;
    double p = pos / 100.0;
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;
    return (float)pow(zmax, p);
}

// Один шлях зміни масштабу для всіх: колеса, повзунка й кнопки «100 %».
// Точка cur лишається під тим самим пікселем зображення — саме через це
// збільшення не «тікає» від того місця, на яке дивляться.
void EdZoomSet(float z, POINT cur)
{
    if (!g_edImg) return;
    const double fit = EdFitScale();
    const float oldZoom = g_edZoom;
    if (z < 1.0f) z = 1.0f;
    const float zmax = EdZoomMax();
    if (z > zmax) z = zmax;
    if (z == oldZoom) return;

    const RECT before = EdImageRect();
    const double sOld = fit * oldZoom;
    const double ix = (sOld > 0) ? (cur.x - before.left) / sOld : 0.0;
    const double iy = (sOld > 0) ? (cur.y - before.top) / sOld : 0.0;

    g_edZoom = z;
    const double sNew = fit * z;
    const int dw = (int)(EdViewW() * sNew + 0.5), dh = (int)(EdViewH() * sNew + 0.5);
    const int cw = g_edRcCanvas.right - g_edRcCanvas.left;
    const int ch = g_edRcCanvas.bottom - g_edRcCanvas.top;
    g_edPanX = (int)(cur.x - ix * sNew - g_edRcCanvas.left - (cw - dw) / 2.0 + 0.5);
    g_edPanY = (int)(cur.y - iy * sNew - g_edRcCanvas.top - (ch - dh) / 2.0 + 0.5);
    EdClampPan(dw, dh);
    InvalidateRect(g_edWnd, nullptr, FALSE);
}

void EdZoomAt(POINT cur, bool in)
{
    EdZoomSet(in ? g_edZoom * 1.25f : g_edZoom / 1.25f, cur);
}

// Прокрутка полотна. Зсув у ЕКРАННИХ точках: коліщатко крутить видиму
// картинку, а не пікселі знімка, і крок не має залежати від масштабу.
// Обмеження рахує EdClampPan усередині EdImageRect — тут воно не потрібне.
void EdScrollBy(int dx, int dy)
{
    if (!g_edImg) return;
    const int oldX = g_edPanX, oldY = g_edPanY;
    g_edPanX += dx;
    g_edPanY += dy;
    const RECT before = EdImageRect();      // сам і затисне значення в межі
    (void)before;
    if (g_edPanX != oldX || g_edPanY != oldY)
        InvalidateRect(g_edWnd, nullptr, FALSE);
}

// Крок коліщатка: одна «зарубка» = 120 одиниць. Беремо системну кількість
// рядків прокрутки, як це робить решта Windows, і рахуємо рядок у 40 точок.
int EdWheelStep(int delta)
{
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    if (lines == 0 || lines > 20) lines = 3;      // «по екрану» теж зводимо до трьох
    return (int)(delta / 120.0 * lines * EdPx(40));
}

POINT EdCanvasCentre()
{
    POINT c = { (g_edRcCanvas.left + g_edRcCanvas.right) / 2,
                (g_edRcCanvas.top + g_edRcCanvas.bottom) / 2 };
    return c;
}

void EdZoomHundred()
{
    const double fit = EdFitScale();
    EdZoomSet(fit > 0.0 ? (float)(1.0 / fit) : 1.0f, EdCanvasCentre());
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

// ⚠ Висоту секції «ЗНІМОК» рахує ОДНА функція: розкладка ставить по ній
// кнопки, малювання — рядки. Дві копії цієї арифметики розійшлися б на першому
// ж рядку, який з'являється не завжди (джерело, HDR).
int EdPanelInfoBottom()
{
    int y = g_edRcPanel.top + EdPx(14) + EdPx(18) + EdPx(12);
    y += EdPx(20) + EdPx(6);                       // «Зображення · Ш × В»
    if (g_edSource[0]) y += EdPx(20) + EdPx(6);
    if (g_edHdr)       y += EdPx(20) + EdPx(6);
    return y + EdPx(20);                           // «Позначок: N»
}

// Потрібна вже в розкладці: кнопки поширення немає там, де меню недоступне.
bool EdShareAvailable();
void EdShareNow(HWND hwnd);

// Потрібні вже в розкладці й у чіпі — тіла нижче, біля решти дій над вибором.
int  EdSelCount();
bool EdManySel();
const RECT* EdRegionRect(EdHit what, int idx);
int EdPickCount(int group);           // селект наконечника довший за решту

void EdLayout(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    g_edRegions.clear();

    const int cap = EdPx(kEdCaption);
    const int strip = EdPx(kEdStrip), rail = EdPx(kEdRail), status = EdPx(kEdStatus);
    const int panel = g_edPanelOpen ? EdPx(kEdPanel) : EdPx(kEdPanelLo);

    g_edRcCaption = { 0, 0, rc.right, cap };
    g_edRcStrip  = { 0, cap, rc.right, cap + strip };
    g_edRcStatus = { 0, rc.bottom - status, rc.right, rc.bottom };
    g_edRcRail   = { 0, cap + strip, rail, rc.bottom - status };
    g_edRcPanel  = { rc.right - panel, cap + strip, rc.right, rc.bottom - status };
    g_edRcCanvas = { rail, cap + strip, rc.right - panel, rc.bottom - status };

    // Клацнули по кружечку — його група стає поточною: так до старої групи
    // повертаються без жодних кнопок.
    if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size() && g_edObjs[g_edSel].kind == EdKind::Counter)
        g_edCounterGroup = g_edObjs[g_edSel].group;

    // заголовок: кнопки вікна праворуч, перед ними — скасувати, повторити, довідка
    {
        const int ccy = (g_edRcCaption.top + g_edRcCaption.bottom) / 2;
        const int bw = EdPx(46);
        int rx = rc.right - bw;
        RECT rcl = { rx, 0, rx + bw, cap }; EdAdd(rcl, EdHit::Close, 0);
        rx -= bw;
        RECT rmx = { rx, 0, rx + bw, cap }; EdAdd(rmx, EdHit::Max, 0);
        rx -= bw;
        RECT rmn = { rx, 0, rx + bw, cap }; EdAdd(rmn, EdHit::Min, 0);

        const int b = EdPx(32);
        rx -= EdPx(10) + b;
        RECT rh = EdPill(rx, ccy, b, b); EdAdd(rh, EdHit::Help, 0);
        rx -= b + EdPx(2);
        RECT rr = EdPill(rx, ccy, b, b); EdAdd(rr, EdHit::Redo, 0);
        rx -= b + EdPx(2);
        RECT ru = EdPill(rx, ccy, b, b); EdAdd(ru, EdHit::Undo, 0);
        g_edCapLimit = rx - EdPx(10);
    }

    EdAdd(g_edRcCanvas, EdHit::Canvas, 0);   // найнижчий пріоритет: перевіряємо останнім

    // Підтвердження кадру живе на самому кадрі. Якщо під ним немає місця —
    // піднімаємо всередину: кнопки, що вилізли за полотно, не натиснути.
    if (g_edCropping) {
        HDC dc0 = GetDC(hwnd);
        const RECT cr0 = EdCropScreen(g_edCropEdit);
        const int bh = EdPx(30);
        const int wOk = EdTextWidth(dc0, S(Str::EdCropApply), g_edFont) + EdPx(28);
        const int wNo = EdTextWidth(dc0, S(Str::EdCropCancel), g_edFont) + EdPx(24);
        int by = cr0.bottom + EdPx(12);
        if (by + bh > g_edRcCanvas.bottom - EdPx(6)) by = cr0.bottom - bh - EdPx(12);
        if (by < g_edRcCanvas.top + EdPx(6)) by = g_edRcCanvas.top + EdPx(6);
        int bx = cr0.right - wOk;
        if (bx < g_edRcCanvas.left + EdPx(6)) bx = g_edRcCanvas.left + EdPx(6);
        RECT rOk = { bx, by, bx + wOk, by + bh };
        EdAdd(rOk, EdHit::CropOk, 0);
        RECT rNo = { bx - EdPx(6) - wNo, by, bx - EdPx(6), by + bh };
        EdAdd(rNo, EdHit::CropNo, 0);
        ReleaseDC(hwnd, dc0);
    }

    // панель інструментів
    {
        const int b = EdPx(40), gap = EdPx(4);
        int y = g_edRcRail.top + EdPx(8);
        const int x = g_edRcRail.left + (rail - b) / 2;
        for (int i = 0; i < 11; ++i) {
            if (i == 1) y += EdPx(7);      // вказівник відділено від фігур
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
        const wchar_t* chip = EdChipLabel();
        if (chip) {
            const int w = EdTextWidth(dc, chip, g_edFontBold) + EdPx(20);
            RECT r = EdPill(x, cy, w, EdPx(26));
            EdAdd(r, EdHit::None, 0);
            x = r.right + gap;
        }

        // CAPS-35: коли вибрано кілька, смуга ІНША — у ній лише дії над ними
        // (рішення власника 21.09). Властивостей окремої позначки тут немає,
        // тож і питання про брак місця не виникає.
        const bool many = (EdSelCount() >= 2);
        if (many) {
            for (int i = 0; i < 8; ++i) {
                RECT r = EdPill(x, cy, EdPx(32), EdPx(28));
                EdAdd(r, EdHit::SelAlign, i);
                x = r.right + ((i == 2 || i == 5) ? EdPx(10) : EdPx(3));
            }
            x += gap - EdPx(3);
            const bool grouped = (g_edSel >= 0 && g_edObjs[g_edSel].grp != 0);
            RECT rg = EdPill(x, cy, EdPx(32), EdPx(28));
            EdAdd(rg, EdHit::SelGroup, grouped ? 1 : 0);
            x = rg.right + gap;
        }

        const bool showProps = (hasSel || g_edTool != EdTool::Select) && !many;
        if (showProps) {
            const EdKind kk = hasSel ? g_edObjs[g_edSel].kind : EdToolKind(g_edTool);
            const int curMode = (hasSel && kk == EdKind::Hide) ? g_edObjs[g_edSel].mode : g_edHideMode;

            // Приховування: три режими й сила. Кольору в розмиття й пікселів
            // немає — фарбувати там нічого, тож і зразків не показуємо.
            if (kk == EdKind::Hide) {
                for (int i = 0; i < 3; ++i) {
                    RECT r = EdPill(x, cy, EdPx(30), EdPx(28));
                    EdAdd(r, EdHit::HideMode, i);
                    x = r.right + EdPx(2);
                }
                x += gap - EdPx(2);
                // Плашка суцільна, і сили в неї немає — повзунок, який нічого не
                // робить, гірший за його відсутність (зауваження власника 20.09).
                if (curMode != 2) {
                    RECT si = EdPill(x, cy, EdPx(18), EdPx(18));
                    EdAdd(si, EdHit::None, 0);
                    x = si.right + EdPx(8);
                    RECT ss = EdPill(x, cy, EdPx(76), EdPx(20));
                    EdAdd(ss, EdHit::Strength, 0);
                    x = ss.right + EdPx(8) + EdPx(44) + gap;
                }
            }

            // Кадр: пропорції й скидання. Підтвердження — на самому кадрі.
            if (kk == EdKind::Rect && g_edTool == EdTool::Crop) {
                for (int i = 0; i < 4; ++i) {
                    const wchar_t* lab = i == 0 ? S(Str::EdAspectFree)
                                       : i == 1 ? L"16:9" : i == 2 ? L"4:3" : L"1:1";
                    RECT r = EdPill(x, cy, EdTextWidth(dc, lab, g_edFont) + EdPx(18), EdPx(28));
                    EdAdd(r, EdHit::Aspect, i);
                    x = r.right + EdPx(4);
                }
                x += gap - EdPx(4);
                RECT rr = EdPill(x, cy, EdTextWidth(dc, S(Str::EdCropReset), g_edFont) + EdPx(20),
                                 EdPx(28));
                EdAdd(rr, EdHit::CropReset, 0);
                x = rr.right + gap;
            }

            // Штампи: шість контурів, чотири швидкі емодзі й «…» на решту.
            if (kk == EdKind::Stamp) {
                for (int i = 0; i < kEdVectorStamps + kEdEmojiQuick; ++i) {
                    RECT r = EdPill(x, cy, EdPx(28), EdPx(28));
                    EdAdd(r, EdHit::StampPick, i);
                    x = r.right + EdPx(2);
                }
                RECT rm = EdPill(x, cy, EdPx(28), EdPx(28));
                EdAdd(rm, EdHit::StampMore, 0);
                x = rm.right + gap;
            }

            // Лічильник читається зліва направо як речення: яка група, з якого
            // числа вона починається, який номер піде наступним. Кнопка нової
            // групи стоїть окремо в кінці смуги — це дія, а не властивість.
            if (kk == EdKind::Counter) {
                const int grp = EdCurGroup();
                wchar_t gb[64];
                wsprintfW(gb, S(Str::EdFmtGroupOnly), grp + 1);
                RECT gt = EdPill(x, cy, EdTextWidth(dc, gb, g_edFontBold) + EdPx(6), EdPx(26));
                EdAdd(gt, EdHit::NumGroup, 0);
                x = gt.right + EdPx(14);

                // Підпис теж є ділянкою — лише заради підказки на наведення.
                RECT lb = EdPill(x, cy, EdTextWidth(dc, S(Str::EdNumStartLabel), g_edFont) + EdPx(4),
                                 EdPx(26));
                EdAdd(lb, EdHit::NumStart, 2);
                x = lb.right + EdPx(6);
                RECT sm = EdPill(x, cy, EdPx(24), EdPx(26)); EdAdd(sm, EdHit::NumStart, 0);
                x = sm.right + EdPx(2) + EdPx(30) + EdPx(2);
                RECT sp = EdPill(x, cy, EdPx(24), EdPx(26)); EdAdd(sp, EdHit::NumStart, 1);
                x = sp.right + EdPx(14);

                wchar_t nx[64];
                wsprintfW(nx, S(Str::EdFmtNext), EdGroupStart(grp) + EdGroupCount(grp));
                RECT nt = EdPill(x, cy, EdTextWidth(dc, nx, g_edFont) + EdPx(4), EdPx(26));
                EdAdd(nt, EdHit::NumNext, 0);
                x = nt.right + gap;

                // Тогл стоїть ПЕРЕД кольором — рівно там, де починається те,
                // на що він впливає (колір, розмір, прозорість).
                RECT ge = EdPill(x, cy, EdPx(32), EdPx(28));
                EdAdd(ge, EdHit::GroupEdit, 0);
                x = ge.right + gap;
            }

            const bool cropMode = (g_edTool == EdTool::Crop);
            int npal = 8;
            const COLORREF* pal = EdPaletteFor(kk, npal);
            // Емодзі мають власний колір, палітра на них не діє.
            const bool emojiStamp = (kk == EdKind::Stamp) &&
                ((hasSel ? g_edObjs[g_edSel].stamp : g_edStamp) >= kEdEmojiBase);
            const bool showPal = ((kk != EdKind::Hide) || (curMode == 2)) && !emojiStamp &&
                                 !cropMode && kk != EdKind::Image;
            if (showPal) {
                const int sw = EdPx(22), sg = EdPx(5);
                for (int i = 0; i < npal; ++i) {
                    RECT r = EdPill(x, cy, sw, sw);
                    EdAdd(r, EdHit::Swatch, i);
                    x = r.right + sg;
                }
                x += gap - sg;
            }
            (void)pal;

            if (EdHasThick(kk) && !cropMode) {
                for (int i = 0; i < 3; ++i) {
                    RECT r = EdPill(x, cy, EdPx(34), EdPx(26));
                    EdAdd(r, EdHit::Thick, i);
                    x = r.right + EdPx(4);
                }
                x += gap - EdPx(4);
            }

            // CAPS-34: стиль лінії — усім контурним; наконечники — лише стрілці.
            // Кожен набір згорнуто у ВИПАДНИЙ СЕЛЕКТ (рішення власника 21.09):
            // кнопка показує поточний вибір, решта варіантів — за нею.
            if (EdHasDash(kk) && !cropMode) {
                const int pn = (kk == EdKind::Line) ? 4 : 1;
                for (int gi = 0; gi < pn; ++gi) {
                    RECT r = EdPill(x, cy, EdPx(42), EdPx(28));
                    EdAdd(r, EdHit::Pick, gi);
                    x = r.right + EdPx(4);
                }
                x += gap - EdPx(4);
            }

            // Набір напису: кегль степером, накреслення, вирівнювання, обводка.
            // Кнопки навмисно вужчі за решту смуги — інакше цей набір не влазить
            // у вікно мінімальної ширини разом із рештою.
            if (kk == EdKind::Text) {
                RECT sm = EdPill(x, cy, EdPx(26), EdPx(26));
                EdAdd(sm, EdHit::Size, 0);
                x = sm.right + EdPx(2) + EdPx(40) + EdPx(2);
                RECT sp = EdPill(x, cy, EdPx(26), EdPx(26));
                EdAdd(sp, EdHit::Size, 1);
                x = sp.right + EdPx(10);

                RECT bb = EdPill(x, cy, EdPx(28), EdPx(26)); EdAdd(bb, EdHit::Bold, 0);
                x = bb.right + EdPx(2);
                RECT ib = EdPill(x, cy, EdPx(28), EdPx(26)); EdAdd(ib, EdHit::Italic, 0);
                x = ib.right + EdPx(10);

                for (int i = 0; i < 3; ++i) {
                    RECT r = EdPill(x, cy, EdPx(28), EdPx(26));
                    EdAdd(r, EdHit::Align, i);
                    x = r.right + EdPx(2);
                }
                x += EdPx(8);
                for (int i = 0; i < 3; ++i) {
                    RECT r = EdPill(x, cy, EdPx(28), EdPx(26));
                    EdAdd(r, EdHit::Stroke, i);
                    x = r.right + EdPx(2);
                }
                x += EdPx(10);
            }

            // Заливка має сенс лише для замкнених фігур: у лінії заливати нічого.
            // Кадр — узагалі не фігура, хоч його вид і рахується прямокутником.
            if (EdCanFill(kk) && !cropMode) {
                const int w1 = EdTextWidth(dc, S(Str::EdOutline), g_edFont) + EdPx(24);
                const int w2 = EdTextWidth(dc, S(Str::EdFilled), g_edFont) + EdPx(24);
                RECT a = EdPill(x, cy, w1, EdPx(28)); EdAdd(a, EdHit::Fill, 0);
                // Проміжок такий самий, як між товщинами: злиті кнопки читалися
                // як один перемикач із двох половин, а це два різні стани.
                RECT b2 = EdPill(a.right + EdPx(4), cy, w2, EdPx(28)); EdAdd(b2, EdHit::Fill, 1);
                x = b2.right + gap;
            }

            RECT ic = EdPill(x, cy, cropMode ? 0 : EdPx(18), EdPx(18));
            if (!cropMode) EdAdd(ic, EdHit::None, 0);
            x = ic.right + (cropMode ? 0 : EdPx(8));
            RECT sl = EdPill(x, cy, cropMode ? 0 : EdPx(76), EdPx(20));
            if (!cropMode) EdAdd(sl, EdHit::Opacity, 0);
            x = sl.right + EdPx(8) + EdPx(44) + gap;

            if (kk == EdKind::Counter) {
                RECT rb = EdPill(x, cy, EdPx(32), EdPx(28));
                EdAdd(rb, EdHit::NumReset, 0);
                x = rb.right + EdPx(4);
                RECT gd = EdPill(x, cy, EdPx(32), EdPx(28));
                EdAdd(gd, EdHit::GroupDel, 0);
                x = gd.right + gap;
            }
        }

        // Скасувати, повторити й довідка переїхали в заголовок (рішення власника
        // 20.09), а дії над вибраним лишились притиснутими праворуч тут: у лівій
        // течії вони налазили на сусідів, щойно кнопок побільшало.
        {
            const int b = EdPx(32);
            int rx = rc.right - EdPx(14) - b;
            if (hasSel) {
                const EdHit acts[4] = { EdHit::Del, EdHit::Dup, EdHit::Back, EdHit::Front };
                for (int i = 0; i < 4; ++i) {
                    RECT ra = EdPill(rx, cy, b, b);
                    EdAdd(ra, acts[i], 0);
                    if (i < 3) rx -= b + EdPx(4);
                }
            } else {
                rx = rc.right - EdPx(14);
            }
            g_edStripLimit = rx - EdPx(10);
        }
        ReleaseDC(hwnd, dc);
    }

    // рядок стану: масштаб
    {
        HDC dc = GetDC(hwnd);
        const int cy = (g_edRcStatus.top + g_edRcStatus.bottom) / 2;
        int x = EdPx(14);

        const int ow = EdTextWidth(dc, S(Str::EdOpenBtn), g_edFont) + EdPx(40);
        RECT ob = EdPill(x, cy, ow, EdPx(30));
        EdAdd(ob, EdHit::Open, 0);
        RECT om = EdPill(ob.right, cy, EdPx(22), EdPx(30));
        EdAdd(om, EdHit::OpenMenu, 0);
        x = om.right + EdPx(12) + 1 + EdPx(12);

        x += EdTextWidth(dc, L"8888 × 8888", g_edFont) + EdPx(12) + 1 + EdPx(12);
        x += EdPx(190) + EdPx(12) + 1 + EdPx(12);   // місце під опис виділення

        RECT zs = EdPill(x, cy, EdPx(104), EdPx(20)); EdAdd(zs, EdHit::Zoom, 0);
        x = zs.right + EdPx(8) + EdPx(46) + EdPx(8);
        const int hw = EdTextWidth(dc, S(Str::EdZoom100), g_edFont) + EdPx(18);
        RECT h1 = EdPill(x, cy, hw, EdPx(26)); EdAdd(h1, EdHit::Zoom100, 0);
        x = h1.right + EdPx(6);
        const int fw = EdTextWidth(dc, S(Str::EdFit), g_edFont) + EdPx(20);
        RECT f = EdPill(x, cy, fw, EdPx(26)); EdAdd(f, EdHit::Fit, 0);

        // CAPS-22: виходи в правому куті, підписані й далеко від системного ✕.
        const int bh = EdPx(32);
        // Клавіша переїхала в підказку, натомість зліва стоїть іконка — як у
        // кнопки «Відкрити», щоб три дії над файлом виглядали однією родиною.
        const int wc = EdTextWidth(dc, S(Str::EdCopy), g_edFont) + EdPx(52);
        const int ws = EdTextWidth(dc, S(Str::EdSaveAs), g_edFont) + EdPx(52);
        int rx = rc.right - EdPx(14) - wc;
        RECT rcCopy = { rx, cy - bh / 2, rx + wc, cy + bh / 2 };
        EdAdd(rcCopy, EdHit::Copy, 0);
        rx -= EdPx(8) + ws;
        RECT rcSave = { rx, cy - bh / 2, rx + ws, cy + bh / 2 };
        EdAdd(rcSave, EdHit::Save, 0);
        // CAPS-36: кнопки немає там, де системне меню поширення недоступне —
        // під адміністратором брокер може не відповісти взагалі.
        if (EdShareAvailable()) {
            const int wsh = EdTextWidth(dc, S(Str::EdShare), g_edFont) + EdPx(26);
            rx -= EdPx(8) + wsh;
            RECT rcShare = { rx, cy - bh / 2, rx + wsh, cy + bh / 2 };
            EdAdd(rcShare, EdHit::Share, 0);
        }
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

    // Розкритий селект додаємо ОСТАННІМ: EdFind іде списком з кінця, тож його
    // ділянки перекривають і полотно, і смугу під ним.
    if (g_edPickOpen >= 0) {
        if (const RECT* btn = EdRegionRect(EdHit::Pick, g_edPickOpen)) {
            const int iw = EdPx(52), ih = EdPx(32);
            const int top = btn->bottom + EdPx(6);
            const int n = EdPickCount(g_edPickOpen);
            for (int i = 0; i < n; ++i) {
                RECT r = { btn->left, top + i * ih, btn->left + iw, top + (i + 1) * ih };
                EdAdd(r, EdHit::PickItem, i);
            }
        }
    }

    // CAPS-28: геометрія знімка і тон. Живуть у правій панелі, бо стосуються
    // САМОГО ЗНІМКА, а не позначки — це і є межа між панеллю і смугою.
    if (g_edPanelOpen) {
        const int px = g_edRcPanel.left + EdPx(14);
        const int pr = g_edRcPanel.right - EdPx(14);
        int y = EdPanelInfoBottom() + EdPx(16);

        const int gb = EdPx(34), gh = EdPx(32);
        const int step = ((pr - px) - gb) / 3;
        const EdHit geo[4] = { EdHit::RotL, EdHit::RotR, EdHit::FlipH, EdHit::FlipV };
        for (int i = 0; i < 4; ++i) {
            RECT r = { px + i * step, y, px + i * step + gb, y + gh };
            EdAdd(r, geo[i], 0);
        }
        // CAPS-44: розмір — над тоном, як просив власник.
        y += gh + EdPx(10);
        const int sbh = EdPx(28);
        RECT rsi = { px, y, pr, y + sbh };
        EdAdd(rsi, EdHit::SizeImg, 0);
        y += sbh + EdPx(6);
        RECT rsc = { px, y, pr, y + sbh };
        EdAdd(rsc, EdHit::SizeCan, 0);
        y += sbh + EdPx(20) + EdPx(18) + EdPx(10);   // + заголовок «ТОН»

        const EdHit sl[3] = { EdHit::Exposure, EdHit::Gamma, EdHit::Contrast };
        for (int i = 0; i < 3; ++i) {
            y += EdPx(18) + EdPx(4);                // підпис і значення над смугою
            RECT r = { px, y, pr, y + EdPx(20) };
            EdAdd(r, sl[i], 0);
            y += EdPx(20) + EdPx(12);
        }

        y += EdPx(6);
        const int bw = (pr - px - EdPx(8)) / 2, bh2 = EdPx(30);
        RECT rt = { px, y, px + bw, y + bh2 };
        EdAdd(rt, EdHit::ToneReset, 0);
        RECT rc2 = { pr - bw, y, pr, y + bh2 };
        EdAdd(rc2, EdHit::Compare, 0);
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

// Повзунок із власним діапазоном. Прозорість і сила живуть у 10…100, тон —
// у трьох різних шкалах, тож діапазон став параметром, а не константою.
void EdPaintSliderRange(Gdiplus::Graphics& g, const RECT& r, const EdTheme& t,
                        int value, int lo, int hi, int zero)
{
    const int cy = (r.top + r.bottom) / 2;
    RECT track = { r.left, cy - EdPx(2), r.right, cy + EdPx(2) };
    Gdiplus::Color bg = EdC(t.border);
    EdFillRound(g, track, (float)EdPx(2), &bg, nullptr);
    const int w = r.right - r.left;
    const int span = (hi > lo) ? (hi - lo) : 1;
    const int fx = r.left + (int)((double)(value - lo) / span * w + 0.5);
    // Заливка йде від ТИПОВОГО значення, а не від лівого краю: у тону нуль
    // посередині, і смужка вліво від нього має означати «темніше», а не «мало».
    const int zx = r.left + (int)((double)(zero - lo) / span * w + 0.5);
    RECT fill = { fx < zx ? fx : zx, track.top, fx < zx ? zx : fx, track.bottom };
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

// ---- CAPS-37: власний заголовок -----------------------------------------
//
// Системний підпис прибрано в WM_NCCALCSIZE, і його місце стало звичайним
// клієнтом. Через це кнопки вікна доводиться малювати самим — зате скасування,
// повторення й довідка живуть там, де в сучасних застосунках, а смуга
// властивостей отримала назад майже сто точок.

void EdPaintCaption(HDC dc, Gdiplus::Graphics& g, const EdTheme& t)
{
    HBRUSH b = CreateSolidBrush(t.chrome);
    FillRect(dc, &g_edRcCaption, b);
    DeleteObject(b);
    RECT line = { g_edRcCaption.left, g_edRcCaption.bottom - 1, g_edRcCaption.right, g_edRcCaption.bottom };
    b = CreateSolidBrush(t.border);
    FillRect(dc, &line, b);
    DeleteObject(b);

    // Значок і назва — те саме, що показував системний підпис.
    const int cy = (g_edRcCaption.top + g_edRcCaption.bottom) / 2;
    int x = EdPx(12);
    if (g_edIcon) {
        const int side = EdPx(16);
        DrawIconEx(dc, x, cy - side / 2, g_edIcon, side, side, 0, nullptr, DI_NORMAL);
        x += side + EdPx(10);
    }
    wchar_t cap[160];
    wsprintfW(cap, L"%s — %s", S(Str::EdTitle), kAppName);
    RECT tr = { x, g_edRcCaption.top, g_edCapLimit > x ? g_edCapLimit : g_edRcCaption.right,
                g_edRcCaption.bottom };
    EdDrawText(dc, tr, cap, g_edFont, g_edActive ? t.text : t.text2,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

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
        EdIcon(g, cmds[i].ico, EdIconBox(*r), EdC(cmds[i].on ? t.text : t.text2, cmds[i].on ? 255 : 130), 1.5f);
    }

    // Кнопки вікна: прямокутні на всю висоту, як у системи. У закривної своя
    // підсвітка — червона; це єдиний колір, який користувач тут очікує.
    const bool zoomed = IsZoomed(g_edWnd) != 0;
    struct { EdHit what; int ico; } wins[3] = {
        { EdHit::Min,   IcoWinMin },
        { EdHit::Max,   zoomed ? IcoWinRestore : IcoWinMax },
        { EdHit::Close, IcoWinClose }
    };
    for (int i = 0; i < 3; ++i) {
        const RECT* r = EdRegionRect(wins[i].what, 0);
        if (!r) continue;
        const bool hot = (g_edHotWhat == wins[i].what);
        COLORREF fg = g_edActive ? t.text : t.text2;
        if (hot) {
            const COLORREF bg = (wins[i].what == EdHit::Close) ? RGB(232, 17, 35) : t.hot;
            HBRUSH hb = CreateSolidBrush(bg);
            FillRect(dc, r, hb);
            DeleteObject(hb);
            if (wins[i].what == EdHit::Close) fg = RGB(255, 255, 255);
        }
        // Піктограма в 20 точок на кнопці в 46: рахуємо квадрат самі, бо
        // EdIconBox бере 58 % меншої сторони, а тут менша — висота.
        const int side = EdPx(20);
        RECT ib = { (r->left + r->right) / 2 - side / 2, (r->top + r->bottom) / 2 - side / 2,
                    (r->left + r->right) / 2 + side / 2, (r->top + r->bottom) / 2 + side / 2 };
        EdIcon(g, wins[i].ico, ib, EdC(fg), 1.2f);
    }
}

// Кадр малюється ПОВЕРХ полотна: затемнення поза ним, лінії третин усередині,
// вісім ручок і підтвердження просто на кадрі — щоб не шукати його в смузі.
void EdPaintCrop(HDC dc, Gdiplus::Graphics& g, const EdTheme& t)
{
    if (!g_edCropping) return;
    RECT r = EdCropScreen(g_edCropEdit);
    const RECT& cv = g_edRcCanvas;

    // Затемнення чотирма смугами, а не одним шляхом із діркою: так само чесно,
    // але без GraphicsPath і його режимів заповнення.
    Gdiplus::SolidBrush dim(Gdiplus::Color(140, 0, 0, 0));
    const RECT parts[4] = {
        { cv.left, cv.top, cv.right, r.top },
        { cv.left, r.bottom, cv.right, cv.bottom },
        { cv.left, r.top, r.left, r.bottom },
        { r.right, r.top, cv.right, r.bottom }
    };
    for (int i = 0; i < 4; ++i) {
        const RECT& p = parts[i];
        if (p.right > p.left && p.bottom > p.top)
            // ⚠ Каст обов'язковий: у GDI+ FillRectangle є і в INT, і в REAL,
            // а LONG однаково добре підходить обом.
            g.FillRectangle(&dim, (INT)p.left, (INT)p.top,
                            (INT)(p.right - p.left), (INT)(p.bottom - p.top));
    }

    Gdiplus::Pen thirds(Gdiplus::Color(120, 255, 255, 255), 1.0f);
    for (int i = 1; i <= 2; ++i) {
        const int x = r.left + (r.right - r.left) * i / 3;
        const int y = r.top + (r.bottom - r.top) * i / 3;
        g.DrawLine(&thirds, (float)x, (float)r.top, (float)x, (float)r.bottom);
        g.DrawLine(&thirds, (float)r.left, (float)y, (float)r.right, (float)y);
    }

    Gdiplus::Pen edge(Gdiplus::Color(230, 255, 255, 255), 1.0f);
    g.DrawRectangle(&edge, (float)r.left, (float)r.top,
                    (float)(r.right - r.left), (float)(r.bottom - r.top));

    RECT hs[8];
    const int hn = EdCropHandles(hs);
    Gdiplus::SolidBrush wb(EdC(RGB(255, 255, 255)));
    Gdiplus::Pen hp(EdC(t.accent), 2.0f);
    for (int i = 0; i < hn; ++i) {
        g.FillRectangle(&wb, (INT)hs[i].left, (INT)hs[i].top,
                        (INT)(hs[i].right - hs[i].left), (INT)(hs[i].bottom - hs[i].top));
        g.DrawRectangle(&hp, (float)hs[i].left, (float)hs[i].top,
                        (float)(hs[i].right - hs[i].left), (float)(hs[i].bottom - hs[i].top));
    }

    // Розмір і дві кнопки — біля кадру, а не в смузі (вимога тікета).
    wchar_t sz[64];
    wsprintfW(sz, L"%d × %d", g_edCropEdit.right - g_edCropEdit.left,
              g_edCropEdit.bottom - g_edCropEdit.top);
    if (const RECT* ok = EdRegionRect(EdHit::CropOk, 0)) {
        const RECT* no = EdRegionRect(EdHit::CropNo, 0);
        RECT bar = *ok;
        if (no) { bar.left = no->left; }
        RECT plate = { bar.left - EdPx(10) - EdTextWidth(dc, sz, g_edFontBold) - EdPx(12),
                       bar.top - EdPx(5), bar.right + EdPx(6), bar.bottom + EdPx(5) };
        Gdiplus::Color pb(220, 24, 24, 28);
        EdFillRound(g, plate, (float)EdPx(7), &pb, nullptr);
        RECT tv = { plate.left + EdPx(10), plate.top, bar.left - EdPx(8), plate.bottom };
        EdDrawText(dc, tv, sz, g_edFontBold, RGB(255, 255, 255),
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (no) {
            EdPaintButton(g, *no, t, false, g_edHotWhat == EdHit::CropNo, false);
            EdDrawText(dc, *no, S(Str::EdCropCancel), g_edFont, t.text,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        Gdiplus::Color fill = EdC(t.accent, g_edHotWhat == EdHit::CropOk ? 225 : 255);
        Gdiplus::Color bd = EdC(t.accent);
        EdFillRound(g, *ok, (float)EdPx(6), &fill, &bd);
        EdDrawText(dc, *ok, S(Str::EdCropApply), g_edFont,
                   g_edDark ? RGB(0, 52, 79) : RGB(255, 255, 255),
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// Наконечник малює той самий код, що й на полотні — інакше зразок у кнопці
// рано чи пізно почав би обіцяти не те, що малюється.
void EdDrawHead(Gdiplus::Graphics& g, const Gdiplus::Color& col, float tipX, float tipY,
                double dirX, double dirY, double len, float pw, int type);

// Поточне значення селекта: у вибраного об'єкта або типове.
// Скільки варіантів у наборі. У наконечників їх чотири — «немає» теж вибір.
int EdPickCount(int group) { return (group == 1 || group == 2) ? 4 : 3; }

int EdPickValue(int group)
{
    const bool sel = (g_edSel >= 0 && g_edSel < (int)g_edObjs.size());
    const EdObj* o = sel ? &g_edObjs[g_edSel] : nullptr;
    switch (group) {
    case 0:  return o ? o->dash     : g_edDash;
    case 1:  return o ? o->headFront : g_edHeadFront;
    case 2:  return o ? o->headBack  : g_edHeadBack;
    default: return o ? o->headSize  : g_edHeadSize;
    }
}

// Зразок у кнопці й у списку: та сама фігура, що буде на полотні, тільки мала.
void EdPickSample(Gdiplus::Graphics& g, const RECT& r, int group, int value,
                  const Gdiplus::Color& c)
{
    const float cy = (r.top + r.bottom) / 2.0f;
    const float x0 = (float)(r.left + EdPx(7)), x1 = (float)(r.right - EdPx(7));
    Gdiplus::Pen pen(c, 2.0f);
    if (group == 0) {
        if (value == 1) pen.SetDashStyle(Gdiplus::DashStyleDash);
        if (value == 2) pen.SetDashStyle(Gdiplus::DashStyleDashDot);
        g.DrawLine(&pen, x0, cy, x1, cy);
        return;
    }
    // ⚠ Наконечники тепер РОЗДІЛЬНІ, тож зразок показує рівно той кінець, про
    // який ця група: передній — праворуч, задній — ліворуч. 0 означає «немає»,
    // і тоді в зразку просто відрізок.
    const int front = (group == 1) ? value : (group == 3) ? 1 : 0;
    const int back  = (group == 2) ? value : 0;
    const double len = (group == 3) ? (5.0 + value * 3.0) : 8.0;
    const float a = back  ? (float)(x0 + len * 0.9) : x0;
    const float b = front ? (float)(x1 - len * 0.9) : x1;
    g.DrawLine(&pen, a, cy, b, cy);
    if (front) EdDrawHead(g, c, x1, cy,  1.0, 0.0, len, 1.6f, front - 1);
    if (back)  EdDrawHead(g, c, x0, cy, -1.0, 0.0, len, 1.6f, back - 1);
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

    // ⚠ Назву чіпа беремо тими самими функціями, що й розкладка. Коли тут стояли
    // прямі Str::EdKindRect і EdTool::Rect (спадок етапу, де фігура була одна),
    // чіп для всіх інструментів писав «прямокутник», та ще й іншої ширини.
    const bool hasSel = (g_edSel >= 0 && g_edSel < (int)g_edObjs.size());
    const wchar_t* chip = EdChipLabel();
    const int cy = (g_edRcStrip.top + g_edRcStrip.bottom) / 2;

    // Усе, що росте зліва, малюємо з клипом по межі правої групи. GDI і GDI+
    // клипають окремо: Graphics узяв клип DC при створенні й сам за ним не
    // стежить, тож ставимо обом.
    const int limit = g_edStripLimit > 0 ? g_edStripLimit : g_edRcStrip.right;
    Gdiplus::GraphicsState clipState = g.Save();
    g.SetClip(Gdiplus::Rect(0, g_edRcStrip.top, limit, g_edRcStrip.bottom - g_edRcStrip.top));
    const int savedDc = SaveDC(dc);
    IntersectClipRect(dc, 0, g_edRcStrip.top, limit, g_edRcStrip.bottom);

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

    const EdKind kkp = hasSel ? g_edObjs[g_edSel].kind : EdToolKind(g_edTool);
    int npalp = 8;
    const COLORREF* palp = EdPaletteFor(kkp, npalp);
    const COLORREF cur = hasSel ? g_edObjs[g_edSel].color
                                : (kkp == EdKind::Mark ? g_edMarkColor : g_edColor);
    for (int i = 0; i < npalp; ++i) {
        const RECT* r = EdRegionRect(EdHit::Swatch, i);
        if (!r) break;
        Gdiplus::Color f = EdC(palp[i]);
        Gdiplus::Color bd = EdC(g_edDark ? RGB(90, 90, 96) : RGB(201, 201, 204));
        EdFillRound(g, *r, (float)EdPx(5), &f, &bd);
        if (palp[i] == cur) {
            RECT ring = { r->left - EdPx(3), r->top - EdPx(3), r->right + EdPx(3), r->bottom + EdPx(3) };
            Gdiplus::Color ac = EdC(t.accent);
            EdFillRound(g, ring, (float)EdPx(7), nullptr, &ac, (float)EdPx(2));
        }
    }

    {
        const int* tset = EdThickSet(kkp);
        const int defThick = (kkp == EdKind::Mark) ? g_edMarkH
                           : EdIsStamped(kkp) ? g_edStampSize : g_edThick;
        const int curThick = hasSel ? EdThickIndex(kkp, g_edObjs[g_edSel].thick)
                                    : EdThickIndex(kkp, defThick);
        for (int i = 0; i < 3; ++i) {
            const RECT* r = EdRegionRect(EdHit::Thick, i);
            if (!r) break;
            const bool on = (i == curThick);
            EdPaintButton(g, *r, t, on, g_edHotWhat == EdHit::Thick && g_edHotIdx == i, false);
            // Розмір штампа показуємо кружечком, а не смужкою: смужка означає
            // товщину лінії, і три різні смисли на одній формі плутають.
            if (EdIsStamped(kkp)) {
                const int side = EdPx(8 + i * 5);
                Gdiplus::SolidBrush sb(EdC(on ? t.accent : t.text));
                g.FillEllipse(&sb, (float)((r->left + r->right) / 2 - side / 2),
                              (float)((r->top + r->bottom) / 2 - side / 2),
                              (float)side, (float)side);
                continue;
            }
            // Смуга маркера вища за лінію — показуємо її в тій самій пропорції,
            // але приборкуємо, щоб найтовща не вилазила за кнопку.
            int bh = EdPx(tset[i]) * 2 / 3;
            if (kkp == EdKind::Mark) bh = EdPx(tset[i]) / 3;
            RECT bar = { r->left + EdPx(8), (r->top + r->bottom) / 2 - bh / 2,
                         r->right - EdPx(8), (r->top + r->bottom) / 2 - bh / 2 + (bh < 2 ? 2 : bh) };
            Gdiplus::Color bc = EdC(on ? t.accent : t.text);
            EdFillRound(g, bar, (float)(bh / 2.0), &bc, nullptr);
            (void)tset;
        }
    }
    {
        const bool fl = hasSel ? g_edObjs[g_edSel].filled : g_edFill;
        const Str lab[2] = { Str::EdOutline, Str::EdFilled };
        for (int i = 0; i < 2; ++i) {
            const RECT* r = EdRegionRect(EdHit::Fill, i);
            if (!r) continue;
            const bool on = (i == (fl ? 1 : 0));
            EdPaintButton(g, *r, t, on, g_edHotWhat == EdHit::Fill && g_edHotIdx == i, false);
            EdDrawText(dc, *r, S(lab[i]), on ? g_edFontBold : g_edFont, on ? t.accent : t.text,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // Властивості напису. Значення беремо з вибраного об'єкта, а коли нічого не
    // вибрано — з типових: так кнопки показують те, що дістанеться наступному.
    {
        const bool selText = hasSel && g_edObjs[g_edSel].kind == EdKind::Text;
        const int  vSize    = selText ? g_edObjs[g_edSel].size    : g_edSize;
        const bool vBold    = selText ? g_edObjs[g_edSel].bold    : g_edBold;
        const bool vItalic  = selText ? g_edObjs[g_edSel].italic  : g_edItalic;
        const int  vAlign   = selText ? g_edObjs[g_edSel].align   : g_edAlign;
        const int  vOutline = selText ? g_edObjs[g_edSel].outline : g_edOutline;

        for (int i = 0; i < 2; ++i) {
            const RECT* r = EdRegionRect(EdHit::Size, i);
            if (!r) break;
            EdPaintButton(g, *r, t, false, g_edHotWhat == EdHit::Size && g_edHotIdx == i, false);
            EdIcon(g, i ? IcoPlus : IcoMinus, EdIconBox(*r), EdC(t.text), 1.6f);
            if (i == 0) {
                const RECT* r2 = EdRegionRect(EdHit::Size, 1);
                if (r2) {
                    wchar_t buf[16];
                    wsprintfW(buf, L"%d", vSize);
                    RECT tv = { r->right, r->top, r2->left, r->bottom };
                    EdDrawText(dc, tv, buf, g_edFontBold, t.text,
                               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
            }
        }

        struct { EdHit what; int ico; bool on; } tog[2] = {
            { EdHit::Bold, IcoBold, vBold }, { EdHit::Italic, IcoItalic, vItalic }
        };
        for (int i = 0; i < 2; ++i) {
            const RECT* r = EdRegionRect(tog[i].what, 0);
            if (!r) continue;
            EdPaintButton(g, *r, t, tog[i].on, g_edHotWhat == tog[i].what, false);
            EdIcon(g, tog[i].ico, EdIconBox(*r), EdC(tog[i].on ? t.accent : t.text), 1.5f);
        }

        for (int i = 0; i < 3; ++i) {
            const RECT* r = EdRegionRect(EdHit::Align, i);
            if (!r) break;
            const bool on = (i == vAlign);
            EdPaintButton(g, *r, t, on, g_edHotWhat == EdHit::Align && g_edHotIdx == i, false);
            EdIcon(g, IcoAlignL + i, EdIconBox(*r), EdC(on ? t.accent : t.text), 1.5f);
        }

        for (int i = 0; i < 3; ++i) {
            const RECT* r = EdRegionRect(EdHit::Stroke, i);
            if (!r) break;
            const bool on = (i == vOutline);
            EdPaintButton(g, *r, t, on, g_edHotWhat == EdHit::Stroke && g_edHotIdx == i, false);
            EdIcon(g, IcoStroke0 + i, EdIconBox(*r), EdC(on ? t.accent : t.text), 1.5f);
        }
    }
    // CAPS-35: смуга кількох вибраних.
    {
        static const int alIco[8] = { IcoAlL, IcoAlCx, IcoAlR, IcoAlT, IcoAlCy, IcoAlB,
                                      IcoDistX, IcoDistY };
        for (int i = 0; i < 8; ++i) {
            const RECT* r = EdRegionRect(EdHit::SelAlign, i);
            if (!r) break;
            EdPaintButton(g, *r, t, false, g_edHotWhat == EdHit::SelAlign && g_edHotIdx == i, false);
            EdIcon(g, alIco[i], EdIconBox(*r), EdC(t.text), 1.5f);
        }
        for (int gi = 0; gi < 2; ++gi) {
            const RECT* r = EdRegionRect(EdHit::SelGroup, gi);
            if (!r) continue;
            EdPaintButton(g, *r, t, gi == 1, g_edHotWhat == EdHit::SelGroup, false);
            EdIcon(g, gi == 1 ? IcoUngroup : IcoGroup, EdIconBox(*r),
                   EdC(gi == 1 ? t.accent : t.text), 1.5f);
        }
    }

    // CAPS-34: кнопки випадних селектів. Кожна показує поточний вибір.
    for (int gi = 0; gi < 4; ++gi) {
        const RECT* r = EdRegionRect(EdHit::Pick, gi);
        if (!r) break;
        const bool open = (g_edPickOpen == gi);
        EdPaintButton(g, *r, t, open, g_edHotWhat == EdHit::Pick && g_edHotIdx == gi, false);
        RECT inner = { r->left, r->top, r->right - EdPx(8), r->bottom };
        EdPickSample(g, inner, gi, EdPickValue(gi), EdC(t.text));
        // Маленька стрілка вниз: кнопка з нею читається як список, а не як тогл.
        Gdiplus::Pen chev(EdC(t.text2), 1.4f);
        const float cxx = (float)(r->right - EdPx(7)), cyy = (float)((r->top + r->bottom) / 2);
        g.DrawLine(&chev, cxx - EdPx(3), cyy - EdPx(1), cxx, cyy + EdPx(2));
        g.DrawLine(&chev, cxx, cyy + EdPx(2), cxx + EdPx(3), cyy - EdPx(1));
    }

    // Кадр: пропорції й скидання.
    for (int i = 0; i < 4; ++i) {
        const RECT* r = EdRegionRect(EdHit::Aspect, i);
        if (!r) break;
        const bool on = (i == g_edCropAspect);
        const wchar_t* lab = i == 0 ? S(Str::EdAspectFree)
                           : i == 1 ? L"16:9" : i == 2 ? L"4:3" : L"1:1";
        EdPaintButton(g, *r, t, on, g_edHotWhat == EdHit::Aspect && g_edHotIdx == i, false);
        EdDrawText(dc, *r, lab, on ? g_edFontBold : g_edFont, on ? t.accent : t.text,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (const RECT* rr = EdRegionRect(EdHit::CropReset, 0)) {
        EdPaintButton(g, *rr, t, false, g_edHotWhat == EdHit::CropReset, false);
        EdDrawText(dc, *rr, S(Str::EdCropReset), g_edFont,
                   EdHasCrop() ? t.text : t.text2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // Штампи: кнопка показує сам штамп. Емодзі малюємо тією самою плиткою, що й
    // на полотні, — інакше в смузі вони були б чорно-білими.
    {
        const int curStamp = (hasSel && g_edObjs[g_edSel].kind == EdKind::Stamp)
                                 ? g_edObjs[g_edSel].stamp : g_edStamp;
        for (int i = 0; i < kEdVectorStamps + kEdEmojiQuick; ++i) {
            const RECT* r = EdRegionRect(EdHit::StampPick, i);
            if (!r) break;
            const bool on = (i < kEdVectorStamps) ? (curStamp == i)
                                                  : (curStamp == kEdEmojiBase + (i - kEdVectorStamps));
            EdPaintButton(g, *r, t, on, g_edHotWhat == EdHit::StampPick && g_edHotIdx == i, false);
            const RECT ib = EdIconBox(*r);
            if (i < kEdVectorStamps) {
                EdStampShape(g, i, (float)ib.left, (float)ib.top,
                             (float)(ib.right - ib.left), EdC(on ? t.accent : t.text));
            } else {
                EdObj probe = EdObj{};
                probe.kind = EdKind::Text;
                probe.text = kEdEmoji[i - kEdVectorStamps];
                probe.size = (ib.right - ib.left) * 96 / (g_edDpi ? g_edDpi : 96);
                probe.alpha = 100;
                probe.color = RGB(255, 255, 255);
                const EdTile* tile = EdTextTile(probe, (double)g_edDpi / 96.0);
                if (tile && tile->bmp) {
                    const int tw = (int)tile->bmp->GetWidth(), th = (int)tile->bmp->GetHeight();
                    g.DrawImage(tile->bmp,
                                Gdiplus::Rect((r->left + r->right) / 2 - tw / 2,
                                              (r->top + r->bottom) / 2 - th / 2, tw, th),
                                0, 0, tw, th, Gdiplus::UnitPixel);
                }
            }
        }
        if (const RECT* rm = EdRegionRect(EdHit::StampMore, 0)) {
            EdPaintButton(g, *rm, t, false, g_edHotWhat == EdHit::StampMore, false);
            EdIcon(g, IcoMore, EdIconBox(*rm), EdC(t.text), 1.5f);
        }
    }

    // Лічильник: група, початковий номер, наступний номер, нова група.
    {
        wchar_t nb[64];
        const int grp = EdCurGroup();
        if (const RECT* gt = EdRegionRect(EdHit::NumGroup, 0)) {
            wsprintfW(nb, S(Str::EdFmtGroupOnly), grp + 1);
            EdDrawText(dc, *gt, nb, g_edFontBold, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        if (const RECT* lb = EdRegionRect(EdHit::NumStart, 2))
            EdDrawText(dc, *lb, S(Str::EdNumStartLabel), g_edFont, t.text2,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        for (int i = 0; i < 2; ++i) {
            const RECT* r = EdRegionRect(EdHit::NumStart, i);
            if (!r) break;
            EdPaintButton(g, *r, t, false, g_edHotWhat == EdHit::NumStart && g_edHotIdx == i, false);
            EdIcon(g, i ? IcoPlus : IcoMinus, EdIconBox(*r), EdC(t.text), 1.6f);
            if (i == 0) {
                const RECT* r2 = EdRegionRect(EdHit::NumStart, 1);
                if (r2) {
                    wsprintfW(nb, L"%d", EdGroupStart(grp));
                    RECT tv = { r->right, r->top, r2->left, r->bottom };
                    EdDrawText(dc, tv, nb, g_edFontBold, t.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
            }
        }
        if (const RECT* nt = EdRegionRect(EdHit::NumNext, 0)) {
            wsprintfW(nb, S(Str::EdFmtNext), EdGroupStart(grp) + EdGroupCount(grp));
            EdDrawText(dc, *nt, nb, g_edFont, t.text2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        if (const RECT* rb = EdRegionRect(EdHit::NumReset, 0)) {
            EdPaintButton(g, *rb, t, false, g_edHotWhat == EdHit::NumReset, false);
            EdIcon(g, IcoNewGroup, EdIconBox(*rb), EdC(t.text), 1.5f);
        }
        if (const RECT* rg = EdRegionRect(EdHit::GroupEdit, 0)) {
            EdPaintButton(g, *rg, t, g_edGroupEdit, g_edHotWhat == EdHit::GroupEdit, false);
            EdIcon(g, IcoGroupEdit, EdIconBox(*rg),
                   EdC(g_edGroupEdit ? t.accent : t.text), 1.6f);
        }
        if (const RECT* rd = EdRegionRect(EdHit::GroupDel, 0)) {
            EdPaintButton(g, *rd, t, false, g_edHotWhat == EdHit::GroupDel, false, true);
            EdIcon(g, IcoGroupDel, EdIconBox(*rd), EdC(t.dangerFg), 1.6f);
        }
    }

    // Приховування: режим і сила.
    {
        const bool selHide = hasSel && g_edObjs[g_edSel].kind == EdKind::Hide;
        const int vMode = selHide ? g_edObjs[g_edSel].mode : g_edHideMode;
        const int vStr  = selHide ? g_edObjs[g_edSel].strength : g_edStrength;
        const int icons[3] = { IcoBlur, IcoPixels, IcoPlate };
        for (int i = 0; i < 3; ++i) {
            const RECT* r = EdRegionRect(EdHit::HideMode, i);
            if (!r) break;
            const bool on = (i == vMode);
            EdPaintButton(g, *r, t, on, g_edHotWhat == EdHit::HideMode && g_edHotIdx == i, false);
            EdIcon(g, icons[i], EdIconBox(*r), EdC(on ? t.accent : t.text), 1.5f);
        }
        if (const RECT* sl = EdRegionRect(EdHit::Strength, 0)) {
            RECT ic = { sl->left - EdPx(8) - EdPx(18), (sl->top + sl->bottom) / 2 - EdPx(9),
                        sl->left - EdPx(8), (sl->top + sl->bottom) / 2 + EdPx(9) };
            EdIcon(g, IcoStrength, ic, EdC(t.text2), 1.6f);
            EdPaintSlider(g, *sl, t, vStr);
            wchar_t sb[32];
            wsprintfW(sb, L"%d %%", vStr);
            RECT tv = { sl->right + EdPx(8), g_edRcStrip.top, sl->right + EdPx(8) + EdPx(44),
                        g_edRcStrip.bottom };
            EdDrawText(dc, tv, sb, g_edFont, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
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

    RestoreDC(dc, savedDc);
    g.Restore(clipState);

    struct { EdHit what; int ico; bool danger; } acts[4] = {
        { EdHit::Front, IcoFront, false }, { EdHit::Back, IcoBack, false },
        { EdHit::Dup, IcoDup, false },     { EdHit::Del, IcoDel, true }
    };
    for (int i = 0; i < 4; ++i) {
        const RECT* r = EdRegionRect(acts[i].what, 0);
        if (!r) continue;
        const bool hot = (g_edHotWhat == acts[i].what);
        EdPaintButton(g, *r, t, false, hot, false, acts[i].danger);
        EdIcon(g, acts[i].ico, EdIconBox(*r), EdC(acts[i].danger ? t.dangerFg : t.text), 1.5f);
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

    const int icos[11] = { IcoSelect, IcoRect, IcoEllipse, IcoLine, IcoPen, IcoText,
                           IcoHide, IcoMark, IcoCounter, IcoStamp, IcoCrop };
    for (int i = 0; i < 11; ++i) {
        const RECT* r = EdRegionRect(EdHit::Tool, i);
        if (!r) break;
        if (i == 1) {                       // лінія-розділювач над фігурами
            HBRUSH sb = CreateSolidBrush(t.border);
            RECT sep = { g_edRcRail.left + EdPx(12), r->top - EdPx(6),
                         g_edRcRail.right - EdPx(12), r->top - EdPx(6) + 1 };
            FillRect(dc, &sep, sb);
            DeleteObject(sb);
        }
        const bool active = ((int)g_edTool == i);
        const bool hot = (g_edHotWhat == EdHit::Tool && g_edHotIdx == i);
        EdPaintButton(g, *r, t, active, hot, !active);
        EdIcon(g, icos[i], EdIconBox(*r), EdC(active ? t.accent : t.text), 1.5f);
    }
}

// ---- CAPS-24: текст через DirectWrite -----------------------------------
//
// Напис малюється НЕ прямо в цільову поверхню, а в окрему плитку з альфою, яку
// потім кладе на місце звичайний GDI+. Так текст лишається просто ще одним
// об'єктом у списку: порядок, прозорість, експорт і скасування працюють без
// жодного винятку для нього, і той самий код обслуговує екран і файл.

const GUID kIID_IDWriteFactory = { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };
const wchar_t* const kEdFontFamily = L"Segoe UI";

IDWriteFactory* g_dw = nullptr;

bool EdEnsureDWrite()
{
    if (!g_dw)
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, kIID_IDWriteFactory, (IUnknown**)&g_dw);
    return g_dw != nullptr;
}

// Ширину спершу міряємо без обмежень, а потім ставимо виміряне як максимальну:
// інакше вирівнюванню по центру й праворуч не було б відносно чого рівнятися —
// воно рівнялося б по нескінченній смузі й нікуди не зсувало рядки.
IDWriteTextLayout* EdTextLayout(const EdObj& o, double s)
{
    if (!EdEnsureDWrite() || o.text.empty()) return nullptr;
    float em = (float)(o.size * s);
    if (em < 1.0f)    em = 1.0f;
    if (em > 1600.0f) em = 1600.0f;

    IDWriteTextFormat* fmt = nullptr;
    if (FAILED(g_dw->CreateTextFormat(kEdFontFamily, nullptr,
                                      o.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                      o.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, em, L"", &fmt)) || !fmt)
        return nullptr;

    IDWriteTextLayout* lay = nullptr;
    const HRESULT hr = g_dw->CreateTextLayout(o.text.c_str(), (UINT32)o.text.size(), fmt,
                                              100000.0f, 100000.0f, &lay);
    fmt->Release();
    if (FAILED(hr) || !lay) return nullptr;

    if (o.boxw > 0) {
        // Ширину задав користувач — переноси вмикаємо, і рядки лягають у блок.
        lay->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        float bw = (float)(o.boxw * s);
        if (bw < 1.0f) bw = 1.0f;
        lay->SetMaxWidth(bw);
        lay->SetMaxHeight(100000.0f);
    } else {
        // Переноси вимикаємо ДО того, як обмежити ширину, і саме тому обмежуємо
        // з запасом у дві точки: максимальна ширина рівно по вимірянiй — це
        // пастка, бо рядок від округлення в неї може вже не влізти й тихо
        // перенестись, а тоді габарити стануть вужчими й вищими за сам напис.
        lay->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TEXT_METRICS m = {};
        if (SUCCEEDED(lay->GetMetrics(&m))) {
            lay->SetMaxWidth(m.widthIncludingTrailingWhitespace + 2.0f);
            lay->SetMaxHeight(m.height + 2.0f);
        }
    }
    lay->SetTextAlignment(o.align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER
                        : o.align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                       : DWRITE_TEXT_ALIGNMENT_LEADING);
    return lay;
}

// Габарити напису в пікселях знімка. Ними живуть рамка виділення й перевірка
// попадання, тож міряємо раз — на фіксації введення, а не щомалювання.
void EdTextMeasure(EdObj& o)
{
    o.w = 0;
    o.h = 0;
    IDWriteTextLayout* lay = EdTextLayout(o, 1.0);
    if (lay) {
        DWRITE_TEXT_METRICS m = {};
        if (SUCCEEDED(lay->GetMetrics(&m))) {
            // При заданій ширині габарит — це БЛОК, а не найдовший рядок:
            // інакше рамка виділення стрибала б за текстом, і вирівняний
            // праворуч напис опинявся б поза власною рамкою.
            o.w = (o.boxw > 0) ? o.boxw : (int)(m.widthIncludingTrailingWhitespace + 0.999f);
            o.h = (int)(m.height + 0.999f);
        }
        lay->Release();
    }
    if (o.w < 1) o.w = 1;
    if (o.h < 1) o.h = 1;
}

// Обводка — не справжній контур гліфа, а дванадцять відбитків по колу під ним.
// Справжній контур вимагав би власного IDWriteTextRenderer із GetGlyphRunOutline;
// на радіусі в два-чотири пікселі різниці не видно, а коду втричі менше. Якщо
// колись знадобиться товста обводка — міняти саме тут.
float EdStrokeRadius(const EdObj& o, double s)
{
    if (o.outline == 0) return 0.0f;
    float r = (float)(o.size * s / 11.0);
    if (r < 1.0f) r = 1.0f;
    if (r > 8.0f) r = 8.0f;
    return r;
}

std::vector<EdTile> g_edTiles;
const size_t kEdTileMax = 24;

void EdBelowClear();                  // композити «що нижче» живуть тим самим життям

void EdTilesClear()
{
    for (size_t i = 0; i < g_edTiles.size(); ++i) delete g_edTiles[i].bmp;
    g_edTiles.clear();
    // Композити зроблено з тих самих пікселів: лишити їх означало б віддати
    // ефектам стару картинку під новим знімком.
    EdBelowClear();
}

// ---- CAPS-28: тон і геометрія -------------------------------------------
//
// Ручна правка тону — паліатив там, де автоматичний тон-мапінг не допоміг або
// оригіналу вже нема: чужа програма поклала в буфер биту 8-бітну картинку, і
// повернути з неї втрачене нічим, крім повзунків.

bool EdToneDefault()
{
    return g_edExposure == 0 && g_edGamma == 100 && g_edContrast == 0;
}

bool EdGeomDefault()
{
    return g_edRot == 0 && !g_edMirror;
}

// Уся арифметика тону вміщується в таблицю на 256 значень: канал 8-бітний, тож
// більше варіантів просто не буває. Завдяки цьому повзунок рухається по 4K так
// само легко, як по мініатюрі.
void EdBuildLut(BYTE lut[256])
{
    const double mul = pow(2.0, g_edExposure / 10.0);
    const double gam = g_edGamma / 100.0;
    // Контраст як нахил навколо середини сірого. Коефіцієнт — класична формула
    // (259*(C+255)) / (255*(259-C)): на C = 0 дає рівно одиницю.
    const double cc = g_edContrast * 255.0 / 100.0;
    const double k  = (259.0 * (cc + 255.0)) / (255.0 * (259.0 - cc));
    for (int i = 0; i < 256; ++i) {
        double v = i / 255.0;
        v *= mul;
        if (v > 1.0) v = 1.0;
        if (gam != 1.0) v = pow(v, 1.0 / gam);
        v = k * (v - 0.5) + 0.5;
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        lut[i] = (BYTE)(v * 255.0 + 0.5);
    }
}

// Поворот і дзеркало віддаємо GDI+: RotateFlip переставляє пікселі без жодної
// інтерполяції, піксель у піксель. Наші «поворот + дзеркало» складаються рівно
// в одне з восьми його значень, бо M(R(a)) = R(-a)(M).
Gdiplus::Bitmap* EdBuildWorking(bool withTone)
{
    if (!g_edSrc) return nullptr;
    const int sw = (int)g_edSrc->GetWidth(), sh = (int)g_edSrc->GetHeight();
    Gdiplus::Bitmap* w = g_edSrc->Clone(0, 0, sw, sh, PixelFormat32bppPARGB);
    if (!w || w->GetLastStatus() != Gdiplus::Ok) { delete w; return nullptr; }

    const int idx = g_edMirror ? 4 + (4 - g_edRot) % 4 : g_edRot;
    if (idx != 0) w->RotateFlip((Gdiplus::RotateFlipType)idx);

    if (withTone && !EdToneDefault()) {
        BYTE lut[256];
        EdBuildLut(lut);
        const int ww = (int)w->GetWidth(), hh = (int)w->GetHeight();
        Gdiplus::BitmapData bd = {};
        Gdiplus::Rect all(0, 0, ww, hh);
        if (w->LockBits(&all, Gdiplus::ImageLockModeRead | Gdiplus::ImageLockModeWrite,
                        PixelFormat32bppPARGB, &bd) == Gdiplus::Ok) {
            BYTE* px = (BYTE*)bd.Scan0;
            if (bd.Stride > 0) {
                for (int y = 0; y < hh; ++y) {
                    BYTE* row = px + (size_t)y * bd.Stride;
                    for (int x = 0; x < ww; ++x) {
                        BYTE* p = row + x * 4;
                        const BYTE a = p[3];
                        for (int c = 0; c < 3; ++c) {
                            // Канали ПРЕМНОЖЕНІ на альфу: піднятись вище за неї
                            // їм не можна, інакше піксель перестає бути дійсним.
                            const BYTE v = lut[p[c]];
                            p[c] = v > a ? a : v;
                        }
                    }
                }
            }
            w->UnlockBits(&bd);
        }
    }
    return w;
}

void EdRebuildImage()
{
    Gdiplus::Bitmap* w = EdBuildWorking(true);
    if (!w) return;
    delete g_edImg;
    g_edImg  = w;
    g_edImgW = (int)w->GetWidth();
    g_edImgH = (int)w->GetHeight();
    // Плитки розмиття й маркера зроблені з ПІКСЕЛІВ знімка. Щойно пікселі інші —
    // плитки брешуть, і жоден ключ кешу цього не помітить.
    EdTilesClear();
}

// Поворот і дзеркало переставляють позначки разом зі знімком: стрілка має
// лишитись на тому самому місці картинки, а не поїхати за край. Альтернатива —
// тримати матрицю і мапити на льоту — протягла б другу систему координат крізь
// влучання, кадр, плитки й експорт.
POINT EdMapPt(int step, int W, int H, POINT p)
{
    POINT q;
    switch (step) {
    case 0: q.x = H - p.y; q.y = p.x;     break;   // за годинниковою
    case 1: q.x = p.y;     q.y = W - p.x; break;   // проти годинникової
    case 2: q.x = W - p.x; q.y = p.y;     break;   // дзеркало по горизонталі
    default: q.x = p.x;    q.y = H - p.y; break;   // дзеркало по вертикалі
    }
    return q;
}

void EdTransformAll(int step, int W, int H)
{
    for (size_t i = 0; i < g_edObjs.size(); ++i) {
        EdObj& o = g_edObjs[i];
        if (!o.pts.empty()) {
            for (size_t k = 0; k < o.pts.size(); ++k) o.pts[k] = EdMapPt(step, W, H, o.pts[k]);
            EdPenBounds(o);
            continue;
        }
        if (EdIsSegment(o.kind)) {
            POINT a = { o.x, o.y }, b = { o.x + o.w, o.y + o.h };
            a = EdMapPt(step, W, H, a);
            b = EdMapPt(step, W, H, b);
            o.x = a.x; o.y = a.y; o.w = b.x - a.x; o.h = b.y - a.y;
            continue;
        }
        // Напис, кружечок і штамп на бік не лягають: у них переїздить місце, а
        // сам блок лишається того самого розміру й тієї самої орієнтації.
        const bool upright = (o.kind == EdKind::Text || o.kind == EdKind::Counter ||
                              o.kind == EdKind::Stamp);
        // ⚠ Позначку, яка вміє власний кут, повертаємо КУТОМ, а не перестановкою
        // сторін. Зробити і те, і те означає повернути її ДВІЧІ — рівно на цьому
        // впав tone_test, коли з'явився CAPS-43. У відрізка й олівця точки вже
        // мапнуті вище, тож їхній власний кут лишається як був.
        const bool byAngle = EdCanRotate(o.kind) && !upright;
        if (byAngle) {
            if (step == 0)      o.rot = (o.rot + 90) % 360;
            else if (step == 1) o.rot = (o.rot + 270) % 360;
            else if (step == 2) o.rot = (360 - o.rot) % 360;
            else                o.rot = (540 - o.rot) % 360;
        }
        POINT c = { o.x + o.w / 2, o.y + o.h / 2 };
        c = EdMapPt(step, W, H, c);
        const bool swap = (step <= 1) && !upright && !byAngle;
        const int nw = swap ? o.h : o.w, nh = swap ? o.w : o.h;
        o.x = c.x - nw / 2; o.y = c.y - nh / 2; o.w = nw; o.h = nh;
    }
    if (EdHasCrop()) {
        POINT a = { g_edCrop.left, g_edCrop.top }, b = { g_edCrop.right, g_edCrop.bottom };
        a = EdMapPt(step, W, H, a);
        b = EdMapPt(step, W, H, b);
        g_edCrop.left   = a.x < b.x ? a.x : b.x;
        g_edCrop.right  = a.x < b.x ? b.x : a.x;
        g_edCrop.top    = a.y < b.y ? a.y : b.y;
        g_edCrop.bottom = a.y < b.y ? b.y : a.y;
    }
}

void EdGeomDone()
{
    EdRebuildImage();
    if (g_edWnd) {
        EdFitView();
        EdLayout(g_edWnd);
        InvalidateRect(g_edWnd, nullptr, TRUE);
    }
}

void EdRotateBy(bool cw)
{
    if (!g_edSrc) return;
    EdPushUndo();
    EdTransformAll(cw ? 0 : 1, g_edImgW, g_edImgH);
    g_edRot = (g_edRot + (cw ? 1 : 3)) % 4;
    EdGeomDone();
}

void EdMirrorBy(bool horizontal)
{
    if (!g_edSrc) return;
    EdPushUndo();
    EdTransformAll(horizontal ? 2 : 3, g_edImgW, g_edImgH);
    // Дзеркало не додається до повороту, а «перевертає» його: M(R(a)) = R(-a)(M).
    g_edRot = horizontal ? (4 - g_edRot) % 4 : (6 - g_edRot) % 4;
    g_edMirror = !g_edMirror;
    EdGeomDone();
}

void EdToneReset()
{
    if (EdToneDefault()) return;
    EdPushUndo();
    g_edExposure = 0; g_edGamma = 100; g_edContrast = 0;
    EdRebuildImage();
    if (g_edWnd) InvalidateRect(g_edWnd, nullptr, FALSE);
}

std::wstring EdTileKey(const EdObj& o, double s)
{
    wchar_t head[96];
    wsprintfW(head, L"%d|%d|%d%d|%d|%d|%d|%d|%08X|", (int)(s * 1000 + 0.5), o.size,
              o.bold ? 1 : 0, o.italic ? 1 : 0, o.align, o.outline, o.alpha, o.boxw,
              (unsigned)o.color);
    return std::wstring(head) + o.text;
}

const EdTile* EdTextTile(const EdObj& o, double s)
{
    const std::wstring key = EdTileKey(o, s);
    for (size_t i = 0; i < g_edTiles.size(); ++i)
        if (g_edTiles[i].key == key) return &g_edTiles[i];
    if (!SvgEnsureFactories()) return nullptr;

    IDWriteTextLayout* lay = EdTextLayout(o, s);
    if (!lay) return nullptr;
    DWRITE_TEXT_METRICS m = {};
    if (FAILED(lay->GetMetrics(&m))) { lay->Release(); return nullptr; }

    const float rad = EdStrokeRadius(o, s);
    const int pad = (int)rad + 2;
    // Плитка завширшки з БЛОК, а не з найдовший рядок: інакше вирівнювання по
    // центру чи праворуч не було б видно — рядок просто лежав би на своєму краю.
    const float tileW = (o.boxw > 0) ? (float)(o.boxw * s)
                                     : m.widthIncludingTrailingWhitespace;
    const int w = (int)(tileW + 0.999f) + pad * 2;
    const int h = (int)(m.height + 0.999f) + pad * 2;
    if (w < 1 || h < 1 || w > 12000 || h > 12000) { lay->Release(); return nullptr; }

    IWICBitmap*            wicBmp = nullptr;
    ID2D1RenderTarget*     rt     = nullptr;
    ID2D1SolidColorBrush*  fill   = nullptr;
    ID2D1SolidColorBrush*  halo   = nullptr;
    Gdiplus::Bitmap*       bmp    = nullptr;
    bool ok = false;

    if (SUCCEEDED(g_wic->CreateBitmap((UINT)w, (UINT)h, kWICPixelFormat32bppPBGRA,
                                      WICBitmapCacheOnLoad, &wicBmp)) && wicBmp) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(g_d2d->CreateWicBitmapRenderTarget(wicBmp, props, &rt)) && rt) {
            // ClearType на прозорому тлі дає кольорову бахрому по краях літер:
            // субпіксельний згладжувач передбачає відоме тло, а його тут немає.
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            const D2D1_COLOR_F fc = D2D1::ColorF(GetRValue(o.color) / 255.0f,
                                                 GetGValue(o.color) / 255.0f,
                                                 GetBValue(o.color) / 255.0f, 1.0f);
            const float hv = (o.outline == 1) ? 1.0f : 0.0f;
            const D2D1_COLOR_F hc = D2D1::ColorF(hv, hv, hv, 1.0f);
            rt->CreateSolidColorBrush(fc, &fill);
            rt->CreateSolidColorBrush(hc, &halo);
        }
        if (fill && halo) {
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0.0f));
            if (rad > 0.0f) {
                for (int i = 0; i < 12; ++i) {
                    const double a = i * 3.14159265358979 / 6.0;
                    // Обводка завжди одноколірна: кольорові емодзі в ній
                    // перетворили б ореол на другий, зсунутий напис.
                    rt->DrawTextLayout(D2D1::Point2F((float)(pad + cos(a) * rad),
                                                     (float)(pad + sin(a) * rad)),
                                       lay, halo, D2D1_DRAW_TEXT_OPTIONS_NONE);
                }
            }
            rt->DrawTextLayout(D2D1::Point2F((float)pad, (float)pad), lay, fill,
                               D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            if (SUCCEEDED(rt->EndDraw())) {
                bmp = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
                Gdiplus::BitmapData bd = {};
                Gdiplus::Rect lock(0, 0, w, h);
                if (bmp->GetLastStatus() == Gdiplus::Ok &&
                    bmp->LockBits(&lock, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &bd) == Gdiplus::Ok) {
                    if (bd.Stride > 0 &&
                        SUCCEEDED(wicBmp->CopyPixels(nullptr, (UINT)bd.Stride, (UINT)bd.Stride * h, (BYTE*)bd.Scan0))) {
                        // Прозорість множимо вже ТУТ, по готовій плитці. Якби
                        // напівпрозорими були самі мазки, обводка просвічувала б
                        // крізь літери, а її дванадцять відбитків темнішали б
                        // один на одному. Множення попередньо помножених каналів
                        // на сталу — точна операція, тому робимо саме так.
                        if (o.alpha < 100) {
                            const unsigned k = (unsigned)(o.alpha * 255 / 100);
                            for (int yy = 0; yy < h; ++yy) {
                                BYTE* row = (BYTE*)bd.Scan0 + (size_t)yy * bd.Stride;
                                for (int xx = 0; xx < w * 4; ++xx)
                                    row[xx] = (BYTE)(row[xx] * k / 255);
                            }
                        }
                        ok = true;
                    }
                    bmp->UnlockBits(&bd);
                }
                if (!ok) { delete bmp; bmp = nullptr; }
            }
        }
    }
    if (fill)   fill->Release();
    if (halo)   halo->Release();
    if (rt)     rt->Release();
    if (wicBmp) wicBmp->Release();
    lay->Release();
    if (!ok) return nullptr;

    if (g_edTiles.size() >= kEdTileMax) {
        delete g_edTiles.front().bmp;
        g_edTiles.erase(g_edTiles.begin());
    }
    EdTile t;
    t.key = key;
    t.bmp = bmp;
    t.pad = pad;
    g_edTiles.push_back(t);
    return &g_edTiles.back();
}

// ---- CAPS-25: приховування й маркер --------------------------------------
//
// Обидва інструменти — дії над ПІКСЕЛЯМИ знімка, а не мазки пером: вони беруть
// ділянку базового бітмапа й повертають її зміненою. Результат лягає в таку
// саму плитку, як текст у CAPS-24, і далі його кладе звичайний GDI+. Тому
// приховане місце лишається звичайним об'єктом списку — його можна посунути
// хоч через десять інших дій, — а незворотним стає лише у вихідному файлі.
//
// ⚠ Наслідок, про який варто пам'ятати: джерело — САМЕ БАЗОВИЙ бітмап. Позначки,
// намальовані раніше в тій самій ділянці, плитка не бачить і накриває собою.

int EdHideRadius(const EdObj& o)
{
    const int side = o.w < o.h ? o.w : o.h;
    int r = side * o.strength / 600;
    if (r < 3)  r = 3;
    if (r > 60) r = 60;
    return r;
}

int EdHideBlock(const EdObj& o)
{
    const int side = o.w < o.h ? o.w : o.h;
    int b = side * o.strength / 500;
    if (b < 4)  b = 4;
    if (b > 64) b = 64;
    return b;
}

// Коробкове розмиття в три проходи: на око це вже гаусове, а рахується лінійно
// від кількості пікселів, а не від радіуса. Для ділянки в пів-екрана різниця
// між «миттєво» і «помітно» саме тут.
void EdBoxBlur(BYTE* px, int w, int h, int stride, int radius)
{
    if (radius < 1 || w < 2 || h < 2) return;
    std::vector<BYTE> tmp((size_t)stride * h);
    for (int pass = 0; pass < 3; ++pass) {
        // горизонтально
        for (int y = 0; y < h; ++y) {
            BYTE* src = px + (size_t)y * stride;
            BYTE* dst = tmp.data() + (size_t)y * stride;
            int sum[4] = { 0, 0, 0, 0 };
            int cnt = 0;
            for (int x = 0; x <= radius && x < w; ++x, ++cnt)
                for (int c = 0; c < 4; ++c) sum[c] += src[x * 4 + c];
            for (int x = 0; x < w; ++x) {
                for (int c = 0; c < 4; ++c) dst[x * 4 + c] = (BYTE)(sum[c] / cnt);
                const int add = x + radius + 1, sub = x - radius;
                if (add < w) { for (int c = 0; c < 4; ++c) sum[c] += src[add * 4 + c]; ++cnt; }
                if (sub >= 0) { for (int c = 0; c < 4; ++c) sum[c] -= src[sub * 4 + c]; --cnt; }
            }
        }
        // вертикально
        for (int x = 0; x < w; ++x) {
            int sum[4] = { 0, 0, 0, 0 };
            int cnt = 0;
            for (int y = 0; y <= radius && y < h; ++y, ++cnt)
                for (int c = 0; c < 4; ++c) sum[c] += tmp[(size_t)y * stride + x * 4 + c];
            for (int y = 0; y < h; ++y) {
                for (int c = 0; c < 4; ++c) px[(size_t)y * stride + x * 4 + c] = (BYTE)(sum[c] / cnt);
                const int add = y + radius + 1, sub = y - radius;
                if (add < h) { for (int c = 0; c < 4; ++c) sum[c] += tmp[(size_t)add * stride + x * 4 + c]; ++cnt; }
                if (sub >= 0) { for (int c = 0; c < 4; ++c) sum[c] -= tmp[(size_t)sub * stride + x * 4 + c]; --cnt; }
            }
        }
    }
}

// Пікселізація на місці: кожен блок стає своїм середнім. Саме середнє й робить
// її незворотною — з нього оригінальних пікселів не дістати.
void EdPixelate(BYTE* px, int w, int h, int stride, int block)
{
    if (block < 2) return;
    for (int by = 0; by < h; by += block) {
        const int y2 = (by + block < h) ? by + block : h;
        for (int bx = 0; bx < w; bx += block) {
            const int x2 = (bx + block < w) ? bx + block : w;
            int sum[4] = { 0, 0, 0, 0 };
            const int n = (x2 - bx) * (y2 - by);
            for (int y = by; y < y2; ++y) {
                const BYTE* row = px + (size_t)y * stride;
                for (int x = bx; x < x2; ++x)
                    for (int c = 0; c < 4; ++c) sum[c] += row[x * 4 + c];
            }
            BYTE avg[4];
            for (int c = 0; c < 4; ++c) avg[c] = (BYTE)(sum[c] / n);
            for (int y = by; y < y2; ++y) {
                BYTE* row = px + (size_t)y * stride;
                for (int x = bx; x < x2; ++x)
                    for (int c = 0; c < 4; ++c) row[x * 4 + c] = avg[c];
            }
        }
    }
}

unsigned long long EdMix(unsigned long long h, long long v)
{
    h ^= (unsigned long long)v;
    return h * 1099511628211ULL;
}

// ⚠ Хеш ОБОВʼЯЗКОВО покриває все, що впливає на пікселі позначки. Забуте поле
// не ламає нічого одразу — воно лишає стару плитку ефекту там, де під нею вже
// інша картинка, і помітно це стає через пів години роботи.
unsigned long long EdObjHash(const EdObj& o)
{
    unsigned long long h = 1469598103934665603ULL;
    const int f[] = { (int)o.kind, o.x, o.y, o.w, o.h, (int)o.color, o.thick, o.alpha,
                      o.filled ? 1 : 0, o.size, o.bold ? 1 : 0, o.italic ? 1 : 0,
                      o.align, o.outline, o.boxw, o.mode, o.strength, o.seq, o.group,
                      o.start, o.stamp, o.dash, o.headFront, o.headBack, o.headSize,
                      o.img, o.rot, o.grp };
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); ++i) h = EdMix(h, f[i]);
    for (size_t i = 0; i < o.text.size(); ++i) h = EdMix(h, (long long)o.text[i]);
    for (size_t i = 0; i < o.pts.size(); ++i) {
        h = EdMix(h, o.pts[i].x);
        h = EdMix(h, o.pts[i].y);
    }
    return h;
}

unsigned long long EdBelowHash(int upto)
{
    unsigned long long h = 14695981039346656ULL;
    if (upto > (int)g_edObjs.size()) upto = (int)g_edObjs.size();
    for (int i = 0; i < upto; ++i) h = EdMix(h, (long long)EdObjHash(g_edObjs[i]));
    return h;
}

// Композит «усе, що нижче»: знімок плюс позначки з меншим номером, у РОЗМІРІ
// знімка. Саме з нього ефект бере пікселі — інакше приховування не бачить ні
// вкинутого зображення, ні полотна, перетвореного на обʼєкт (зауваження
// власника), ні сусідньої позначки.
struct EdBelowEntry { int upto; unsigned long long h; Gdiplus::Bitmap* bmp; };
std::vector<EdBelowEntry> g_edBelow;
int g_edBelowDepth = 0;                 // глибина вкладених побудов

void EdBelowClear()
{
    for (size_t i = 0; i < g_edBelow.size(); ++i) delete g_edBelow[i].bmp;
    g_edBelow.clear();
}

void EdDrawObject(Gdiplus::Graphics& g, const EdObj& o, double s, double ox, double oy, int idx);

Gdiplus::Bitmap* EdBelowImage(int upto)
{
    if (!g_edImg) return nullptr;
    if (upto > (int)g_edObjs.size()) upto = (int)g_edObjs.size();
    if (upto <= 0) return g_edImg;                 // під першою позначкою — сам знімок
    const unsigned long long h = EdBelowHash(upto);
    for (size_t i = 0; i < g_edBelow.size(); ++i)
        if (g_edBelow[i].upto == upto && g_edBelow[i].h == h) return g_edBelow[i].bmp;

    Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(g_edImgW, g_edImgH, PixelFormat32bppPARGB);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) { delete bmp; return g_edImg; }
    {
        Gdiplus::Graphics gg(bmp);
        gg.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        gg.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        gg.DrawImage(g_edImg, Gdiplus::Rect(0, 0, g_edImgW, g_edImgH),
                     0, 0, g_edImgW, g_edImgH, Gdiplus::UnitPixel);
        gg.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        // ⚠ Рекурсія скінченна: ефект усередині просить композит із МЕНШИМ
        // upto, тож ланцюжок строго спадає до нуля.
        ++g_edBelowDepth;
        for (int i = 0; i < upto; ++i) EdDrawObject(gg, g_edObjs[i], 1.0, 0.0, 0.0, i);
        --g_edBelowDepth;
    }
    // Чистимо лише на верхньому рівні: нижче хтось іще тримає вказівник на
    // свій композит, і видалити його зараз означало б малювати по звільненому.
    if (g_edBelowDepth == 0 && g_edBelow.size() >= 4) EdBelowClear();
    EdBelowEntry e = { upto, h, bmp };
    g_edBelow.push_back(e);
    return bmp;
}

std::wstring EdFxKey(const EdObj& o, double s, unsigned long long below)
{
    wchar_t head[200];
    wsprintfW(head, L"fx|%d|%d|%d|%d|%d|%d|%d|%d|%d|%08X|%08X%08X",
              (int)o.kind, (int)(s * 1000 + 0.5), o.x, o.y, o.w, o.h,
              o.mode, o.strength, o.alpha, (unsigned)o.color,
              (unsigned)(below >> 32), (unsigned)(below & 0xFFFFFFFFu));
    return std::wstring(head);
}

// Плитка ефекту в РОЗМІРІ ЗНІМКА, потім за потреби зменшена до екранного
// масштабу. Рахувати одразу в масштабі екрана не можна: у файл пішов би слабший
// ефект, ніж той, що бачив користувач.
const EdTile* EdEffectTile(const EdObj& o, double s, int idx)
{
    const unsigned long long below = EdBelowHash(idx);
    const std::wstring key = EdFxKey(o, s, below);
    for (size_t i = 0; i < g_edTiles.size(); ++i)
        if (g_edTiles[i].key == key) return &g_edTiles[i];
    if (!g_edImg || o.w < 1 || o.h < 1) return nullptr;

    // Ділянка може вийти за край знімка — беремо перетин, решта лишається порожньою.
    int rx = o.x, ry = o.y, rw = o.w, rh = o.h;
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > g_edImgW) rw = g_edImgW - rx;
    if (ry + rh > g_edImgH) rh = g_edImgH - ry;
    if (rw < 1 || rh < 1) return nullptr;

    Gdiplus::Bitmap* src = EdBelowImage(idx);
    if (!src) src = g_edImg;
    Gdiplus::Bitmap* full = new Gdiplus::Bitmap(rw, rh, PixelFormat32bppPARGB);
    if (!full || full->GetLastStatus() != Gdiplus::Ok) { delete full; return nullptr; }
    {
        Gdiplus::Graphics g(full);
        g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        g.DrawImage(src, Gdiplus::Rect(0, 0, rw, rh), rx, ry, rw, rh, Gdiplus::UnitPixel);
    }

    Gdiplus::BitmapData bd = {};
    Gdiplus::Rect lock(0, 0, rw, rh);
    bool ok = false;
    if (full->LockBits(&lock, Gdiplus::ImageLockModeRead | Gdiplus::ImageLockModeWrite,
                       PixelFormat32bppPARGB, &bd) == Gdiplus::Ok) {
        BYTE* px = (BYTE*)bd.Scan0;
        if (bd.Stride > 0) {
            if (o.kind == EdKind::Mark) {
                // Множення на фон — порозрядним І. Для кольорів із каналами 0/255
                // це точно множення, а головне — воно однакове і на екрані, і в файлі.
                const BYTE mr = GetRValue(o.color), mg = GetGValue(o.color), mb = GetBValue(o.color);
                for (int y = 0; y < rh; ++y) {
                    BYTE* row = px + (size_t)y * bd.Stride;
                    for (int x = 0; x < rw; ++x) {
                        row[x * 4 + 0] = (BYTE)(row[x * 4 + 0] & mb);
                        row[x * 4 + 1] = (BYTE)(row[x * 4 + 1] & mg);
                        row[x * 4 + 2] = (BYTE)(row[x * 4 + 2] & mr);
                    }
                }
            } else if (o.mode == 2) {
                const BYTE pr = GetRValue(o.color), pg = GetGValue(o.color), pb = GetBValue(o.color);
                for (int y = 0; y < rh; ++y) {
                    BYTE* row = px + (size_t)y * bd.Stride;
                    for (int x = 0; x < rw; ++x) {
                        row[x * 4 + 0] = pb; row[x * 4 + 1] = pg;
                        row[x * 4 + 2] = pr; row[x * 4 + 3] = 255;
                    }
                }
            } else if (o.mode == 1) {
                EdPixelate(px, rw, rh, bd.Stride, EdHideBlock(o));
            } else {
                EdBoxBlur(px, rw, rh, bd.Stride, EdHideRadius(o));
            }
            if (o.alpha < 100) {
                const unsigned k = (unsigned)(o.alpha * 255 / 100);
                for (int y = 0; y < rh; ++y) {
                    BYTE* row = px + (size_t)y * bd.Stride;
                    for (int x = 0; x < rw * 4; ++x) row[x] = (BYTE)(row[x] * k / 255);
                }
            }
            ok = true;
        }
        full->UnlockBits(&bd);
    }
    if (!ok) { delete full; return nullptr; }

    // На екрані плитку показуємо зменшеною; у файл (s == 1) іде як є.
    Gdiplus::Bitmap* out = full;
    const int dw = (int)(rw * s + 0.5), dh = (int)(rh * s + 0.5);
    if (dw != rw || dh != rh) {
        if (dw < 1 || dh < 1) { delete full; return nullptr; }
        out = new Gdiplus::Bitmap(dw, dh, PixelFormat32bppPARGB);
        if (out && out->GetLastStatus() == Gdiplus::Ok) {
            Gdiplus::Graphics g(out);
            // Пікселі зменшуємо без згладжування: інакше блоки розмиються і
            // приховане місце почне «проступати» назад.
            g.SetInterpolationMode(o.mode == 1 && o.kind == EdKind::Hide
                                       ? Gdiplus::InterpolationModeNearestNeighbor
                                       : Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(full, Gdiplus::Rect(0, 0, dw, dh), 0, 0, rw, rh, Gdiplus::UnitPixel);
        } else {
            delete out;
            out = full;
        }
        if (out != full) delete full;
    }

    if (g_edTiles.size() >= kEdTileMax) {
        delete g_edTiles.front().bmp;
        g_edTiles.erase(g_edTiles.begin());
    }
    EdTile t;
    t.key = key;
    t.bmp = out;
    t.pad = 0;
    g_edTiles.push_back(t);
    return &g_edTiles.back();
}

// ---- CAPS-26: лічильник і штампи -----------------------------------------
//
// Шість штампів — власні контури: беруть колір із палітри й масштабуються без
// втрат, бо це шляхи, а не картинки. Емодзі натомість малює DirectWrite тією
// самою плиткою, що й текст: GDI+ не вміє кольорових шрифтів COLR/CBDT і видав
// би Segoe UI Emoji чорно-білим.

// Контури задано в квадраті 0..20, як і піктограми інтерфейсу: далі його просто
// масштабуємо в розмір штампа.
void EdStampShape(Gdiplus::Graphics& g, int id, float ox, float oy, float side,
                  Gdiplus::Color c)
{
    if (side <= 0) return;
    Gdiplus::GraphicsState st = g.Save();
    g.TranslateTransform(ox, oy);
    g.ScaleTransform(side / 20.0f, side / 20.0f);

    Gdiplus::Pen pen(c, 2.6f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush br(c);

    switch (id) {
    case 0: {   // галочка
        Gdiplus::PointF p[3] = { { 3.4f, 10.8f }, { 8.0f, 15.6f }, { 16.6f, 4.8f } };
        g.DrawLines(&pen, p, 3);
        break;
    }
    case 1:     // хрестик
        g.DrawLine(&pen, 4.2f, 4.2f, 15.8f, 15.8f);
        g.DrawLine(&pen, 15.8f, 4.2f, 4.2f, 15.8f);
        break;
    case 2:     // знак питання: гачок, плавний перехід у ніжку, крапка окремо
        g.DrawArc(&pen, 5.6f, 2.4f, 8.8f, 8.8f, 180.0f, 200.0f);
        g.DrawBezier(&pen, 14.14f, 8.3f, 13.2f, 11.2f, 10.0f, 11.0f, 10.0f, 13.6f);
        g.FillEllipse(&br, 8.7f, 15.4f, 2.6f, 2.6f);
        break;
    case 3:     // знак оклику
        g.DrawLine(&pen, 10.0f, 3.0f, 10.0f, 12.6f);
        g.FillEllipse(&br, 8.7f, 15.0f, 2.6f, 2.6f);
        break;
    case 4: {   // зірочка
        Gdiplus::PointF p[10];
        for (int i = 0; i < 10; ++i) {
            const double a = -3.14159265358979 / 2.0 + i * 3.14159265358979 / 5.0;
            const double r = (i % 2 == 0) ? 8.6 : 3.6;
            p[i] = Gdiplus::PointF((float)(10.0 + cos(a) * r), (float)(10.0 + sin(a) * r));
        }
        g.FillPolygon(&br, p, 10);
        break;
    }
    default: {  // трикутник уваги
        Gdiplus::PointF p[3] = { { 10.0f, 2.6f }, { 18.4f, 16.8f }, { 1.6f, 16.8f } };
        g.DrawPolygon(&pen, p, 3);
        g.DrawLine(&pen, 10.0f, 7.6f, 10.0f, 12.0f);
        g.FillEllipse(&br, 8.9f, 13.6f, 2.2f, 2.2f);
        break;
    }
    }
    g.Restore(st);
}

// Поточна група: якщо вибрано кружечок — його, інакше остання використана.
// Так повернутись до старої групи = клацнути по будь-якому її кружечку.
int EdCurGroup()
{
    if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size() && g_edObjs[g_edSel].kind == EdKind::Counter)
        return g_edObjs[g_edSel].group;
    return g_edCounterGroup;
}

int EdGroupStart(int grp)
{
    for (size_t i = 0; i < g_edObjs.size(); ++i)
        if (g_edObjs[i].kind == EdKind::Counter && g_edObjs[i].group == grp) return g_edObjs[i].start;
    return g_edStartNum;
}

const wchar_t* EdChipLabel()
{
    if (EdSelCount() >= 2) {
        static wchar_t many[48];
        wsprintfW(many, S(Str::EdFmtPicked), EdSelCount());
        return many;
    }
    if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size()) {
        const EdKind k = g_edObjs[g_edSel].kind;
        return k == EdKind::Counter ? nullptr : S(EdKindName(k));
    }
    if (g_edTool == EdTool::Select || g_edTool == EdTool::Counter) return nullptr;
    return S(EdToolName(g_edTool));
}

// Смуга зараз про лічильник? Питання те саме, що й для решти властивостей:
// вибране важливіше за інструмент.
bool EdCounterKind()
{
    if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size())
        return g_edObjs[g_edSel].kind == EdKind::Counter;
    return EdToolKind(g_edTool) == EdKind::Counter;
}

// Правка всієї групи. Поле: 0 колір, 1 розмір, 2 прозорість — рівно те, що
// «зовнішнє» в кружечку. Номер, порядок і початок групи це не чіпає: вони
// рахуються з порядку, і міняти їх пачкою означало б ламати нумерацію.
bool EdGroupWould(int field, int value)
{
    const int grp = EdCurGroup();
    for (size_t i = 0; i < g_edObjs.size(); ++i) {
        const EdObj& o = g_edObjs[i];
        if (o.kind != EdKind::Counter || o.group != grp) continue;
        if (field == 0)      { if (o.color != (COLORREF)value) return true; }
        else if (field == 1) { if (o.thick != value)           return true; }
        else                 { if (o.alpha != value)           return true; }
    }
    return false;
}

// Без знімка: потрібне повзунку прозорості, який кладе знімок один раз на
// натискання, а потім сипле значеннями на кожен рух миші.
bool EdGroupSet(int field, int value)
{
    const int grp = EdCurGroup();
    if (!EdGroupWould(field, value)) return false;
    for (size_t i = 0; i < g_edObjs.size(); ++i) {
        EdObj& o = g_edObjs[i];
        if (o.kind != EdKind::Counter || o.group != grp) continue;
        if (field == 0) o.color = (COLORREF)value;
        else if (field == 1) {
            // Кружечок росте від СВОГО центра — інакше вся група поїхала б
            // управо вниз, і розставлені кроки перестали б показувати на своє.
            o.x += (o.w - value) / 2;
            o.y += (o.h - value) / 2;
            o.w = o.h = value;
            o.thick = value;
        }
        else o.alpha = value;
    }
    if (g_edWnd) InvalidateRect(g_edWnd, nullptr, FALSE);
    return true;
}

// ⚠ Знімок кладемо ЛИШЕ коли щось справді зміниться: клік по вже активному
// зразку інакше плодив би порожні кроки, і Ctrl+Z переставав би працювати.
bool EdGroupApply(int field, int value)
{
    if (!EdGroupWould(field, value)) return false;
    EdPushUndo();
    return EdGroupSet(field, value);
}

// Видалення групи одним кроком: десять окремих Ctrl+Z за одну дію — це не
// скасування, а покарання.
void EdGroupDelete()
{
    const int grp = EdCurGroup();
    bool any = false;
    for (size_t i = 0; i < g_edObjs.size(); ++i)
        if (g_edObjs[i].kind == EdKind::Counter && g_edObjs[i].group == grp) { any = true; break; }
    if (!any) return;
    EdPushUndo();
    for (size_t i = g_edObjs.size(); i-- > 0; )
        if (g_edObjs[i].kind == EdKind::Counter && g_edObjs[i].group == grp)
            g_edObjs.erase(g_edObjs.begin() + i);
    g_edSel = -1;
    if (g_edWnd) { EdLayout(g_edWnd); InvalidateRect(g_edWnd, nullptr, FALSE); }
}

int EdGroupCount(int grp)
{
    int n = 0;
    for (size_t i = 0; i < g_edObjs.size(); ++i)
        if (g_edObjs[i].kind == EdKind::Counter && g_edObjs[i].group == grp) ++n;
    return n;
}

int EdCounterNumber(const EdObj& o)
{
    int rank = 0;
    for (size_t i = 0; i < g_edObjs.size(); ++i) {
        const EdObj& c = g_edObjs[i];
        if (c.kind == EdKind::Counter && c.group == o.group && c.seq < o.seq) ++rank;
    }
    return o.start + rank;
}

// Число в кружечку має читатися й на темному, і на світлому кольорі позначки.
COLORREF EdOnColor(COLORREF c)
{
    const int lum = (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
    return lum > 140 ? RGB(24, 24, 28) : RGB(255, 255, 255);
}

// Емодзі — це текстова плитка з одного гліфа. Складаємо для неї тимчасовий
// об'єкт: так емодзі задарма отримують і кеш, і прозорість, і той самий шлях
// на екран та у файл.
EdObj EdGlyphObj(const EdObj& o, const wchar_t* glyph, int size, COLORREF col)
{
    EdObj t = EdObj{};
    t.kind    = EdKind::Text;
    t.text    = glyph;
    t.size    = size;
    t.bold    = false;
    t.italic  = false;
    t.align   = 0;
    t.outline = 0;
    t.boxw    = 0;
    t.color   = col;
    t.alpha   = o.alpha;
    return t;
}

// Центр позначки в координатах ЗНІМКА. Поворот завжди навколо нього: кут,
// прив'язаний до кута рамки, робив би обертання схожим на перетягування.
// Один малювальник на екран і на експорт. Якби їх було два, збережений файл
// рано чи пізно розійшовся б із тим, що показано на екрані.
// ⚠ Пунктир із круглими ковпачками перетворюється на низку крапок: кожен
// штрих отримує по півкола з кожного боку й заповнює проміжок. Для пунктиру
// ковпачки мусять бути плоскі.
void EdApplyDash(Gdiplus::Pen& pen, int dash)
{
    if (dash <= 0) return;
    pen.SetDashStyle(dash == 1 ? Gdiplus::DashStyleDash : Gdiplus::DashStyleDashDot);
    pen.SetStartCap(Gdiplus::LineCapFlat);
    pen.SetEndCap(Gdiplus::LineCapFlat);
    pen.SetDashCap(Gdiplus::DashCapFlat);
}

// Довжина наконечника в пікселях ЗНІМКА. Раніше вона задавалась в одиницях
// товщини пера (AdjustableArrowCap), і «великий наконечник» на тонкій лінії
// лишався маленьким. Тепер розмір — це розмір, а товщина лише додає трохи.
double EdHeadLen(const EdObj& o, double s)
{
    static const double kMul[3] = { 3.0, 4.5, 6.5 };
    const int idx = (o.headSize < 0 || o.headSize > 2) ? 1 : o.headSize;
    double len = (o.thick * kMul[idx] + 4.0) * s;
    if (len < 6.0) len = 6.0;
    return len;
}

// Наконечник власною геометрією: три форми, і кожна знає свій розмір у
// пікселях. Малюється НА кінці відрізка, а сам відрізок під ним коротшає.
void EdDrawHead(Gdiplus::Graphics& g, const Gdiplus::Color& col, float tipX, float tipY,
                double dirX, double dirY, double len, float pw, int type)
{
    const double nx = -dirY, ny = dirX;            // нормаль до напрямку
    const double halfW = len * 0.42;
    Gdiplus::SolidBrush brush(col);
    Gdiplus::Pen pen(col, pw);
    pen.SetLineJoin(Gdiplus::LineJoinMiter);
    if (type == 2) {
        const float r = (float)(len * 0.36);
        g.FillEllipse(&brush, tipX - r, tipY - r, r * 2, r * 2);
        return;
    }
    const float bx = (float)(tipX - dirX * len), by = (float)(tipY - dirY * len);
    Gdiplus::PointF p1((float)(bx + nx * halfW), (float)(by + ny * halfW));
    Gdiplus::PointF p2((float)(bx - nx * halfW), (float)(by - ny * halfW));
    if (type == 1) {
        // «Пташка»: дві лінії від вістря, без заливки.
        g.DrawLine(&pen, p1.X, p1.Y, tipX, tipY);
        g.DrawLine(&pen, p2.X, p2.Y, tipX, tipY);
        return;
    }
    Gdiplus::PointF tri[3] = { Gdiplus::PointF(tipX, tipY), p1, p2 };
    g.FillPolygon(&brush, tri, 3);
}

// idx — номер позначки в списку. Він потрібен рівно одному місцю (ефектам), але
// проходить наскрізь: інакше ефект не знав би, що саме під ним.
void EdDrawObject(Gdiplus::Graphics& g, const EdObj& o, double s, double ox, double oy, int idx)
{
    float pw = (float)(o.thick * s);
    if (pw < 1.0f) pw = 1.0f;              // тонше пікселя — це вже невидимо
    const Gdiplus::Color col = EdC(o.color, o.alpha * 255 / 100);
    Gdiplus::Pen pen(col, pw);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush brush(col);
    const float half = pw / 2.0f;
    const float x = (float)(ox + o.x * s), y = (float)(oy + o.y * s);
    const float w = (float)(o.w * s), h = (float)(o.h * s);

    // Поворот — це перетворення полотна навколо центра позначки. Так він діє
    // на ВСЕ, включно з плитками тексту, емодзі й лічильника, які кладуться
    // через DrawImage: інакше довелося б повертати кожен шлях окремо.
    Gdiplus::GraphicsState rotState = 0;
    const bool rotated = (o.rot != 0 && EdCanRotate(o.kind));
    if (rotated) {
        rotState = g.Save();
        const float cx = x + w / 2.0f, cy = y + h / 2.0f;
        g.TranslateTransform(cx, cy);
        g.RotateTransform((float)o.rot);
        g.TranslateTransform(-cx, -cy);
    }

    switch (o.kind) {
    case EdKind::Rect:
        if (o.filled) g.FillRectangle(&brush, x, y, w, h);
        else { EdApplyDash(pen, o.dash); g.DrawRectangle(&pen, x + half, y + half, w - pw, h - pw); }
        break;
    case EdKind::Ellipse:
        if (o.filled) g.FillEllipse(&brush, x, y, w, h);
        else { EdApplyDash(pen, o.dash); g.DrawEllipse(&pen, x + half, y + half, w - pw, h - pw); }
        break;

    case EdKind::Line: {
        // Наконечники малюємо самі — див. EdDrawHead. AdjustableArrowCap мав
        // розмір В ОДИНИЦЯХ ТОВЩИНИ, тож «великий» на тонкій лінії лишався
        // маленьким, а ще не вмів ні «пташки», ні кружечка.
        const double len = sqrt((double)w * w + (double)h * h);
        if (len < 0.5) break;
        const double dx = w / len, dy = h / len;
        const double hl = EdHeadLen(o, s);
        const bool atEnd   = (o.headFront != 0);
        const bool atStart = (o.headBack != 0);
        // Лінія коротшає рівно на ту частину, яку закриває наконечник, —
        // інакше вістря наїжджає за точку відпускання миші.
        const double backE = atEnd   ? EdMinD(hl * 0.85, len * 0.5) : 0.0;
        const double backS = atStart ? EdMinD(hl * 0.85, len * 0.5) : 0.0;
        const float sx = (float)(x + dx * backS),      sy2 = (float)(y + dy * backS);
        const float ex = (float)(x + w - dx * backE),  ey  = (float)(y + h - dy * backE);
        EdApplyDash(pen, o.dash);
        if (!o.dash) { pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound); }
        g.DrawLine(&pen, sx, sy2, ex, ey);
        if (atEnd)   EdDrawHead(g, col, (float)(x + w), (float)(y + h),  dx,  dy, hl, pw, o.headFront - 1);
        if (atStart) EdDrawHead(g, col, x,              y,              -dx, -dy, hl, pw, o.headBack - 1);
        break;
    }
    case EdKind::Image: {
        Gdiplus::Bitmap* bmp = EdImageOf(o);
        if (!bmp) break;
        // ⚠ Прямокутник призначення ЗАВЖДИ явний: без нього GDI+ перераховує
        // 96 крапок на дюйм у роздільність поверхні — та сама пастка, що з
        // плитками тексту.
        const Gdiplus::Rect dst((INT)(x + 0.5f), (INT)(y + 0.5f), (INT)w, (INT)h);
        if (o.alpha >= 100) {
            g.DrawImage(bmp, dst, 0, 0, (INT)bmp->GetWidth(), (INT)bmp->GetHeight(),
                        Gdiplus::UnitPixel);
        } else {
            // Прозорість — матрицею кольору: множити самі пікселі означало б
            // псувати оригінал, який ще знадобиться на наступному перемалюванні.
            Gdiplus::ColorMatrix cm = {};
            cm.m[0][0] = cm.m[1][1] = cm.m[2][2] = 1.0f;
            cm.m[3][3] = o.alpha / 100.0f;
            cm.m[4][4] = 1.0f;
            Gdiplus::ImageAttributes ia;
            ia.SetColorMatrix(&cm);
            g.DrawImage(bmp, dst, 0, 0, (INT)bmp->GetWidth(), (INT)bmp->GetHeight(),
                        Gdiplus::UnitPixel, &ia);
        }
        break;
    }
    case EdKind::Pen: {
        EdApplyDash(pen, o.dash);
        const size_t n = o.pts.size();
        if (n < 2) {
            if (n == 1) {
                const float px = (float)(ox + o.pts[0].x * s), py = (float)(oy + o.pts[0].y * s);
                g.FillEllipse(&brush, px - half, py - half, pw, pw);
            }
            break;
        }
        std::vector<Gdiplus::PointF> p(n);
        for (size_t i = 0; i < n; ++i)
            p[i] = Gdiplus::PointF((float)(ox + o.pts[i].x * s), (float)(oy + o.pts[i].y * s));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        // Крива, а не ламана: слід миші сам по собі кутастий, і без згладжування
        // це видно. Натяг малий, щоб крива не «вилітала» за точки на різких кутах.
        if (n >= 3) g.DrawCurve(&pen, p.data(), (INT)n, 0.3f);
        else g.DrawLines(&pen, p.data(), (INT)n);
        break;
    }
    case EdKind::Counter: {
        const Gdiplus::Color col = EdC(o.color, o.alpha * 255 / 100);
        Gdiplus::SolidBrush disc(col);
        g.FillEllipse(&disc, x, y, w, h);
        // Тонка світла облямівка: без неї кружечок губиться на позначці того ж
        // кольору, а таке трапляється частіше, ніж здається.
        Gdiplus::Pen ring(EdC(RGB(255, 255, 255), o.alpha * 255 / 100), (float)(w / 16.0));
        g.DrawEllipse(&ring, x + (float)(w / 32.0), y + (float)(w / 32.0),
                      w - (float)(w / 16.0), h - (float)(h / 16.0));
        wchar_t nb[16];
        wsprintfW(nb, L"%d", EdCounterNumber(o));
        EdObj t = EdGlyphObj(o, nb, o.thick * 52 / 100, EdOnColor(o.color));
        t.bold = true;
        const EdTile* tile = EdTextTile(t, s);
        if (tile && tile->bmp) {
            const int tw = (int)tile->bmp->GetWidth(), th = (int)tile->bmp->GetHeight();
            g.DrawImage(tile->bmp,
                        Gdiplus::Rect((int)(x + w / 2 - tw / 2.0f + 0.5f),
                                      (int)(y + h / 2 - th / 2.0f + 0.5f), tw, th),
                        0, 0, tw, th, Gdiplus::UnitPixel);
        }
        break;
    }
    case EdKind::Stamp: {
        if (o.stamp >= kEdEmojiBase) {
            const int ei = o.stamp - kEdEmojiBase;
            if (ei < 0 || ei >= kEdEmojiCount) break;
            EdObj t = EdGlyphObj(o, kEdEmoji[ei], o.thick, o.color);
            const EdTile* tile = EdTextTile(t, s);
            if (!tile || !tile->bmp) break;
            const int tw = (int)tile->bmp->GetWidth(), th = (int)tile->bmp->GetHeight();
            g.DrawImage(tile->bmp,
                        Gdiplus::Rect((int)(x + w / 2 - tw / 2.0f + 0.5f),
                                      (int)(y + h / 2 - th / 2.0f + 0.5f), tw, th),
                        0, 0, tw, th, Gdiplus::UnitPixel);
        } else {
            EdStampShape(g, o.stamp, x, y, w, EdC(o.color, o.alpha * 255 / 100));
        }
        break;
    }
    case EdKind::Hide:
    case EdKind::Mark: {
        const EdTile* tile = EdEffectTile(o, s, idx);
        if (!tile || !tile->bmp) break;
        const int tw = (int)tile->bmp->GetWidth(), th = (int)tile->bmp->GetHeight();
        g.DrawImage(tile->bmp, Gdiplus::Rect((int)(x + 0.5f), (int)(y + 0.5f), tw, th),
                    0, 0, tw, th, Gdiplus::UnitPixel);
        break;
    }
    case EdKind::Text: {
        const EdTile* tile = EdTextTile(o, s);
        if (!tile || !tile->bmp) break;
        const int tw = (int)tile->bmp->GetWidth(), th = (int)tile->bmp->GetHeight();
        // Прямокутник призначення задаємо явно: DrawImage без нього перерахував
        // би плитку з 96 крапок на дюйм у роздільність поверхні й розмив напис.
        g.DrawImage(tile->bmp,
                    Gdiplus::Rect((int)(x + 0.5f) - tile->pad, (int)(y + 0.5f) - tile->pad, tw, th),
                    0, 0, tw, th, Gdiplus::UnitPixel);
        break;
    }
    default: break;
    }
    if (rotated) g.Restore(rotState);
}

// Плитка «шахівниці» — 16×16, дві ледь відмінні сірі. Робиться один раз:
// кожне перемальовування створювало б її наново разом із пензлем.
Gdiplus::Bitmap* EdCheckerTile()
{
    static Gdiplus::Bitmap* tile = nullptr;
    if (tile) return tile;
    tile = new Gdiplus::Bitmap(16, 16, PixelFormat32bppPARGB);
    if (!tile || tile->GetLastStatus() != Gdiplus::Ok) { delete tile; tile = nullptr; return nullptr; }
    Gdiplus::Graphics g(tile);
    Gdiplus::SolidBrush light(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::SolidBrush dark(Gdiplus::Color(255, 226, 226, 230));
    g.FillRectangle(&light, 0, 0, 16, 16);
    g.FillRectangle(&dark, 0, 0, 8, 8);
    g.FillRectangle(&dark, 8, 8, 8, 8);
    return tile;
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
    // Поки тримають «Порівняти», полотно показує кадр без тону і БЕЗ позначок:
    // це вихідний кадр, а не «те саме, тільки блідіше». Позначки ще й розійшлися
    // б із плитками розмиття, зробленими вже з виправлених пікселів.
    // ⚠ Під знімком — шахівниця, навколо — межа. Без них після збільшення
    // полотна не видно ні того, де воно закінчується, ні того, що порожнє місце
    // прозоре, а не біле (зауваження власника).
    if (Gdiplus::Bitmap* tile = EdCheckerTile()) {
        Gdiplus::TextureBrush tb(tile);
        tb.SetWrapMode(Gdiplus::WrapModeTile);
        g.FillRectangle(&tb, (INT)ir.left, (INT)ir.top,
                        (INT)(ir.right - ir.left), (INT)(ir.bottom - ir.top));
    }

    const bool orig = (g_edCompare && g_edCmp);
    g.DrawImage(orig ? g_edCmp : g_edImg,
                Gdiplus::Rect(ir.left, ir.top, ir.right - ir.left, ir.bottom - ir.top),
                EdViewX(), EdViewY(), EdViewW(), EdViewH(), Gdiplus::UnitPixel);
    if (orig) {
        wchar_t ob[64];
        lstrcpynW(ob, S(Str::EdOriginal), 64);
        const int tw = EdTextWidth(dc, ob, g_edFontBold);
        RECT plate = { g_edRcCanvas.left + EdPx(14), g_edRcCanvas.top + EdPx(14),
                       g_edRcCanvas.left + EdPx(14) + tw + EdPx(20),
                       g_edRcCanvas.top + EdPx(14) + EdPx(28) };
        Gdiplus::Color pb(220, 24, 24, 28);
        EdFillRound(g, plate, (float)EdPx(7), &pb, nullptr);
        EdDrawText(dc, plate, ob, g_edFontBold, RGB(255, 255, 255),
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        g.Restore(st);
        return;
    }

    // Межа полотна — тонка й спокійна, але завжди на місці.
    {
        Gdiplus::Pen edge(EdC(g_edDark ? RGB(120, 120, 126) : RGB(150, 150, 156)), 1.0f);
        g.DrawRectangle(&edge, (float)ir.left - 0.5f, (float)ir.top - 0.5f,
                        (float)(ir.right - ir.left), (float)(ir.bottom - ir.top));
    }

    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const double s = EdScale();
    // Зсув на початок кадру: позначки носять абсолютні координати знімка.
    const double ox = ir.left - EdViewX() * s, oy = ir.top - EdViewY() * s;
    for (size_t i = 0; i < g_edObjs.size(); ++i) {
        // Напис, який саме зараз правлять, показує поле введення — інакше під
        // ним просвічував би його ж старий текст.
        if (g_edEdit && (int)i == g_edEditIdx) continue;
        EdDrawObject(g, g_edObjs[i], s, ox, oy, (int)i);
    }

    // Те, що зараз тягнуть мишею, ще не в списку — малюємо окремо.
    if (g_edDrag == EdDrag::New)
        // Те, що ще тягнуть, лежить НАД усім: ефект під курсором має вже
        // накривати все намальоване, інакше маркер «прозріває» після кнопки.
        EdDrawObject(g, g_edNew, s, ox, oy, (int)g_edObjs.size());

    // Решта вибраних — тонкою рамкою без ручок: ручки має лише головний,
    // інакше незрозуміло, що саме потягнеться.
    const std::vector<int> selDots = EdManySel() ? EdSelAll() : g_edSelMore;
    for (size_t k = 0; k < selDots.size(); ++k) {
        const int idx2 = selDots[k];
        if (idx2 < 0 || idx2 >= (int)g_edObjs.size()) continue;
        const RECT rr = EdObjScreen(g_edObjs[idx2]);
        Gdiplus::Pen more(EdC(t.accent, 200), 1.0f);
        more.SetDashStyle(Gdiplus::DashStyleDot);
        g.DrawRectangle(&more, (float)rr.left, (float)rr.top,
                        (float)(rr.right - rr.left), (float)(rr.bottom - rr.top));
    }
    // ⚠ Коли вибрано кілька — рамка й ручки СПІЛЬНІ (зауваження власника):
    // ручки головного тягли б лише його, а решта їхала б слідом лише на око.
    // Тонкі пунктири окремих позначок лишаємо: без них не видно, що саме
    // потрапило у вибір, коли позначки рознесені по кутах.
    if (EdManySel()) {
        RECT box;
        if (EdSelScreenBox(&box)) {
            Gdiplus::Pen mark(EdC(RGB(255, 255, 255), 190), 1.0f);
            mark.SetDashStyle(Gdiplus::DashStyleDash);
            g.DrawRectangle(&mark, (float)box.left, (float)box.top,
                            (float)(box.right - box.left), (float)(box.bottom - box.top));
            Gdiplus::SolidBrush wb(EdC(RGB(255, 255, 255)));
            Gdiplus::Pen hp(EdC(t.accent), 2.0f);
            RECT rh;
            if (EdManyRotHandle(&rh)) {
                g.FillEllipse(&wb, (INT)rh.left, (INT)rh.top,
                              (INT)(rh.right - rh.left), (INT)(rh.bottom - rh.top));
                g.DrawEllipse(&hp, (float)rh.left, (float)rh.top,
                              (float)(rh.right - rh.left), (float)(rh.bottom - rh.top));
            }
            RECT hs[8];
            const int hn = EdManyHandles(hs);
            for (int i = 0; i < hn; ++i) {
                g.FillRectangle(&wb, (INT)hs[i].left, (INT)hs[i].top,
                                (INT)(hs[i].right - hs[i].left), (INT)(hs[i].bottom - hs[i].top));
                g.DrawRectangle(&hp, (float)hs[i].left, (float)hs[i].top,
                                (float)(hs[i].right - hs[i].left), (float)(hs[i].bottom - hs[i].top));
            }
        }
    } else if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size() && !(g_edEdit && g_edSel == g_edEditIdx)) {
        const EdObj& so = g_edObjs[g_edSel];
        const RECT r = EdObjScreen(so);
        Gdiplus::Pen mark(EdC(RGB(255, 255, 255), 190), 1.0f);
        mark.SetDashStyle(Gdiplus::DashStyleDash);
        if (so.rot != 0 && EdCanRotate(so.kind)) {
            // Рамка повертається разом із позначкою — інакше вона обіцяла б
            // габарити, яких немає.
            const double cx = (r.left + r.right) / 2.0, cy = (r.top + r.bottom) / 2.0;
            double xs[4] = { (double)r.left, (double)r.right, (double)r.right, (double)r.left };
            double ys[4] = { (double)r.top,  (double)r.top,   (double)r.bottom, (double)r.bottom };
            Gdiplus::PointF pts[5];
            for (int i = 0; i < 4; ++i) {
                EdRotatePt(cx, cy, so.rot, xs[i], ys[i]);
                pts[i] = Gdiplus::PointF((float)xs[i], (float)ys[i]);
            }
            pts[4] = pts[0];
            g.DrawLines(&mark, pts, 5);
        } else {
            g.DrawRectangle(&mark, (float)r.left, (float)r.top,
                            (float)(r.right - r.left), (float)(r.bottom - r.top));
        }
        RECT rh;
        if (EdRotHandle(so, &rh)) {
            Gdiplus::SolidBrush rb(EdC(RGB(255, 255, 255)));
            Gdiplus::Pen rp(EdC(t.accent), 2.0f);
            g.FillEllipse(&rb, (INT)rh.left, (INT)rh.top,
                          (INT)(rh.right - rh.left), (INT)(rh.bottom - rh.top));
            g.DrawEllipse(&rp, (float)rh.left, (float)rh.top,
                          (float)(rh.right - rh.left), (float)(rh.bottom - rh.top));
        }
        RECT hs[8];
        const int hn = EdHandles(g_edObjs[g_edSel], hs);
        Gdiplus::SolidBrush wb(EdC(RGB(255, 255, 255)));
        Gdiplus::Pen hp(EdC(t.accent), 2.0f);
        for (int i = 0; i < hn; ++i) {
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
        EdIcon(g, g_edPanelOpen ? IcoChevR : IcoChevL, EdIconBox(*r), EdC(t.text2), 1.5f);
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

    if (g_edHdr) {
        wchar_t hb[128];
        if (g_edSdrWhite > 1.0f) wsprintfW(hb, S(Str::EdFmtHdr), (int)(g_edSdrWhite + 0.5f));
        else lstrcpynW(hb, S(Str::EdHdrNote), 128);
        RECT lh = { x, y, g_edRcPanel.right - EdPx(14), y + EdPx(20) };
        EdDrawText(dc, lh, hb, g_edFont, t.text2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y = lh.bottom + EdPx(6);
    }

    wsprintfW(buf, S(Str::EdFmtMarks), (int)g_edObjs.size());
    RECT l3 = { x, y, g_edRcPanel.right - EdPx(14), y + EdPx(20) };
    EdDrawText(dc, l3, buf, g_edFont, t.text2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Далі — CAPS-28. Жодної власної арифметики: усі прямокутники вже пораховані
    // в розкладці, тут їх лише впізнають. Саме через це панель не роз'їжджається
    // з тим, у що клікають.
    const int pr = g_edRcPanel.right - EdPx(14);
    const EdHit geo[4] = { EdHit::RotL, EdHit::RotR, EdHit::FlipH, EdHit::FlipV };
    const int gico[4]  = { IcoRotL, IcoRotR, IcoFlipH, IcoFlipV };
    for (int i = 0; i < 4; ++i) {
        const RECT* r = EdRegionRect(geo[i], 0);
        if (!r) return;
        EdPaintButton(g, *r, t, false, g_edHotWhat == geo[i], false);
        EdIcon(g, gico[i], EdIconBox(*r), EdC(t.text), 1.5f);
    }
    {
        const struct { EdHit what; Str label; } sz[2] = {
            { EdHit::SizeImg, Str::EdBtnSizeImg }, { EdHit::SizeCan, Str::EdBtnSizeCan }
        };
        for (int i = 0; i < 2; ++i) {
            const RECT* r = EdRegionRect(sz[i].what, 0);
            if (!r) continue;
            EdPaintButton(g, *r, t, false, g_edHotWhat == sz[i].what, false);
            EdDrawText(dc, *r, S(sz[i].label), g_edFont, t.text,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    if (const RECT* r0 = EdRegionRect(EdHit::SizeCan, 0)) {
        RECT th = { x, r0->bottom + EdPx(20), pr, r0->bottom + EdPx(20) + EdPx(18) };
        EdDrawText(dc, th, S(Str::EdSecTone), g_edFontSmall, t.text2,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    const EdHit sls[3] = { EdHit::Exposure, EdHit::Gamma, EdHit::Contrast };
    const Str   labs[3] = { Str::EdExposure, Str::EdGamma, Str::EdContrast };
    for (int i = 0; i < 3; ++i) {
        const RECT* r = EdRegionRect(sls[i], 0);
        if (!r) return;
        RECT lr = { x, r->top - EdPx(4) - EdPx(18), pr, r->top - EdPx(4) };
        EdDrawText(dc, lr, S(labs[i]), g_edFont, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        // Значення поруч із підписом: інакше дізнатися, на скільки саме зсунуто,
        // можна хіба що на око.
        wchar_t vb[32];
        if (i == 0) {
            const int a = g_edExposure < 0 ? -g_edExposure : g_edExposure;
            wsprintfW(vb, L"%s%d,%d EV", g_edExposure < 0 ? L"-" : (g_edExposure > 0 ? L"+" : L""),
                      a / 10, a % 10);
        } else if (i == 1) {
            wsprintfW(vb, L"%d,%02d", g_edGamma / 100, g_edGamma % 100);
        } else {
            wsprintfW(vb, L"%s%d", g_edContrast > 0 ? L"+" : L"",
                      g_edContrast);
        }
        EdDrawText(dc, lr, vb, g_edFont, t.text2, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        const int val  = i == 0 ? g_edExposure : i == 1 ? g_edGamma : g_edContrast;
        const int lo   = i == 0 ? -20 : i == 1 ?  50 : -50;
        const int hi   = i == 0 ?  20 : i == 1 ? 200 :  50;
        const int zero = i == 1 ? 100 : 0;
        EdPaintSliderRange(g, *r, t, val, lo, hi, zero);
    }

    // Обидві кнопки мовчать, поки тон типовий: скидати й порівнювати нема чого.
    const bool touched = !EdToneDefault();
    if (const RECT* r = EdRegionRect(EdHit::ToneReset, 0)) {
        EdPaintButton(g, *r, t, false, touched && g_edHotWhat == EdHit::ToneReset, false);
        EdDrawText(dc, *r, S(Str::EdToneReset), g_edFont, touched ? t.text : t.text2,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (const RECT* r = EdRegionRect(EdHit::Compare, 0)) {
        EdPaintButton(g, *r, t, g_edCompare, touched && g_edHotWhat == EdHit::Compare, false);
        const COLORREF fg = touched ? t.text : t.text2;
        RECT ic = { r->left + EdPx(8), r->top, r->left + EdPx(8) + EdPx(16), r->bottom };
        EdIcon(g, IcoCompare, EdIconBox(ic), EdC(fg), 1.4f);
        RECT lr = { ic.right + EdPx(4), r->top, r->right - EdPx(6), r->bottom };
        EdDrawText(dc, lr, S(Str::EdCompare), g_edFont, fg, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

// Розкритий список малюється ОСТАННІМ у всьому вікні — інакше його накриє
// полотно, поверх якого він висить.
void EdPaintPick(HDC dc, Gdiplus::Graphics& g, const EdTheme& t)
{
    if (g_edPickOpen < 0) return;
    const RECT* first = EdRegionRect(EdHit::PickItem, 0);
    const RECT* last  = EdRegionRect(EdHit::PickItem, EdPickCount(g_edPickOpen) - 1);
    if (!first || !last) return;
    RECT box = { first->left - EdPx(4), first->top - EdPx(4),
                 last->right + EdPx(4), last->bottom + EdPx(4) };
    Gdiplus::Color shadow(60, 0, 0, 0);
    RECT sh = box;
    OffsetRect(&sh, 0, EdPx(2));
    EdFillRound(g, sh, (float)EdPx(8), &shadow, nullptr);
    Gdiplus::Color fill = EdC(t.chrome), bd = EdC(t.border);
    EdFillRound(g, box, (float)EdPx(8), &fill, &bd);
    const int cur = EdPickValue(g_edPickOpen);
    const int n = EdPickCount(g_edPickOpen);
    for (int i = 0; i < n; ++i) {
        const RECT* r = EdRegionRect(EdHit::PickItem, i);
        if (!r) break;
        const bool hot = (g_edHotWhat == EdHit::PickItem && g_edHotIdx == i);
        if (i == cur || hot) {
            Gdiplus::Color f = EdC(i == cur ? t.accentBg : t.hot);
            RECT rr = *r;
            InflateRect(&rr, -EdPx(2), -EdPx(2));
            EdFillRound(g, rr, (float)EdPx(5), &f, nullptr);
        }
        EdPickSample(g, *r, g_edPickOpen, i, EdC(i == cur ? t.accent : t.text));
    }
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

    // «Відкрити» і стрілка списку — одна кнопка на вигляд, дві на дотик.
    if (const RECT* ob = EdRegionRect(EdHit::Open, 0)) {
        const RECT* om = EdRegionRect(EdHit::OpenMenu, 0);
        RECT whole = *ob;
        if (om) whole.right = om->right;
        const bool hot = (g_edHotWhat == EdHit::Open || g_edHotWhat == EdHit::OpenMenu);
        EdPaintButton(g, whole, t, false, hot, false);
        RECT ic = { ob->left + EdPx(8), ob->top, ob->left + EdPx(8) + EdPx(18), ob->bottom };
        EdIcon(g, IcoOpen, EdIconBox(ic), EdC(t.text), 1.5f);
        RECT lb = { ic.right + EdPx(6), ob->top, ob->right, ob->bottom };
        EdDrawText(dc, lb, S(Str::EdOpenBtn), g_edFont, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (om) {
            RECT sep = { om->left, om->top + EdPx(5), om->left + 1, om->bottom - EdPx(5) };
            HBRUSH sb = CreateSolidBrush(t.border);
            FillRect(dc, &sep, sb);
            DeleteObject(sb);
            EdIcon(g, IcoChevD, EdIconBox(*om), EdC(t.text2), 1.6f);
        }
    }

    wchar_t buf[128];
    int x = EdPx(14);
    if (const RECT* om = EdRegionRect(EdHit::OpenMenu, 0)) x = om->right + EdPx(12) + 1 + EdPx(12);
    wsprintfW(buf, L"%d × %d", EdViewW(), EdViewH());
    RECT r1 = { x, g_edRcStatus.top, x + EdTextWidth(dc, L"8888 × 8888", g_edFont), g_edRcStatus.bottom };
    EdDrawText(dc, r1, buf, g_edFont, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    x = r1.right + EdPx(12);
    RECT d1 = { x, (g_edRcStatus.top + g_edRcStatus.bottom) / 2 - EdPx(9), x + 1,
                (g_edRcStatus.top + g_edRcStatus.bottom) / 2 + EdPx(9) };
    b = CreateSolidBrush(t.border);
    FillRect(dc, &d1, b);
    DeleteObject(b);
    x = d1.right + EdPx(12);

    const bool toast = (g_edToast != Str::Empty) && (int)(g_edToastUntil - GetTickCount()) > 0;
    if (toast)
        lstrcpynW(buf, S(g_edToast), 128);
    else if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size())
        wsprintfW(buf, S(Str::EdFmtSel), g_edObjs[g_edSel].w, g_edObjs[g_edSel].h);
    else if (EdOutsideCount() > 0)
        wsprintfW(buf, S(Str::EdFmtOutside), EdOutsideCount());
    else
        lstrcpynW(buf, S(Str::EdNoSel), 128);
    RECT r2 = { x, g_edRcStatus.top, x + EdPx(190), g_edRcStatus.bottom };
    EdDrawText(dc, r2, buf, toast ? g_edFontBold : g_edFont, toast ? t.accent : t.text2,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    x = r2.right + EdPx(12);
    RECT d2 = { x, d1.top, x + 1, d1.bottom };
    b = CreateSolidBrush(t.border);
    FillRect(dc, &d2, b);
    DeleteObject(b);

    const int pct = (int)(EdScale() * 100.0 + 0.5);
    if (const RECT* rz = EdRegionRect(EdHit::Zoom, 0)) {
        EdPaintSliderRange(g, *rz, t, EdZoomToSlider(g_edZoom), 0, 100, 0);
        wsprintfW(buf, L"%d %%", pct);
        RECT rv = { rz->right + EdPx(8), g_edRcStatus.top,
                    rz->right + EdPx(8) + EdPx(46), g_edRcStatus.bottom };
        EdDrawText(dc, rv, buf, g_edFont, t.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (const RECT* rh = EdRegionRect(EdHit::Zoom100, 0)) {
        // Кнопка підсвічена, коли масштаб уже рівно сто відсотків: інакше
        // незрозуміло, натиснута вона вже чи ні.
        const bool on = (pct == 100);
        EdPaintButton(g, *rh, t, on, g_edHotWhat == EdHit::Zoom100, false);
        EdDrawText(dc, *rh, S(Str::EdZoom100), on ? g_edFontBold : g_edFont,
                   on ? t.accent : t.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (const RECT* rf = EdRegionRect(EdHit::Fit, 0)) {
        EdPaintButton(g, *rf, t, false, g_edHotWhat == EdHit::Fit, false);
        EdDrawText(dc, *rf, S(Str::EdFit), g_edFont, t.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (const RECT* rsh = EdRegionRect(EdHit::Share, 0)) {
        EdPaintButton(g, *rsh, t, false, g_edHotWhat == EdHit::Share, false);
        EdDrawText(dc, *rsh, S(Str::EdShare), g_edFont, t.text,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // Дві рівноправні кнопки. Підсвічена — та, якою користувалися востаннє:
    // вона ж спрацює на Enter. Друга нікуди не дівається.
    struct { EdHit what; Str label; int ico; bool primary; } outs[2] = {
        { EdHit::Save, Str::EdSaveAs, IcoSave, g_edLastAction == 1 },
        { EdHit::Copy, Str::EdCopy,   IcoCopy, g_edLastAction == 0 }
    };
    for (int i = 0; i < 2; ++i) {
        const RECT* r = EdRegionRect(outs[i].what, 0);
        if (!r) continue;
        const bool hot = (g_edHotWhat == outs[i].what);
        if (outs[i].primary) {
            Gdiplus::Color fill = EdC(t.accent, hot ? 225 : 255);
            Gdiplus::Color bd = EdC(t.accent);
            EdFillRound(g, *r, (float)EdPx(6), &fill, &bd);
        } else {
            EdPaintButton(g, *r, t, false, hot, false);
        }
        const COLORREF fg = outs[i].primary ? (g_edDark ? RGB(0, 52, 79) : RGB(255, 255, 255)) : t.text;
        RECT ic = { r->left + EdPx(10), r->top, r->left + EdPx(10) + EdPx(18), r->bottom };
        EdIcon(g, outs[i].ico, EdIconBox(ic), EdC(fg), 1.5f);
        RECT lr = { ic.right + EdPx(6), r->top, r->right - EdPx(10), r->bottom };
        EdDrawText(dc, lr, S(outs[i].label), g_edFont, fg, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

void EdPaint(HWND hwnd, HDC dc)
{
    const EdTheme t = EdColors(g_edDark);
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    EdPaintCanvas(dc, g, t);
    EdPaintCrop(dc, g, t);
    EdPaintCaption(dc, g, t);
    EdPaintRail(dc, g, t);
    EdPaintPanel(dc, g, t);
    EdPaintStrip(dc, g, t);
    EdPaintStatus(dc, g, t);
    EdPaintPick(dc, g, t);     // розкритий селект — поверх усього
}

// ---- дії ---------------------------------------------------------------

// Вибір у селекті: вибраному об'єкту — зі знімком для скасування, і завжди
// в типові значення, щоб наступна позначка успадкувала те саме.
void EdPickApply(int group, int value)
{
    const bool sel = (g_edSel >= 0 && g_edSel < (int)g_edObjs.size());
    if (sel) {
        EdObj& o = g_edObjs[g_edSel];
        int* dst = (group == 0) ? &o.dash : (group == 1) ? &o.headFront
                 : (group == 2) ? &o.headBack : &o.headSize;
        if (*dst != value) { EdPushUndo(); *dst = value; }
    }
    switch (group) {
    case 0:  g_edDash = value; break;
    case 1:  g_edHeadFront = value; break;
    case 2:  g_edHeadBack = value; break;
    default: g_edHeadSize = value; break;
    }
}

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

void EdSelMoreSnap(EdSnap& s) { s.selMore = g_edSelMore; }

bool EdManySel() { return !g_edSelMore.empty(); }

int EdSelCount()
{
    if (g_edSel < 0) return 0;
    return 1 + (int)g_edSelMore.size();
}

bool EdIsSelected(int i)
{
    if (i == g_edSel) return true;
    for (size_t k = 0; k < g_edSelMore.size(); ++k) if (g_edSelMore[k] == i) return true;
    return false;
}

void EdSelClear()
{
    g_edSel = -1;
    g_edSelMore.clear();
}

// Усі вибрані одним списком — щоб дії над ними писались один раз.
std::vector<int> EdSelAll()
{
    std::vector<int> v;
    if (g_edSel >= 0) v.push_back(g_edSel);
    for (size_t k = 0; k < g_edSelMore.size(); ++k) v.push_back(g_edSelMore[k]);
    return v;
}

// Вибір ОДНОГО об'єкта: якщо він у групі — вибираємо всю групу. Саме тут
// група й перетворюється на «кілька вибраних», як вирішив власник.
void EdSelectOne(int i)
{
    EdSelClear();
    if (i < 0 || i >= (int)g_edObjs.size()) return;
    g_edSel = i;
    const int grp = g_edObjs[i].grp;
    if (grp == 0) return;
    for (int k = 0; k < (int)g_edObjs.size(); ++k)
        if (k != i && g_edObjs[k].grp == grp) g_edSelMore.push_back(k);
}

void EdSelToggle(int i)
{
    if (i < 0 || i >= (int)g_edObjs.size()) return;
    if (g_edSel < 0) { EdSelectOne(i); return; }
    // Повторний Shift-клік прибирає з вибору; якщо прибрали головного —
    // головним стає перший із решти.
    for (size_t k = 0; k < g_edSelMore.size(); ++k) {
        if (g_edSelMore[k] == i) { g_edSelMore.erase(g_edSelMore.begin() + k); return; }
    }
    if (i == g_edSel) {
        if (g_edSelMore.empty()) { EdSelClear(); return; }
        g_edSel = g_edSelMore.front();
        g_edSelMore.erase(g_edSelMore.begin());
        return;
    }
    g_edSelMore.push_back(i);
}

// Спільні габарити вибраного — по них вирівнюють і малюють зовнішню рамку.
bool EdSelBounds(RECT* out)
{
    const std::vector<int> all = EdSelAll();
    if (all.empty()) return false;
    bool first = true;
    for (size_t k = 0; k < all.size(); ++k) {
        const EdObj& o = g_edObjs[all[k]];
        RECT r = { o.x, o.y, o.x + o.w, o.y + o.h };
        if (r.right < r.left) { const LONG t = r.left; r.left = r.right; r.right = t; }
        if (r.bottom < r.top) { const LONG t = r.top; r.top = r.bottom; r.bottom = t; }
        if (first) { *out = r; first = false; continue; }
        if (r.left < out->left) out->left = r.left;
        if (r.top < out->top) out->top = r.top;
        if (r.right > out->right) out->right = r.right;
        if (r.bottom > out->bottom) out->bottom = r.bottom;
    }
    return !first;
}

// Одна позначка з ПОЧАТКОВОЇ копії в нове місце спільної рамки. Масштабуємо
// лише те, що має габарити: напис, лічильник і штамп переїздять центром і
// лишаються свого розміру — рівно як при зміні розміру всього знімка (CAPS-44),
// інакше група з лічильниками після розтягування стала б нечитабельною.
void EdScaleOne(EdObj& o, const EdObj& src, double ax, double ay, double kx, double ky)
{
    const bool upright = (src.kind == EdKind::Text || src.kind == EdKind::Counter ||
                          src.kind == EdKind::Stamp);
    const double cx = ax + (src.x + src.w / 2.0 - ax) * kx;
    const double cy = ay + (src.y + src.h / 2.0 - ay) * ky;
    if (upright) {
        o.x = (int)(cx - src.w / 2.0 + 0.5);
        o.y = (int)(cy - src.h / 2.0 + 0.5);
        return;
    }
    o.x = (int)(ax + (src.x - ax) * kx + 0.5);
    o.y = (int)(ay + (src.y - ay) * ky + 0.5);
    o.w = (int)(src.w * kx + (src.w < 0 ? -0.5 : 0.5));
    o.h = (int)(src.h * ky + (src.h < 0 ? -0.5 : 0.5));
    for (size_t i = 0; i < o.pts.size() && i < src.pts.size(); ++i) {
        o.pts[i].x = (LONG)(ax + (src.pts[i].x - ax) * kx + 0.5);
        o.pts[i].y = (LONG)(ay + (src.pts[i].y - ay) * ky + 0.5);
    }
}

// Спільна зміна розміру: нерухомим лишається ПРОТИЛЕЖНИЙ бік рамки, як і в
// однієї позначки. Ручки середин сторін тягнуть лише свою вісь.
void EdManyResize(int handle, POINT img, bool keep)
{
    if (g_edManyOrig.empty()) return;
    RECT b = g_edManyBox;
    const bool left  = (handle == 0 || handle == 6 || handle == 7);
    const bool right = (handle >= 2 && handle <= 4);
    const bool top   = (handle <= 2);
    const bool bot   = (handle >= 4 && handle <= 6);
    if (left)  b.left = img.x;
    if (right) b.right = img.x;
    if (top)   b.top = img.y;
    if (bot)   b.bottom = img.y;
    if (keep) {
        int l = b.left, t = b.top, r = b.right, bo = b.bottom;
        EdKeepAspect(handle, g_edManyBox.right - g_edManyBox.left,
                     g_edManyBox.bottom - g_edManyBox.top, l, t, r, bo);
        b.left = l; b.top = t; b.right = r; b.bottom = bo;
    }
    // Рамка не має вивертатися навиворіт: мінімум чотири точки на бік.
    if (b.right - b.left < 4) { if (left) b.left = b.right - 4; else b.right = b.left + 4; }
    if (b.bottom - b.top < 4) { if (top) b.top = b.bottom - 4; else b.bottom = b.top + 4; }
    const double ow = (double)(g_edManyBox.right - g_edManyBox.left);
    const double oh = (double)(g_edManyBox.bottom - g_edManyBox.top);
    if (ow < 1.0 || oh < 1.0) return;
    const double kx = (b.right - b.left) / ow, ky = (b.bottom - b.top) / oh;
    // Якірна точка — той кут, який НЕ рухається.
    const double ax = left ? (double)b.right : (double)g_edManyBox.left;
    const double ay = top  ? (double)b.bottom : (double)g_edManyBox.top;
    for (size_t i = 0; i < g_edManyIdx.size(); ++i) {
        const int k = g_edManyIdx[i];
        if (k < 0 || k >= (int)g_edObjs.size()) continue;
        EdScaleOne(g_edObjs[k], g_edManyOrig[i], ax, ay, kx, ky);
    }
}

// Спільний поворот: кожна позначка обертається НАВКОЛО ЦЕНТРА РАМКИ — їде її
// центр, а власний кут додається зверху. Позначки без власного кута (приховання
// й маркер) лише переїздять: нахилити їх нічим, і вигадувати для них окремий
// механізм заради спільного повороту було б дорожче, ніж воно того варте.
void EdManyRotate(int delta)
{
    if (g_edManyOrig.empty()) return;
    const double cx = (g_edManyBox.left + g_edManyBox.right) / 2.0;
    const double cy = (g_edManyBox.top + g_edManyBox.bottom) / 2.0;
    for (size_t i = 0; i < g_edManyIdx.size(); ++i) {
        const int k = g_edManyIdx[i];
        if (k < 0 || k >= (int)g_edObjs.size()) continue;
        const EdObj& src = g_edManyOrig[i];
        EdObj& o = g_edObjs[k];
        double ox = src.x + src.w / 2.0, oy = src.y + src.h / 2.0;
        EdRotatePt(cx, cy, delta, ox, oy);
        o.x = (int)(ox - src.w / 2.0 + 0.5);
        o.y = (int)(oy - src.h / 2.0 + 0.5);
        for (size_t j = 0; j < o.pts.size() && j < src.pts.size(); ++j) {
            double px = src.pts[j].x, py = src.pts[j].y;
            EdRotatePt(cx, cy, delta, px, py);
            o.pts[j].x = (LONG)(px + 0.5);
            o.pts[j].y = (LONG)(py + 0.5);
        }
        if (EdCanRotate(src.kind)) {
            int d = (src.rot + delta) % 360;
            if (d < 0) d += 360;
            o.rot = d;
        }
    }
}

// Знімок вибраного на початку спільного тягнення.
void EdManyBegin()
{
    g_edManyIdx = EdSelAll();
    g_edManyOrig.clear();
    for (size_t i = 0; i < g_edManyIdx.size(); ++i)
        g_edManyOrig.push_back(g_edObjs[g_edManyIdx[i]]);
    if (!EdSelBounds(&g_edManyBox)) g_edManyBox = RECT{ 0, 0, 0, 0 };
}

// Зсув однієї позначки разом із її слідом — потрібен і стрілкам, і спільному
// переміщенню, і вирівнюванню.
void EdMoveObj(EdObj& o, int dx, int dy)
{
    o.x += dx;
    o.y += dy;
    for (size_t i = 0; i < o.pts.size(); ++i) { o.pts[i].x += dx; o.pts[i].y += dy; }
}

// Вирівнювання: 0 ліворуч, 1 по центру вертикалі, 2 праворуч,
// 3 верх, 4 по центру горизонталі, 5 низ, 6 розподіл по X, 7 розподіл по Y.
void EdAlignSel(int what)
{
    std::vector<int> all = EdSelAll();
    if ((int)all.size() < 2) return;
    RECT b;
    if (!EdSelBounds(&b)) return;
    EdPushUndo();
    if (what >= 6) {
        // Розподіл: крайні лишаються на місці, решта лягає рівними проміжками
        // між ними — інакше «розподілити» тягло б усю групу кудись убік.
        const bool byX = (what == 6);
        for (size_t i = 0; i + 1 < all.size(); ++i)
            for (size_t j = 0; j + 1 < all.size() - i; ++j) {
                const EdObj& a = g_edObjs[all[j]];
                const EdObj& c = g_edObjs[all[j + 1]];
                const int av = byX ? a.x + a.w / 2 : a.y + a.h / 2;
                const int cv = byX ? c.x + c.w / 2 : c.y + c.h / 2;
                if (av > cv) { const int t = all[j]; all[j] = all[j + 1]; all[j + 1] = t; }
            }
        const EdObj& f = g_edObjs[all.front()];
        const EdObj& l = g_edObjs[all.back()];
        const int from = byX ? f.x + f.w / 2 : f.y + f.h / 2;
        const int to   = byX ? l.x + l.w / 2 : l.y + l.h / 2;
        const int n = (int)all.size() - 1;
        for (int i = 1; i < n; ++i) {
            EdObj& o = g_edObjs[all[i]];
            const int want = from + (to - from) * i / n;
            const int have = byX ? o.x + o.w / 2 : o.y + o.h / 2;
            EdMoveObj(o, byX ? want - have : 0, byX ? 0 : want - have);
        }
    } else {
        for (size_t k = 0; k < all.size(); ++k) {
            EdObj& o = g_edObjs[all[k]];
            int dx = 0, dy = 0;
            switch (what) {
            case 0: dx = b.left - o.x; break;
            case 1: dx = (b.left + b.right) / 2 - (o.x + o.w / 2); break;
            case 2: dx = b.right - (o.x + o.w); break;
            case 3: dy = b.top - o.y; break;
            case 4: dy = (b.top + b.bottom) / 2 - (o.y + o.h / 2); break;
            default: dy = b.bottom - (o.y + o.h); break;
            }
            EdMoveObj(o, dx, dy);
        }
    }
    if (g_edWnd) InvalidateRect(g_edWnd, nullptr, FALSE);
}

void EdGroupSel(bool group)
{
    const std::vector<int> all = EdSelAll();
    if (all.size() < 2 && group) return;
    if (all.empty()) return;
    EdPushUndo();
    const int id = group ? g_edNextGrp++ : 0;
    for (size_t k = 0; k < all.size(); ++k) g_edObjs[all[k]].grp = id;
    if (g_edWnd) { EdLayout(g_edWnd); InvalidateRect(g_edWnd, nullptr, FALSE); }
}

// Зсув вибраної позначки на крок у пікселях ЗНІМКА, а не екрана: інакше та
// сама клавіша рухала б по-різному на різних масштабах.
void EdNudgeSel(int dx, int dy)
{
    if (g_edSel < 0 || g_edSel >= (int)g_edObjs.size()) return;
    if (!g_edNudging) { EdPushUndo(); g_edNudging = true; }
    const std::vector<int> all = EdSelAll();
    for (size_t k = 0; k < all.size(); ++k) EdMoveObj(g_edObjs[all[k]], dx, dy);
    if (g_edWnd) InvalidateRect(g_edWnd, nullptr, FALSE);
}

// Вкинуте чи вставлене зображення лягає позначкою: у ту точку, куди його
// відпустили, і зменшеним, якщо воно більше за сам знімок — інакше воно накрило
// б кадр цілком, і першою дією користувача було б «зменшити».
void EdPlaceImage(Gdiplus::Bitmap* bmp, POINT imgPt)
{
    if (!bmp) return;
    int w = (int)bmp->GetWidth(), h = (int)bmp->GetHeight();
    if (w < 1 || h < 1) { delete bmp; return; }
    const int maxW = EdViewW() * 4 / 5, maxH = EdViewH() * 4 / 5;
    if (maxW > 0 && maxH > 0 && (w > maxW || h > maxH)) {
        const double k = EdMinD((double)maxW / w, (double)maxH / h);
        w = (int)(w * k + 0.5);
        h = (int)(h * k + 0.5);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
    }
    const int id = EdAddImage(bmp);
    if (id < 0) return;
    EdObj o = EdObj{};
    o.kind  = EdKind::Image;
    o.img   = id;
    o.alpha = 100;
    o.color = g_edColor;
    o.w = w; o.h = h;
    o.x = imgPt.x - w / 2;
    o.y = imgPt.y - h / 2;
    EdPushUndo();
    g_edObjs.push_back(o);
    g_edSel = (int)g_edObjs.size() - 1;
    g_edTool = EdTool::Select;
    if (g_edWnd) { EdLayout(g_edWnd); InvalidateRect(g_edWnd, nullptr, FALSE); }
}

void EdDeleteSel()
{
    std::vector<int> all = EdSelAll();
    if (all.empty()) return;
    EdPushUndo();
    // ⚠ Видаляємо З КІНЦЯ: після кожного erase індекси за ним зсуваються, і
    // список, зібраний наперед, почав би вказувати не на тих.
    for (size_t i = 0; i + 1 < all.size(); ++i)
        for (size_t j = 0; j + 1 < all.size() - i; ++j)
            if (all[j] < all[j + 1]) { const int t = all[j]; all[j] = all[j + 1]; all[j + 1] = t; }
    for (size_t k = 0; k < all.size(); ++k)
        if (all[k] >= 0 && all[k] < (int)g_edObjs.size())
            g_edObjs.erase(g_edObjs.begin() + all[k]);
    EdSelClear();
    InvalidateRect(g_edWnd, nullptr, FALSE);
}

// Копія лягає зі зсувом: рівно поверх оригіналу вона виглядала б так, ніби
// нічого не сталося, і наступний клік вибрав би не те, що думає користувач.
void EdDuplicateSel()
{
    if (g_edSel < 0 || g_edSel >= (int)g_edObjs.size()) return;
    EdObj o = g_edObjs[g_edSel];
    const int d = 12;                    // пікселі ЗНІМКА, як і решта розмірів
    o.x += d;
    o.y += d;
    for (size_t i = 0; i < o.pts.size(); ++i) { o.pts[i].x += d; o.pts[i].y += d; }
    if (o.kind == EdKind::Counter) o.seq = ++g_edSeq;   // копія стає останньою в групі
    EdPushUndo();
    g_edObjs.push_back(o);
    g_edSel = (int)g_edObjs.size() - 1;
    if (g_edWnd) {
        EdLayout(g_edWnd);
        InvalidateRect(g_edWnd, nullptr, FALSE);
    }
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
    // Тогл «Редагування групи» перехоплює повзунок: інакше довелося б пояснювати,
    // чому колір і розмір ідуть усій групі, а прозорість — ні.
    if (g_edGroupEdit && EdCounterKind()) { EdGroupSet(2, p); g_edAlpha = p; }
    else if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size()) g_edObjs[g_edSel].alpha = p;
    else g_edAlpha = p;
    InvalidateRect(g_edWnd, nullptr, FALSE);
}

void EdSetStrengthAt(int mouseX)
{
    const RECT* sl = EdRegionRect(EdHit::Strength, 0);
    if (!sl) return;
    const int w = sl->right - sl->left;
    if (w <= 0) return;
    int p = 10 + (int)((mouseX - sl->left) * 90.0 / w + 0.5);
    if (p < 10)  p = 10;
    if (p > 100) p = 100;
    if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size() && g_edObjs[g_edSel].kind == EdKind::Hide)
        g_edObjs[g_edSel].strength = p;
    else
        g_edStrength = p;
    InvalidateRect(g_edWnd, nullptr, FALSE);
}

// Повзунок масштабу: позиція 0..100 по логарифмічній шкалі, а зсув рахуємо
// від центра полотна — тягнучи повзунок унизу, на нього ніхто не дивиться.
void EdSetZoomAt(int mouseX)
{
    const RECT* sl = EdRegionRect(EdHit::Zoom, 0);
    if (!sl) return;
    const int w = sl->right - sl->left;
    if (w <= 0) return;
    int pos = (int)((mouseX - sl->left) * 100.0 / w + 0.5);
    if (pos < 0) pos = 0;
    if (pos > 100) pos = 100;
    EdZoomSet(EdSliderToZoom(pos), EdCanvasCentre());
}

// Три повзунки тону — одна функція: діапазони різні, а поведінка однакова.
// Знімок для Ctrl+Z кладемо на ПЕРШІЙ реальній зміні, а не на натисканні:
// інакше клік, який нічого не зсунув, залишав би порожній крок скасування.
void EdSetToneAt(EdHit what, int mouseX)
{
    const RECT* sl = EdRegionRect(what, 0);
    if (!sl) return;
    const int w = sl->right - sl->left;
    if (w <= 0) return;
    int lo, hi;
    int* dst;
    if (what == EdHit::Exposure)   { lo = -20; hi =  20; dst = &g_edExposure; }
    else if (what == EdHit::Gamma) { lo =  50; hi = 200; dst = &g_edGamma; }
    else                           { lo = -50; hi =  50; dst = &g_edContrast; }
    int v = lo + (int)((mouseX - sl->left) * (double)(hi - lo) / w + 0.5);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    if (*dst == v) return;
    if (!g_edTonePushed) { EdPushUndo(); g_edTonePushed = true; }
    *dst = v;
    EdRebuildImage();
    InvalidateRect(g_edWnd, nullptr, FALSE);
}

bool EdHitObject(const EdObj& o, POINT pt)
{
    const RECT ir = EdImageRect();
    const double sc = EdScale();
    // ⚠ Повернуту позначку ловимо, повернувши НАЗАД саму точку: інакше клікати
    // довелося б по невидимій прямій рамці, а не по тому, що намальовано.
    if (o.rot != 0 && EdCanRotate(o.kind)) {
        const RECT r = EdObjScreen(o);
        double px = pt.x, py = pt.y;
        EdRotatePt((r.left + r.right) / 2.0, (r.top + r.bottom) / 2.0, -o.rot, px, py);
        pt.x = (LONG)(px + 0.5);
        pt.y = (LONG)(py + 0.5);
    }
    const double tol = EdPx(4) + o.thick * sc / 2.0;
    if (EdIsSegment(o.kind)) {
        const double ax = ir.left + o.x * sc, ay = ir.top + o.y * sc;
        const double bx = ir.left + (o.x + o.w) * sc, by = ir.top + (o.y + o.h) * sc;
        return EdDistToSeg(pt.x, pt.y, ax, ay, bx, by) <= tol;
    }
    if (o.kind == EdKind::Pen) {
        for (size_t i = 1; i < o.pts.size(); ++i) {
            const double ax = ir.left + o.pts[i - 1].x * sc, ay = ir.top + o.pts[i - 1].y * sc;
            const double bx = ir.left + o.pts[i].x * sc, by = ir.top + o.pts[i].y * sc;
            if (EdDistToSeg(pt.x, pt.y, ax, ay, bx, by) <= tol) return true;
        }
        return false;
    }
    RECT r = EdObjScreen(o);          // прямокутник і овал ловляться всією площею:
    const int t = EdPx(3);            // так у них легше влучити, ніж по контуру
    InflateRect(&r, t, t);
    return PtInRect(&r, pt) != 0;
}

int EdPick(POINT pt)
{
    for (int i = (int)g_edObjs.size() - 1; i >= 0; --i)
        if (EdHitObject(g_edObjs[i], pt)) return i;
    return -1;
}

void EdNormalize(EdObj& o)
{
    if (EdIsSegment(o.kind)) return;   // напрямок відрізка — це не «від'ємний розмір»
    if (o.w < 0) { o.x += o.w; o.w = -o.w; }
    if (o.h < 0) { o.y += o.h; o.h = -o.h; }
}

// Габарити сліду олівця: ними живуть виділення, ручки й перевірка попадання.
void EdPenBounds(EdObj& o)
{
    if (o.pts.empty()) return;
    long lo_x = o.pts[0].x, hi_x = o.pts[0].x, lo_y = o.pts[0].y, hi_y = o.pts[0].y;
    for (size_t i = 1; i < o.pts.size(); ++i) {
        if (o.pts[i].x < lo_x) lo_x = o.pts[i].x;
        if (o.pts[i].x > hi_x) hi_x = o.pts[i].x;
        if (o.pts[i].y < lo_y) lo_y = o.pts[i].y;
        if (o.pts[i].y > hi_y) hi_y = o.pts[i].y;
    }
    o.x = (int)lo_x; o.y = (int)lo_y;
    o.w = (int)(hi_x - lo_x); o.h = (int)(hi_y - lo_y);
}

// Відстань від точки до відрізка. Для лінії й стрілки саме вона вирішує, чи
// користувач у них влучив: рамка навколо діагоналі ловила б півекрана.
double EdDistToSeg(double px, double py, double ax, double ay, double bx, double by)
{
    const double dx = bx - ax, dy = by - ay;
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 0.0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    const double qx = ax + t * dx - px, qy = ay + t * dy - py;
    return sqrt(qx * qx + qy * qy);
}

// Shift: квадрат і коло за меншою стороною, лінія й стрілка — по найближчій
// з восьми осей через 45 градусів.
void EdConstrain(EdObj& o)
{
    if (EdIsSegment(o.kind)) {
        const double len = sqrt((double)o.w * o.w + (double)o.h * o.h);
        if (len < 1.0) return;
        const double step = 3.14159265358979 / 4.0;
        const double a = atan2((double)o.h, (double)o.w);
        const double snapped = floor(a / step + 0.5) * step;
        o.w = (int)(cos(snapped) * len + (cos(snapped) >= 0 ? 0.5 : -0.5));
        o.h = (int)(sin(snapped) * len + (sin(snapped) >= 0 ? 0.5 : -0.5));
        return;
    }
    const int aw = o.w < 0 ? -o.w : o.w, ah = o.h < 0 ? -o.h : o.h;
    const int side = aw < ah ? aw : ah;
    o.w = o.w < 0 ? -side : side;
    o.h = o.h < 0 ? -side : side;
}

// Shift у зміні розміру: нову рамку підганяємо під співвідношення сторін
// ОРИГІНАЛУ. Нерухомим лишається той самий бік, що й без Shift; вісь, за яку
// не тягнули, росте від свого центра — інакше фігура при кожному русі
// стрибала б униз-праворуч.
void EdKeepAspect(int handle, int ow, int oh, int& l, int& t, int& r, int& b)
{
    if (ow < 1 || oh < 1) return;
    const bool left  = (handle == 0 || handle == 6 || handle == 7);
    const bool right = (handle >= 2 && handle <= 4);
    const bool top   = (handle <= 2);
    const bool bot   = (handle >= 4 && handle <= 6);
    const double ar = (double)ow / (double)oh;
    double nw = (double)(r - l), nh = (double)(b - t);
    const double relW = fabs(nw) / ow, relH = fabs(nh) / oh;
    if ((left || right) && (top || bot)) {
        // Кут: провідною стає та вісь, яка змінилась сильніше.
        if (relW >= relH) nh = (nh < 0 ? -1.0 : 1.0) * fabs(nw) / ar;
        else              nw = (nw < 0 ? -1.0 : 1.0) * fabs(nh) * ar;
    } else if (left || right) {
        nh = (nh < 0 ? -1.0 : 1.0) * fabs(nw) / ar;
    } else if (top || bot) {
        nw = (nw < 0 ? -1.0 : 1.0) * fabs(nh) * ar;
    } else {
        return;
    }
    if (left)       l = r - (int)(nw + (nw < 0 ? -0.5 : 0.5));
    else if (right) r = l + (int)(nw + (nw < 0 ? -0.5 : 0.5));
    else {                                  // ширину не тягли — ростимо від центра
        const double cx = (l + r) / 2.0;
        l = (int)(cx - nw / 2.0 + 0.5);
        r = (int)(cx + nw / 2.0 + 0.5);
    }
    if (top)      t = b - (int)(nh + (nh < 0 ? -0.5 : 0.5));
    else if (bot) b = t + (int)(nh + (nh < 0 ? -0.5 : 0.5));
    else {
        const double cy = (t + b) / 2.0;
        t = (int)(cy - nh / 2.0 + 0.5);
        b = (int)(cy + nh / 2.0 + 0.5);
    }
}

void EdResizeSel(int handle, POINT img, bool keep)
{
    EdObj o = g_edDragOrig;

    // Маркер: ручка тягне край смуги, висота лишається сталою.
    if (o.kind == EdKind::Mark) {
        EdObj n = o;
        const int right = o.x + o.w;
        if (handle == 0) {
            int nx = img.x;
            if (nx > right - 6) nx = right - 6;
            n.x = nx;
            n.w = right - nx;
        } else {
            int nw = img.x - o.x;
            if (nw < 6) nw = 6;
            n.w = nw;
        }
        g_edObjs[g_edSel] = n;
        return;
    }

    // Напис: ручка тягне край БЛОКА. Мінімум — приблизно одна літера, інакше
    // блок можна було б зім'яти в нуль і більше ніколи не знайти ручку.
    if (o.kind == EdKind::Text) {
        const int minw = o.size > 8 ? o.size : 8;
        EdObj n = o;
        const int right = o.x + o.w;
        if (handle == 0) {
            int nx = img.x;
            if (nx > right - minw) nx = right - minw;
            n.x = nx;
            n.boxw = right - nx;
        } else {
            int nw = img.x - o.x;
            if (nw < minw) nw = minw;
            n.boxw = nw;
        }
        EdTextMeasure(n);
        g_edObjs[g_edSel] = n;
        return;
    }

    // Відрізок: ручка — це кінець, і тягнеться саме він.
    if (EdIsSegment(o.kind)) {
        EdObj n = o;
        if (handle == 0) { n.x = img.x; n.y = img.y; n.w = o.x + o.w - img.x; n.h = o.y + o.h - img.y; }
        else             { n.w = img.x - o.x; n.h = img.y - o.y; }
        // ⚠ «Пропорції» відрізка — це його КУТ: тримаємо ті самі вісім осей
        // через 45°, що й при малюванні. Нерухомим лишається протилежний кінець,
        // тож для ручки 0 вирівнюємо вектор і переставляємо початок назад.
        if (keep) {
            if (handle == 0) {
                const int ex = n.x + n.w, ey = n.y + n.h;
                EdConstrain(n);
                n.x = ex - n.w;
                n.y = ey - n.h;
            } else {
                EdConstrain(n);
            }
        }
        g_edObjs[g_edSel] = n;
        return;
    }

    // Олівець: тягнемо габарити, а сам слід масштабується разом із ними.
    if (o.kind == EdKind::Pen) {
        int l0 = o.x, t0 = o.y, r0 = o.x + o.w, b0 = o.y + o.h;
        switch (handle) {
        case 0: l0 = img.x; t0 = img.y; break;
        case 1: t0 = img.y; break;
        case 2: r0 = img.x; t0 = img.y; break;
        case 3: r0 = img.x; break;
        case 4: r0 = img.x; b0 = img.y; break;
        case 5: b0 = img.y; break;
        case 6: l0 = img.x; b0 = img.y; break;
        case 7: l0 = img.x; break;
        default: break;
        }
        if (keep) EdKeepAspect(handle, o.w, o.h, l0, t0, r0, b0);
        if (r0 - l0 < 2) r0 = l0 + 2;
        if (b0 - t0 < 2) b0 = t0 + 2;
        EdObj n = o;
        const double kx = o.w > 0 ? (double)(r0 - l0) / o.w : 1.0;
        const double ky = o.h > 0 ? (double)(b0 - t0) / o.h : 1.0;
        for (size_t i = 0; i < n.pts.size(); ++i) {
            n.pts[i].x = (LONG)(l0 + (o.pts[i].x - o.x) * kx + 0.5);
            n.pts[i].y = (LONG)(t0 + (o.pts[i].y - o.y) * ky + 0.5);
        }
        EdPenBounds(n);
        g_edObjs[g_edSel] = n;
        return;
    }

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
    if (keep) EdKeepAspect(handle, o.w, o.h, l, tp, r, bt);
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

    // ⚠ Спершу WIC, і лише потім GDI+. Фільтр діалогу обіцяє WebP, а GDI+ його
    // не вміє — користувач вибирав зі СВОГО ж списку й отримував «не вдалося
    // відкрити». WIC той самий, що в перегляді (CAPS-16), і віддає рівно той
    // 32bppPARGB, який нам потрібен; заразом підхоплює HEIC і AVIF, якщо в
    // системі стоять кодеки. GDI+ лишається запасним шляхом.
    Gdiplus::Bitmap* out = ImageDecodeWic(st);
    if (!out) {
        LARGE_INTEGER zero = {};
        st->Seek(zero, STREAM_SEEK_SET, nullptr);
        Gdiplus::Bitmap* src = Gdiplus::Bitmap::FromStream(st);
        if (src && src->GetLastStatus() == Gdiplus::Ok && src->GetWidth() && src->GetHeight()) {
            // Копія у власну пам'ять: далі потік можна відпустити.
            out = src->Clone(0, 0, (INT)src->GetWidth(), (INT)src->GetHeight(), PixelFormat32bppPARGB);
            if (out && out->GetLastStatus() != Gdiplus::Ok) { delete out; out = nullptr; }
        }
        delete src;
    }
    st->Release();
    return out;
}

// GUID-и своєю копією — як і решта COM у цьому файлі: MinGW тримає їх в uuid.lib,
// MSVC в іншій, і сходяться вони лише так.
const GUID kCLSID_FileOpenDialog = { 0xdc1c5a9c, 0xe88a, 0x4dde, { 0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7 } };
const GUID kIID_IFileOpenDialog  = { 0xd57c7288, 0xd4ad, 0x4768, { 0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60 } };
const GUID kCLSID_FileSaveDialog = { 0xc0b4e2f3, 0xba21, 0x4773, { 0x8d, 0xba, 0x33, 0x5e, 0xc9, 0x46, 0xeb, 0x8b } };
const GUID kIID_IFileSaveDialog  = { 0x84bccd23, 0x5fde, 0x4cdb, { 0xae, 0xa4, 0xaf, 0x64, 0xb8, 0x3d, 0x78, 0xab } };
const GUID kIID_IShellItem       = { 0x43826d1e, 0xe718, 0x42ee, { 0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe } };

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

// Захоплення екрана описане нижче за редактор, а кнопка «Відкрити» потрібна
// саме тут. Оголошуємо наперед; типовий аргумент sdrWhite лишається за
// визначенням, тому виклики звідси передають усі шість.
enum class CapMode { Screen, Window, Region, Clipboard };
void CapTake(HINSTANCE hInst, HWND owner, CapMode mode, HWND target);
bool EdPickFile(HWND owner, wchar_t* out, size_t cch);
void EdOpenBitmap(HINSTANCE hInst, Gdiplus::Bitmap* bmp, const wchar_t* label,
                  bool hdr, bool toneMapped, float sdrWhite);

// Курсор над полотном. Винесено в окрему функцію не заради краси: інакше його
// не перевірити — він живе тільки поки миша справді над вікном, а харнес рухає
// мишу повідомленнями. Тепер тест питає ту саму функцію, що й малювання.
LPCWSTR EdCursorFor(POINT pt)
{
    static const LPCWSTR kSide[8] = { IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE,
                                      IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE };
    if (g_edCropping) {
        RECT hs[8];
        const int hn = EdCropHandles(hs);
        for (int i = 0; i < hn; ++i)
            if (PtInRect(&hs[i], pt)) return kSide[i];
        const RECT cr = EdCropScreen(g_edCropEdit);
        return PtInRect(&cr, pt) ? IDC_SIZEALL : IDC_ARROW;
    }
    if (EdManySel()) {
        RECT rh;
        if (EdManyRotHandle(&rh) && PtInRect(&rh, pt)) return IDC_HAND;
        RECT hs[8];
        const int hn = EdManyHandles(hs);
        for (int i = 0; i < hn; ++i)
            if (PtInRect(&hs[i], pt)) return kSide[i % 8];
    } else if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size()) {
        const EdObj& o = g_edObjs[g_edSel];
        RECT rh;
        if (EdRotHandle(o, &rh) && PtInRect(&rh, pt)) return IDC_HAND;
        RECT hs[8];
        const int hn = EdHandles(o, hs);
        // ⚠ У відрізка ручка — це КІНЕЦЬ лінії, і тягнеться він куди завгодно, а
        // не по діагоналі. Діагональна стрілка з набору для прямокутника тут
        // нічого не пояснює (зауваження власника по 3.9.0).
        const bool seg  = EdIsSegment(o.kind);
        const bool wide = (o.kind == EdKind::Text);
        for (int i = 0; i < hn; ++i)
            if (PtInRect(&hs[i], pt))
                return seg ? IDC_SIZEALL : (wide ? IDC_SIZEWE : kSide[i % 8]);
    }
    return (g_edTool != EdTool::Select) ? IDC_CROSS : IDC_ARROW;
}

// Тіла нижче — біля решти дій над знімком; діалогу вони потрібні вже тут.
bool EdResizeImage(int nw, int nh, bool scaleText, bool sharp);
bool EdResizeCanvas(int nw, int nh);

// ---- CAPS-36: системне меню поширення -----------------------------------
//
// Ми НЕ хостимо чужого коду (запобіжник 20.09): дані віддаються системному
// брокеру, а весь інтерфейс малює сама Windows. Межа проходить саме тут.
//
// ⚠ Процес запускається requireAdministrator, і меню поширення в елевейтованому
// процесі історично не з'являється. Тому доступність перевіряється на льоту, і
// кнопки просто немає там, де вона не працює, — замість кнопки, яка мовчить.

namespace EdShareNs {

using namespace ABI::Windows::ApplicationModel::DataTransfer;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Storage;
using namespace ABI::Windows::Storage::Streams;

std::wstring g_tempFile;     // останній тимчасовий PNG — прибираємо за собою
// Скільки разів система СПРАВДІ спитала в нас дані. Нуль після показу меню —
// це діагноз, а не дрібниця: пакет був би порожній, хоч би що ми в нього клали.
int g_asked = 0;

void CleanTemp()
{
    if (g_tempFile.empty()) return;
    DeleteFileW(g_tempFile.c_str());
    g_tempFile.clear();
}

// ⚠ Заголовки MinGW оголошують IStorageFileStatics лише ВПЕРЕД — тіла в них
// немає. Описуємо рівно перший метод: у таблиці він стоїть одразу після
// IInspectable, тож зсув правильний, а решти ми не викликаємо.
// ⚠ Ідентифікатор інтерфейсу здобуто НА ЖИВІЙ СИСТЕМІ (IActivationFactory →
// GetIids), а не з памʼяті: перша ж спроба «згадати» його дала E_NOINTERFACE.
// ⚠ Усі три наші обʼєкти віддаються БРОКЕРУ, тобто в інший процес. Системні
// колекції WinRT (те, що в C++/WinRT робить single_threaded_vector) агільні —
// наші мусять бути теж, інакше кожен виклик із чужої квартири йде через
// маршалінг параметризованого інтерфейсу й має всі шанси не дійти.
// Ідентифікатор беремо константою: __uuidof(IAgileObject) у MinGW дає
// невизначений символ на етапі компонування.
const GUID kIID_IAgileObject =
    { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };

const GUID kIID_StorageFileStatics =
    { 0x5984c710, 0xdaf2, 0x43c8, { 0x8b, 0xb4, 0xa4, 0xd3, 0xea, 0xcf, 0xd0, 0x3f } };

struct IStorageFileStaticsMin : public IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE GetFileFromPathAsync(
        HSTRING path, __FIAsyncOperation_1_Windows__CStorage__CStorageFile** op) = 0;
};

typedef ABI::Windows::Foundation::Collections::IIterable<IStorageItem*> ItemIterableBase;
typedef ABI::Windows::Foundation::Collections::IIterator<IStorageItem*> ItemIteratorBase;

// Список із ОДНОГО файлу. Готового вектора в ABI-шарі WinRT немає — у C++/WinRT
// його дає single_threaded_vector, якого тут нема, — тож пишемо власний
// перелічувач. Він короткий саме тому, що елемент завжди один.
struct ItemIterator : ItemIteratorBase
{
    LONG rc = 1;
    IStorageItem* item = nullptr;
    bool done = false;
    ~ItemIterator() { if (item) item->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IInspectable)) ||
            IsEqualIID(riid, kIID_IAgileObject) ||
            IsEqualIID(riid, __uuidof(ItemIteratorBase))) {
            *out = static_cast<ItemIteratorBase*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&rc); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG n = InterlockedDecrement(&rc);
        if (n == 0) delete this;
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* n, IID** p) override { *n = 0; *p = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* h) override { *h = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* t) override { *t = BaseTrust; return S_OK; }

    HRESULT STDMETHODCALLTYPE get_Current(IStorageItem** value) override
    {
        if (!value) return E_POINTER;
        if (done || !item) { *value = nullptr; return E_BOUNDS; }
        *value = item;
        item->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean* value) override
    {
        if (!value) return E_POINTER;
        *value = (!done && item) ? 1 : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MoveNext(boolean* value) override
    {
        done = true;                         // елемент один: після нього кінець
        if (value) *value = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMany(UINT32 cap, IStorageItem** items, UINT32* got) override
    {
        if (!got) return E_POINTER;
        *got = 0;
        if (done || !item || cap == 0) return S_OK;
        items[0] = item;
        item->AddRef();
        *got = 1;
        done = true;
        return S_OK;
    }
};

struct ItemList : ItemIterableBase
{
    LONG rc = 1;
    IStorageItem* item = nullptr;
    ~ItemList() { if (item) item->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IInspectable)) ||
            IsEqualIID(riid, kIID_IAgileObject) ||
            IsEqualIID(riid, __uuidof(ItemIterableBase))) {
            *out = static_cast<ItemIterableBase*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&rc); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG n = InterlockedDecrement(&rc);
        if (n == 0) delete this;
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* n, IID** p) override { *n = 0; *p = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* h) override { *h = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* t) override { *t = BaseTrust; return S_OK; }

    HRESULT STDMETHODCALLTYPE First(ItemIteratorBase** value) override
    {
        if (!value) return E_POINTER;
        ItemIterator* it = new ItemIterator();
        it->item = item;
        if (item) item->AddRef();
        *value = it;
        return S_OK;
    }
};

// StorageFile за шляхом. Операція асинхронна, але чекаємо ми її ЗАЗДАЛЕГІДЬ —
// до показу меню, а не в обробнику запиту: там доведення (deferral) коштувало б
// ще одного COM-класу заради того самого файлу.
IStorageFile* OpenStorageFile(const std::wstring& path)
{
    IStorageFileStaticsMin* sf = nullptr;
    HSTRING_HEADER ch; HSTRING cls = nullptr;
    const wchar_t* n = RuntimeClass_Windows_Storage_StorageFile;
    if (FAILED(WindowsCreateStringReference(n, (UINT32)wcslen(n), &ch, &cls))) return nullptr;
    if (FAILED(RoGetActivationFactory(cls, kIID_StorageFileStatics, (void**)&sf)) || !sf)
        return nullptr;

    HSTRING_HEADER ph; HSTRING ps = nullptr;
    __FIAsyncOperation_1_Windows__CStorage__CStorageFile* op = nullptr;
    IStorageFile* file = nullptr;
    if (SUCCEEDED(WindowsCreateStringReference(path.c_str(), (UINT32)path.size(), &ph, &ps)) &&
        SUCCEEDED(sf->GetFileFromPathAsync(ps, &op)) && op) {
        IAsyncInfo* info = nullptr;
        if (SUCCEEDED(op->QueryInterface(__uuidof(IAsyncInfo), (void**)&info)) && info) {
            AsyncStatus st = AsyncStatus::Started;
            // Опитуємо, а не вішаємо обробник завершення: операція живе на
            // пулі потоків і до нашого циклу повідомлень діла не має. Стеля в
            // три секунди — щоб редактор не завис, якщо файлова система стала.
            for (int i = 0; i < 600; ++i) {
                if (FAILED(info->get_Status(&st)) || st != AsyncStatus::Started) break;
                Sleep(5);
            }
            if (st == AsyncStatus::Completed) op->GetResults(&file);
            info->Release();
        }
        op->Release();
    }
    sf->Release();
    return file;
}

// Посилання на потік для SetBitmap. Перший шлях — із самого StorageFile;
// запасний — синхронний потік ShCore просто за шляхом.
IRandomAccessStreamReference* StreamRef(IStorageFile* file, const std::wstring& path)
{
    IRandomAccessStreamReferenceStatics* st = nullptr;
    HSTRING_HEADER sh; HSTRING scls = nullptr;
    const wchar_t* n = RuntimeClass_Windows_Storage_Streams_RandomAccessStreamReference;
    if (FAILED(WindowsCreateStringReference(n, (UINT32)wcslen(n), &sh, &scls))) return nullptr;
    if (FAILED(RoGetActivationFactory(scls, __uuidof(IRandomAccessStreamReferenceStatics),
                                      (void**)&st)) || !st) return nullptr;
    IRandomAccessStreamReference* ref = nullptr;
    if (file) st->CreateFromFile(file, &ref);
    if (!ref) {
        IRandomAccessStream* ras = nullptr;
        if (SUCCEEDED(CreateRandomAccessStreamOnFile(path.c_str(), STGM_READ,
                                                     __uuidof(IRandomAccessStream), (void**)&ras)) && ras) {
            st->CreateFromStream(ras, &ref);
            ras->Release();
        }
    }
    st->Release();
    return ref;
}

// ⚠ Псевдонім потрібен не для краси: __uuidof у MinGW — МАКРОС, і кома між
// параметрами шаблону всередині нього розбирається як кома аргументів макроса.
typedef ITypedEventHandler<DataTransferManager*, DataRequestedEventArgs*> ShareHandlerBase;

// Обробник запиту даних. Живе рівно стільки, скільки система його тримає, і
// НІЧОГО не добуває сам: усе готове ще до показу меню, тож Invoke синхронний.
struct Handler : public ShareHandlerBase
{
    LONG rc = 1;
    std::wstring title;
    IStorageFile* file = nullptr;
    IRandomAccessStreamReference* ref = nullptr;

    ~Handler()
    {
        if (file) file->Release();
        if (ref) ref->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kIID_IAgileObject) ||
            IsEqualIID(riid, __uuidof(ShareHandlerBase))) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&rc); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG n = InterlockedDecrement(&rc);
        if (n == 0) delete this;
        return (ULONG)n;
    }

    HRESULT STDMETHODCALLTYPE Invoke(IDataTransferManager*, IDataRequestedEventArgs* args) override
    {
        ++g_asked;
        if (!args) return S_OK;
        IDataRequest* req = nullptr;
        if (FAILED(args->get_Request(&req)) || !req) return S_OK;
        IDataPackage* pkg = nullptr;
        if (SUCCEEDED(req->get_Data(&pkg)) && pkg) {
            IDataPackagePropertySet* props = nullptr;
            if (SUCCEEDED(pkg->get_Properties(&props)) && props) {
                HSTRING_HEADER th; HSTRING ts = nullptr;
                if (SUCCEEDED(WindowsCreateStringReference(title.c_str(), (UINT32)title.size(), &th, &ts)))
                    props->put_Title(ts);
                props->Release();
            }
            // ⚠ Кладемо ОБИДВА формати. Одні цілі (месенджери, пошта) беруть
            // зображення, інші (редактори, Провідник) — файл; той, хто вміє
            // лише щось одне, мовчки отримував порожнечу.
            if (file) {
                IStorageItem* item = nullptr;
                if (SUCCEEDED(file->QueryInterface(__uuidof(IStorageItem), (void**)&item)) && item) {
                    ItemList* list = new ItemList();
                    list->item = item;          // посилання переходить до списку
                    pkg->SetStorageItems(list, true);
                    list->Release();
                }
            }
            if (ref) pkg->SetBitmap(ref);
            pkg->Release();
        }
        req->Release();
        return S_OK;
    }
};

IDataTransferManagerInterop* Interop()
{
    IDataTransferManagerInterop* it = nullptr;
    HSTRING_HEADER hh; HSTRING cls = nullptr;
    const wchar_t* name = RuntimeClass_Windows_ApplicationModel_DataTransfer_DataTransferManager;
    if (FAILED(WindowsCreateStringReference(name, (UINT32)wcslen(name), &hh, &cls))) return nullptr;
    if (FAILED(RoGetActivationFactory(cls, IID_IDataTransferManagerInterop, (void**)&it))) return nullptr;
    return it;
}

// ⚠ Менеджер у вікна ОДИН, і кожне поширення додавало б іще один обробник.
// Тримаємо реєстрацію рівно одну: попередню знімаємо перед новою.
IDataTransferManager* g_dtm = nullptr;
EventRegistrationToken g_tok = {};

void Unhook()
{
    if (!g_dtm) return;
    if (g_tok.value) g_dtm->remove_DataRequested(g_tok);
    g_dtm->Release();
    g_dtm = nullptr;
    g_tok.value = 0;
}

}  // namespace EdShareNs

// Чи має сенс показувати кнопку. Питаємо систему ОДИН раз: під адміністратором
// брокер поширення може бути недоступний, і тоді кнопка лише збивала б з пантелику.
bool EdShareAvailable()
{
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    cached = 0;
    if (IDataTransferManagerInterop* it = EdShareNs::Interop()) {
        cached = 1;
        it->Release();
    }
    return cached != 0;
}

// ---- CAPS-44: діалог розміру -------------------------------------------
//
// Власне вікно з дочірніми контролами, а не DLGTEMPLATE: шаблон довелося б
// збирати в пам'яті побайтово, а виграшу — нуль. Модальність робимо самі:
// вимикаємо власника й крутимо власний цикл повідомлень.

HWND g_edSzWnd = nullptr;
HWND g_edSzW = nullptr, g_edSzH = nullptr;
// Відсоток свій у кожної осі (вимога власника): при вимкнених пропорціях
// «70 % завширшки й 100 % заввишки» інакше не набрати взагалі.
HWND g_edSzPctW = nullptr, g_edSzPctH = nullptr;
HWND g_edSzKeep = nullptr, g_edSzText = nullptr, g_edSzSharp = nullptr;
bool g_edSzCanvas = false;      // полотно чи зображення
// Вимкнув один раз — лишається вимкненим (зауваження власника). Стан живе,
// поки живе програма: у реєстр його не пишемо, щоб не плодити ключів заради
// однієї галочки.
bool g_edSzKeepAspect = true;
bool g_edSzGuard = false;       // щоб перерахунок полів не ганявся сам за собою
int  g_edSzOrigW = 0, g_edSzOrigH = 0;
bool g_edSzOk = false;
int  g_edSzLastW = 0, g_edSzLastH = 0;
bool g_edSzLastText = false, g_edSzLastSharp = false;

int EdSzRead(HWND e)
{
    wchar_t buf[32] = {};
    GetWindowTextW(e, buf, 32);
    return _wtoi(buf);
}

void EdSzWrite(HWND e, int v)
{
    wchar_t buf[32];
    wsprintfW(buf, L"%d", v);
    SetWindowTextW(e, buf);
}

// Чотири поля тримають ДВА числа: ширину й висоту. Відсотки — лише інший спосіб
// їх набрати, тож перерахунок іде в один бік — «що змінили → пікселі → решта
// полів», а не кожне поле в кожне.
void EdSzSync(HWND from)
{
    if (g_edSzGuard) return;
    g_edSzGuard = true;
    const bool keep = (SendMessageW(g_edSzKeep, BM_GETCHECK, 0, 0) == BST_CHECKED);
    int w = EdSzRead(g_edSzW), h = EdSzRead(g_edSzH);

    if (from == g_edSzPctW && g_edSzOrigW > 0) {
        int p = EdSzRead(g_edSzPctW);
        if (p < 1) p = 1;
        if (p > 1000) p = 1000;
        w = (int)((double)g_edSzOrigW * p / 100.0 + 0.5);
        if (keep && g_edSzOrigH > 0) h = (int)((double)g_edSzOrigH * p / 100.0 + 0.5);
    } else if (from == g_edSzPctH && g_edSzOrigH > 0) {
        int p = EdSzRead(g_edSzPctH);
        if (p < 1) p = 1;
        if (p > 1000) p = 1000;
        h = (int)((double)g_edSzOrigH * p / 100.0 + 0.5);
        if (keep && g_edSzOrigW > 0) w = (int)((double)g_edSzOrigW * p / 100.0 + 0.5);
    } else if (from == g_edSzW) {
        if (keep && w > 0 && g_edSzOrigW > 0 && g_edSzOrigH > 0)
            h = (int)((double)w * g_edSzOrigH / g_edSzOrigW + 0.5);
    } else if (from == g_edSzH) {
        if (keep && h > 0 && g_edSzOrigW > 0 && g_edSzOrigH > 0)
            w = (int)((double)h * g_edSzOrigW / g_edSzOrigH + 0.5);
    }

    // Назад у поля пишемо лише ті, яких користувач зараз не набирає: інакше
    // курсор стрибав би на початок після кожної цифри.
    if (from != g_edSzW) EdSzWrite(g_edSzW, w);
    if (from != g_edSzH) EdSzWrite(g_edSzH, h);
    if (from != g_edSzPctW && g_edSzOrigW > 0)
        EdSzWrite(g_edSzPctW, (int)((double)w * 100.0 / g_edSzOrigW + 0.5));
    if (from != g_edSzPctH && g_edSzOrigH > 0)
        EdSzWrite(g_edSzPctH, (int)((double)h * 100.0 / g_edSzOrigH + 0.5));
    g_edSzGuard = false;
}

LRESULT CALLBACK EdSzProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    // ⚠ Своє вікно, а не DLGTEMPLATE, тож WM_CTLCOLORDLG сюди не приходить:
    // тло стираємо самі. Заразом це лікує й другу половину зауваження —
    // статичний текст мав СВОЄ тло, відмінне від тла вікна.
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH b = CreateSolidBrush(g_edDark ? kDkBg : GetSysColor(COLOR_WINDOW));
        FillRect((HDC)wp, &rc, b);
        DeleteObject(b);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        // Прозоре тло під підписом: він лягає рівно на тло вікна, яким би воно
        // не було. Пензель повертаємо статичний — його не можна видаляти.
        static HBRUSH bgDark = nullptr, bgLight = nullptr;
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, g_edDark ? kDkText : GetSysColor(COLOR_WINDOWTEXT));
        if (g_edDark) {
            if (!bgDark) bgDark = CreateSolidBrush(kDkBg);
            return (LRESULT)bgDark;
        }
        if (!bgLight) bgLight = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
        return (LRESULT)bgLight;
    }
    case WM_CTLCOLOREDIT: {
        static HBRUSH edDark = nullptr;
        if (!g_edDark) break;
        SetTextColor((HDC)wp, kDkText);
        SetBkColor((HDC)wp, kDkEdit);
        if (!edDark) edDark = CreateSolidBrush(kDkEdit);
        return (LRESULT)edDark;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE) { EdSzSync((HWND)lp); return 0; }
        if (LOWORD(wp) == 1) {              // Змінити
            // ⚠ Значення знімаємо ДО знищення вікна: після DestroyWindow
            // контролів уже немає, а GetWindowText мовчки віддасть порожнє.
            // ⚠ Питаємо контроли ЧЕРЕЗ GetDlgItem, а не через глобальні
            // вказівники: глобальні пережили б минулий діалог, і «Змінити»
            // прочитало б поля, яких уже немає.
            g_edSzLastW = EdSzRead(GetDlgItem(hwnd, 10));
            g_edSzLastH = EdSzRead(GetDlgItem(hwnd, 11));
            g_edSzLastText = g_edSzText && SendMessageW(g_edSzText, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_edSzLastSharp = g_edSzSharp && SendMessageW(g_edSzSharp, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_edSzOk = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == 13) {          // «Тримати пропорції» — запам'ятовуємо
            g_edSzKeepAspect = (SendMessageW(g_edSzKeep, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        }
        if (LOWORD(wp) == 2) { DestroyWindow(hwnd); return 0; }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_edSzWnd = nullptr;
        // ⚠ Будимо власний цикл: він стоїть у GetMessage, і без цього штурхана
        // вийде з нього лише з наступним випадковим повідомленням — тобто
        // натиснуте «Змінити» спрацювало б із затримкою невідомої довжини.
        PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND EdSzMake(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
              int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), nullptr);
    if (c) {
        SendMessageW(c, WM_SETFONT, (WPARAM)g_edFont, TRUE);
        // Рамку поля й галочку малює системна тема — своїми руками ми
        // домалювали б лише другу, гіршу копію.
        if (g_edDark) SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
    }
    return c;
}

// Показує діалог і застосовує зміну. Повертає true, якщо розмір змінено.
bool EdSizeDialog(HWND owner, bool canvas)
{
    if (!g_edImg || g_edSzWnd) return false;
    g_edSzCanvas = canvas;
    g_edSzOrigW = g_edImgW;
    g_edSzOrigH = g_edImgH;
    g_edSzOk = false;

    static bool reg = false;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
    if (!reg) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = EdSzProc;
        wc.hInstance     = inst;
        wc.lpszClassName = L"lilhelpers_size";
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;       // стираємо самі, див. WM_ERASEBKGND
        RegisterClassW(&wc);
        reg = true;
    }

    const int pad = EdPx(14), lh = EdPx(24), gap = EdPx(10);
    // ⚠ Висота залежить від того, на скільки рядків розлізеться примітка: у
    // діалозі зображення вона довша й накривала кнопки.
    const int w = EdPx(canvas ? 360 : 380), h = EdPx(canvas ? 276 : 348);
    RECT orc;
    GetWindowRect(owner, &orc);
    const int px = orc.left + ((orc.right - orc.left) - w) / 2;
    const int py = orc.top + ((orc.bottom - orc.top) - h) / 3;
    g_edSzWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"lilhelpers_size",
                                S(canvas ? Str::EdSizeCanTitle : Str::EdSizeImgTitle),
                                WS_POPUPWINDOW | WS_CAPTION, px, py, w, h,
                                owner, nullptr, inst, nullptr);
    if (!g_edSzWnd) return false;
    // Підпис вікна теж темний — інакше світла смуга над темним вікном.
    const BOOL darkBar = g_edDark ? TRUE : FALSE;
    DwmSetWindowAttribute(g_edSzWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
                          &darkBar, sizeof(darkBar));

    wchar_t now[64];
    wsprintfW(now, S(Str::EdFmtSizeNow), g_edSzOrigW, g_edSzOrigH);
    int y = pad;
    EdSzMake(g_edSzWnd, L"STATIC", now, 0, pad, y, w - pad * 2, lh, 0);
    y += lh + gap;
    EdSzMake(g_edSzWnd, L"STATIC", S(Str::EdSizeW), 0, pad, y + EdPx(3), EdPx(70), lh, 0);
    g_edSzW = EdSzMake(g_edSzWnd, L"EDIT", L"", WS_BORDER | ES_NUMBER,
                       pad + EdPx(78), y, EdPx(80), lh, 10);
    EdSzMake(g_edSzWnd, L"STATIC", S(Str::EdSizeH), 0, pad + EdPx(176), y + EdPx(3), EdPx(70), lh, 0);
    g_edSzH = EdSzMake(g_edSzWnd, L"EDIT", L"", WS_BORDER | ES_NUMBER,
                       pad + EdPx(246), y, EdPx(80), lh, 11);
    y += lh + gap;
    EdSzMake(g_edSzWnd, L"STATIC", S(Str::EdSizePct), 0, pad, y + EdPx(3), EdPx(70), lh, 0);
    g_edSzPctW = EdSzMake(g_edSzWnd, L"EDIT", L"", WS_BORDER | ES_NUMBER,
                          pad + EdPx(78), y, EdPx(80), lh, 12);
    EdSzMake(g_edSzWnd, L"STATIC", S(Str::EdSizePct), 0, pad + EdPx(176), y + EdPx(3), EdPx(70), lh, 0);
    g_edSzPctH = EdSzMake(g_edSzWnd, L"EDIT", L"", WS_BORDER | ES_NUMBER,
                          pad + EdPx(246), y, EdPx(80), lh, 16);
    y += lh + gap;
    g_edSzKeep = EdSzMake(g_edSzWnd, L"BUTTON", S(Str::EdSizeKeep), BS_AUTOCHECKBOX,
                          pad, y + EdPx(2), EdPx(220), lh, 13);
    SendMessageW(g_edSzKeep, BM_SETCHECK, g_edSzKeepAspect ? BST_CHECKED : BST_UNCHECKED, 0);
    y += lh + gap;

    if (!canvas) {
        g_edSzText = EdSzMake(g_edSzWnd, L"BUTTON", S(Str::EdSizeText), BS_AUTOCHECKBOX,
                              pad, y, w - pad * 2, lh, 14);
        y += lh;
        g_edSzSharp = EdSzMake(g_edSzWnd, L"BUTTON", S(Str::EdSizeSharp), BS_AUTOCHECKBOX,
                               pad, y, w - pad * 2, lh, 15);
        y += lh + EdPx(4);
    } else {
        g_edSzText = nullptr;
        g_edSzSharp = nullptr;
    }
    EdSzMake(g_edSzWnd, L"STATIC", S(canvas ? Str::EdSizeNoteCan : Str::EdSizeNoteImg), 0,
             pad, y, w - pad * 2, lh * (canvas ? 2 : 3), 0);

    // ⚠ Кнопки ставимо від КЛІЄНТСЬКОЇ висоти, а не від висоти вікна: підпис
    // і рамку система забирає собі, і на різних темах по-різному.
    RECT crc = {};
    GetClientRect(g_edSzWnd, &crc);
    const int bw = EdPx(110), bh = EdPx(30);
    const int by = crc.bottom - bh - EdPx(12);
    EdSzMake(g_edSzWnd, L"BUTTON", S(Str::EdSizeApply), BS_DEFPUSHBUTTON,
             crc.right - pad - bw * 2 - EdPx(8), by, bw, bh, 1);
    EdSzMake(g_edSzWnd, L"BUTTON", S(Str::EdSizeCancel), 0,
             crc.right - pad - bw, by, bw, bh, 2);

    g_edSzGuard = true;
    EdSzWrite(g_edSzW, g_edSzOrigW);
    EdSzWrite(g_edSzH, g_edSzOrigH);
    EdSzWrite(g_edSzPctW, 100);
    EdSzWrite(g_edSzPctH, 100);
    g_edSzGuard = false;

    EnableWindow(owner, FALSE);
    ShowWindow(g_edSzWnd, SW_SHOW);
    SetFocus(g_edSzW);

    MSG msg;
    while (g_edSzWnd && GetMessageW(&msg, nullptr, 0, 0)) {
        if (g_edSzWnd && IsDialogMessageW(g_edSzWnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);

    if (!g_edSzOk) return false;
    if (g_edSzLastW < 1 || g_edSzLastH < 1) return false;
    return canvas ? EdResizeCanvas(g_edSzLastW, g_edSzLastH)
                  : EdResizeImage(g_edSzLastW, g_edSzLastH, g_edSzLastText, g_edSzLastSharp);
}

// ---- CAPS-37: підказки над кнопками -------------------------------------
//
// Кнопки редактора — не вікна, а прямокутники зі списку, тож звичайний тултіп
// із TTM_ADDTOOL на кожну з них неможливий. Беремо ОДИН системний тултіп у
// режимі стеження (TTF_TRACK) і показуємо його самі, коли підсвічена ділянка
// не змінюється довше за паузу. Вигляд, шрифт і тінь лишаються системні —
// малювати своє вікно заради цього було б і більше коду, і гірший результат.

HWND g_edTip = nullptr;
// ⚠ Розмір структури — саме V1 (56 байтів), а не sizeof(TTTOOLINFOW) (72).
// З повним розміром TTM_ADDTOOL тут просто повертає 0, тултіп лишається без
// жодного інструмента й мовчки не показується — перевірено окремою пробою
// (tipprobe.cpp): V1 додається й показується, повний — ні. Полів поза V1
// (lParam, lpReserved) ми все одно не використовуємо.
const UINT kEdTipInfoSize = TTTOOLINFOW_V1_SIZE;   // макет, а не constexpr: макрос рахує зсув поля
constexpr UINT kEdTipTimer = 8;
constexpr UINT kEdShareTimer = 9;      // контроль: чи спитали в нас дані
int g_edShareAskedAt = 0;              // скільки запитів було до показу меню
constexpr UINT kEdTipDelay = 450;

// Підказка більше не одна константа: до інструментів і виходів дописується
// клавіша. Тримати для кожної пари «назва + клавіша» окремий рядок у таблиці
// перекладів означало б перекладати клавіші, яких ніхто не перекладає.
Str EdTipFor(EdHit what, int idx)
{
    switch (what) {
    case EdHit::Tool:
        switch (idx) {
        case 0: return Str::EdToolSelect;
        case 1: return Str::EdToolRect;
        // ⚠ Рядок еліпса випав разом зі стрілкою в CAPS-48, і еліпс поїхав у
        // default, тобто в «Кадр». Тримати тут наскрізну нумерацію — пастка:
        // кожне прибирання інструмента мовчки перейменовує сусіда.
        case 2: return Str::EdToolEllipse;
        case 3: return Str::EdToolLine;
        case 4: return Str::EdToolPen;
        case 5: return Str::EdToolText;
        case 6: return Str::EdToolHide;
        case 7: return Str::EdToolMark;
        case 8: return Str::EdToolCounter;
        case 9: return Str::EdToolStamp;
        default: return Str::EdToolCrop;
        }
    case EdHit::Swatch:  return EdCounterKind() ? Str::EdTipColorGroup : Str::EdTipColor;
    case EdHit::Pick:
        return idx == 0 ? Str::EdTipDash : idx == 1 ? Str::EdTipHeadFront
             : idx == 2 ? Str::EdTipHeadBack : Str::EdTipHeadSize;
    case EdHit::SelAlign: {
        static const Str al[8] = { Str::EdTipAlL, Str::EdTipAlCx, Str::EdTipAlR,
                                   Str::EdTipAlT, Str::EdTipAlCy, Str::EdTipAlB,
                                   Str::EdTipDistX, Str::EdTipDistY };
        return (idx >= 0 && idx < 8) ? al[idx] : Str::Empty;
    }
    case EdHit::SelGroup: return idx == 1 ? Str::EdTipUngroup : Str::EdTipMakeGroup;
    case EdHit::GroupEdit: return Str::EdTipGroupEdit;
    case EdHit::GroupDel:  return Str::EdTipGroupDel;
    case EdHit::Thick:
        {
            const EdKind kt = (g_edSel >= 0 && g_edSel < (int)g_edObjs.size())
                                  ? g_edObjs[g_edSel].kind : EdToolKind(g_edTool);
            if (kt == EdKind::Mark)    return Str::EdTipMarkH;
            if (kt == EdKind::Counter) return Str::EdTipSizeGroup;
            if (EdIsStamped(kt))       return Str::EdTipSize;
            return Str::EdTipThick;
        }
    case EdHit::HideMode: return idx == 1 ? Str::EdTipPixels : idx == 2 ? Str::EdTipPlate : Str::EdTipBlur;
    case EdHit::NumStart: return Str::EdTipNumStart;
    case EdHit::NumGroup: return Str::EdTipGroup;
    case EdHit::Aspect:   return Str::EdTipAspect;
    case EdHit::CropReset:return Str::EdCropReset;
    case EdHit::CropOk:   return Str::EdCropApply;
    case EdHit::CropNo:   return Str::EdCropCancel;
    case EdHit::NumReset: return Str::EdTipNumReset;
    case EdHit::StampMore: return Str::EdTipStampMore;
    case EdHit::Strength: return Str::EdTipStrength;
    case EdHit::Share:     return Str::EdTipShare;
    case EdHit::SizeImg:   return Str::EdTipSizeImg;
    case EdHit::SizeCan:   return Str::EdTipSizeCan;
    case EdHit::RotL:      return Str::EdTipRotL;
    case EdHit::RotR:      return Str::EdTipRotR;
    case EdHit::FlipH:     return Str::EdTipFlipH;
    case EdHit::FlipV:     return Str::EdTipFlipV;
    case EdHit::Exposure:  return Str::EdTipExposure;
    case EdHit::Gamma:     return Str::EdTipGamma;
    case EdHit::Contrast:  return Str::EdTipContrast;
    case EdHit::ToneReset: return Str::EdTipToneReset;
    case EdHit::Compare:   return Str::EdTipCompare;
    case EdHit::Size:    return idx ? Str::EdTipSizeUp : Str::EdTipSizeDn;
    case EdHit::Bold:    return Str::EdTipBold;
    case EdHit::Italic:  return Str::EdTipItalic;
    case EdHit::Align:   return idx == 1 ? Str::EdTipAlignC : idx == 2 ? Str::EdTipAlignR : Str::EdTipAlignL;
    case EdHit::Stroke:  return idx == 1 ? Str::EdTipStroke1 : idx == 2 ? Str::EdTipStroke2 : Str::EdTipStroke0;
    case EdHit::Opacity: return Str::EdTipAlpha;
    case EdHit::Front:   return Str::EdTipFront;
    case EdHit::Back:    return Str::EdTipBack;
    case EdHit::Dup:     return Str::EdTipDup;
    case EdHit::Del:     return Str::EdTipDel;
    case EdHit::Undo:    return Str::EdTipUndo;
    case EdHit::Redo:    return Str::EdTipRedo;
    case EdHit::Help:    return Str::EdTipHelp;
    case EdHit::Zoom:    return Str::EdTipZoom;
    case EdHit::Zoom100: return Str::EdTipZoom100;
    case EdHit::Fit:     return Str::EdTipFit;
    case EdHit::Panel:   return g_edPanelOpen ? Str::EdTipPanelHide : Str::EdTipPanelShow;
    case EdHit::Copy:    return Str::EdCopy;
    case EdHit::Save:    return Str::EdSaveAs;
    case EdHit::Min:     return Str::EdTipMin;
    case EdHit::Max:     return IsZoomed(g_edWnd) ? Str::EdTipRestore : Str::EdTipMax;
    case EdHit::Close:   return Str::EdTipClose;
    case EdHit::Open:    return Str::EdOpenTitle;
    case EdHit::OpenMenu:return Str::EdTipOpenMore;
    default:             return Str::Empty;
    }
}

// Клавіша інструмента — та сама, що в довідці. Один масив на обидва місця:
// якби їх було два, вони розійшлися б за перший же новий інструмент.
const wchar_t* const kEdToolKeys[11] = { L"V", L"R", L"E", L"L", L"P", L"T", L"B", L"H",
                                        L"N", L"S", L"C" };

void EdTipText(EdHit what, int idx, wchar_t* out, int cch)
{
    out[0] = L'\0';
    const Str st = EdTipFor(what, idx);
    if (st == Str::Empty) return;
    const wchar_t* key = nullptr;
    if (what == EdHit::Tool && idx >= 0 && idx < 11) key = kEdToolKeys[idx];
    else if (what == EdHit::Copy) key = L"Ctrl+C";
    else if (what == EdHit::Save) key = L"Ctrl+S";
    if (key) {
        lstrcpynW(out, S(st), cch);
        const int len = lstrlenW(out);
        if (len + 6 < cch) {
            lstrcpynW(out + len, L" (", cch - len);
            lstrcpynW(out + len + 2, key, cch - len - 2);
            const int l2 = lstrlenW(out);
            if (l2 + 2 < cch) lstrcpynW(out + l2, L")", cch - l2);
        }
    } else {
        lstrcpynW(out, S(st), cch);
    }
}

void EdTipHide()
{
    if (!g_edTip) return;
    TTTOOLINFOW ti = {};
    ti.cbSize = kEdTipInfoSize;
    ti.hwnd   = g_edWnd;
    ti.uId    = 1;
    SendMessageW(g_edTip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
}

bool EdTipEnsure(HWND hwnd)
{
    if (g_edTip) return true;
    g_edTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                              WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                              hwnd, nullptr,
                              (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), nullptr);
    if (!g_edTip) return false;
    TTTOOLINFOW ti = {};
    ti.cbSize   = kEdTipInfoSize;
    ti.uFlags   = TTF_TRACK | TTF_ABSOLUTE;
    ti.hwnd     = hwnd;
    ti.uId      = 1;
    ti.lpszText = (LPWSTR)L" ";
    // Реєстрація може й не вдатися (найчастіше — через несподіваний cbSize).
    // Тоді тултіп без інструмента мовчки не показувався б узагалі, тож краще
    // одразу прибрати вікно й чесно сказати «підказок немає».
    if (!SendMessageW(g_edTip, TTM_ADDTOOLW, 0, (LPARAM)&ti)) {
        DestroyWindow(g_edTip);
        g_edTip = nullptr;
        return false;
    }
    return true;
}

void EdTipTheme()
{
    // Тултіпи малює comctl32 власною темою. «DarkMode_Explorer» — те саме, чим
    // темнить свої підказки Провідник; якщо збірка Windows цього не підтримує,
    // підказка лишиться світлою, і це не привід писати власне вікно.
    if (g_edTip) SetWindowTheme(g_edTip, g_edDark ? L"DarkMode_Explorer" : nullptr, nullptr);
}

void EdTipShow(HWND hwnd)
{
    wchar_t tip[160];
    EdTipText(g_edHotWhat, g_edHotIdx, tip, 160);
    if (!tip[0] || !EdTipEnsure(hwnd)) return;

    TTTOOLINFOW ti = {};
    ti.cbSize   = kEdTipInfoSize;
    ti.hwnd     = hwnd;
    ti.uId      = 1;
    ti.lpszText = tip;
    SendMessageW(g_edTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);

    // ⚠ Спершу вмикаємо, потім позиціонуємо — саме в такому порядку, як у
    // прикладі Microsoft. У зворотному порядку позиція лягає на ще неактивний
    // тултіп, і він не з'являється.
    SendMessageW(g_edTip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
    POINT p = {};
    GetCursorPos(&p);
    SendMessageW(g_edTip, TTM_TRACKPOSITION, 0, MAKELPARAM(p.x + EdPx(14), p.y + EdPx(22)));
}

// ---- CAPS-24: введення напису -------------------------------------------
//
// Каретку, виділення, Ctrl+стрілки й системне меню правки нам безкоштовно дає
// звичайний EDIT. Писати власний каретковий редактор заради підпису на знімку
// було б непропорційно, тому поле живе рівно стільки, скільки триває введення,
// і зникає разом із ним, лишаючи по собі готовий об'єкт.

constexpr int kEdEditId = 401;

EdObj   g_edEditObj;
HFONT   g_edEditFont = nullptr;
WNDPROC g_edEditPrev = nullptr;

std::wstring EdEditText()
{
    if (!g_edEdit) return std::wstring();
    const int len = GetWindowTextLengthW(g_edEdit);
    if (len <= 0) return std::wstring();
    std::wstring t((size_t)len + 1, L'\0');
    GetWindowTextW(g_edEdit, &t[0], len + 1);
    t.resize((size_t)len);
    return t;
}

// Поле росте за текстом, щоб уже під час набору було видно, скільки місця
// напис займе на знімку.
void EdTextFitBox()
{
    if (!g_edEdit) return;
    EdObj probe = g_edEditObj;
    probe.text = EdEditText();
    if (probe.text.empty()) probe.text = L"M";
    EdTextMeasure(probe);

    const double sc = EdScale();
    // При заданій ширині росте лише висота: ширину обрав користувач, і поле не
    // має права її міняти під час набору.
    RECT cur = {};
    GetWindowRect(g_edEdit, &cur);
    int ew = (g_edEditObj.boxw > 0) ? (cur.right - cur.left)
                                    : (int)(probe.w * sc + 0.5) + EdPx(18);
    int eh = (int)(probe.h * sc + 0.5) + EdPx(12);
    if (ew < EdPx(90)) ew = EdPx(90);
    SetWindowPos(g_edEdit, nullptr, 0, 0, ew, eh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void EdTextCommit()
{
    if (!g_edEdit) return;
    HWND box = g_edEdit;
    std::wstring t = EdEditText();
    g_edEdit = nullptr;               // спершу гасимо ознаку: DestroyWindow нижче
                                      // зніме фокус, а це знову приведе сюди
    // Порожні хвости нічого не малюють, зате роздувають рамку виділення.
    while (!t.empty() && (t[t.size() - 1] == L'\r' || t[t.size() - 1] == L'\n' ||
                          t[t.size() - 1] == L' '  || t[t.size() - 1] == L'\t'))
        t.resize(t.size() - 1);

    DestroyWindow(box);
    if (g_edEditFont) { DeleteObject(g_edEditFont); g_edEditFont = nullptr; }
    const int idx = g_edEditIdx;
    g_edEditIdx = -1;

    if (t.empty()) {
        // Стерли весь текст — напису більше немає. Це єдиний спосіб прибрати
        // його «зсередини», і він природніший за Delete по рамці.
        if (idx >= 0 && idx < (int)g_edObjs.size()) {
            EdPushUndo();
            g_edObjs.erase(g_edObjs.begin() + idx);
            g_edSel = -1;
        }
    } else {
        EdObj o = g_edEditObj;
        o.text = t;
        EdTextMeasure(o);
        if (idx >= 0 && idx < (int)g_edObjs.size()) {
            if (g_edObjs[idx].text != t) { EdPushUndo(); g_edObjs[idx] = o; }
            g_edSel = idx;
        } else {
            EdPushUndo();
            g_edObjs.push_back(o);
            g_edSel = (int)g_edObjs.size() - 1;
            if (!g_edKeepTool) g_edTool = EdTool::Select;
        }
    }
    if (g_edWnd) {
        SetFocus(g_edWnd);
        EdLayout(g_edWnd);
        InvalidateRect(g_edWnd, nullptr, FALSE);
    }
}

void EdTextCancel()
{
    if (!g_edEdit) return;
    HWND box = g_edEdit;
    g_edEdit = nullptr;
    g_edEditIdx = -1;
    DestroyWindow(box);
    if (g_edEditFont) { DeleteObject(g_edEditFont); g_edEditFont = nullptr; }
    if (g_edWnd) {
        SetFocus(g_edWnd);
        InvalidateRect(g_edWnd, nullptr, FALSE);
    }
}

LRESULT CALLBACK EdEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        // Enter фіксує напис, Shift+Enter робить новий рядок. Ctrl+Enter EDIT
        // перетворює на символ 0x0A і рядка все одно не додає, тому саме Shift.
        if (wp == VK_RETURN && GetKeyState(VK_SHIFT) >= 0) {
            PostMessageW(g_edWnd, WMAPP_EDTEXT, 1, 0);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            PostMessageW(g_edWnd, WMAPP_EDTEXT, 0, 0);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == VK_RETURN && GetKeyState(VK_SHIFT) >= 0) return 0;   // інакше EDIT пискне
        break;
    case WM_KILLFOCUS:
        // Пішли кудись іще — напис фіксуємо. Руйнувати вікно просто тут не можна,
        // тому лише повідомляємо редактор.
        PostMessageW(g_edWnd, WMAPP_EDTEXT, 1, 0);
        break;
    default: break;
    }
    return CallWindowProcW(g_edEditPrev, hwnd, msg, wp, lp);
}

// Те саме питання, що й на закритті, але іншими словами: там редактор
// зникає, тут — лише вміст.
bool EdConfirmReplace()
{
    // Порожній список позначок ще не означає «нічого не зроблено»: поворот
    // і тон — теж робота, і втрачати їх мовчки не можна.
    if (g_edSaved || (g_edObjs.empty() && EdToneDefault() && EdGeomDefault())) return true;
    return MessageBoxW(g_edWnd, S(Str::EdAskReplace), kAppName,
                       MB_OKCANCEL | MB_ICONQUESTION) == IDOK;
}

void EdOpenFileHere(HWND hwnd)
{
    if (!EdConfirmReplace()) return;
    wchar_t path[MAX_PATH] = {};
    if (!EdPickFile(hwnd, path, MAX_PATH)) return;
    Gdiplus::Bitmap* bmp = EdBitmapFromFile(path);
    if (!bmp) {
        MessageBoxW(hwnd, S(Str::EdErrOpen), kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    EdOpenBitmap((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), bmp,
                 PathFindFileNameW(path), false, false, -1.0f);
}

// Список поруч із «Відкрити»: ті самі джерела, що й у меню трея, але під рукою
// в самому редакторі. Розкривається ВГОРУ — кнопка стоїть у рядку стану, і вниз
// списку нікуди йти.
void EdOpenMenu(HWND hwnd, const RECT& btn)
{
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, 1, S(Str::EdCapClip));
    AppendMenuW(m, MF_STRING, 2, S(Str::EdCapRegion));

    POINT p = { btn.left, btn.top };
    ClientToScreen(hwnd, &p);
    const int cmd = (int)TrackPopupMenu(m, TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RETURNCMD |
                                           TPM_NONOTIFY | TPM_LEFTBUTTON,
                                        p.x, p.y, 0, hwnd, nullptr);
    DestroyMenu(m);
    if (!cmd) return;
    if (!EdConfirmReplace()) return;

    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    if (cmd == 1) {
        CapTake(inst, hwnd, CapMode::Clipboard, nullptr);
        return;
    }
    // ⚠ Знімок ділянки фотографує все, що видно, — разом із самим редактором.
    // Ховаємо вікно на час захоплення. Якщо користувач передумав, захоплення
    // мовчки повертається, і показати вікно назад маємо ми самі.
    ShowWindow(hwnd, SW_HIDE);
    CapTake(inst, hwnd, CapMode::Region, nullptr);
    if (IsWindow(hwnd) && !IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
    }
}

void EdTextBegin(HWND hwnd, POINT img, int idx)
{
    if (g_edEdit) EdTextCommit();
    if (!g_edImg) return;

    EdObj o;
    if (idx >= 0 && idx < (int)g_edObjs.size() && g_edObjs[idx].kind == EdKind::Text) {
        o = g_edObjs[idx];
    } else {
        idx = -1;
        o = EdObj{};
        o.kind    = EdKind::Text;
        o.x       = img.x;
        o.y       = img.y;
        o.color   = g_edColor;
        o.thick   = g_edThick;
        o.alpha   = g_edAlpha;
        o.size    = g_edSize;
        o.bold    = g_edBold;
        o.italic  = g_edItalic;
        o.align   = g_edAlign;
        o.outline = g_edOutline;
        o.w = o.h = 0;
    }
    g_edEditObj = o;
    g_edEditIdx = idx;

    const double sc = EdScale();
    const RECT ir = EdImageRect();
    const int px = ir.left + (int)(o.x * sc + 0.5);
    const int py = ir.top  + (int)(o.y * sc + 0.5);
    int ew = (int)((o.boxw > 0 ? o.boxw : o.w) * sc + 0.5) + EdPx(18);
    int eh = (int)(o.h * sc + 0.5) + EdPx(12);
    if (ew < EdPx(90)) ew = EdPx(90);
    if (eh < (int)(o.size * sc * 1.4 + 0.5) + EdPx(12)) eh = (int)(o.size * sc * 1.4 + 0.5) + EdPx(12);

    // Задана ширина = переноси, тож ES_AUTOHSCROLL тоді не ставимо: саме він
    // не дає EDIT переносити рядки, і набране розходилось би з майбутнім написом.
    const DWORD wrapStyle = (o.boxw > 0) ? 0 : ES_AUTOHSCROLL;
    g_edEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", o.text.c_str(),
                               WS_CHILD | WS_VISIBLE | ES_MULTILINE | wrapStyle | ES_NOHIDESEL,
                               px - EdPx(6), py - EdPx(6), ew, eh, hwnd,
                               (HMENU)(INT_PTR)kEdEditId,
                               (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), nullptr);
    if (!g_edEdit) { g_edEditIdx = -1; return; }

    // Шрифт поля — той самий сімейством і кеглем, що й у майбутнього напису,
    // тож набране на екрані вже займає стільки ж місця, скільки займе на знімку.
    int fh = (int)(o.size * sc + 0.5);
    if (fh < 8)   fh = 8;
    if (fh > 400) fh = 400;
    g_edEditFont = CreateFontW(-fh, 0, 0, 0, o.bold ? FW_BOLD : FW_NORMAL, o.italic ? TRUE : FALSE,
                               FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, VARIABLE_PITCH, kEdFontFamily);
    if (g_edEditFont) SendMessageW(g_edEdit, WM_SETFONT, (WPARAM)g_edEditFont, TRUE);

    g_edEditPrev = (WNDPROC)SetWindowLongPtrW(g_edEdit, GWLP_WNDPROC, (LONG_PTR)EdEditProc);
    SetFocus(g_edEdit);
    SendMessageW(g_edEdit, EM_SETSEL, 0, -1);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ---- CAPS-44: розмір зображення й полотна -------------------------------
//
// ⚠ Обидві дії ЗАПІКАЮТЬ поточний рецепт (поворот, дзеркало, тон) у новий
// оригінал. Інакше «Скинути» в тоні повернуло б кадр іншого розміру, ніж той,
// що на екрані, — а це гірше за втрату самого рецепта.

// Позначки прив'язані до зображення КООРДИНАТАМИ, але товщина ліній і розміри
// кружечків та штампів лишаються як були (рішення власника 21.09): інакше
// кегль 18 після зменшення вдвічі стає дев'яткою, якої немає в наборі.
void EdScaleObjs(double kx, double ky, bool scaleText)
{
    for (size_t i = 0; i < g_edObjs.size(); ++i) {
        EdObj& o = g_edObjs[i];
        const bool upright = (o.kind == EdKind::Text || o.kind == EdKind::Counter ||
                              o.kind == EdKind::Stamp);
        if (upright) {
            // Переїздить центр, розмір лишається — крім кегля, і то за згодою.
            const int cx = (int)((o.x + o.w / 2) * kx + 0.5);
            const int cy = (int)((o.y + o.h / 2) * ky + 0.5);
            if (o.kind == EdKind::Text && scaleText) {
                const double k = (kx + ky) / 2.0;
                o.size = (int)(o.size * k + 0.5);
                if (o.size < 6) o.size = 6;
                o.w = (int)(o.w * kx + 0.5);
                o.h = (int)(o.h * ky + 0.5);
                if (o.boxw) o.boxw = (int)(o.boxw * kx + 0.5);
            }
            o.x = cx - o.w / 2;
            o.y = cy - o.h / 2;
            continue;
        }
        o.x = (int)(o.x * kx + 0.5);
        o.y = (int)(o.y * ky + 0.5);
        o.w = (int)(o.w * kx + 0.5);
        o.h = (int)(o.h * ky + 0.5);
        for (size_t k = 0; k < o.pts.size(); ++k) {
            o.pts[k].x = (LONG)(o.pts[k].x * kx + 0.5);
            o.pts[k].y = (LONG)(o.pts[k].y * ky + 0.5);
        }
    }
    if (EdHasCrop()) {
        g_edCrop.left   = (LONG)(g_edCrop.left * kx + 0.5);
        g_edCrop.right  = (LONG)(g_edCrop.right * kx + 0.5);
        g_edCrop.top    = (LONG)(g_edCrop.top * ky + 0.5);
        g_edCrop.bottom = (LONG)(g_edCrop.bottom * ky + 0.5);
    }
}

// Рецепт запечено — далі він порожній, а оригіналом стає те, що показували.
void EdAdoptSource(Gdiplus::Bitmap* fresh)
{
    // ⚠ Старий оригінал НЕ видаляємо: на нього ще посилаються знімки
    // скасування. Банк чиститься разом із новим знімком екрана.
    g_edSrcBank.push_back(fresh);
    g_edSrcId = (int)g_edSrcBank.size() - 1;
    g_edSrc = fresh;
    g_edRot = 0;
    g_edMirror = false;
    g_edExposure = 0; g_edGamma = 100; g_edContrast = 0;
    delete g_edCmp; g_edCmp = nullptr;
    g_edCompare = false;
    EdRebuildImage();
    if (g_edWnd) {
        EdFitView();
        EdLayout(g_edWnd);
        InvalidateRect(g_edWnd, nullptr, TRUE);
    }
}

bool EdResizeImage(int nw, int nh, bool scaleText, bool sharp)
{
    if (!g_edImg || nw < 1 || nh < 1 || nw > 20000 || nh > 20000) return false;
    const int ow = g_edImgW, oh = g_edImgH;
    if (nw == ow && nh == oh) return false;
    Gdiplus::Bitmap* out = new Gdiplus::Bitmap(nw, nh, PixelFormat32bppPARGB);
    if (!out || out->GetLastStatus() != Gdiplus::Ok) { delete out; return false; }
    {
        Gdiplus::Graphics g(out);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetInterpolationMode(sharp ? Gdiplus::InterpolationModeNearestNeighbor
                                     : Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(g_edImg, Gdiplus::Rect(0, 0, nw, nh), 0, 0, ow, oh, Gdiplus::UnitPixel);
    }
    EdPushUndo();
    EdScaleObjs((double)nw / ow, (double)nh / oh, scaleText);
    EdAdoptSource(out);
    return true;
}

// Полотно більшає — наявне зображення СТАЄ ОКРЕМИМ ОБ'ЄКТОМ (вимога власника),
// а тло під ним лишається прозорим. Далі його можна рухати, як будь-яку
// позначку, або підкласти під нього прямокутник потрібного кольору.
// Чи є в поточному тлі бодай один непрозорий піксель. Саме це, а не окремий
// прапорець, відповідає на питання «чи є що перетворювати на об'єкт»: воно
// лишається правильним і після того, як користувач сам видалив те зображення.
bool EdBaseHasPixels()
{
    if (!g_edImg) return false;
    const int w = g_edImgW, h = g_edImgH;
    Gdiplus::BitmapData bd = {};
    Gdiplus::Rect all(0, 0, w, h);
    if (g_edImg->LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bd) != Gdiplus::Ok)
        return true;                     // не змогли прочитати — вважаємо, що є
    bool any = false;
    if (bd.Stride > 0) {
        for (int y = 0; y < h && !any; ++y) {
            const BYTE* row = (const BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
            for (int x = 0; x < w; ++x)
                if (row[x * 4 + 3] != 0) { any = true; break; }
        }
    } else {
        any = true;
    }
    g_edImg->UnlockBits(&bd);
    return any;
}

bool EdResizeCanvas(int nw, int nh)
{
    if (!g_edImg || nw < 1 || nh < 1 || nw > 20000 || nh > 20000) return false;
    const int ow = g_edImgW, oh = g_edImgH;
    if (nw == ow && nh == oh) return false;
    Gdiplus::Bitmap* fresh = new Gdiplus::Bitmap(nw, nh, PixelFormat32bppPARGB);
    if (!fresh || fresh->GetLastStatus() != Gdiplus::Ok) { delete fresh; return false; }
    Gdiplus::Bitmap* old = g_edImg->Clone(0, 0, ow, oh, PixelFormat32bppPARGB);
    if (!old || old->GetLastStatus() != Gdiplus::Ok) { delete old; delete fresh; return false; }

    const int dx = (nw - ow) / 2, dy = (nh - oh) / 2;
    EdPushUndo();
    // ⚠ Об'єктом знімок стає ЛИШЕ поки в тлі щось є. Друге й наступні
    // збільшення полотна не мають плодити порожніх «зображень», які потім
    // можна видаляти й повертати (зауваження власника).
    const bool convert = EdBaseHasPixels();
    const int id = convert ? EdAddImage(old) : -1;
    if (!convert) delete old;
    if (id >= 0) {
        EdObj o = EdObj{};
        o.kind = EdKind::Image;
        o.img = id;
        o.alpha = 100;
        o.color = g_edColor;
        o.x = dx; o.y = dy; o.w = ow; o.h = oh;
        // Колишній знімок лягає НАЙНИЖЧЕ: він тло, а не остання позначка.
        g_edObjs.insert(g_edObjs.begin(), o);
        if (g_edSel >= 0) ++g_edSel;
        for (size_t k = 0; k < g_edSelMore.size(); ++k) ++g_edSelMore[k];
    }
    // Решта позначок їде разом із ним — вони показували на його пікселі.
    for (size_t i = (id >= 0 ? 1 : 0); i < g_edObjs.size(); ++i)
        EdMoveObj(g_edObjs[i], dx, dy);
    if (EdHasCrop()) OffsetRect(&g_edCrop, dx, dy);
    EdAdoptSource(fresh);
    return true;
}

// ---- CAPS-27: кадрування -------------------------------------------------

const double kEdAspects[4] = { 0.0, 16.0 / 9.0, 4.0 / 3.0, 1.0 };

// Кадр не виходить за знімок і не буває меншим за двадцять пікселів: із
// меншого ручки вже не витягнеш.
void EdCropClamp(RECT& r)
{
    if (r.right - r.left < 20) r.right = r.left + 20;
    if (r.bottom - r.top < 20) r.bottom = r.top + 20;
    if (r.left < 0) { r.right -= r.left; r.left = 0; }
    if (r.top < 0)  { r.bottom -= r.top; r.top = 0; }
    if (r.right > g_edImgW)  { r.left -= r.right - g_edImgW;  r.right = g_edImgW; }
    if (r.bottom > g_edImgH) { r.top -= r.bottom - g_edImgH; r.bottom = g_edImgH; }
    if (r.left < 0) r.left = 0;
    if (r.top < 0)  r.top = 0;
}

// ⚠ Пропорції ВПИСУЄМО в наявний прямокутник, а не розтягуємо під нього.
// Інакше кадр вилазить за знімок, обмеження тягне його назад — і пропорція,
// заради якої все робилося, губиться: 1:1 на повному кадрі давало 1200×800.
void EdCropAspect(RECT& r, int anchorRight, int anchorBottom)
{
    const double a = kEdAspects[g_edCropAspect];
    if (a <= 0.0) return;
    const int w = r.right - r.left, h = r.bottom - r.top;
    int nw = w, nh = (int)(w / a + 0.5);
    if (nh > h) { nh = h; nw = (int)(h * a + 0.5); }
    if (nw < 20) { nw = 20; nh = (int)(nw / a + 0.5); }
    if (nh < 20) { nh = 20; nw = (int)(nh * a + 0.5); }
    if (anchorRight)  r.left = r.right - nw; else r.right = r.left + nw;
    if (anchorBottom) r.top = r.bottom - nh; else r.bottom = r.top + nh;
}

int EdOutsideCount()
{
    if (!EdHasCrop()) return 0;
    int n = 0;
    for (size_t i = 0; i < g_edObjs.size(); ++i) {
        const EdObj& o = g_edObjs[i];
        RECT b = { o.x, o.y, o.x + o.w, o.y + o.h };
        if (b.right < g_edCrop.left || b.left > g_edCrop.right ||
            b.bottom < g_edCrop.top || b.top > g_edCrop.bottom) ++n;
    }
    return n;
}

// Прямокутник кадру на екрані. Під час редагування — той, що тягнуть;
// координати ті самі, абсолютні по знімку.
RECT EdCropScreen(const RECT& c)
{
    const RECT ir = EdImageRect();
    const double s = EdScale();
    RECT r;
    r.left   = ir.left + (int)((c.left - EdViewX()) * s + 0.5);
    r.top    = ir.top  + (int)((c.top - EdViewY()) * s + 0.5);
    r.right  = ir.left + (int)((c.right - EdViewX()) * s + 0.5);
    r.bottom = ir.top  + (int)((c.bottom - EdViewY()) * s + 0.5);
    return r;
}

int EdCropHandles(RECT out[8])
{
    const RECT r = EdCropScreen(g_edCropEdit);
    const int h = EdPx(11), k = h / 2;
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
    return 8;
}

void EdCropBegin()
{
    // Кадрувати наосліп неможливо: спершу показуємо весь знімок, інакше рамка
    // й половина кадру опиняються за краєм вікна (зауваження власника).
    EdFitView();
    g_edCropping = true;
    g_edCropEdit = EdHasCrop() ? g_edCrop
                               : RECT{ 0, 0, g_edImgW, g_edImgH };
    g_edSel = -1;
}

void EdCropFinish(bool apply)
{
    if (apply) {
        RECT n = g_edCropEdit;
        EdCropClamp(n);
        // Кадр на весь знімок — це відсутність кадру, а не кадр «як є»:
        // інакше «Скинути» мусив би вміти два різні порожні стани.
        const bool whole = (n.left <= 0 && n.top <= 0 &&
                            n.right >= g_edImgW && n.bottom >= g_edImgH);
        if (!EqualRect(&n, &g_edCrop) || (whole && EdHasCrop())) {
            EdPushUndo();
            g_edCrop = whole ? RECT{ 0, 0, 0, 0 } : n;
        }
    }
    g_edCropping = false;
    g_edTool = EdTool::Select;
    if (g_edWnd) {
        EdFitView();
        EdLayout(g_edWnd);
        InvalidateRect(g_edWnd, nullptr, FALSE);
    }
}

void EdCropReset()
{
    if (!EdHasCrop()) return;
    EdPushUndo();
    g_edCrop = RECT{ 0, 0, 0, 0 };
    if (g_edWnd) {
        EdFitView();
        EdLayout(g_edWnd);
        InvalidateRect(g_edWnd, nullptr, FALSE);
    }
}

// ---- CAPS-26: сітка емодзі ------------------------------------------------
//
// Своє вікно, а не системна панель Win+. — вона вставляє символ лише в
// текстове поле з фокусом, тож довелося б тримати приховане поле й ловити
// введене. До того ж у процесі з правами адміністратора вона може й не
// відкритися. Своя сітка малюється тим самим DirectWrite і не залежить ні від
// фокуса, ні від прав.

constexpr int kEdEmojiCols = 8;
constexpr int kEdEmojiCell = 34;

HWND g_edEmojiWnd = nullptr;
int  g_edEmojiHot = -1;

LRESULT CALLBACK EdEmojiProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);

        const EdTheme t = EdColors(g_edDark);
        HBRUSH b = CreateSolidBrush(t.surface);
        FillRect(mem, &rc, b);
        DeleteObject(b);
        RECT edge = rc;
        HBRUSH eb = CreateSolidBrush(t.border);
        FrameRect(mem, &edge, eb);
        DeleteObject(eb);

        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const int cell = EdPx(kEdEmojiCell);
        const int pad = EdPx(6);
        for (int i = 0; i < kEdEmojiCount; ++i) {
            RECT r = { pad + (i % kEdEmojiCols) * cell, pad + (i / kEdEmojiCols) * cell, 0, 0 };
            r.right = r.left + cell;
            r.bottom = r.top + cell;
            if (i == g_edEmojiHot) {
                RECT hr = r;
                InflateRect(&hr, -EdPx(2), -EdPx(2));
                Gdiplus::Color hc = EdC(t.hot);
                EdFillRound(g, hr, (float)EdPx(5), &hc, nullptr);
            }
            EdObj probe = EdObj{};
            probe.kind  = EdKind::Text;
            probe.text  = kEdEmoji[i];
            probe.size  = kEdEmojiCell * 2 / 3;
            probe.alpha = 100;
            probe.color = RGB(255, 255, 255);
            const EdTile* tile = EdTextTile(probe, (double)g_edDpi / 96.0);
            if (tile && tile->bmp) {
                const int tw = (int)tile->bmp->GetWidth(), th = (int)tile->bmp->GetHeight();
                g.DrawImage(tile->bmp,
                            Gdiplus::Rect((r.left + r.right) / 2 - tw / 2,
                                          (r.top + r.bottom) / 2 - th / 2, tw, th),
                            0, 0, tw, th, Gdiplus::UnitPixel);
            }
        }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        const int cell = EdPx(kEdEmojiCell), pad = EdPx(6);
        const int cx = (GET_X_LPARAM(lp) - pad) / cell, cy = (GET_Y_LPARAM(lp) - pad) / cell;
        int idx = (cx >= 0 && cx < kEdEmojiCols && cy >= 0) ? cy * kEdEmojiCols + cx : -1;
        if (idx >= kEdEmojiCount) idx = -1;
        if (idx != g_edEmojiHot) {
            g_edEmojiHot = idx;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (g_edEmojiHot >= 0 && g_edEmojiHot < kEdEmojiCount) {
            const int id = kEdEmojiBase + g_edEmojiHot;
            if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size() &&
                g_edObjs[g_edSel].kind == EdKind::Stamp && g_edObjs[g_edSel].stamp != id) {
                EdPushUndo();
                g_edObjs[g_edSel].stamp = id;
            }
            g_edStamp = id;
            if (g_edWnd) {
                EdLayout(g_edWnd);
                InvalidateRect(g_edWnd, nullptr, FALSE);
            }
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;

    case WM_KILLFOCUS:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_edEmojiWnd = nullptr;
        g_edEmojiHot = -1;
        return 0;

    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EdEmojiPick(HWND owner, const RECT& btn)
{
    if (g_edEmojiWnd) { DestroyWindow(g_edEmojiWnd); g_edEmojiWnd = nullptr; }

    static bool registered = false;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = EdEmojiProc;
        wc.hInstance     = inst;
        wc.lpszClassName = L"lilhelpers_emoji";
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    const int cell = EdPx(kEdEmojiCell), pad = EdPx(6);
    const int rows = (kEdEmojiCount + kEdEmojiCols - 1) / kEdEmojiCols;
    const int w = kEdEmojiCols * cell + pad * 2;
    const int h = rows * cell + pad * 2;

    POINT p = { btn.left, btn.bottom + EdPx(4) };
    ClientToScreen(owner, &p);
    // Не даємо сітці вилізти за край монітора: вона з'являється біля кнопки в
    // смузі, а та буває близько до правого краю.
    HMONITOR mon = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi)) {
        if (p.x + w > mi.rcWork.right)  p.x = mi.rcWork.right - w;
        if (p.y + h > mi.rcWork.bottom) p.y = mi.rcWork.bottom - h;
        if (p.x < mi.rcWork.left) p.x = mi.rcWork.left;
    }

    g_edEmojiWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"lilhelpers_emoji", nullptr,
                                   WS_POPUP, p.x, p.y, w, h, owner, nullptr, inst, nullptr);
    if (!g_edEmojiWnd) return;
    ShowWindow(g_edEmojiWnd, SW_SHOWNA);
    SetForegroundWindow(g_edEmojiWnd);
    SetFocus(g_edEmojiWnd);
}

LRESULT CALLBACK EdWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_edDpi = (int)GetDpiForWindow(hwnd);
        EdMakeFonts();
        EdApplyTheme(hwnd);
        // ⚠ Без цього рядка власний заголовок НЕ з'являється, і вікно отримує
        // два підписи — системний і наш. При створенні WM_NCCALCSIZE приходить
        // рівно один раз і з wParam = FALSE, тобто повз нашу гілку; рамку
        // рахують за звичайними правилами. SWP_FRAMECHANGED змушує систему
        // перепитати з wParam = TRUE — і аж тоді підпис переходить клієнту.
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        // CAPS-42: вікно приймає файли. Вкинуте лягає ПОЗНАЧКОЮ, а не заміняє
        // знімок — заміна лишилась свідомим вибором у списку біля «Відкрити».
        DragAcceptFiles(hwnd, TRUE);
        // ⚠ Провідник працює БЕЗ підвищення, а ми — з ним, і UIPI мовчки
        // викидає повідомлення «знизу вгору». Курсор при цьому показує плюс,
        // а відпускання не робить нічого — рівно те, що побачив власник.
        // Лікується тим самим, чим і зникла іконка трею (CAPS-17): точковим
        // дозволом саме на ці повідомлення.
        ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);   // CAPS-48
        ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(hwnd, 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW, nullptr);
        return 0;

    // ---- власний заголовок: підпис віддаємо клієнту ----------------------
    case WM_NCCALCSIZE: {
        if (!wp) break;
        NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lp;
        const LONG top = p->rgrc[0].top;
        DefWindowProcW(hwnd, msg, wp, lp);       // хай порахує рамки як завжди
        p->rgrc[0].top = top;                    // а верх лишаємо клієнтові
        if (IsZoomed(hwnd)) {
            // ⚠ Розгорнуте вікно вилазить за межі монітора рівно на товщину
            // рамки. Якщо верх не підрізати, перший рядок заголовка опиниться
            // за екраном — класична вада власних підписів.
            const UINT dpi = GetDpiForWindow(hwnd);
            p->rgrc[0].top += GetSystemMetricsForDpi(SM_CYFRAME, dpi)
                            + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        }
        return 0;
    }

    case WM_NCHITTEST: {
        const LRESULT sys = DefWindowProcW(hwnd, msg, wp, lp);
        if (sys != HTCLIENT) return sys;         // рамки й кути лишаються системі
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        if (pt.y >= g_edRcCaption.bottom) return HTCLIENT;
        if (!IsZoomed(hwnd)) {
            // Верхньої рамки більше немає — смужку під зміну розміру лишаємо самі.
            const UINT dpi = GetDpiForWindow(hwnd);
            const int grip = GetSystemMetricsForDpi(SM_CYFRAME, dpi)
                           + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            if (pt.y < grip) return HTTOP;
        }
        const EdRegion* r = EdFind(pt);
        // Кнопка «розгорнути» віддається системі як HTMAXBUTTON: лише так
        // Windows 11 показує над нею підказку з розкладками вікон.
        if (r && r->what == EdHit::Max) return HTMAXBUTTON;
        if (r) return HTCLIENT;
        return HTCAPTION;
    }

    // HTMAXBUTTON обслуговує система, тож клік і наведення приходять сюди
    // неклієнтськими повідомленнями, а не звичайними.
    case WM_NCMOUSEMOVE:
        if (wp == HTMAXBUTTON) {
            if (g_edHotWhat != EdHit::Max) {
                g_edHotWhat = EdHit::Max;
                g_edHotIdx = 0;
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, &g_edRcCaption, FALSE);
            }
            return 0;
        }
        break;

    case WM_NCMOUSELEAVE:
        if (g_edHotWhat == EdHit::Max) {
            g_edHotWhat = EdHit::None;
            g_edHotIdx = -1;
            InvalidateRect(hwnd, &g_edRcCaption, FALSE);
        }
        break;

    case WM_NCLBUTTONDOWN:
        if (wp == HTMAXBUTTON) return 0;         // натиск ковтаємо, діємо на відпусканні
        break;

    case WM_NCLBUTTONUP:
        if (wp == HTMAXBUTTON) {
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        }
        break;

    case WM_ACTIVATE:
        g_edActive = (LOWORD(wp) != WA_INACTIVE);
        InvalidateRect(hwnd, &g_edRcCaption, FALSE);
        break;

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
        EdTipTheme();
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = EdPx(kEdMinW);
        mm->ptMinTrackSize.y = EdPx(kEdMinH);
        // Мінімум виріс разом зі смугою властивостей напису, і на малому екрані
        // з великим масштабом він може перевищити сам екран. Вікно, яке не
        // звужується до екрана, гірше за трохи затісну смугу, тож поступаємось.
        RECT work = {};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) && work.right > work.left) {
            const int maxW = work.right - work.left;
            if (mm->ptMinTrackSize.x > maxW) mm->ptMinTrackSize.x = maxW;
        }
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
        if (g_edDrag == EdDrag::Strength) { EdSetStrengthAt(pt.x); return 0; }
        if (g_edDrag == EdDrag::Tone) { EdSetToneAt(g_edToneWhat, pt.x); return 0; }
        if (g_edDrag == EdDrag::Zoom) { EdSetZoomAt(pt.x); return 0; }
        if (g_edDrag == EdDrag::Pan) {
            g_edPanX += pt.x - g_edDragFrom.x;
            g_edPanY += pt.y - g_edDragFrom.y;
            g_edDragFrom = pt;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_edDrag == EdDrag::New) {
            const POINT img = EdToImage(pt);
            if (g_edNew.kind == EdKind::Pen) {
                // Точки, ближчі за два пікселі зображення, не додаємо: вони не
                // додають форми, зате роздувають список і роблять криву хвилястою.
                const POINT& last = g_edNew.pts.back();
                const long dx = img.x - last.x, dy = img.y - last.y;
                if (dx * dx + dy * dy >= 4) g_edNew.pts.push_back(img);
                EdPenBounds(g_edNew);
            } else if (g_edNew.kind == EdKind::Mark) {
                // Тягнемо тільки по горизонталі: висота вже стала.
                if (img.x < g_edNewOrigin.x) {
                    g_edNew.x = img.x;
                    g_edNew.w = g_edNewOrigin.x - img.x;
                } else {
                    g_edNew.x = g_edNewOrigin.x;
                    g_edNew.w = img.x - g_edNewOrigin.x;
                }
            } else {
                g_edNew.w = img.x - g_edNew.x;
                g_edNew.h = img.y - g_edNew.y;
                if (wp & MK_SHIFT) EdConstrain(g_edNew);   // теж із повідомлення
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_edDrag == EdDrag::Move && g_edSel >= 0) {
            const double s = EdScale();
            const int dx = (int)((pt.x - g_edDragFrom.x) / (s > 0 ? s : 1.0) + (pt.x >= g_edDragFrom.x ? 0.5 : -0.5));
            const int dy = (int)((pt.y - g_edDragFrom.y) / (s > 0 ? s : 1.0) + (pt.y >= g_edDragFrom.y ? 0.5 : -0.5));
            const int px = g_edObjs[g_edSel].x, py = g_edObjs[g_edSel].y;
            g_edObjs[g_edSel].x = g_edDragOrig.x + dx;
            g_edObjs[g_edSel].y = g_edDragOrig.y + dy;
            if (g_edObjs[g_edSel].kind == EdKind::Pen) {
                for (size_t i = 0; i < g_edDragOrig.pts.size(); ++i) {
                    g_edObjs[g_edSel].pts[i].x = g_edDragOrig.pts[i].x + dx;
                    g_edObjs[g_edSel].pts[i].y = g_edDragOrig.pts[i].y + dy;
                }
            }
            // Решта вибраних їдуть на ту саму різницю, що й головний: так група
            // рухається як ціле, не накопичуючи розбіжності від округлень.
            const int mdx = g_edObjs[g_edSel].x - px, mdy = g_edObjs[g_edSel].y - py;
            if (mdx || mdy)
                for (size_t k = 0; k < g_edSelMore.size(); ++k)
                    if (g_edSelMore[k] >= 0 && g_edSelMore[k] < (int)g_edObjs.size())
                        EdMoveObj(g_edObjs[g_edSelMore[k]], mdx, mdy);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_edDrag == EdDrag::Crop) {
            const POINT img = EdToImage(pt);
            RECT n = g_edCropOrig;
            switch (g_edHandle) {
            case 0: n.left = img.x; n.top = img.y; break;
            case 1: n.top = img.y; break;
            case 2: n.right = img.x; n.top = img.y; break;
            case 3: n.right = img.x; break;
            case 4: n.right = img.x; n.bottom = img.y; break;
            case 5: n.bottom = img.y; break;
            case 6: n.left = img.x; n.bottom = img.y; break;
            default: n.left = img.x; break;
            }
            if (n.right < n.left) { const LONG v = n.left; n.left = n.right; n.right = v; }
            if (n.bottom < n.top) { const LONG v = n.top; n.top = n.bottom; n.bottom = v; }
            EdCropAspect(n, g_edHandle == 0 || g_edHandle == 6 || g_edHandle == 7,
                            g_edHandle == 0 || g_edHandle == 1 || g_edHandle == 2);
            EdCropClamp(n);
            g_edCropEdit = n;
            EdLayout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_edDrag == EdDrag::CropMove) {
            const double sc = EdScale();
            const int dx = (int)((pt.x - g_edDragFrom.x) / sc + (pt.x > g_edDragFrom.x ? 0.5 : -0.5));
            const int dy = (int)((pt.y - g_edDragFrom.y) / sc + (pt.y > g_edDragFrom.y ? 0.5 : -0.5));
            RECT n = g_edCropOrig;
            OffsetRect(&n, dx, dy);
            EdCropClamp(n);
            g_edCropEdit = n;
            EdLayout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_edDrag == EdDrag::ManyResize) {
            EdManyResize(g_edHandle, EdToImage(pt), (wp & MK_SHIFT) != 0);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_edDrag == EdDrag::ManyRotate) {
            const double cx = (g_edManyBox.left + g_edManyBox.right) / 2.0;
            const double cy = (g_edManyBox.top + g_edManyBox.bottom) / 2.0;
            const POINT im = EdToImage(pt);
            double deg = atan2(im.x - cx, cy - im.y) * 180.0 / 3.14159265358979 - g_edManyAng0;
            // Shift — як і в одиночного повороту, через кожні 15°.
            if (wp & MK_SHIFT) deg = floor(deg / 15.0 + 0.5) * 15.0;
            int d = (int)((deg < 0) ? deg - 0.5 : deg + 0.5) % 360;
            EdManyRotate(d);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_edDrag == EdDrag::Rotate && g_edSel >= 0) {
            EdObj& o = g_edObjs[g_edSel];
            const RECT r = EdObjScreen(g_edDragOrig);
            const double cx = (r.left + r.right) / 2.0, cy = (r.top + r.bottom) / 2.0;
            // Кут рахуємо від напрямку «вгору»: ручка стоїть саме там, і на
            // початку тягнення поворот має дорівнювати нулю, а не дев'яноста.
            double deg = atan2(pt.x - cx, cy - pt.y) * 180.0 / 3.14159265358979;
            // ⚠ Shift беремо з wParam повідомлення, а не з GetKeyState: він
            // ПРИХОДИТЬ разом із рухом миші, і лише так цю гілку можна
            // перевірити харнесом (той шле повідомлення, а не тисне клавіші).
            if (wp & MK_SHIFT) deg = floor(deg / 15.0 + 0.5) * 15.0;
            int d = (int)(deg + 0.5) % 360;
            if (d < 0) d += 360;
            o.rot = d;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_edDrag == EdDrag::Resize && g_edSel >= 0) {
            // ⚠ Повернуту позначку тягнемо в ЇЇ власних координатах: точку
            // повертаємо назад, інакше ручка «правий край» тягла б угору.
            POINT rp = pt;
            const EdObj& ro = g_edObjs[g_edSel];
            if (ro.rot != 0 && EdCanRotate(ro.kind)) {
                const RECT rr = EdObjScreen(ro);
                double px = rp.x, py = rp.y;
                EdRotatePt((rr.left + rr.right) / 2.0, (rr.top + rr.bottom) / 2.0,
                           -ro.rot, px, py);
                rp.x = (LONG)(px + 0.5);
                rp.y = (LONG)(py + 0.5);
            }
            // ⚠ Shift беремо з wParam повідомлення, а не з GetKeyState — див.
            // сусідню гілку повороту.
            EdResizeSel(g_edHandle, EdToImage(rp), (wp & MK_SHIFT) != 0);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        const EdRegion* r = EdFind(pt);
        const EdHit what = (r && r->what != EdHit::Canvas) ? r->what : EdHit::None;
        const int idx = r ? r->idx : -1;
        if (what != g_edHotWhat || idx != g_edHotIdx) {
            g_edHotWhat = what;
            g_edHotIdx = idx;
            // Підказка чекає паузи: миша, що просто проходить над смугою, не
            // має тягнути за собою шлейф підказок.
            EdTipHide();
            KillTimer(hwnd, kEdTipTimer);
            if (EdTipFor(what, idx) != Str::Empty) SetTimer(hwnd, kEdTipTimer, kEdTipDelay, nullptr);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (!g_edTracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_edTracking = true;
        }
        // Курсор над полотном вибирає ОДНА функція — див. EdCursorFor.
        if (r && r->what == EdHit::Canvas)
            SetCursor(LoadCursorW(nullptr, EdCursorFor(pt)));
        return 0;
    }

    case WM_KEYUP:
        // Клавішу відпустили — серія скінчилась, наступна почне новий крок
        // скасування. Без цього двадцять натискань дали б двадцять кроків.
        if (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN)
            g_edNudging = false;
        return 0;

    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        wchar_t path[MAX_PATH] = {};
        POINT pt = {};
        DragQueryPoint(drop, &pt);
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        // Кілька файлів кладемо сходинкою: рівно один на одного вони лягли б
        // так, ніби вкинувся лише останній.
        int step = 0;
        for (UINT i = 0; i < n && i < 8; ++i) {
            if (!DragQueryFileW(drop, i, path, MAX_PATH)) continue;
            Gdiplus::Bitmap* b = EdBitmapFromFile(path);
            if (!b) continue;
            POINT where = pt;
            where.x += step * EdPx(18);
            where.y += step * EdPx(18);
            ++step;
            EdPlaceImage(b, EdToImage(where));
        }
        if (!step) MessageBoxW(hwnd, S(Str::EdErrOpen), kAppName, MB_OK | MB_ICONWARNING);
        DragFinish(drop);
        SetForegroundWindow(hwnd);
        return 0;
    }

    case WM_MOUSELEAVE:
        g_edTracking = false;
        EdTipHide();
        KillTimer(hwnd, kEdTipTimer);
        if (g_edHotWhat != EdHit::None) {
            g_edHotWhat = EdHit::None;
            g_edHotIdx = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        // Клік повз розкритий селект закриває його — і НЕ робить нічого іншого:
        // інакше той самий клік, яким список згортають, ще й малював би позначку.
        if (g_edPickOpen >= 0) {
            const EdRegion* pr = EdFind(pt);
            if (!pr || (pr->what != EdHit::Pick && pr->what != EdHit::PickItem)) {
                g_edPickOpen = -1;
                EdLayout(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        // Клік будь-де поза полем спершу фіксує напис — СИНХРОННО, до розбору
        // самого кліка: інакше «жирний» одразу після набору потрапив би в
        // старий об'єкт, бо фіксація прийшла б повідомленням уже після.
        if (g_edEdit) EdTextCommit();
        EdTipHide();
        KillTimer(hwnd, kEdTipTimer);
        SetFocus(hwnd);
        const EdRegion* r = EdFind(pt);
        if (!r) return 0;
        switch (r->what) {
        case EdHit::Tool:
            if (g_edCropping && (EdTool)r->idx != EdTool::Crop) EdCropFinish(false);
            g_edTool = (EdTool)r->idx;
            if (g_edTool == EdTool::Crop) EdCropBegin();
            if (g_edTool != EdTool::Select) g_edSel = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case EdHit::Swatch: {
            const EdKind kk = (g_edSel >= 0 && g_edSel < (int)g_edObjs.size())
                                  ? g_edObjs[g_edSel].kind : EdToolKind(g_edTool);
            int np = 8;
            const COLORREF* pl = EdPaletteFor(kk, np);
            if (r->idx < 0 || r->idx >= np) return 0;
            // ⚠ У маркера ВЛАСНА палітра, і його колір не має ставати типовим
            // для решти: інакше після маркера малювався б яскраво-салатовий
            // прямокутник, якого в палітрі прямокутника немає (зауваження
            // власника). Тому для маркера — лише його змінна, і вихід.
            if (kk == EdKind::Mark) {
                g_edMarkColor = pl[r->idx];
                if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size() &&
                    g_edObjs[g_edSel].kind == EdKind::Mark &&
                    g_edObjs[g_edSel].color != pl[r->idx]) {
                    EdPushUndo();
                    g_edObjs[g_edSel].color = pl[r->idx];
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // Тогл увімкнено — колір іде всій групі, і вибраний кружечок у ній
            // теж. Робити і те, і те означало б два кроки скасування на один клік.
            if (kk == EdKind::Counter && g_edGroupEdit) {
                EdGroupApply(0, (int)pl[r->idx]);
                g_edColor = pl[r->idx];
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                EdSetColor(pl[r->idx]);
            }
            return 0;
        }
        case EdHit::StampPick: {
            const int id = (r->idx < kEdVectorStamps)
                               ? r->idx : kEdEmojiBase + (r->idx - kEdVectorStamps);
            if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size() &&
                g_edObjs[g_edSel].kind == EdKind::Stamp && g_edObjs[g_edSel].stamp != id) {
                EdPushUndo();
                g_edObjs[g_edSel].stamp = id;
            }
            g_edStamp = id;
            EdLayout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case EdHit::StampMore:
            EdEmojiPick(hwnd, r->r);
            return 0;
        case EdHit::Aspect:
            g_edCropAspect = r->idx;
            if (g_edCropping) {
                RECT n = g_edCropEdit;
                EdCropAspect(n, 0, 0);
                EdCropClamp(n);
                g_edCropEdit = n;
            }
            EdLayout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case EdHit::CropReset:
            EdCropReset();
            if (g_edCropping) g_edCropEdit = RECT{ 0, 0, g_edImgW, g_edImgH };
            EdLayout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case EdHit::CropOk:  EdCropFinish(true);  return 0;
        case EdHit::CropNo:  EdCropFinish(false); return 0;
        case EdHit::NumGroup:
        case EdHit::NumNext:
            return 0;                      // самі лише підписи, з підказкою
        case EdHit::NumStart: {
            if (r->idx > 1) return 0;      // підпис «Початковий номер» не клікається
            // Початок живе в кружечках групи, тож зміна перенумеровує їх усіх
            // і лягає в скасування. Порожня група тримає початок у типовому.
            const int grp = EdCurGroup();
            int v = EdGroupStart(grp) + (r->idx ? 1 : -1);
            if (v < 0)   v = 0;
            if (v > 999) v = 999;
            if (EdGroupCount(grp) > 0) {
                EdPushUndo();
                for (size_t i = 0; i < g_edObjs.size(); ++i)
                    if (g_edObjs[i].kind == EdKind::Counter && g_edObjs[i].group == grp)
                        g_edObjs[i].start = v;
            } else {
                g_edStartNum = v;
            }
            EdLayout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case EdHit::NumReset: {
            // Нова група: нумерація знову з початку, стара група лишається як є.
            int mx = -1;
            for (size_t i = 0; i < g_edObjs.size(); ++i)
                if (g_edObjs[i].kind == EdKind::Counter && g_edObjs[i].group > mx) mx = g_edObjs[i].group;
            g_edCounterGroup = mx + 1;
            g_edSel = -1;
            EdLayout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case EdHit::HideMode: {
            if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size() &&
                g_edObjs[g_edSel].kind == EdKind::Hide && g_edObjs[g_edSel].mode != r->idx) {
                EdPushUndo();
                g_edObjs[g_edSel].mode = r->idx;
            }
            g_edHideMode = r->idx;
            EdLayout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case EdHit::Strength:
            if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size()) EdPushUndo();
            g_edDrag = EdDrag::Strength;
            SetCapture(hwnd);
            EdSetStrengthAt(pt.x);
            return 0;
        case EdHit::Thick:
            {
                const EdKind kk = (g_edSel >= 0 && g_edSel < (int)g_edObjs.size())
                                      ? g_edObjs[g_edSel].kind : EdToolKind(g_edTool);
                const int val = EdThickSet(kk)[r->idx];
                if (kk == EdKind::Counter && g_edGroupEdit) {
                    EdGroupApply(1, val);
                    g_edStampSize = val;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size() &&
                    g_edObjs[g_edSel].thick != val) {
                    EdPushUndo();
                    g_edObjs[g_edSel].thick = val;
                    if (kk == EdKind::Mark) {
                        // Висота смуги росте від її середини, інакше маркер
                        // з'їжджав би вниз від рядка, який підкреслює.
                        EdObj& mo = g_edObjs[g_edSel];
                        mo.y += (mo.h - val) / 2;
                        mo.h = val;
                    }
                }
                if (kk == EdKind::Mark)        g_edMarkH = val;
                else if (EdIsStamped(kk))      g_edStampSize = val;
                else                           g_edThick = val;
                if (EdIsStamped(kk) && g_edSel >= 0 && g_edSel < (int)g_edObjs.size() &&
                    EdIsStamped(g_edObjs[g_edSel].kind)) {
                    // Штамп росте від свого центра, а не від лівого верхнього кута.
                    EdObj& so = g_edObjs[g_edSel];
                    so.x += (so.w - val) / 2;
                    so.y += (so.h - val) / 2;
                    so.w = so.h = val;
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case EdHit::Fill: {
            const bool want = (r->idx == 1);
            if (g_edSel >= 0 && g_edSel < (int)g_edObjs.size() &&
                EdCanFill(g_edObjs[g_edSel].kind) && g_edObjs[g_edSel].filled != want) {
                EdPushUndo();
                g_edObjs[g_edSel].filled = want;
            }
            g_edFill = want;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case EdHit::Size: case EdHit::Bold: case EdHit::Italic:
        case EdHit::Align: case EdHit::Stroke: {
            const bool selText = (g_edSel >= 0 && g_edSel < (int)g_edObjs.size() &&
                                  g_edObjs[g_edSel].kind == EdKind::Text);
            // Спершу рахуємо НОВЕ значення і лише тоді, якщо воно справді інше,
            // кладемо знімок у скасування: повторний клік по вже активній кнопці
            // інакше плодив би порожні кроки, і Ctrl+Z переставав би працювати.
            int  nSize    = selText ? g_edObjs[g_edSel].size    : g_edSize;
            bool nBold    = selText ? g_edObjs[g_edSel].bold    : g_edBold;
            bool nItalic  = selText ? g_edObjs[g_edSel].italic  : g_edItalic;
            int  nAlign   = selText ? g_edObjs[g_edSel].align   : g_edAlign;
            int  nOutline = selText ? g_edObjs[g_edSel].outline : g_edOutline;
            switch (r->what) {
            case EdHit::Size:   nSize   = EdSizeStep(nSize, r->idx ? 1 : -1); break;
            case EdHit::Bold:   nBold   = !nBold;   break;
            case EdHit::Italic: nItalic = !nItalic; break;
            case EdHit::Align:  nAlign  = r->idx;   break;
            default:            nOutline = r->idx;  break;
            }
            if (selText) {
                const EdObj& cur = g_edObjs[g_edSel];
                if (cur.size != nSize || cur.bold != nBold || cur.italic != nItalic ||
                    cur.align != nAlign || cur.outline != nOutline) {
                    EdPushUndo();
                    EdObj& o = g_edObjs[g_edSel];
                    o.size = nSize; o.bold = nBold; o.italic = nItalic;
                    o.align = nAlign; o.outline = nOutline;
                    EdTextMeasure(o);   // кегль і накреслення міняють габарити
                }
            }
            // Типові значення йдуть слідом за вибраним — як і в товщині: те, що
            // щойно налаштували, дістається наступному напису.
            g_edSize = nSize; g_edBold = nBold; g_edItalic = nItalic;
            g_edAlign = nAlign; g_edOutline = nOutline;
            EdLayout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case EdHit::Opacity:
            if ((g_edGroupEdit && EdCounterKind()) ||
                (g_edSel >= 0 && g_edSel < (int)g_edObjs.size())) EdPushUndo();
            g_edDrag = EdDrag::Slider;
            SetCapture(hwnd);
            EdSetAlphaAt(pt.x);
            return 0;
        case EdHit::SelAlign:
            EdAlignSel(r->idx);
            return 0;
        case EdHit::SelGroup:
            EdGroupSel(r->idx == 0);
            return 0;
        case EdHit::Pick:
            g_edPickOpen = (g_edPickOpen == r->idx) ? -1 : r->idx;
            EdLayout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case EdHit::PickItem:
            EdPickApply(g_edPickOpen, r->idx);
            g_edPickOpen = -1;
            EdLayout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case EdHit::GroupEdit:
            g_edGroupEdit = !g_edGroupEdit;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case EdHit::GroupDel:
            EdGroupDelete();
            return 0;
        case EdHit::Share:   if (!g_edCropping) EdShareNow(hwnd); return 0;
        case EdHit::SizeImg: EdSizeDialog(hwnd, false); return 0;
        case EdHit::SizeCan: EdSizeDialog(hwnd, true);  return 0;
        case EdHit::RotL:  EdRotateBy(false); return 0;
        case EdHit::RotR:  EdRotateBy(true);  return 0;
        case EdHit::FlipH: EdMirrorBy(true);  return 0;
        case EdHit::FlipV: EdMirrorBy(false); return 0;
        case EdHit::ToneReset: EdToneReset(); return 0;
        case EdHit::Exposure:
        case EdHit::Gamma:
        case EdHit::Contrast:
            g_edToneWhat = r->what;
            g_edTonePushed = false;
            g_edDrag = EdDrag::Tone;
            SetCapture(hwnd);
            EdSetToneAt(r->what, pt.x);
            return 0;
        case EdHit::Compare:
            // Вихідний кадр будуємо один раз на натискання: той самий рецепт
            // без тону. Тримати його постійно означало б другу копію 4K у пам'яті.
            if (EdToneDefault()) return 0;
            delete g_edCmp;
            g_edCmp = EdBuildWorking(false);
            g_edCompare = true;
            g_edDrag = EdDrag::Compare;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case EdHit::Min:   ShowWindow(hwnd, SW_MINIMIZE); return 0;
        case EdHit::Max:   ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); return 0;
        case EdHit::Close: SendMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
        case EdHit::Undo:  EdUndoAction(); return 0;
        case EdHit::Redo:  EdRedoAction(); return 0;
        case EdHit::Help:  MessageBoxW(hwnd, S(Str::EdHelpBody), S(Str::EdHelpTitle),
                                       MB_OK | MB_ICONINFORMATION); return 0;
        case EdHit::Dup:   EdDuplicateSel(); return 0;
        case EdHit::Front: EdRaise(true);  return 0;
        case EdHit::Back:  EdRaise(false); return 0;
        case EdHit::Del:   EdDeleteSel();  return 0;
        case EdHit::Zoom:
            g_edDrag = EdDrag::Zoom;
            SetCapture(hwnd);
            EdSetZoomAt(pt.x);
            return 0;
        case EdHit::Zoom100: EdZoomHundred(); return 0;
        case EdHit::Fit:   EdFitView(); return 0;
        // Доки кадр не підтверджено, виходи мовчать: незрозуміло, що саме вони
        // мали б віддати — кадр чи весь знімок.
        case EdHit::Copy:  if (!g_edCropping) EdDoCopy();  return 0;
        case EdHit::Open:  if (!g_edCropping) EdOpenFileHere(hwnd); return 0;
        case EdHit::OpenMenu: {
            if (g_edCropping) return 0;
            const RECT* ob = EdRegionRect(EdHit::Open, 0);
            if (ob) EdOpenMenu(hwnd, *ob);
            return 0;
        }
        case EdHit::Save:  if (!g_edCropping) EdDoSave();  return 0;
        case EdHit::Panel:
            g_edPanelOpen = !g_edPanelOpen;
            EdLayout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case EdHit::Canvas: break;
        default: return 0;
        }

        if (!g_edImg) return 0;

        // Режим кадру забирає полотно собі: ручки, тягнення рамки, і нічого
        // більше — жодних нових позначок, доки кадр не підтверджено.
        if (g_edCropping) {
            RECT hs[8];
            const int hn = EdCropHandles(hs);
            for (int i = 0; i < hn; ++i) {
                if (PtInRect(&hs[i], pt)) {
                    g_edDrag = EdDrag::Crop;
                    g_edHandle = i;
                    g_edCropOrig = g_edCropEdit;
                    SetCapture(hwnd);
                    return 0;
                }
            }
            const RECT cr = EdCropScreen(g_edCropEdit);
            if (PtInRect(&cr, pt)) {
                g_edDrag = EdDrag::CropMove;
                g_edDragFrom = pt;
                g_edCropOrig = g_edCropEdit;
                SetCapture(hwnd);
            }
            return 0;
        }

        if (GetKeyState(VK_SPACE) < 0) {
            g_edDrag = EdDrag::Pan;
            g_edDragFrom = pt;
            SetCapture(hwnd);
            return 0;
        }
        // ⚠ Ручки вибраного об'єкта перевіряємо ДО інструмента. Курсор над ними
        // й так показував стрілку розміру, а тягнути починало новий об'єкт:
        // обіцянка курсора розходилась із дією. Хто бачить ручку — той її тягне.
        if (EdManySel()) {
            RECT rh;
            if (EdManyRotHandle(&rh) && PtInRect(&rh, pt)) {
                EdPushUndo();
                EdManyBegin();
                const double cx = (g_edManyBox.left + g_edManyBox.right) / 2.0;
                const double cy = (g_edManyBox.top + g_edManyBox.bottom) / 2.0;
                const POINT im = EdToImage(pt);
                g_edManyAng0 = atan2(im.x - cx, cy - im.y) * 180.0 / 3.14159265358979;
                g_edDrag = EdDrag::ManyRotate;
                SetCapture(hwnd);
                return 0;
            }
            RECT hs[8];
            const int hn = EdManyHandles(hs);
            for (int i = 0; i < hn; ++i) {
                if (PtInRect(&hs[i], pt)) {
                    EdPushUndo();
                    EdManyBegin();
                    g_edHandle = i;
                    g_edDrag = EdDrag::ManyResize;
                    SetCapture(hwnd);
                    return 0;
                }
            }
        }
        if (!EdManySel() && g_edSel >= 0 && g_edSel < (int)g_edObjs.size()) {
            RECT rh;
            if (EdRotHandle(g_edObjs[g_edSel], &rh) && PtInRect(&rh, pt)) {
                EdPushUndo();
                g_edDrag = EdDrag::Rotate;
                g_edDragOrig = g_edObjs[g_edSel];
                SetCapture(hwnd);
                return 0;
            }
            RECT hs[8];
            const int hn = EdHandles(g_edObjs[g_edSel], hs);
            for (int i = 0; i < hn; ++i) {
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
        if (g_edTool == EdTool::Text) {
            EdTextBegin(hwnd, EdToImage(pt), -1);
            return 0;
        }
        // Лічильник і штамп ставляться одним кліком: рамку для них тягнути
        // нема сенсу, розмір у них із набору.
        if (EdIsStamped(EdToolKind(g_edTool))) {
            const POINT img = EdToImage(pt);
            EdObj o = EdObj{};
            o.kind  = EdToolKind(g_edTool);
            o.thick = g_edStampSize;
            o.w = o.h = g_edStampSize;
            o.x = img.x - g_edStampSize / 2;
            o.y = img.y - g_edStampSize / 2;
            o.color = g_edColor;
            o.alpha = g_edAlpha;
            o.stamp = g_edStamp;
            if (o.kind == EdKind::Counter) {
                o.group = EdCurGroup();
                o.start = EdGroupStart(o.group);
                o.seq   = ++g_edSeq;
                g_edCounterGroup = o.group;
            }
            EdPushUndo();
            g_edObjs.push_back(o);
            g_edSel = (int)g_edObjs.size() - 1;
            if (!g_edKeepTool) g_edTool = EdTool::Select;
            EdLayout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_edTool != EdTool::Select) {
            const POINT img = EdToImage(pt);
            g_edNew = EdObj{};
            g_edNew.kind = EdToolKind(g_edTool);
            g_edNew.x = img.x; g_edNew.y = img.y; g_edNew.w = 0; g_edNew.h = 0;
            g_edNew.color = g_edColor;
            g_edNew.thick = g_edThick;
            g_edNew.alpha = g_edAlpha;
            g_edNew.filled = EdCanFill(g_edNew.kind) ? g_edFill : false;
            g_edNew.mode = g_edHideMode;
            g_edNew.strength = g_edStrength;
            g_edNew.dash = g_edDash;
            g_edNew.headFront = g_edHeadFront;
            g_edNew.headBack = g_edHeadBack;
            g_edNew.headSize = g_edHeadSize;
            g_edNewOrigin = img;
            if (g_edNew.kind == EdKind::Mark) {
                // Смуга маркера має сталу висоту й тягнеться лише вшир, тому
                // верх і низ визначено вже тут, на натиску.
                g_edNew.color = g_edMarkColor;
                g_edNew.thick = g_edMarkH;
                g_edNew.h = g_edMarkH;
                g_edNew.y = img.y - g_edMarkH / 2;
                g_edNew.w = 0;
            }
            if (g_edNew.kind == EdKind::Pen) g_edNew.pts.push_back(img);
            g_edDrag = EdDrag::New;
            SetCapture(hwnd);
            return 0;
        }
        // Ручки вже перевірено вище; лишилось підняти сам об'єкт зверху вниз.
        {
            const int hit = EdPick(pt);
            if (wp & MK_SHIFT) {
                // Shift додає до вибору й прибирає з нього. Модифікатор беремо
                // з повідомлення — так само, як усюди після CAPS-46.
                if (hit >= 0) {
                    EdSelToggle(hit);
                    EdLayout(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (hit >= 0 && EdIsSelected(hit) && EdManySel()) {
                // Клік по вже вибраному в групі не збиває вибір — інакше
                // групу неможливо було б потягнути.
            } else if (hit != g_edSel || EdManySel()) {
                EdSelectOne(hit);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
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
            bool keep;
            if (o.kind == EdKind::Pen)          keep = o.pts.size() >= 2;
            else if (EdIsSegment(o.kind))       keep = (o.w * o.w + o.h * o.h) >= 36;
            else if (o.kind == EdKind::Mark)    keep = (o.w >= 6);
            else                                keep = (o.w >= 3 && o.h >= 3);
            if (keep) {
                EdPushUndo();
                g_edObjs.push_back(o);
                g_edSel = (int)g_edObjs.size() - 1;
                // Інструмент лишається активним: зазвичай далі малюють ще одну
                // позначку. Перемикається чекбоксом у налаштуваннях.
                if (!g_edKeepTool) g_edTool = EdTool::Select;
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
        if (g_edDrag == EdDrag::ManyResize || g_edDrag == EdDrag::ManyRotate) {
            // Клік по ручці без руху — теж не привід для кроку скасування.
            bool same = !g_edUndo.empty();
            if (same) {
                const EdSnap& prev = g_edUndo.back();
                for (size_t i = 0; same && i < g_edManyIdx.size(); ++i) {
                    const int k = g_edManyIdx[i];
                    if (k < 0 || k >= (int)g_edObjs.size() || k >= (int)prev.objs.size()) { same = false; break; }
                    const EdObj& a = prev.objs[k];
                    const EdObj& b = g_edObjs[k];
                    if (a.x != b.x || a.y != b.y || a.w != b.w || a.h != b.h || a.rot != b.rot) same = false;
                }
            }
            if (same) g_edUndo.pop_back();
            g_edManyOrig.clear();
            g_edManyIdx.clear();
        }
        if (g_edDrag == EdDrag::Compare) {
            g_edCompare = false;
            delete g_edCmp;
            g_edCmp = nullptr;
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
        if (!PtInRect(&g_edRcCanvas, pt)) {
            // Подвійний клік по кольору чи розміру — на ВСЮ групу лічильника.
            // Перший клік із пари вже застосувався до одного кружечка, тож цей
            // лише добирає решту; окремий крок скасування для нього не потрібен,
            // бо EdGroupApply кладе знімок сам і лише коли є що міняти.
            const EdRegion* dr = EdFind(pt);
            if (dr && EdCounterKind()) {
                if (dr->what == EdHit::Swatch) {
                    int np = 8;
                    const COLORREF* pl = EdPaletteFor(EdKind::Counter, np);
                    if (dr->idx >= 0 && dr->idx < np) EdGroupApply(0, (int)pl[dr->idx]);
                } else if (dr->what == EdHit::Thick && dr->idx >= 0 && dr->idx < 3) {
                    EdGroupApply(1, EdThickSet(EdKind::Counter)[dr->idx]);
                }
            }
            return 0;
        }
        const int hit = EdPick(pt);
        // Подвійний клік по напису відкриває його на правку — хоч через годину
        // після створення. По порожньому місцю — вписує зображення, як і було.
        if (hit >= 0 && g_edObjs[hit].kind == EdKind::Text) {
            g_edSel = hit;
            EdTextBegin(hwnd, EdToImage(pt), hit);
        } else if (hit < 0) {
            EdFitView();
        }
        return 0;
    }

    case WMAPP_EDTEXT:
        if (wp) EdTextCommit(); else EdTextCancel();
        return 0;

    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == kEdEditId) { EdTextFitBox(); return 0; }
        break;

    // ⚠ Коліщатко ПРОКРУЧУЄ, а не зумить (зауваження власника 21.09). Зум лишився
    // на повзунку й на Alt+коліщатко: спроба «докрутити до краю» не має міняти
    // масштаб — це найчастіший жест і найгірша несподіванка.
    case WM_MOUSEWHEEL: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        if (!PtInRect(&g_edRcCanvas, pt)) return 0;
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (GetKeyState(VK_MENU) < 0) { EdZoomAt(pt, delta > 0); return 0; }
        // Ctrl — для мишей без горизонтального коліщатка; Shift робить те саме,
        // бо в решті Windows горизонтальна прокрутка живе саме на ньому.
        // ⚠ Модифікатори беремо з wParam, а не з GetKeyState: вони ПРИХОДЯТЬ
        // разом із повідомленням, тож стан клавіатури тут ні до чого — і саме
        // тому це можна перевірити харнесом, який шле повідомлення.
        const bool horz = (LOWORD(wp) & (MK_CONTROL | MK_SHIFT)) != 0;
        const int step = EdWheelStep(delta);
        if (horz) EdScrollBy(step, 0);
        else      EdScrollBy(0, step);
        return 0;
    }

    // Горизонтальне коліщатко: нахил праворуч дає ДОДАТНУ дельту, а видима
    // картинка при цьому має їхати вліво — звідси мінус.
    case WM_MOUSEHWHEEL: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        if (!PtInRect(&g_edRcCanvas, pt)) return 0;
        EdScrollBy(-EdWheelStep(GET_WHEEL_DELTA_WPARAM(wp)), 0);
        return 0;
    }

    case WM_KEYDOWN: {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        switch (wp) {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: {
            // Стрілки належать ПОЗНАЧЦІ, а не полотну: полотно рухають
            // коліщатко й середня кнопка (CAPS-46), і ділити з ними клавіші
            // означало б два різні рухи на тих самих кнопках.
            if (g_edCropping || g_edEdit) return 0;
            if (g_edSel < 0 || g_edSel >= (int)g_edObjs.size()) return 0;
            const int step = (GetKeyState(VK_SHIFT) < 0) ? 10 : 1;
            const int dx = (wp == VK_LEFT) ? -step : (wp == VK_RIGHT) ? step : 0;
            const int dy = (wp == VK_UP)   ? -step : (wp == VK_DOWN)  ? step : 0;
            EdNudgeSel(dx, dy);
            return 0;
        }
        case 'Z': if (ctrl) EdUndoAction(); return 0;
        case 'Y': if (ctrl) EdRedoAction(); return 0;
        case 'D': if (ctrl) EdDuplicateSel(); return 0;
        case VK_RETURN:
            if (g_edCropping) { EdCropFinish(true); return 0; }
            if (g_edLastAction == 1) EdDoSave(); else EdDoCopy();
            return 0;
        case 'V':
            if (ctrl) {
                // Ctrl+V вставляє ОКРЕМИМ ОБ'ЄКТОМ (рішення власника 21.09).
                // Заміна всього вмісту лишилась у списку біля «Відкрити»:
                // це різні наміри, і плутати їх однією клавішею не можна.
                if (g_edCropping) return 0;
                if (Gdiplus::Bitmap* b = CapFromClipboard()) {
                    POINT c = EdCanvasCentre();
                    EdPlaceImage(b, EdToImage(c));
                } else {
                    MessageBoxW(hwnd, S(Str::EdErrOpen), kAppName, MB_OK | MB_ICONWARNING);
                }
                return 0;
            }
            if (g_edCropping) EdCropFinish(false);
            g_edTool = EdTool::Select;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case 'O':
            // Літера більше не інструмент — лише Ctrl+O, і лише «Відкрити».
            if (ctrl && !g_edCropping) EdOpenFileHere(hwnd);
            return 0;
        case 'C':
            // ⚠ Як і 'S', літера носить дві ролі: сама по собі — кадр, із Ctrl —
            // копіювання. Окремий case для Ctrl дав би «duplicate case value».
            if (ctrl) { if (!g_edCropping) EdDoCopy(); return 0; }
            g_edTool = EdTool::Crop;
            EdCropBegin();
            EdLayout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        // ⚠ Мітки й тіло мають лишатися разом. Коли між ними вклинився ще один
        // case, КОЖНА літера інструмента почала відкривати кадр — і помітно це
        // стало лише на знімку харнеса.
        case 'R': case 'E': case 'L': case 'P': case 'T': case 'B': case 'H':
        case 'N': case 'S':
            // ⚠ 'S' носить дві ролі: сам по собі — штамп, із Ctrl — збереження.
            // ⚠ Еліпс переїхав із 'O' на 'E' саме для того, щоб звільнити Ctrl+O
            // під «Відкрити»: раніше Ctrl+O потрапляв сюди й мовчки з'їдався.
            if (ctrl) {
                if (wp == 'S') EdDoSave();
                return 0;
            }
            if (g_edCropping) EdCropFinish(false);
            {
                g_edTool = (wp == 'R') ? EdTool::Rect : (wp == 'E') ? EdTool::Ellipse
                         : (wp == 'L') ? EdTool::Line
                         : (wp == 'T') ? EdTool::Text : (wp == 'B') ? EdTool::Hide
                         : (wp == 'H') ? EdTool::Mark : (wp == 'N') ? EdTool::Counter
                         : (wp == 'S') ? EdTool::Stamp : EdTool::Pen;
                g_edSel = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case VK_DELETE: EdDeleteSel(); return 0;
        case VK_ESCAPE:
            // Esc знімає рівно один шар за раз: спершу розкритий селект, потім
            // кадр, потім вибір, потім інструмент — і лише тоді закриває вікно.
            if (g_edPickOpen >= 0) {
                g_edPickOpen = -1;
                EdLayout(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_edCropping) { EdCropFinish(false); return 0; }
            if (g_edSel >= 0) { g_edSel = -1; InvalidateRect(hwnd, nullptr, FALSE); }
            else if (g_edTool != EdTool::Select) { g_edTool = EdTool::Select; InvalidateRect(hwnd, nullptr, FALSE); }
            else if (EdConfirmClose()) DestroyWindow(hwnd);
            return 0;
        case VK_F1:
            MessageBoxW(hwnd, S(Str::EdHelpBody), S(Str::EdHelpTitle), MB_OK | MB_ICONINFORMATION);
            return 0;
        default: break;
        }
        return 0;
    }

    case WM_CLOSE:
        if (!EdConfirmClose()) return 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_TIMER:
        // ⚠ Поширення або спрацює, або промовчить — третього не буває, і саме
        // мовчання коштувало двох релізів наосліп. Через чотири секунди після
        // показу меню перевіряємо, чи система ВЗАГАЛІ спитала дані.
        if (wp == kEdShareTimer) {
            KillTimer(hwnd, kEdShareTimer);
            if (EdShareNs::g_asked == g_edShareAskedAt)
                MessageBoxW(hwnd, S(g_shareUnlock ? Str::EdErrShareStill : Str::EdErrShareQuiet),
                            kAppName, MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (wp == kEdTipTimer) {
            KillTimer(hwnd, kEdTipTimer);
            EdTipShow(hwnd);
            return 0;
        }
        if (wp == 7) {
            if ((int)(g_edToastUntil - GetTickCount()) <= 0) {
                KillTimer(hwnd, 7);
                g_edToast = Str::Empty;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_DESTROY:
        EdTextCancel();
        if (g_edEmojiWnd) { DestroyWindow(g_edEmojiWnd); g_edEmojiWnd = nullptr; }
        KillTimer(hwnd, kEdTipTimer);
        if (g_edTip) { DestroyWindow(g_edTip); g_edTip = nullptr; }
        EdTilesClear();
        delete g_edImg; g_edImg = nullptr;
        for (size_t i = 0; i < g_edSrcBank.size(); ++i) delete g_edSrcBank[i];
        g_edSrcBank.clear();
        g_edSrc = nullptr;
        g_edSrcId = -1;
        delete g_edCmp; g_edCmp = nullptr;
        g_edCompare = false;
        EdImageBankClear();
        EdShareNs::Unhook();
        EdShareNs::CleanTemp();
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

// Редактор приймає ГОТОВИЙ бітмап і стає його власником. Через це той самий
// шлях обслуговує і файл, і знімок екрана, і буфер обміну — жодне джерело не
// має привілею.
void EdOpenBitmap(HINSTANCE hInst, Gdiplus::Bitmap* bmp, const wchar_t* label,
                  bool hdr, bool toneMapped, float sdrWhite = -1.0f)
{
    if (!bmp) return;

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

    // Редактор бере у власність ОРИГІНАЛ, а показує похідне від нього. Новий
    // знімок приходить із чистим рецептом: чужий поворот і чужий тон на ньому
    // не мали б сенсу.
    for (size_t i = 0; i < g_edSrcBank.size(); ++i) delete g_edSrcBank[i];
    g_edSrcBank.clear();
    g_edSrcBank.push_back(bmp);
    g_edSrcId = 0;
    g_edSrc  = bmp;
    g_edExposure = 0; g_edGamma = 100; g_edContrast = 0;
    g_edRot = 0; g_edMirror = false; g_edCompare = false;
    delete g_edCmp; g_edCmp = nullptr;
    EdImageBankClear();          // новий знімок — нові вкладені зображення
    EdRebuildImage();
    g_edHdr        = hdr;
    g_edToneMapped = toneMapped;
    g_edSdrWhite   = sdrWhite;
    g_edSaved      = false;
    g_edToast      = Str::Empty;
    lstrcpynW(g_edSource, label ? label : L"", MAX_PATH);
    g_edObjs.clear();
    g_edUndo.clear();
    g_edRedo.clear();
    EdSelClear();
    g_edNextGrp = 1;
    g_edTool = EdTool::Select;
    g_edZoom = 1.0f;
    g_edPanX = g_edPanY = 0;
    g_edPanelOpen = true;
    g_edCrop = RECT{ 0, 0, 0, 0 };      // новий знімок приходить без кадру
    g_edCropping = false;
    g_edCounterGroup = 0;               // новий знімок — нумерація з початку сама
    g_edSeq = 0;

    if (g_edWnd) {                      // уже відкрите — просто новий вміст
        EdFitView();
        EdLayout(g_edWnd);
        InvalidateRect(g_edWnd, nullptr, TRUE);
        ShowWindow(g_edWnd, SW_RESTORE);
        SetForegroundWindow(g_edWnd);
        return;
    }

    if (!g_edIcon)
        g_edIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                     LR_DEFAULTCOLOR | LR_SHARED);

    wchar_t caption[160];
    wsprintfW(caption, L"%s — %s", S(Str::EdTitle), kAppName);

    const int dpi = (int)GetDpiForSystem();
    const int want = MulDiv(1320, dpi, 96), wantH = MulDiv(760, dpi, 96);
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
        delete g_edImg; g_edImg = nullptr;
        for (size_t i = 0; i < g_edSrcBank.size(); ++i) delete g_edSrcBank[i];
        g_edSrcBank.clear();
        g_edSrc = nullptr;
        g_edSrcId = -1;
        return;
    }
    ShowWindow(g_edWnd, SW_SHOW);
    SetForegroundWindow(g_edWnd);
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
    EdOpenBitmap(hInst, bmp, PathFindFileNameW(path), false, false);
}

// Активне вікно на момент знімка. Якщо попереду наше власне (меню трею саме
// його й піднімає), беремо наступне чуже за порядком Z.
HWND CapForegroundTarget()
{
    DWORD me = GetCurrentProcessId();
    HWND h = GetForegroundWindow();
    for (int guard = 0; h && guard < 40; ++guard) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != me && IsWindowVisible(h) && !IsIconic(h)) {
            RECT rc = {};
            GetWindowRect(h, &rc);
            if (rc.right - rc.left > 16 && rc.bottom - rc.top > 16) return h;
        }
        h = GetWindow(h, GW_HWNDNEXT);
    }
    return nullptr;
}

// ---- CAPS-22: виходи — буфер обміну і файл ------------------------------
// Дві рівноправні дії. «Рівноправні» тут не гасло, а вимога до коду: жодна з
// них не є гілкою іншої, обидві беруть один і той самий растеризований кадр,
// і програма лише ЗАПАМ'ЯТОВУЄ, якою користувалися востаннє, щоб підсвітити її
// як дію для Enter.

const wchar_t* kRegEdLast     = L"EditorLastAction";     // 0 буфер, 1 файл
const wchar_t* kRegEdKeepTool = L"EditorKeepTool";       // 1 = інструмент лишається
const wchar_t* kRegEdSaveDir  = L"EditorSaveDir";

bool EdEncoderClsid(const wchar_t* mime, CLSID* out)
{
    UINT n = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&n, &size);
    if (!size) return false;
    std::vector<BYTE> buf(size);
    Gdiplus::ImageCodecInfo* info = (Gdiplus::ImageCodecInfo*)buf.data();
    if (Gdiplus::GetImageEncoders(n, size, info) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < n; ++i)
        if (!lstrcmpW(info[i].MimeType, mime)) { *out = info[i].Clsid; return true; }
    return false;
}

// Растеризація. Об'єкти малюються в РОЗМІРІ ЗОБРАЖЕННЯ, а не екрана: те, що
// видно на екрані, — лише перегляд у масштабі, а йде у файл завжди повний кадр.
Gdiplus::Bitmap* EdRender()
{
    if (!g_edImg) return nullptr;
    // У файл іде КАДР, а не весь знімок — і рівно те, що видно на полотні.
    const int vw = EdViewW(), vh = EdViewH();
    Gdiplus::Bitmap* out = new Gdiplus::Bitmap(vw, vh, PixelFormat32bppPARGB);
    if (!out || out->GetLastStatus() != Gdiplus::Ok) { delete out; return nullptr; }
    Gdiplus::Graphics g(out);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
    g.DrawImage(g_edImg, Gdiplus::Rect(0, 0, vw, vh),
                EdViewX(), EdViewY(), vw, vh, Gdiplus::UnitPixel);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    for (size_t i = 0; i < g_edObjs.size(); ++i)
        EdDrawObject(g, g_edObjs[i], 1.0, -EdViewX(), -EdViewY(), (int)i);
    return out;
}

// CF_DIB: знизу вгору, 32 біти, BI_RGB. Альфа в цьому форматі нічия — більшість
// програм її ігнорує, — тому байт альфи ставимо 255 і не сподіваємось на нього.
HGLOBAL EdDibGlobal(Gdiplus::Bitmap* bmp)
{
    const int w = (int)bmp->GetWidth(), h = (int)bmp->GetHeight();
    Gdiplus::BitmapData bd;
    Gdiplus::Rect all(0, 0, w, h);
    if (bmp->LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bd) != Gdiplus::Ok)
        return nullptr;
    const SIZE_T bits = (SIZE_T)w * h * 4;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + bits);
    if (mem) {
        BYTE* p = (BYTE*)GlobalLock(mem);
        BITMAPINFOHEADER* bi = (BITMAPINFOHEADER*)p;
        ZeroMemory(bi, sizeof(*bi));
        bi->biSize = sizeof(BITMAPINFOHEADER);
        bi->biWidth = w;
        bi->biHeight = h;                 // додатна = знизу вгору
        bi->biPlanes = 1;
        bi->biBitCount = 32;
        bi->biCompression = BI_RGB;
        bi->biSizeImage = (DWORD)bits;
        BYTE* dst = p + sizeof(BITMAPINFOHEADER);
        for (int y = 0; y < h; ++y) {
            const BYTE* src = (const BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
            BYTE* row = dst + (size_t)(h - 1 - y) * w * 4;
            for (int x = 0; x < w; ++x) {
                row[x * 4 + 0] = src[x * 4 + 0];
                row[x * 4 + 1] = src[x * 4 + 1];
                row[x * 4 + 2] = src[x * 4 + 2];
                row[x * 4 + 3] = 255;
            }
        }
        GlobalUnlock(mem);
    }
    bmp->UnlockBits(&bd);
    return mem;
}

HGLOBAL EdPngGlobal(Gdiplus::Bitmap* bmp)
{
    CLSID png;
    if (!EdEncoderClsid(L"image/png", &png)) return nullptr;
    IStream* st = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, FALSE, &st)) || !st) return nullptr;
    HGLOBAL mem = nullptr;
    if (bmp->Save(st, &png, nullptr) == Gdiplus::Ok) {
        HGLOBAL src = nullptr;
        if (SUCCEEDED(GetHGlobalFromStream(st, &src)) && src) {
            const SIZE_T n = GlobalSize(src);
            mem = GlobalAlloc(GMEM_MOVEABLE, n);
            if (mem) {
                void* d = GlobalLock(mem);
                const void* s = GlobalLock(src);
                if (d && s) memcpy(d, s, n);
                if (s) GlobalUnlock(src);
                if (d) GlobalUnlock(mem);
            }
        }
    }
    st->Release();
    return mem;
}

void EdToast(Str s)
{
    g_edToast = s;
    g_edToastUntil = GetTickCount() + 2200;
    if (g_edWnd) {
        SetTimer(g_edWnd, 7, 400, nullptr);
        InvalidateRect(g_edWnd, nullptr, FALSE);
    }
}

bool EdCopy()
{
    Gdiplus::Bitmap* flat = EdRender();
    if (!flat) return false;

    // Три формати одразу: PNG для месенджерів і браузерів, CF_DIB для Office,
    // CF_BITMAP для найстарішого, що трапляється. Жоден із них поодинці не
    // приймається скрізь.
    HGLOBAL png = EdPngGlobal(flat);
    HGLOBAL dib = EdDibGlobal(flat);
    HBITMAP ddb = nullptr;
    flat->GetHBITMAP(Gdiplus::Color(255, 255, 255, 255), &ddb);
    delete flat;

    bool ok = false;
    if (OpenClipboard(g_edWnd)) {
        EmptyClipboard();
        const UINT pngFmt = CapClipboardPngFormat();
        if (png && pngFmt && SetClipboardData(pngFmt, png)) { png = nullptr; ok = true; }
        if (dib && SetClipboardData(CF_DIB, dib))           { dib = nullptr; ok = true; }
        if (ddb && SetClipboardData(CF_BITMAP, ddb))        { ddb = nullptr; ok = true; }
        CloseClipboard();
    }
    if (png) GlobalFree(png);
    if (dib) GlobalFree(dib);
    if (ddb) DeleteObject(ddb);
    return ok;
}

void EdSaveDirRemember(const wchar_t* path)
{
    wchar_t dir[MAX_PATH] = {};
    lstrcpynW(dir, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(dir, L'\\')) {
        *slash = 0;
        HKEY key;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                            KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(key, kRegEdSaveDir, 0, REG_SZ, (const BYTE*)dir,
                           (DWORD)((lstrlenW(dir) + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
        }
    }
}

// Єдине місце, де редактор пише файл. Діалог лише добуває шлях: так те, що
// зберігається, не може розійтися з тим, що показано, і те саме можна
// перевірити без діалогу взагалі.
bool EdWriteFile(const wchar_t* path)
{
    if (!path || !*path) return false;
    bool ok = false;
    if (Gdiplus::Bitmap* flat = EdRender()) {
        const wchar_t* ext = wcsrchr(path, L'.');
        const bool jpg = ext && (!lstrcmpiW(ext, L".jpg") || !lstrcmpiW(ext, L".jpeg"));
        CLSID enc;
        if (EdEncoderClsid(jpg ? L"image/jpeg" : L"image/png", &enc)) {
            // ⚠ JPEG альфи не має, і прозоре тло стало б чорним. Кладемо кадр
            // на біле: це єдиний колір, який у такому випадку нікого не дивує.
            Gdiplus::Bitmap* opaque = nullptr;
            if (jpg) {
                opaque = new Gdiplus::Bitmap((INT)flat->GetWidth(), (INT)flat->GetHeight(),
                                             PixelFormat32bppPARGB);
                if (opaque && opaque->GetLastStatus() == Gdiplus::Ok) {
                    Gdiplus::Graphics g(opaque);
                    Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
                    g.FillRectangle(&white, 0, 0, (INT)flat->GetWidth(), (INT)flat->GetHeight());
                    g.DrawImage(flat, 0, 0, (INT)flat->GetWidth(), (INT)flat->GetHeight());
                    delete flat;
                    flat = opaque;
                } else { delete opaque; }
            }
            if (jpg) {
                // Якість фіксована 92: помітної втрати ще нема, а повзунок у
                // системному діалозі не поставиш — окремий контроль буде в
                // налаштуваннях.
                ULONG q = 92;
                Gdiplus::EncoderParameters ep;
                ep.Count = 1;
                ep.Parameter[0].Guid = Gdiplus::EncoderQuality;
                ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
                ep.Parameter[0].NumberOfValues = 1;
                ep.Parameter[0].Value = &q;
                ok = flat->Save(path, &enc, &ep) == Gdiplus::Ok;
            } else {
                ok = flat->Save(path, &enc, nullptr) == Gdiplus::Ok;
            }
        }
        delete flat;
    }
    if (ok) EdSaveDirRemember(path);
    else MessageBoxW(g_edWnd, S(Str::EdErrSave), kAppName, MB_OK | MB_ICONWARNING);
    return ok;
}

// CAPS-36. Знімок лягає в %TEMP% і звідти йде системному брокеру. Тимчасовий
// файл прибираємо перед наступним поширенням і на закритті редактора: це знімок
// екрана користувача, він не має лежати там вічно.
void EdShareNow(HWND hwnd)
{
    EdShareNs::CleanTemp();

    wchar_t dir[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, dir)) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    wsprintfW(path, L"%slilhelpers-%04d%02d%02d-%02d%02d%02d.png", dir,
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    bool wrote = false;
    if (Gdiplus::Bitmap* flat = EdRender()) {
        CLSID enc;
        if (EdEncoderClsid(L"image/png", &enc))
            wrote = (flat->Save(path, &enc, nullptr) == Gdiplus::Ok);
        delete flat;
    }
    if (!wrote) {
        MessageBoxW(hwnd, S(Str::EdErrSave), kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    EdShareNs::g_tempFile = path;

    // ⚠ Файл і потік добуваємо ЗАРАЗ, поки можна чекати: обробник запиту
    // мусить бути синхронним, інакше довелося б доведення (deferral).
    ABI::Windows::Storage::IStorageFile* sfile = EdShareNs::OpenStorageFile(path);
    ABI::Windows::Storage::Streams::IRandomAccessStreamReference* sref =
        EdShareNs::StreamRef(sfile, path);
    if (!sfile && !sref) {
        if (sfile) sfile->Release();
        MessageBoxW(hwnd, S(Str::EdErrShare), kAppName, MB_OK | MB_ICONWARNING);
        return;
    }

    IDataTransferManagerInterop* it = EdShareNs::Interop();
    if (!it) {
        if (sfile) sfile->Release();
        if (sref) sref->Release();
        MessageBoxW(hwnd, S(Str::EdErrShare), kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    EdShareNs::Unhook();
    ABI::Windows::ApplicationModel::DataTransfer::IDataTransferManager* dtm = nullptr;
    HRESULT hr = it->GetForWindow(hwnd, __uuidof(ABI::Windows::ApplicationModel::DataTransfer::IDataTransferManager),
                                  (void**)&dtm);
    if (SUCCEEDED(hr) && dtm) {
        EdShareNs::Handler* h = new EdShareNs::Handler();
        h->title = S(Str::EdTitle);
        h->file  = sfile;                   // посилання переходять до обробника
        h->ref   = sref;
        sfile = nullptr;
        sref = nullptr;
        hr = dtm->add_DataRequested(h, &EdShareNs::g_tok);
        h->Release();                       // тримає тепер система
        if (SUCCEEDED(hr)) {
            EdShareNs::g_dtm = dtm;         // знімемо реєстрацію наступного разу
            g_edShareAskedAt = EdShareNs::g_asked;
            hr = it->ShowShareUIForWindow(hwnd);
            if (SUCCEEDED(hr)) SetTimer(hwnd, kEdShareTimer, 4000, nullptr);
        } else {
            dtm->Release();
        }
    }
    it->Release();
    if (sfile) sfile->Release();
    if (sref) sref->Release();
    if (FAILED(hr)) MessageBoxW(hwnd, S(Str::EdErrShare), kAppName, MB_OK | MB_ICONWARNING);
}

bool EdSaveAs()
{
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                kIID_IFileSaveDialog, (void**)&dlg)) || !dlg)
        return false;

    COMDLG_FILTERSPEC fs[2];
    fs[0].pszName = S(Str::EdFmtPng);
    fs[0].pszSpec = L"*.png";
    fs[1].pszName = S(Str::EdFmtJpg);
    fs[1].pszSpec = L"*.jpg";
    dlg->SetFileTypes(2, fs);
    dlg->SetFileTypeIndex(1);
    dlg->SetDefaultExtension(L"png");
    dlg->SetTitle(S(Str::EdSaveTitle));

    // Ім'я за шаблоном: дата й час у назві роблять теку зі знімками
    // впорядкованою самі собою.
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[160];
    wsprintfW(name, L"%s %04d-%02d-%02d %02d-%02d-%02d", S(Str::EdSaveName),
              t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    dlg->SetFileName(name);

    wchar_t dir[MAX_PATH] = {};
    DWORD cb = sizeof(dir);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, kRegEdSaveDir, RRF_RT_REG_SZ,
                     nullptr, dir, &cb) == ERROR_SUCCESS && dir[0]) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(dir, nullptr, kIID_IShellItem, (void**)&folder)) && folder) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }

    bool ok = false;
    if (SUCCEEDED(dlg->Show(g_edWnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                ok = EdWriteFile(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

void EdDoCopy()
{
    if (!EdCopy()) {
        MessageBoxW(g_edWnd, S(Str::EdErrCopy), kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    g_edSaved = true;
    g_edLastAction = 0;
    RegSaveInt(kRegEdLast, 0);
    // Вікно НЕ закриваємо: скопіювати — не означає закінчити. Часто далі
    // домальовують ще одну позначку й копіюють знову.
    EdToast(Str::EdCopied);
}

void EdDoSave()
{
    if (!EdSaveAs()) return;      // скасування діалогу помилкою не є
    g_edSaved = true;
    g_edLastAction = 1;
    RegSaveInt(kRegEdLast, 1);
    EdToast(Str::EdSaved);
}

// Esc і закриття: питаємо лише тоді, коли є що втрачати.
bool EdConfirmClose()
{
    // Порожній список позначок ще не означає «нічого не зроблено»: поворот
    // і тон — теж робота, і втрачати їх мовчки не можна.
    if (g_edSaved || (g_edObjs.empty() && EdToneDefault() && EdGeomDefault())) return true;
    return MessageBoxW(g_edWnd, S(Str::EdAskDiscard), kAppName,
                       MB_OKCANCEL | MB_ICONQUESTION) == IDOK;
}

// ---- CAPS-21: гарячі клавіші -------------------------------------------
// Значення зберігаємо як модифікатори у старшому слові й VK у молодшому. Саме
// VK, а не символ: VK не залежить від розкладки, а програма про розкладки й є.
// Нуль означає «клавішу вимкнено».
//
// Дефолти виміряно RegisterHotKey на PLUM-MEDIA 20.09.2026 — усі три вільні.
// Уся родина Win+модифікатор+цифра належить оболонці (помилка 1409) і для нас
// недоступна в принципі.

constexpr int kHkIdClip = 11, kHkIdRegion = 12, kHkIdScreen = 13;
const wchar_t* kRegHkClip   = L"CapHotkeyClipboard";
const wchar_t* kRegHkRegion = L"CapHotkeyRegion";
const wchar_t* kRegHkScreen = L"CapHotkeyScreen";

constexpr int kHkDefClip   = (int)(((MOD_CONTROL | MOD_ALT) << 16) | '4');
constexpr int kHkDefRegion = (int)(((MOD_ALT | MOD_SHIFT) << 16) | '4');
constexpr int kHkDefScreen = (int)(((MOD_ALT | MOD_SHIFT) << 16) | '3');

int  g_hk[3]   = { kHkDefClip, kHkDefRegion, kHkDefScreen };
bool g_hkOk[3] = { false, false, false };

void CapLoadHotkeys()
{
    g_hk[0] = RegLoadInt(kRegHkClip,   kHkDefClip,   0, 0x7FFFFFFF);
    g_hk[1] = RegLoadInt(kRegHkRegion, kHkDefRegion, 0, 0x7FFFFFFF);
    g_hk[2] = RegLoadInt(kRegHkScreen, kHkDefScreen, 0, 0x7FFFFFFF);
}

void CapSaveHotkeys()
{
    RegSaveInt(kRegHkClip,   g_hk[0]);
    RegSaveInt(kRegHkRegion, g_hk[1]);
    RegSaveInt(kRegHkScreen, g_hk[2]);
}

// Повертає true, якщо всі ввімкнені клавіші зайнялись. Мовчазна невдача тут
// найгірша з можливих: користувач натискає й нічого не відбувається, а
// програма вдає, що все гаразд.
bool CapApplyHotkeys(HWND hwnd)
{
    const int ids[3] = { kHkIdClip, kHkIdRegion, kHkIdScreen };
    bool all = true;
    for (int i = 0; i < 3; ++i) {
        UnregisterHotKey(hwnd, ids[i]);
        g_hkOk[i] = false;
        if (!g_hk[i]) continue;
        const UINT mods = (UINT)(((unsigned)g_hk[i] >> 16) & 0xFFFF) | MOD_NOREPEAT;
        const UINT vk   = (UINT)(g_hk[i] & 0xFFFF);
        g_hkOk[i] = RegisterHotKey(hwnd, ids[i], mods, vk) != 0;
        if (!g_hkOk[i]) all = false;
    }
    return all;
}

// Чи вільна комбінація просто зараз. Використовує окреме приховане вікно, щоб
// не зачепити вже зареєстровані клавіші самої програми.
bool CapHotkeyFree(int packed)
{
    if (!packed) return true;
    static HWND probe = nullptr;
    if (!probe) {
        static bool reg = false;
        if (!reg) {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"lilhelpers_hkprobe";
            RegisterClassW(&wc);
            reg = true;
        }
        probe = CreateWindowExW(0, L"lilhelpers_hkprobe", L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    if (!probe) return true;
    const UINT mods = (UINT)(((unsigned)packed >> 16) & 0xFFFF) | MOD_NOREPEAT;
    const UINT vk   = (UINT)(packed & 0xFFFF);
    if (!RegisterHotKey(probe, 99, mods, vk)) return false;
    UnregisterHotKey(probe, 99);
    return true;
}

// Людський підпис комбінації для поля налаштувань.
void CapHotkeyText(int packed, wchar_t* out, int cch)
{
    if (!packed) { lstrcpynW(out, S(Str::CapHkNone), cch); return; }
    const UINT mods = (UINT)(((unsigned)packed >> 16) & 0xFFFF);
    const UINT vk   = (UINT)(packed & 0xFFFF);
    wchar_t buf[128] = {};
    if (mods & MOD_CONTROL) lstrcatW(buf, L"Ctrl + ");
    if (mods & MOD_ALT)     lstrcatW(buf, L"Alt + ");
    if (mods & MOD_SHIFT)   lstrcatW(buf, L"Shift + ");
    if (mods & MOD_WIN)     lstrcatW(buf, L"Win + ");
    wchar_t key[64] = {};
    // Ім'я клавіші беремо в системи: воно вже локалізоване й правильне для
    // не-символьних клавіш. Для цифр і літер GetKeyNameText теж дає своє.
    const UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END ||
        vk == VK_PRIOR || vk == VK_NEXT || vk == VK_LEFT || vk == VK_RIGHT ||
        vk == VK_UP || vk == VK_DOWN || vk == VK_SNAPSHOT)
        lp |= (1L << 24);
    if (!GetKeyNameTextW(lp, key, 64) || !key[0])
        wsprintfW(key, L"0x%02X", vk);
    lstrcatW(buf, key);
    lstrcpynW(out, buf, cch);
}

// ---- CAPS-21: одна точка входу для всіх способів знімка ------------------

// CapMode оголошено вище, разом із кнопкою «Відкрити» в редакторі.

// Захоплення міряє екран у ФІЗИЧНИХ пікселях, а сама програма оголошена лише
// system-DPI-aware. На однаковому DPI різниці немає, на змішаному координати
// монітора віртуалізуються і знімок поїхав би. Перемикаємо усвідомленість
// НА ЧАС захвату й повертаємо назад: так решта вікон програми лишається в тому
// самому режимі, у якому працювала завжди.
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif
struct CapDpiScope {
    DPI_AWARENESS_CONTEXT prev;
    CapDpiScope()  { prev = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); }
    ~CapDpiScope() { if (prev) SetThreadDpiAwarenessContext(prev); }
};

// ---- CAPS-21: поля перехоплення гарячих клавіш --------------------------

HWND g_capHkEdit[3] = {};
HWND g_capHkStatus  = nullptr;

void CapHkRefresh()
{
    for (int i = 0; i < 3; ++i) {
        if (!g_capHkEdit[i]) continue;
        wchar_t buf[128];
        CapHotkeyText(g_hk[i], buf, 128);
        SetWindowTextW(g_capHkEdit[i], buf);
    }
    if (!g_capHkStatus) return;
    // Один рядок стану на всі три: місця на сторінці 420 px, а окрема колонка
    // під кожним полем не вміщає жодного осмисленого тексту.
    wchar_t line[512] = {};
    const Str names[3] = { Str::CapHkClipL, Str::EdCapRegion, Str::EdCapScreen };
    for (int i = 0; i < 3; ++i) {
        const wchar_t* what = nullptr;
        if (!g_hk[i])         what = S(Str::CapHkOff);
        else if (!g_hkOk[i])  what = S(Str::CapHkTaken);
        if (!what) continue;
        if (line[0]) lstrcatW(line, L"    ");
        lstrcatW(line, S(names[i]));
        lstrcatW(line, L" — ");
        lstrcatW(line, what);
    }
    SetWindowTextW(g_capHkStatus, line);
}

void CapHkSet(int slot, int packed)
{
    const int prev = g_hk[slot];
    if (packed == prev) { CapHkRefresh(); return; }
    g_hk[slot] = packed;
    CapApplyHotkeys(g_mainWnd);
    if (packed && !g_hkOk[slot]) {       // не далась — вертаємо як було
        g_hk[slot] = prev;
        CapApplyHotkeys(g_mainWnd);
    } else {
        CapSaveHotkeys();
    }
    CapHkRefresh();
}

LRESULT CALLBACK CapHkSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR)
{
    const int slot = (int)id;
    switch (msg) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS:
        SetWindowTextW(h, S(Str::CapHkPress));
        break;
    case WM_KILLFOCUS:
        CapHkRefresh();
        break;
    case WM_CHAR:
    case WM_SYSCHAR:
        return 0;                         // щоб поле не наповнювалось літерами
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const UINT vk = (UINT)wp;
        if (vk == VK_TAB) {
            SetFocus(GetNextDlgTabItem(GetParent(h), h, GetKeyState(VK_SHIFT) < 0));
            return 0;
        }
        if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU ||
            vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL)
            return 0;                     // самі модифікатори нічого не задають
        if (vk == VK_ESCAPE) { CapHkRefresh(); return 0; }
        if (vk == VK_BACK || vk == VK_DELETE) { CapHkSet(slot, 0); return 0; }
        UINT mods = 0;
        if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
        if (GetKeyState(VK_MENU) < 0)    mods |= MOD_ALT;
        if (GetKeyState(VK_SHIFT) < 0)   mods |= MOD_SHIFT;
        if (!mods) return 0;              // без модифікатора глобальна клавіша не має сенсу
        CapHkSet(slot, (int)((mods << 16) | vk));
        return 0;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, CapHkSubclass, id);
        break;
    default: break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void CapTake(HINSTANCE hInst, HWND owner, CapMode mode, HWND target)
{
    CapShot shot = {};
    bool ok = false;
    g_capCancelled = false;

    if (mode == CapMode::Clipboard) {
        shot.bmp = CapFromClipboard();
        if (shot.bmp) {
            shot.w = (int)shot.bmp->GetWidth();
            shot.h = (int)shot.bmp->GetHeight();
            ok = true;
        }
    } else {
        CapDpiScope dpi;
        switch (mode) {
        case CapMode::Screen: ok = CapScreen(&shot); break;
        case CapMode::Window: ok = CapWindow(target ? target : CapForegroundTarget(), &shot); break;
        case CapMode::Region: ok = CapRegion(&shot); break;
        default: break;
        }
    }

    if (g_capCancelled) return;          // користувач передумав, мовчимо
    if (!ok || !shot.bmp) {
        MessageBoxW(owner, S(mode == CapMode::Clipboard ? Str::EdErrClip : Str::EdErrCapture),
                    kAppName, MB_OK | MB_ICONWARNING);
        return;
    }

    Str label = Str::EdCapScreen;
    if (mode == CapMode::Window)         label = Str::EdCapWindow;
    else if (mode == CapMode::Region)    label = Str::EdCapRegion;
    else if (mode == CapMode::Clipboard) label = Str::EdCapClip;
    EdOpenBitmap(hInst, shot.bmp, S(label), shot.hdr, shot.toneMapped, shot.sdrWhite);
}

// =================== кінець редактора знімків (CAPS-20) ===================

void ShowTrayMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, S(Str::MenuSettings));
    // Знімки — окремим підменю: у головному списку вони перекривали решту
    // програми, хоч це лише одна з її функцій.
    HMENU shots = CreatePopupMenu();
    AppendMenuW(shots, MF_STRING, IDM_CAPSCREEN, S(Str::EdCapScreen));
    AppendMenuW(shots, MF_STRING, IDM_CAPWINDOW, S(Str::EdCapWindow));
    AppendMenuW(shots, MF_STRING, IDM_CAPREGION, S(Str::EdCapRegion));
    AppendMenuW(shots, MF_STRING, IDM_CAPCLIP, S(Str::EdCapClip));
    AppendMenuW(shots, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(shots, MF_STRING, IDM_EDITOR, S(Str::EdMenu));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)shots, S(Str::TabShots));
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
        SwitchLayout();
        return 0;

    case WM_HOTKEY:      // від системної реєстрації клавіші
        switch ((int)wp) {
        case kHkIdClip:   CapTake(GetModuleHandleW(nullptr), hwnd, CapMode::Clipboard, nullptr); return 0;
        case kHkIdRegion: CapTake(GetModuleHandleW(nullptr), hwnd, CapMode::Region,    nullptr); return 0;
        case kHkIdScreen: CapTake(GetModuleHandleW(nullptr), hwnd, CapMode::Screen,    nullptr); return 0;
        default: break;
        }
        SwitchLayout();   // запасний режим розкладки (HOTKEY_ID)
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
        case IDC_CAP_SHAREUNLOCK:
            // Саме послаблення вмикається лише на старті — тут тільки памʼять.
            g_shareUnlock = SendMessageW(GetDlgItem(hwnd, IDC_CAP_SHAREUNLOCK), BM_GETCHECK, 0, 0) == BST_CHECKED;
            RegSaveInt(kRegShareUnlock, g_shareUnlock ? 1 : 0);
            return 0;
        case IDC_CAP_KEEPTOOL:
            g_edKeepTool = SendMessageW(GetDlgItem(hwnd, IDC_CAP_KEEPTOOL), BM_GETCHECK, 0, 0) == BST_CHECKED;
            RegSaveInt(kRegEdKeepTool, g_edKeepTool ? 1 : 0);
            break;
        case IDC_CAP_HKRESET:
            g_hk[0] = kHkDefClip;
            g_hk[1] = kHkDefRegion;
            g_hk[2] = kHkDefScreen;
            CapApplyHotkeys(hwnd);
            CapSaveHotkeys();
            CapHkRefresh();
            break;
        case IDM_CAPSCREEN:
            CapTake(GetModuleHandleW(nullptr), hwnd, CapMode::Screen, nullptr);
            break;
        case IDM_CAPWINDOW:
            CapTake(GetModuleHandleW(nullptr), hwnd, CapMode::Window, (HWND)lp);
            break;
        case IDM_CAPREGION:
            CapTake(GetModuleHandleW(nullptr), hwnd, CapMode::Region, nullptr);
            break;
        case IDM_CAPCLIP:
            CapTake(GetModuleHandleW(nullptr), hwnd, CapMode::Clipboard, nullptr);
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

    // CAPS-52. Типово COM елевейтованого процесу не пускає викликів «знизу
    // вгору», і меню поширення, яке живе БЕЗ підвищення, не може спитати в нас
    // даних — воно й не питало. Дозвіл вмикається лише свідомо, чекбоксом.
    // ⚠ Викликати можна РІВНО ОДИН раз і тільки тут: після першого ж виклику,
    // що потребує безпеки, COM виставляє її сам, і пізніше вже не змінити.
    // Дескриптор: право COM_RIGHTS_EXECUTE усім (WD) і пакетним застосункам
    // (AC, бо приймачі бувають із Магазину), мітка цілісності НИЗЬКА з NX —
    // тобто пускаємо середній рівень, але не недовірений.
    g_shareUnlock = RegLoadInt(kRegShareUnlock, 0, 0, 1) != 0;
    if (g_shareUnlock) {
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"O:BAG:BAD:(A;;0x1;;;WD)(A;;0x1;;;AC)S:(ML;;NX;;;LW)",
                SDDL_REVISION_1, &sd, nullptr) && sd) {
            CoInitializeSecurity(sd, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                                 RPC_C_IMP_LEVEL_IDENTIFY, nullptr, EOAC_NONE, nullptr);
            LocalFree(sd);
        }
    }

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
    // ⚠ Сторінки НЕ прокручуються, тож висота вікна — це межа вмісту.
    // 2.6.0: вкладка «Перегляд» переросла попередню; 3.23.0: «Знімки» переросли
    // цю, коли туди додався дозвіл для меню поширення з його поясненням.
    constexpr int W = 500, H = 690;
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
    auto addK  = [&](HWND c) { return AddTo(g_pageShots,    g_pageShotsN,    c); };   // CAPS-21

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

    // ---- вкладка «Знімки» (CAPS-21) ----
    y = PY;
    sec(addK, Str::CapSecHotkeys);
    {
        const Str names[3] = { Str::CapHkClipL, Str::EdCapRegion, Str::EdCapScreen };
        for (int i = 0; i < 3; ++i) {
            addK(mkS(L"STATIC", names[i], 0, PX, y + 5, 186, 20, 0));
            g_capHkEdit[i] = addK(mk(L"EDIT", L"", ES_CENTER | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                     PX + 192, y, 224, 26, IDC_CAP_HK1 + i));
            SetWindowSubclass(g_capHkEdit[i], CapHkSubclass, (UINT_PTR)i, 0);
            y += 32;
        }
    }
    y += 6;
    hint(addK, Str::CapHkHint, 3);
    g_capHkStatus = addK(mkS(L"STATIC", Str::Empty, 0, PX, y, PW, 36, IDC_HINT_GRAY));
    y += 42;
    button(addK, Str::CapHkDefaults, PX, 170, IDC_CAP_HKRESET);
    y += 38;
    sec(addK, Str::CapSecOutput);
    check(addK, Str::CapKeepTool, IDC_CAP_KEEPTOOL, g_edKeepTool, 2);
    check(addK, Str::CapShareUnlock, IDC_CAP_SHAREUNLOCK, g_shareUnlock, 2);
    hint(addK, Str::CapShareUnlockHint, 3);

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

    // CAPS-21: гарячі клавіші знімків. Якщо котрась зайнята — кажемо про це
    // вголос: мовчазна невдача виглядає як «програма зламалась».
    g_edLastAction  = RegLoadInt(kRegEdLast, 0, 0, 1);
    g_edKeepTool    = RegLoadInt(kRegEdKeepTool, 1, 0, 1) != 0;
    CapLoadHotkeys();
    if (!CapApplyHotkeys(hwnd)) TrayBalloon(kAppName, S(Str::CapHkBusy));
    CapHkRefresh();
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
