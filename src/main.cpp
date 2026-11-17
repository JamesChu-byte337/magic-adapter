// Magic Adapter - makes a wired Apple Magic Keyboard behave like it does on a Mac.
// Single-file Win32 app: low-level keyboard hook + tray icon + a hand-painted settings window.
// No runtime dependencies and no UI framework.
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <setupapi.h>
#include <dbt.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <math.h>
#include <string.h>

#define MAGIC       0x4D41474BUL          // tags our own injected keystrokes
#define WM_TRAY     (WM_APP + 1)
#define WM_LOCK     (WM_APP + 2)
#define WM_BRIGHT   (WM_APP + 3)
#define T_SCAN      1
#define T_BATT      2
#define ID_SETTINGS 100
#define ID_STARTUP  101
#define ID_QUIT     103
#define VK_MASK     0xE8                  // unassigned key, hides lone Alt taps from menus

static const wchar_t *APP = L"Magic Adapter";
static const wchar_t *RUNKEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t *CFGKEY = L"Software\\MagicAdapter";

static HWND  g_hwnd;         // hidden main window: tray icon, timers, device notifications
static HWND  g_ui;           // settings window, only exists while open
static HHOOK g_hook;
static UINT  g_taskbarMsg;
static NOTIFYICONDATAW g_nid;
static bool  g_present;      // wired Magic Keyboard plugged in -> remapping active
static int   g_batt = -1;    // percent, -1 = unknown
static WORD  g_pid;          // USB product id of the connected keyboard

// ---- settings (saved in the registry)
static const BYTE FDEFAULT[12] = { 1, 2, 3, 4, 5, 0, 6, 7, 8, 9, 10, 11 };
static bool  g_fkeys;        // top row sends plain F1-F12
static BYTE  g_fact[12];     // action per top-row key, index into ACTIONS
static DWORD g_rules;        // bit i set = RULES[i] enabled
static bool  g_cmdWin[2];    // [left, right] Command acts as the plain Windows key

static void defaults() {
    g_fkeys = false;
    CopyMemory(g_fact, FDEFAULT, 12);
    g_rules = 0xFFFFFFFF;
    g_cmdWin[0] = g_cmdWin[1] = false;
}

// physical key state (as seen by the hook) and the modifiers we are holding down ourselves
static bool lwin, rwin, lalt, ralt, lctrl, rctrl, lshift, rshift;
static bool vCtrl;           // injected Ctrl standing in for Command
static bool switcher;        // injected Alt held for Cmd+Tab
static bool swallowed[256];

enum { CMD = 1, OPT = 2, CTL = 4, SHF = 8, WIN = 16 };

// settings window hooks, defined further down
static void uiKey(DWORD vk, bool down);
static void uiLast(int what);
static void render(int secs);
static void updateTip();

// ---------------------------------------------------------------- key injection

static bool isExt(WORD vk) {
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT: case VK_INSERT: case VK_DELETE: case VK_LWIN: case VK_RWIN:
    case VK_RCONTROL: case VK_RMENU: case VK_SNAPSHOT:
    case VK_MEDIA_PREV_TRACK: case VK_MEDIA_NEXT_TRACK: case VK_MEDIA_PLAY_PAUSE:
    case VK_VOLUME_MUTE: case VK_VOLUME_DOWN: case VK_VOLUME_UP:
        return true;
    }
    return false;
}

static void put(INPUT *&p, WORD vk, bool up) {
    ZeroMemory(p, sizeof *p);
    p->type = INPUT_KEYBOARD;
    p->ki.wVk = vk;
    p->ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    p->ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (isExt(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    p->ki.dwExtraInfo = MAGIC;
    p++;
}

static void key(WORD vk, bool up) {
    INPUT in[1], *p = in;
    put(p, vk, up);
    SendInput(1, in, sizeof(INPUT));
}

// Send `vk` with exactly the modifiers in `want`, temporarily lifting whatever else is held.
// keepShift: leave a physically held Shift alone (so Shift+shortcut extends a selection).
static void combo(int want, WORD vk, bool keepShift = false) {
    struct { WORD vk; int cls; bool down; } cur[] = {
        { VK_LCONTROL, CTL, vCtrl || lctrl }, { VK_RCONTROL, CTL, rctrl },
        { VK_LMENU,    OPT, lalt || switcher }, { VK_RMENU,   OPT, ralt },
        { VK_LSHIFT,   SHF, lshift },          { VK_RSHIFT,   SHF, rshift },
    };
    static const struct { WORD vk; int cls; } gen[] = {
        { VK_LCONTROL, CTL }, { VK_LMENU, OPT }, { VK_LSHIFT, SHF }, { VK_LWIN, WIN },
    };
    int have = 0;
    for (auto &c : cur) if (c.down) have |= c.cls;
    if (keepShift) want |= have & SHF;

    INPUT in[40], *p = in;
    bool altLifted = (have & OPT) && !(want & OPT);
    if (altLifted) { put(p, VK_MASK, false); put(p, VK_MASK, true); }
    for (auto &c : cur) if (c.down && !(want & c.cls)) put(p, c.vk, true);
    for (auto &g : gen) if ((want & g.cls) && !(have & g.cls)) put(p, g.vk, false);
    if (vk) { put(p, vk, false); put(p, vk, true); }
    for (int i = 3; i >= 0; i--) if ((want & gen[i].cls) && !(have & gen[i].cls)) put(p, gen[i].vk, true);
    for (auto &c : cur) if (c.down && !(want & c.cls)) put(p, c.vk, false);
    if (altLifted) { put(p, VK_MASK, false); put(p, VK_MASK, true); }
    SendInput((UINT)(p - in), in, sizeof(INPUT));
}

static void resetState() {
    if (vCtrl) key(VK_LCONTROL, true);
    if (switcher) key(VK_LMENU, true);
    lwin = rwin = lalt = ralt = lctrl = rctrl = lshift = rshift = vCtrl = switcher = false;
    ZeroMemory(swallowed, sizeof swallowed);
}

// ---------------------------------------------------------------- shortcut table

enum { A_KEY, A_SWITCH, A_MIN, A_LOCK, A_DELLINE };
enum { ANYS = 1, REP = 2 };   // ANYS: Shift optional and kept; REP: auto-repeat allowed

struct Rule { BYTE mods, flags; WORD vk; BYTE act, omods; WORD ovk; const wchar_t *mac, *win; };

// At most 32 entries: each one has an on/off bit in g_rules.
static const Rule RULES[] = {
    { CMD,       ANYS | REP, VK_TAB,    A_SWITCH, 0, 0,               L"Cmd+Tab",             L"Switch apps" },
    { CMD,       0,          'Q',       A_KEY, OPT, VK_F4,            L"Cmd+Q",               L"Quit app" },
    { CMD,       0,          'H',       A_MIN, 0, 0,                  L"Cmd+H",               L"Minimize window" },
    { CMD,       0,          'M',       A_MIN, 0, 0,                  L"Cmd+M",               L"Minimize window" },
    { CMD,       0,          VK_SPACE,  A_KEY, 0, VK_LWIN,            L"Cmd+Space",           L"Start / Search" },
    { CMD | SHF, 0,          'Z',       A_KEY, CTL, 'Y',              L"Cmd+Shift+Z",         L"Redo" },
    { CMD,       ANYS | REP, VK_LEFT,   A_KEY, 0, VK_HOME,            L"Cmd+Left",            L"Start of line" },
    { CMD,       ANYS | REP, VK_RIGHT,  A_KEY, 0, VK_END,             L"Cmd+Right",           L"End of line" },
    { CMD,       ANYS | REP, VK_UP,     A_KEY, CTL, VK_HOME,          L"Cmd+Up",              L"Start of document" },
    { CMD,       ANYS | REP, VK_DOWN,   A_KEY, CTL, VK_END,           L"Cmd+Down",            L"End of document" },
    { OPT,       ANYS | REP, VK_LEFT,   A_KEY, CTL, VK_LEFT,          L"Option+Left",         L"Previous word" },
    { OPT,       ANYS | REP, VK_RIGHT,  A_KEY, CTL, VK_RIGHT,         L"Option+Right",        L"Next word" },
    { OPT,       REP,        VK_BACK,   A_KEY, CTL, VK_BACK,          L"Option+Backspace",    L"Delete word" },
    { CMD,       REP,        VK_BACK,   A_DELLINE, 0, 0,              L"Cmd+Backspace",       L"Delete to line start" },
    { CMD,       0,          VK_OEM_4,  A_KEY, OPT, VK_LEFT,          L"Cmd+[",               L"Back" },
    { CMD,       0,          VK_OEM_6,  A_KEY, OPT, VK_RIGHT,         L"Cmd+]",               L"Forward" },
    { CMD | SHF, REP,        VK_OEM_4,  A_KEY, CTL | SHF, VK_TAB,     L"Cmd+Shift+[",         L"Previous tab" },
    { CMD | SHF, REP,        VK_OEM_6,  A_KEY, CTL, VK_TAB,           L"Cmd+Shift+]",         L"Next tab" },
    { CMD | SHF, 0,          '3',       A_KEY, WIN, VK_SNAPSHOT,      L"Cmd+Shift+3",         L"Screenshot to file" },
    { CMD | SHF, 0,          '4',       A_KEY, WIN | SHF, 'S',        L"Cmd+Shift+4",         L"Snipping tool" },
    { CMD | SHF, 0,          '5',       A_KEY, WIN | SHF, 'S',        L"Cmd+Shift+5",         L"Snipping tool" },
    { CMD | CTL, 0,          'Q',       A_LOCK, 0, 0,                 L"Control+Cmd+Q",       L"Lock PC" },
    { CMD | CTL, 0,          'F',       A_KEY, 0, VK_F11,             L"Control+Cmd+F",       L"Full screen" },
    { CMD | CTL, 0,          VK_SPACE,  A_KEY, WIN, VK_OEM_PERIOD,    L"Control+Cmd+Space",   L"Emoji picker" },
    { CMD | OPT, 0,          VK_ESCAPE, A_KEY, CTL | SHF, VK_ESCAPE,  L"Cmd+Option+Esc",      L"Task Manager" },
    { CTL | OPT, 0,          VK_LEFT,   A_KEY, WIN, VK_LEFT,          L"Control+Option+Left", L"Snap window left" },
    { CTL | OPT, 0,          VK_RIGHT,  A_KEY, WIN, VK_RIGHT,         L"Control+Option+Right",L"Snap window right" },
    { CTL | OPT, 0,          VK_UP,     A_KEY, WIN, VK_UP,            L"Control+Option+Up",   L"Maximize window" },
    { CTL | OPT, 0,          VK_DOWN,   A_KEY, WIN, VK_DOWN,          L"Control+Option+Down", L"Restore / minimize" },
};
#define NRULES ((int)(sizeof RULES / sizeof *RULES))

// What a top-row key can do. Index is stored in g_fact, so only append to this list.
static const wchar_t *ACTIONS[] = {
    L"Plain F-key", L"Brightness down", L"Brightness up", L"Task View", L"Search", L"Voice typing",
    L"Previous track", L"Play / Pause", L"Next track", L"Mute", L"Volume down", L"Volume up",
    L"Notifications", L"Show desktop", L"Snipping tool", L"Lock PC", L"File Explorer", L"Settings",
};
#define NACTIONS ((int)(sizeof ACTIONS / sizeof *ACTIONS))

static bool inExplorer() {
    wchar_t cls[32] = L"";
    GetClassNameW(GetForegroundWindow(), cls, 32);
    return !lstrcmpW(cls, L"CabinetWClass") || !lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW");
}

static void runRule(const Rule &r) {
    switch (r.act) {
    case A_KEY:
        combo(r.omods, r.ovk, r.flags & ANYS);
        break;
    case A_SWITCH:
        if (!switcher) {
            if (vCtrl) { if (!lctrl) key(VK_LCONTROL, true); vCtrl = false; }
            if (!lalt) key(VK_LMENU, false);
            switcher = true;
        }
        combo(OPT, VK_TAB, true);
        break;
    case A_MIN:
        PostMessageW(GetForegroundWindow(), WM_SYSCOMMAND, SC_MINIMIZE, 0);
        break;
    case A_LOCK:
        PostMessageW(g_hwnd, WM_LOCK, 0, 0);
        break;
    case A_DELLINE:
        if (inExplorer()) combo(0, VK_DELETE);            // Cmd+Backspace = move to Recycle Bin
        else { combo(SHF, VK_HOME); combo(0, VK_BACK); }  // delete to start of line
        break;
    }
}

// Top row: run the action assigned to this key. Returns false if it has none.
static bool topRow(DWORD vk, bool repeat) {
    int a = g_fact[vk - VK_F1];
    if (a <= 0 || a >= NACTIONS) return false;
    bool repeats = a == 1 || a == 2 || a == 10 || a == 11;   // brightness and volume auto-repeat
    if (repeat && !repeats) return true;
    switch (a) {
    case 1:  PostMessageW(g_hwnd, WM_BRIGHT, 0, -10); break;
    case 2:  PostMessageW(g_hwnd, WM_BRIGHT, 0, 10); break;
    case 3:  combo(WIN, VK_TAB); break;
    case 4:  combo(WIN, 'S'); break;
    case 5:  combo(WIN, 'H'); break;
    case 6:  combo(0, VK_MEDIA_PREV_TRACK); break;
    case 7:  combo(0, VK_MEDIA_PLAY_PAUSE); break;
    case 8:  combo(0, VK_MEDIA_NEXT_TRACK); break;
    case 9:  combo(0, VK_VOLUME_MUTE); break;
    case 10: combo(0, VK_VOLUME_DOWN); break;
    case 11: combo(0, VK_VOLUME_UP); break;
    case 12: combo(WIN, 'N'); break;
    case 13: combo(WIN, 'D'); break;
    case 14: combo(WIN | SHF, 'S'); break;
    case 15: PostMessageW(g_hwnd, WM_LOCK, 0, 0); break;
    case 16: combo(WIN, 'E'); break;
    case 17: combo(WIN, 'I'); break;
    }
    return true;
}

// ---------------------------------------------------------------- keyboard hook

static LRESULT CALLBACK hookProc(int code, WPARAM wp, LPARAM lp) {
    KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
    if (code != HC_ACTION || k->dwExtraInfo == MAGIC || !g_present || k->vkCode > 255)
        return CallNextHookEx(g_hook, code, wp, lp);

    bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    DWORD vk = k->vkCode;
    if (g_ui) uiKey(vk, down);                            // light the key up on the on-screen keyboard

    switch (vk) {
    case VK_LWIN: case VK_RWIN: {                         // Command: never reaches Windows
        if (g_cmdWin[vk == VK_RWIN]) goto pass;           // ...unless set to act as the Windows key
        bool was = lwin || rwin;
        (vk == VK_LWIN ? lwin : rwin) = down;
        bool now = lwin || rwin;
        if (now && !was) {
            if (!switcher) { key(VK_LCONTROL, false); vCtrl = true; }
        } else if (!now && was) {
            if (switcher) { if (!lalt) key(VK_LMENU, true); switcher = false; }
            if (vCtrl) { if (!lctrl) key(VK_LCONTROL, true); vCtrl = false; }
        }
        return 1;
    }
    case VK_LCONTROL: lctrl = down; if (!down && vCtrl) return 1; goto pass;
    case VK_RCONTROL: rctrl = down; goto pass;
    case VK_LMENU:    lalt = down; if (!down && switcher) return 1; goto pass;
    case VK_RMENU:    ralt = down; goto pass;
    case VK_LSHIFT:   lshift = down; goto pass;
    case VK_RSHIFT:   rshift = down; goto pass;
    }

    if (!down) {
        if (swallowed[vk]) { swallowed[vk] = false; return 1; }
        goto pass;
    }

    {
        int mods = (lwin || rwin ? CMD : 0) | (lalt || ralt ? OPT : 0) |
                   (lctrl || rctrl ? CTL : 0) | (lshift || rshift ? SHF : 0);
        bool repeat = swallowed[vk];

        if (vk >= VK_F1 && vk <= VK_F12) {
            // Right Option + top-row key gives the "other" meaning of the key.
            bool rightOpt = mods == OPT && ralt && !lalt;
            if (g_fkeys ? rightOpt : mods == 0) {
                if (topRow(vk, repeat)) {
                    if (!repeat) uiLast(100 + (int)(vk - VK_F1));
                    swallowed[vk] = true;
                    return 1;
                }
            } else if (!g_fkeys && rightOpt) {
                combo(0, (WORD)vk);
                swallowed[vk] = true;
                return 1;
            }
            goto pass;
        }

        for (int i = 0; i < NRULES; i++) {
            const Rule &r = RULES[i];
            if (r.vk != vk || !(g_rules & (1u << i))) continue;
            int m = (r.flags & ANYS) ? (mods & ~SHF) : mods;
            if (m != r.mods) continue;
            if (!repeat || (r.flags & REP)) runRule(r);
            if (!repeat) uiLast(i);
            swallowed[vk] = true;
            return 1;
        }
    }
pass:
    return CallNextHookEx(g_hook, code, wp, lp);
}

// ---------------------------------------------------------------- brightness (built-in display)

#define IOCTL_LCD_QUERY_SUPPORTED 0x00230494   // IOCTL_VIDEO_QUERY_SUPPORTED_BRIGHTNESS
#define IOCTL_LCD_QUERY           0x00230498   // IOCTL_VIDEO_QUERY_DISPLAY_BRIGHTNESS
#define IOCTL_LCD_SET             0x0023049C   // IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS

static void brightness(int delta) {
    HANDLE h = CreateFileW(L"\\\\.\\LCD", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    BYTE levels[256], cur[3];
    DWORD n = 0, r = 0;
    if (DeviceIoControl(h, IOCTL_LCD_QUERY_SUPPORTED, NULL, 0, levels, sizeof levels, &n, NULL) && n &&
        DeviceIoControl(h, IOCTL_LCD_QUERY, NULL, 0, cur, sizeof cur, &r, NULL) && r == 3) {
        int now = cur[0] == 2 ? cur[2] : cur[1];          // policy 2 = on battery
        int target = now + delta, best = now, bestDist = 1000;
        for (DWORD i = 0; i < n; i++) {                   // nearest supported level in the right direction
            int l = levels[i];
            if ((delta > 0 && l <= now) || (delta < 0 && l >= now)) continue;
            int d = l > target ? l - target : target - l;
            if (d < bestDist) { bestDist = d; best = l; }
        }
        BYTE set[3] = { 3, (BYTE)best, (BYTE)best };      // 3 = apply to AC and battery
        DeviceIoControl(h, IOCTL_LCD_SET, set, sizeof set, NULL, 0, &r, NULL);
    }
    CloseHandle(h);
}

// ---------------------------------------------------------------- keyboard detection + battery

static int readBattery(HANDLE h, PHIDP_PREPARSED_DATA pp, const HIDP_CAPS &c) {
    BYTE buf[256];
    static HIDP_VALUE_CAPS vc[64];
    for (int t = 0; t < 2; t++) {                         // standard HID battery usages
        HIDP_REPORT_TYPE rt = t ? HidP_Feature : HidP_Input;
        USHORT n = t ? c.NumberFeatureValueCaps : c.NumberInputValueCaps;
        USHORT len = t ? c.FeatureReportByteLength : c.InputReportByteLength;
        if (!n || n > 64 || !len || len > sizeof buf) continue;
        if (HidP_GetValueCaps(rt, vc, &n, pp) != HIDP_STATUS_SUCCESS) continue;
        for (USHORT i = 0; i < n; i++) {
            USAGE u = vc[i].IsRange ? vc[i].Range.UsageMin : vc[i].NotRange.Usage, pg = vc[i].UsagePage;
            if (!((pg == 0x06 && u == 0x20) || (pg == 0x85 && u == 0x65))) continue;
            ZeroMemory(buf, sizeof buf);
            buf[0] = vc[i].ReportID;
            if (!(t ? HidD_GetFeature(h, buf, len) : HidD_GetInputReport(h, buf, len))) continue;
            ULONG v = 0;
            if (HidP_GetUsageValue(rt, pg, 0, u, &v, pp, (PCHAR)buf, len) != HIDP_STATUS_SUCCESS) continue;
            if (vc[i].LogicalMax > 0 && vc[i].LogicalMax != 100) v = v * 100 / vc[i].LogicalMax;
            if (v <= 100) return (int)v;
        }
    }
    for (int t = 0; t < 2; t++) {                         // Apple vendor report 0x90: [id, status, percent]
        ZeroMemory(buf, sizeof buf);
        buf[0] = 0x90;
        if ((t ? HidD_GetFeature(h, buf, 3) : HidD_GetInputReport(h, buf, 3)) && buf[0] == 0x90 && buf[2] <= 100)
            return buf[2];
    }
    return -1;
}

static bool hasNoCase(const wchar_t *s, const wchar_t *sub) {
    int n = lstrlenW(sub);
    for (; lstrlenW(s) >= n; s++)
        if (CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, s, n, sub, n) == CSTR_EQUAL) return true;
    return false;
}

static void scan() {
    bool present = false;
    int batt = -1;
    WORD pid = 0;
    GUID g;
    HidD_GetHidGuid(&g);
    HDEVINFO di = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA ifd = { sizeof ifd };
        static BYTE detBuf[1024];
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, NULL, &g, i, &ifd); i++) {
            SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)detBuf;
            det->cbSize = sizeof *det;
            if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, sizeof detBuf, NULL, NULL)) continue;
            if (!hasNoCase(det->DevicePath, L"hid#vid_05ac")) continue;   // Apple, USB only
            HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE)
                h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE) continue;
            PHIDP_PREPARSED_DATA pp;
            HIDP_CAPS c;
            if (HidD_GetPreparsedData(h, &pp)) {
                if (HidP_GetCaps(pp, &c) == HIDP_STATUS_SUCCESS) {
                    if (c.UsagePage == 1 && c.Usage == 6) {
                        HIDD_ATTRIBUTES a = { sizeof a };
                        present = true;
                        if (HidD_GetAttributes(h, &a)) pid = a.ProductID;
                    }
                    if (batt < 0) batt = readBattery(h, pp, c);
                }
                HidD_FreePreparsedData(pp);
            }
            CloseHandle(h);
        }
        SetupDiDestroyDeviceInfoList(di);
    }
    if (g_present && !present) resetState();
    g_present = present;
    g_pid = pid;
    g_batt = present ? batt : -1;
    updateTip();
}

// ---------------------------------------------------------------- saved settings

static bool startupEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, RUNKEY, L"MagicAdapter", RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static void setStartup(bool on) {
    if (!on) { RegDeleteKeyValueW(HKEY_CURRENT_USER, RUNKEY, L"MagicAdapter"); return; }
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 16];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wsprintfW(cmd, L"\"%s\" /tray", exe);
    RegSetKeyValueW(HKEY_CURRENT_USER, RUNKEY, L"MagicAdapter", REG_SZ, cmd, (lstrlenW(cmd) + 1) * sizeof(wchar_t));
}

static bool regDword(const wchar_t *name, DWORD *v) {
    DWORD sz = sizeof *v;
    return RegGetValueW(HKEY_CURRENT_USER, CFGKEY, name, RRF_RT_REG_DWORD, NULL, v, &sz) == ERROR_SUCCESS;
}

static void loadConfig() {
    defaults();
    DWORD v;
    if (regDword(L"FKeys", &v)) g_fkeys = v != 0;
    if (regDword(L"Rules", &v)) g_rules = v;
    if (regDword(L"LeftCmdWin", &v)) g_cmdWin[0] = v != 0;
    if (regDword(L"RightCmdWin", &v)) g_cmdWin[1] = v != 0;
    BYTE f[12];
    DWORD sz = sizeof f;
    if (RegGetValueW(HKEY_CURRENT_USER, CFGKEY, L"TopRow", RRF_RT_REG_BINARY, NULL, f, &sz) == ERROR_SUCCESS && sz == 12)
        for (int i = 0; i < 12; i++) if (f[i] < NACTIONS) g_fact[i] = f[i];
}

static void saveConfig() {
    DWORD v;
    v = g_fkeys;     RegSetKeyValueW(HKEY_CURRENT_USER, CFGKEY, L"FKeys", REG_DWORD, &v, sizeof v);
    v = g_rules;     RegSetKeyValueW(HKEY_CURRENT_USER, CFGKEY, L"Rules", REG_DWORD, &v, sizeof v);
    v = g_cmdWin[0]; RegSetKeyValueW(HKEY_CURRENT_USER, CFGKEY, L"LeftCmdWin", REG_DWORD, &v, sizeof v);
    v = g_cmdWin[1]; RegSetKeyValueW(HKEY_CURRENT_USER, CFGKEY, L"RightCmdWin", REG_DWORD, &v, sizeof v);
    RegSetKeyValueW(HKEY_CURRENT_USER, CFGKEY, L"TopRow", REG_BINARY, g_fact, 12);
}

// The settings window (drawing, layout, input) lives in its own file; it is part of this translation unit.
#include "ui.inl"

// ---------------------------------------------------------------- tray

static void batteryText(wchar_t *out) {
    if (!g_present) lstrcpyW(out, L"Battery: -");
    else if (g_batt >= 0) wsprintfW(out, L"Battery: %d%%", g_batt);
    else lstrcpyW(out, L"Battery: not readable over USB");
}

static void updateTip() {
    wchar_t b[64];
    batteryText(b);
    wsprintfW(g_nid.szTip, L"%s - %s\n%s", APP, g_present ? L"Mac mode on" : L"keyboard not connected", b);
    g_nid.uFlags = NIF_TIP | NIF_ICON | NIF_MESSAGE;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    if (!g_present) ZeroMemory(g_keys, sizeof g_keys);
    render(SEC_HEAD | SEC_KB);
}

static void showMenu() {
    wchar_t b[64];
    batteryText(b);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, g_present ? L"Magic Keyboard connected - Mac mode on" : L"Magic Keyboard not connected");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, b);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Settings...");
    AppendMenuW(m, MF_STRING | (startupEnabled() ? MF_CHECKED : 0), ID_STARTUP, L"Run on startup");
    SetMenuDefaultItem(m, ID_SETTINGS, FALSE);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) openSettings();
        else if (LOWORD(lp) == WM_RBUTTONUP) showMenu();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SETTINGS: openSettings(); break;
        case ID_STARTUP:
            setStartup(!startupEnabled());
            g_startup = startupEnabled();
            render(SEC_FOOT);
            break;
        case ID_QUIT: DestroyWindow(h); break;
        }
        return 0;
    case WM_LOCK:
        resetState();
        LockWorkStation();
        return 0;
    case WM_BRIGHT:
        brightness((int)lp);
        return 0;
    case WM_DEVICECHANGE:
        SetTimer(h, T_SCAN, 1500, NULL);                  // let the device settle, then rescan
        return TRUE;
    case WM_TIMER:
        if (wp == T_SCAN) KillTimer(h, T_SCAN);
        scan();
        return 0;
    case WM_DESTROY:
        resetState();
        if (g_ui) DestroyWindow(g_ui);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_taskbarMsg && g_taskbarMsg) {            // Explorer restarted: put the icon back
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdLine, int) {
    CreateMutexW(NULL, TRUE, L"MagicAdapter.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {         // already running: just show its settings window
        HWND other = FindWindowW(L"MagicAdapterWnd", NULL);
        if (other) PostMessageW(other, WM_COMMAND, ID_SETTINGS, 0);
        return 0;
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"MagicAdapterWnd";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, APP, 0, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;

    loadConfig();
    g_taskbarMsg = RegisterWindowMessageW(L"TaskbarCreated");
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = makeIcon(GetSystemMetrics(SM_CXSMICON));
    lstrcpyW(g_nid.szTip, APP);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    DEV_BROADCAST_DEVICEINTERFACE_W f = {};
    f.dbcc_size = sizeof f;
    f.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    HidD_GetHidGuid(&f.dbcc_classguid);
    RegisterDeviceNotificationW(g_hwnd, &f, DEVICE_NOTIFY_WINDOW_HANDLE);

    scan();
    SetTimer(g_hwnd, T_BATT, 60000, NULL);
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, hookProc, inst, 0);
    if (!g_hook) {
        MessageBoxW(NULL, L"Could not install the keyboard hook.", APP, MB_ICONERROR);
        return 1;
    }
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);   // trim startup memory

    // Opened by hand (not by the startup entry, which passes /tray): show the settings window.
    if (!cmdLine || !strstr(cmdLine, "/tray")) openSettings();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    UnhookWindowsHookEx(g_hook);
    return 0;
}
