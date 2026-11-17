// Settings window for Magic Adapter. Included by main.cpp (same translation unit).
//
// Everything is painted by hand into an off-screen bitmap: no common controls, no UI framework,
// no image files and no icon fonts. The key icons are drawn from lines, arcs and triangles below.
// The bitmap, the fonts and the window itself only exist while the window is open.

enum {                                                    // palette, 0xRRGGBB
    C_BG = 0xF5F5F7, C_CARD = 0xFFFFFF, C_LINE = 0xE0E0E5, C_SEP = 0xEDEDF0, C_HOVER = 0xF2F2F7, C_TRACK = 0xE4E4E9,
    C_KBBODY = 0xE2E3E6, C_KBLINE = 0xCBCCD1, C_KEY = 0xFFFFFF, C_KEYLN = 0xD2D2D7, C_KEYSH = 0xC6C7CC,
    C_KEYOFF = 0xF3F3F5, C_OFFLN = 0xDADADF, C_OFFTX = 0xBDBDC2,
    C_TEXT = 0x1D1D1F, C_TEXT2 = 0x6E6E73, C_TEXT3 = 0xA8A8AD, C_LEG = 0x55555A, C_CHIPTX = 0x3A3A3C,
    C_ACC = 0x007AFF, C_ACCDK = 0x0062CC, C_TINT = 0xE3F0FF, C_GREEN = 0x34C759, C_RED = 0xFF3B30, C_ORANGE = 0xFF9500,
    C_WHITE = 0xFFFFFF, C_MENU = 0xF6F6F8,
};

enum {                                                    // layout, in 96-dpi units
    CW = 940, CH = 596, HH = 52,
    KBX = 20, KBY = 60, KBW = 560, KBH = 242,             // keyboard
    TPX = 20, TPY = 312, TPW = 560, TPH = 232,            // top row + command keys
    LSX = 592, LSY = 60, LSW = 328, LSH = 484,            // shortcut list
    FTY = 556,                                            // footer controls
};

enum { SEC_HEAD = 1, SEC_KB = 2, SEC_TOP = 4, SEC_LIST = 8, SEC_FOOT = 16, SEC_ALL = 31 };
enum { Z_F = 0, Z_CMD = 20, Z_SEG = 30, Z_START = 40, Z_DEF, Z_QUIT, Z_THUMB, Z_CLOSE, Z_MIN, Z_ROW = 100 };

struct Surface { DWORD *px; int w, h; HDC dc; RECT clip; };
static Surface sf;
static float   S = 1;                                     // UI scale, 1 = 96 dpi
static HBITMAP uiBmp;
static HGDIOBJ uiOldBmp;
static HBRUSH  menuBrush;
static int     hot = -1;                                  // zone under the mouse
static int     scrollY;                                   // shortcut list scroll, pixels
static bool    dragging, tracking, defArmed, g_startup;
static int     dragOff;
static int     g_last = -1;                               // last translated shortcut (rule index, or 100 + F-key)
static bool    g_keys[256];                               // keys held right now, for the on-screen keyboard
static int     menuCur;

static inline int px(float u) { return (int)floorf(u * S + 0.5f); }
static inline int hair() { int h = (int)S; return h < 1 ? 1 : h; }
static inline COLORREF cref(DWORD c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }
static inline float fmn(float a, float b) { return a < b ? a : b; }
static inline float fmx(float a, float b) { return a > b ? a : b; }
static RECT R(float x, float y, float w, float h) { RECT r = { px(x), px(y), px(x + w), px(y + h) }; return r; }

// ---------------------------------------------------------------- drawing primitives

static void setClip(const RECT *rc) {
    RECT full = { 0, 0, sf.w, sf.h };
    if (rc) IntersectRect(&sf.clip, rc, &full); else sf.clip = full;
    HRGN rg = CreateRectRgnIndirect(&sf.clip);
    SelectClipRgn(sf.dc, rg);
    DeleteObject(rg);
}

static void fill(RECT rc, DWORD c) {
    RECT r;
    if (!IntersectRect(&r, &rc, &sf.clip)) return;
    for (int y = r.top; y < r.bottom; y++) {
        DWORD *p = sf.px + y * sf.w + r.left;
        for (int x = r.left; x < r.right; x++) *p++ = c;
    }
}

static inline void mix(DWORD *p, DWORD c, int a) {        // a = 0..256
    DWORD d = *p, k = 256 - a;
    *p = ((((d & 0xFF00FF) * k + (c & 0xFF00FF) * a) >> 8) & 0xFF00FF) |
         ((((d & 0x00FF00) * k + (c & 0x00FF00) * a) >> 8) & 0x00FF00);
}

// Signed distance to a rounded rectangle; dx, dy are measured from its inner (corner-centre) box.
static inline float sdf(float dx, float dy, float r) {
    return (dx > 0 && dy > 0) ? sqrtf(dx * dx + dy * dy) - r : (dx > dy ? dx : dy) - r;
}

// Anti-aliased rounded rectangle on whole pixels, with an optional hairline inner border.
static void box(RECT rc, float rad, DWORD fillC, int fa = 256, DWORD lineC = 0, int la = 0) {
    float w = (float)(rc.right - rc.left), h = (float)(rc.bottom - rc.top);
    if (w <= 0 || h <= 0) return;
    float r = rad * S, m = (w < h ? w : h) * 0.5f, bw = (float)hair();
    if (r > m) r = m;
    float cx = rc.left + w * 0.5f, cy = rc.top + h * 0.5f, hw = w * 0.5f - r, hh = h * 0.5f - r;
    RECT c;
    if (!IntersectRect(&c, &rc, &sf.clip)) return;
    for (int y = c.top; y < c.bottom; y++) {
        float dy = fabsf(y + 0.5f - cy) - hh;
        DWORD *p = sf.px + y * sf.w + c.left;
        for (int x = c.left; x < c.right; x++, p++) {
            float d = sdf(fabsf(x + 0.5f - cx) - hw, dy, r);
            float ao = 0.5f - d;
            if (ao <= 0) continue;
            if (ao > 1) ao = 1;
            float ai = ao;
            if (la) { ai = 0.5f - (d + bw); ai = ai < 0 ? 0 : ai > 1 ? 1 : ai; }
            if (ai >= 1 && fa == 256) { *p = fillC; continue; }
            DWORD aF = (DWORD)(ai * fa), aL = (DWORD)((ao - ai) * la), k = 256 - aF - aL, dst = *p;
            *p = ((((dst & 0xFF00FF) * k + (fillC & 0xFF00FF) * aF + (lineC & 0xFF00FF) * aL) >> 8) & 0xFF00FF) |
                 ((((dst & 0x00FF00) * k + (fillC & 0x00FF00) * aF + (lineC & 0x00FF00) * aL) >> 8) & 0x00FF00);
        }
    }
}

// Generic anti-aliased shape: `dist` returns the signed distance (negative inside) at a pixel centre.
template <class F> static void shade(float x0, float y0, float x1, float y1, DWORD c, int al, F dist) {
    RECT b = { (int)floorf(x0) - 1, (int)floorf(y0) - 1, (int)ceilf(x1) + 1, (int)ceilf(y1) + 1 }, r;
    if (!IntersectRect(&r, &b, &sf.clip)) return;
    for (int y = r.top; y < r.bottom; y++) {
        DWORD *p = sf.px + y * sf.w + r.left;
        for (int x = r.left; x < r.right; x++, p++) {
            float cov = 0.5f - dist(x + 0.5f, y + 0.5f);
            if (cov <= 0) continue;
            mix(p, c, (int)((cov > 1 ? 1 : cov) * al));
        }
    }
}

static void seg(float ax, float ay, float bx, float by, float w, DWORD c, int al = 256) {   // round-capped line
    float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy, hw = w * 0.5f;
    shade(fmn(ax, bx) - hw, fmn(ay, by) - hw, fmx(ax, bx) + hw, fmx(ay, by) + hw, c, al, [=](float x, float y) {
        float t = len2 > 0 ? ((x - ax) * dx + (y - ay) * dy) / len2 : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float ex = x - ax - t * dx, ey = y - ay - t * dy;
        return sqrtf(ex * ex + ey * ey) - hw;
    });
}

static void ring(float cx, float cy, float r, float w, DWORD c, int al = 256) {
    float hw = w * 0.5f, o = r + hw;
    shade(cx - o, cy - o, cx + o, cy + o, c, al, [=](float x, float y) {
        float ex = x - cx, ey = y - cy;
        return fabsf(sqrtf(ex * ex + ey * ey) - r) - hw;
    });
}

static void disc(float cx, float cy, float r, DWORD c, int al = 256) {
    shade(cx - r, cy - r, cx + r, cy + r, c, al, [=](float x, float y) {
        float ex = x - cx, ey = y - cy;
        return sqrtf(ex * ex + ey * ey) - r;
    });
}

// Round-capped arc centred on direction (ux, uy), spanning +-acos(cosHalf) either side of it.
static void arc(float cx, float cy, float r, float w, float ux, float uy, float cosHalf, DWORD c, int al = 256) {
    float hw = w * 0.5f, o = r + hw, sinHalf = sqrtf(1 - cosHalf * cosHalf);
    float e1x = cx + r * (ux * cosHalf - uy * sinHalf), e1y = cy + r * (uy * cosHalf + ux * sinHalf);
    float e2x = cx + r * (ux * cosHalf + uy * sinHalf), e2y = cy + r * (uy * cosHalf - ux * sinHalf);
    shade(cx - o, cy - o, cx + o, cy + o, c, al, [=](float x, float y) {
        float ex = x - cx, ey = y - cy, l = sqrtf(ex * ex + ey * ey);
        if (ex * ux + ey * uy >= cosHalf * l) return fabsf(l - r) - hw;
        float a1 = sqrtf((x - e1x) * (x - e1x) + (y - e1y) * (y - e1y));
        float a2 = sqrtf((x - e2x) * (x - e2x) + (y - e2y) * (y - e2y));
        return (a1 < a2 ? a1 : a2) - hw;
    });
}

static void poly(const float *p, int n, DWORD c, int al = 256) {   // filled convex polygon, up to 4 points
    struct { float x, y, nx, ny; } e[4];
    float area = 0, x0 = p[0], y0 = p[1], x1 = x0, y1 = y0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += p[i * 2] * p[j * 2 + 1] - p[j * 2] * p[i * 2 + 1];
        x0 = fmn(x0, p[i * 2]); x1 = fmx(x1, p[i * 2]);
        y0 = fmn(y0, p[i * 2 + 1]); y1 = fmx(y1, p[i * 2 + 1]);
    }
    float sgn = area > 0 ? 1.0f : -1.0f;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float dx = p[j * 2] - p[i * 2], dy = p[j * 2 + 1] - p[i * 2 + 1], len = sqrtf(dx * dx + dy * dy);
        if (len <= 0) len = 1;
        e[i].x = p[i * 2]; e[i].y = p[i * 2 + 1];
        e[i].nx = sgn * dy / len; e[i].ny = -sgn * dx / len;
    }
    shade(x0, y0, x1, y1, c, al, [=](float x, float y) {
        float d = -1e9f;
        for (int i = 0; i < n; i++) {
            float v = (x - e[i].x) * e[i].nx + (y - e[i].y) * e[i].ny;
            if (v > d) d = v;
        }
        return d;
    });
}

static void rrLine(float x0, float y0, float x1, float y1, float r, float w, DWORD c, int al = 256) {   // outline
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hw = (x1 - x0) * 0.5f - r, hh = (y1 - y0) * 0.5f - r, h = w * 0.5f;
    shade(x0 - h, y0 - h, x1 + h, y1 + h, c, al, [=](float x, float y) {
        return fabsf(sdf(fabsf(x - cx) - hw, fabsf(y - cy) - hh, r)) - h;
    });
}

static void rrFill(float x0, float y0, float x1, float y1, float r, DWORD c, int al = 256) {
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hw = (x1 - x0) * 0.5f - r, hh = (y1 - y0) * 0.5f - r;
    shade(x0, y0, x1, y1, c, al, [=](float x, float y) { return sdf(fabsf(x - cx) - hw, fabsf(y - cy) - hh, r); });
}

// ---------------------------------------------------------------- icons, drawn from scratch
// Thin rounded line icons in the spirit of the legends printed on the keyboard.
// Ids 1..17 line up with the ACTIONS table in main.cpp.

enum { I_NONE, I_SUNLO, I_SUN, I_WINDOWS, I_SEARCH, I_MIC, I_PREV, I_PLAY, I_NEXT, I_MUTE, I_VOL1, I_VOL3,
       I_BELL, I_DESKTOP, I_SNIP, I_LOCK, I_FOLDER, I_GEAR, I_GLOBE, I_CHEVUD, I_CHECK };

// cx, cy: centre in pixels; s: size of the icon's square in pixels.
static void icon(int id, float cx, float cy, float s, DWORD c, int al = 256) {
    float lw = s * 0.085f;
    if (lw < 1.25f) lw = 1.25f;
    auto X = [=](float u) { return cx + u * s; };
    auto Y = [=](float v) { return cy + v * s; };
    auto L = [=](float a, float b, float d, float e) { seg(X(a), Y(b), X(d), Y(e), lw, c, al); };
    auto B = [=](float a, float b, float d, float e, float r) { rrLine(X(a), Y(b), X(d), Y(e), r * s, lw, c, al); };
    auto T = [=](float a, float b, float d, float e, float f, float g) {
        float p[6] = { X(a), Y(b), X(d), Y(e), X(f), Y(g) };
        poly(p, 3, c, al);
    };
    auto A = [=](float u, float v, float r, float ux, float uy, float cosHalf) { arc(X(u), Y(v), r * s, lw, ux, uy, cosHalf, c, al); };
    auto SPK = [=](float ox) {                            // loudspeaker, left edge at ox
        rrFill(X(ox), Y(-.115f), X(ox + .17f), Y(.115f), .03f * s, c, al);
        float p[8] = { X(ox + .12f), Y(-.115f), X(ox + .36f), Y(-.31f), X(ox + .36f), Y(.31f), X(ox + .12f), Y(.115f) };
        poly(p, 4, c, al);
    };
    static const float DIR[8][2] = { { 1, 0 }, { .7071f, .7071f }, { 0, 1 }, { -.7071f, .7071f },
                                     { -1, 0 }, { -.7071f, -.7071f }, { 0, -1 }, { .7071f, -.7071f } };
    switch (id) {
    case I_SUNLO: case I_SUN: {
        float r = id == I_SUN ? .16f : .12f, a = id == I_SUN ? .29f : .24f, b = id == I_SUN ? .43f : .31f;
        ring(X(0), Y(0), r * s, lw, c, al);
        for (auto &d : DIR) L(d[0] * a, d[1] * a, d[0] * b, d[1] * b);
        break;
    }
    case I_WINDOWS:
        B(-.43f, -.37f, .43f, -.04f, .06f);
        B(-.43f, .09f, -.05f, .37f, .06f);
        B(.05f, .09f, .43f, .37f, .06f);
        break;
    case I_SEARCH:
        ring(X(-.07f), Y(-.07f), .26f * s, lw, c, al);
        L(.13f, .13f, .39f, .39f);
        break;
    case I_MIC:
        B(-.13f, -.44f, .13f, .08f, .13f);
        A(0, -.03f, .27f, 0, 1, 0);
        L(0, .24f, 0, .41f);
        L(-.14f, .41f, .14f, .41f);
        break;
    case I_PREV:
        T(-.44f, 0, -.04f, -.26f, -.04f, .26f);
        T(0, 0, .40f, -.26f, .40f, .26f);
        break;
    case I_NEXT:
        T(.44f, 0, .04f, -.26f, .04f, .26f);
        T(0, 0, -.40f, -.26f, -.40f, .26f);
        break;
    case I_PLAY:
        T(-.45f, -.27f, -.45f, .27f, -.03f, 0);
        seg(X(.15f), Y(-.21f), X(.15f), Y(.21f), s * .105f, c, al);
        seg(X(.37f), Y(-.21f), X(.37f), Y(.21f), s * .105f, c, al);
        break;
    case I_MUTE:
        SPK(-.42f);
        L(.10f, -.13f, .36f, .13f);
        L(.10f, .13f, .36f, -.13f);
        break;
    case I_VOL1: case I_VOL3:
        SPK(-.46f);
        A(-.16f, 0, .17f, 1, 0, .766f);
        if (id == I_VOL3) { A(-.16f, 0, .31f, 1, 0, .766f); A(-.16f, 0, .45f, 1, 0, .766f); }
        break;
    case I_BELL:
        A(0, -.04f, .25f, 0, -1, 0);
        L(-.25f, -.04f, -.29f, .20f);
        L(.25f, -.04f, .29f, .20f);
        L(-.38f, .22f, .38f, .22f);
        A(0, .29f, .08f, 0, 1, 0);
        L(0, -.29f, 0, -.37f);
        break;
    case I_DESKTOP:
        B(-.44f, -.36f, .44f, .18f, .06f);
        L(0, .18f, 0, .36f);
        L(-.20f, .38f, .20f, .38f);
        break;
    case I_SNIP:
        for (int i = 0; i < 4; i++) {
            float sx = (i & 1) ? .40f : -.40f, sy = (i & 2) ? .40f : -.40f;
            L(sx, sy, sx, sy * .4f);
            L(sx, sy, sx * .4f, sy);
        }
        disc(X(0), Y(0), .07f * s, c, al);
        break;
    case I_LOCK:
        B(-.29f, -.06f, .29f, .42f, .08f);
        A(0, -.15f, .18f, 0, -1, 0);
        L(-.18f, -.15f, -.18f, -.06f);
        L(.18f, -.15f, .18f, -.06f);
        break;
    case I_FOLDER:
        B(-.44f, -.20f, .44f, .36f, .07f);
        L(-.44f, -.12f, -.44f, -.28f);
        L(-.39f, -.33f, -.14f, -.33f);
        L(-.14f, -.33f, -.03f, -.20f);
        A(-.39f, -.28f, .05f, -.7071f, -.7071f, .7071f);
        break;
    case I_GEAR:
        ring(X(0), Y(0), .12f * s, lw, c, al);
        ring(X(0), Y(0), .28f * s, lw, c, al);
        for (auto &d : DIR) seg(X(d[0] * .30f), Y(d[1] * .30f), X(d[0] * .40f), Y(d[1] * .40f), lw * 1.7f, c, al);
        break;
    case I_GLOBE:
        ring(X(0), Y(0), .42f * s, lw, c, al);
        L(-.42f, 0, .42f, 0);
        A(.341f, 0, .541f, -1, 0, .631f);
        A(-.341f, 0, .541f, 1, 0, .631f);
        break;
    case I_CHEVUD:
        L(-.18f, -.12f, 0, -.30f); L(0, -.30f, .18f, -.12f);
        L(-.18f, .12f, 0, .30f);   L(0, .30f, .18f, .12f);
        break;
    case I_CHECK:
        L(-.30f, .02f, -.09f, .24f);
        L(-.09f, .24f, .32f, -.24f);
        break;
    }
}

// ---------------------------------------------------------------- text

enum { F_UI, F_UIS, F_UIB, F_TITLE, F_KEY, F_TINY, F_SYM, F_SYMS, F_SYMX, NFONT };
static HFONT fnt[NFONT];

static int CALLBACK fontFound(const LOGFONTW *, const TEXTMETRICW *, DWORD, LPARAM lp) { *(bool *)lp = true; return 0; }

static bool fontExists(const wchar_t *face) {
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    bool found = false;
    HDC dc = GetDC(NULL);
    EnumFontFamiliesExW(dc, &lf, fontFound, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

static HFONT mkFont(float size, int weight, const wchar_t *face) {
    return CreateFontW(-px(size), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

static void mkFonts() {
    const wchar_t *ui = fontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t *sym = L"Segoe UI Symbol";              // only for the standard Mac modifier symbols and arrows
    fnt[F_UI]    = mkFont(12.5f, 400, ui);
    fnt[F_UIS]   = mkFont(11.5f, 400, ui);
    fnt[F_UIB]   = mkFont(12.5f, 600, ui);
    fnt[F_TITLE] = mkFont(13, 600, ui);
    fnt[F_KEY]   = mkFont(11.5f, 400, ui);
    fnt[F_TINY]  = mkFont(8.5f, 400, ui);
    fnt[F_SYM]   = mkFont(13.5f, 400, sym);
    fnt[F_SYMS]  = mkFont(10.5f, 400, sym);
    fnt[F_SYMX]  = mkFont(6.5f, 400, sym);
}

static void txt(int f, DWORD col, const wchar_t *s, RECT rc, UINT fmt = DT_LEFT) {
    SelectObject(sf.dc, fnt[f]);
    SetTextColor(sf.dc, cref(col));
    if (!(fmt & DT_END_ELLIPSIS)) fmt |= DT_NOCLIP;
    DrawTextW(sf.dc, s, -1, &rc, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

static int txtW(int f, const wchar_t *s) {
    SIZE z = {};
    SelectObject(sf.dc, fnt[f]);
    GetTextExtentPoint32W(sf.dc, s, lstrlenW(s), &z);
    return z.cx;
}

// ---------------------------------------------------------------- the keyboard

enum { K_CHAR, K_SYM, K_ARROW, K_TXT, K_ICON, K_F, K_CMD };
struct Key { BYTE row, half; float x, w; WORD vk; BYTE kind; const wchar_t *label; BYTE ic; };

// Magic Keyboard (compact, US layout). x and w are in key widths; vk 0 = a key Windows cannot see.
static const Key KEYS[] = {
    { 0, 0, 0, 1.5f, VK_ESCAPE, K_TXT, L"esc" },
    { 0, 0, 1.5f, 1, VK_F1, K_F, L"F1" },    { 0, 0, 2.5f, 1, VK_F2, K_F, L"F2" },    { 0, 0, 3.5f, 1, VK_F3, K_F, L"F3" },
    { 0, 0, 4.5f, 1, VK_F4, K_F, L"F4" },    { 0, 0, 5.5f, 1, VK_F5, K_F, L"F5" },    { 0, 0, 6.5f, 1, VK_F6, K_F, L"F6" },
    { 0, 0, 7.5f, 1, VK_F7, K_F, L"F7" },    { 0, 0, 8.5f, 1, VK_F8, K_F, L"F8" },    { 0, 0, 9.5f, 1, VK_F9, K_F, L"F9" },
    { 0, 0, 10.5f, 1, VK_F10, K_F, L"F10" }, { 0, 0, 11.5f, 1, VK_F11, K_F, L"F11" }, { 0, 0, 12.5f, 1, VK_F12, K_F, L"F12" },
    { 0, 0, 13.5f, 1, 0, K_ICON, L"", I_LOCK },

    { 1, 0, 0, 1, VK_OEM_3, K_CHAR, L"`" },
    { 1, 0, 1, 1, '1', K_CHAR, L"1" }, { 1, 0, 2, 1, '2', K_CHAR, L"2" }, { 1, 0, 3, 1, '3', K_CHAR, L"3" },
    { 1, 0, 4, 1, '4', K_CHAR, L"4" }, { 1, 0, 5, 1, '5', K_CHAR, L"5" }, { 1, 0, 6, 1, '6', K_CHAR, L"6" },
    { 1, 0, 7, 1, '7', K_CHAR, L"7" }, { 1, 0, 8, 1, '8', K_CHAR, L"8" }, { 1, 0, 9, 1, '9', K_CHAR, L"9" },
    { 1, 0, 10, 1, '0', K_CHAR, L"0" }, { 1, 0, 11, 1, VK_OEM_MINUS, K_CHAR, L"-" }, { 1, 0, 12, 1, VK_OEM_PLUS, K_CHAR, L"=" },
    { 1, 0, 13, 1.5f, VK_BACK, K_SYM, L"⌫" },

    { 2, 0, 0, 1.5f, VK_TAB, K_SYM, L"⇥" },
    { 2, 0, 1.5f, 1, 'Q', K_CHAR, L"Q" }, { 2, 0, 2.5f, 1, 'W', K_CHAR, L"W" }, { 2, 0, 3.5f, 1, 'E', K_CHAR, L"E" },
    { 2, 0, 4.5f, 1, 'R', K_CHAR, L"R" }, { 2, 0, 5.5f, 1, 'T', K_CHAR, L"T" }, { 2, 0, 6.5f, 1, 'Y', K_CHAR, L"Y" },
    { 2, 0, 7.5f, 1, 'U', K_CHAR, L"U" }, { 2, 0, 8.5f, 1, 'I', K_CHAR, L"I" }, { 2, 0, 9.5f, 1, 'O', K_CHAR, L"O" },
    { 2, 0, 10.5f, 1, 'P', K_CHAR, L"P" }, { 2, 0, 11.5f, 1, VK_OEM_4, K_CHAR, L"[" }, { 2, 0, 12.5f, 1, VK_OEM_6, K_CHAR, L"]" },
    { 2, 0, 13.5f, 1, VK_OEM_5, K_CHAR, L"\\" },

    { 3, 0, 0, 1.75f, VK_CAPITAL, K_SYM, L"⇪" },
    { 3, 0, 1.75f, 1, 'A', K_CHAR, L"A" }, { 3, 0, 2.75f, 1, 'S', K_CHAR, L"S" }, { 3, 0, 3.75f, 1, 'D', K_CHAR, L"D" },
    { 3, 0, 4.75f, 1, 'F', K_CHAR, L"F" }, { 3, 0, 5.75f, 1, 'G', K_CHAR, L"G" }, { 3, 0, 6.75f, 1, 'H', K_CHAR, L"H" },
    { 3, 0, 7.75f, 1, 'J', K_CHAR, L"J" }, { 3, 0, 8.75f, 1, 'K', K_CHAR, L"K" }, { 3, 0, 9.75f, 1, 'L', K_CHAR, L"L" },
    { 3, 0, 10.75f, 1, VK_OEM_1, K_CHAR, L";" }, { 3, 0, 11.75f, 1, VK_OEM_7, K_CHAR, L"'" },
    { 3, 0, 12.75f, 1.75f, VK_RETURN, K_SYM, L"↩" },

    { 4, 0, 0, 2.25f, VK_LSHIFT, K_SYM, L"⇧" },
    { 4, 0, 2.25f, 1, 'Z', K_CHAR, L"Z" }, { 4, 0, 3.25f, 1, 'X', K_CHAR, L"X" }, { 4, 0, 4.25f, 1, 'C', K_CHAR, L"C" },
    { 4, 0, 5.25f, 1, 'V', K_CHAR, L"V" }, { 4, 0, 6.25f, 1, 'B', K_CHAR, L"B" }, { 4, 0, 7.25f, 1, 'N', K_CHAR, L"N" },
    { 4, 0, 8.25f, 1, 'M', K_CHAR, L"M" }, { 4, 0, 9.25f, 1, VK_OEM_COMMA, K_CHAR, L"," },
    { 4, 0, 10.25f, 1, VK_OEM_PERIOD, K_CHAR, L"." }, { 4, 0, 11.25f, 1, VK_OEM_2, K_CHAR, L"/" },
    { 4, 0, 12.25f, 2.25f, VK_RSHIFT, K_SYM, L"⇧" },

    { 5, 0, 0, 1, 0, K_ICON, L"", I_GLOBE },
    { 5, 0, 1, 1, VK_LCONTROL, K_SYM, L"⌃" }, { 5, 0, 2, 1, VK_LMENU, K_SYM, L"⌥" },
    { 5, 0, 3, 1.25f, VK_LWIN, K_CMD, L"⌘" }, { 5, 0, 4.25f, 5, VK_SPACE, K_TXT, L"" },
    { 5, 0, 9.25f, 1.25f, VK_RWIN, K_CMD, L"⌘" }, { 5, 0, 10.5f, 1, VK_RMENU, K_SYM, L"⌥" },
    { 5, 0, 11.5f, 1, VK_LEFT, K_ARROW, L"◀" }, { 5, 1, 12.5f, 1, VK_UP, K_ARROW, L"▲" },
    { 5, 2, 12.5f, 1, VK_DOWN, K_ARROW, L"▼" }, { 5, 0, 13.5f, 1, VK_RIGHT, K_ARROW, L"▶" },
};
#define NKEYS ((int)(sizeof KEYS / sizeof *KEYS))

static int kbP, kbV, kbG, kbX, kbY;                       // key pitch (x, y), gap and origin, in pixels

// Whole-pixel key grid: every 1-unit key gets the same width, the wide keys absorb the rounding.
static void kbLayout() {
    RECT b = R(KBX, KBY, KBW, KBH);
    int pad = px(11);
    kbG = px(4);
    kbP = (int)((b.right - b.left - 2 * pad + kbG) / 14.5f);
    kbV = (b.bottom - b.top - 2 * pad + kbG) / 6;
    if (kbV > kbP) kbV = kbP;
    int tw = (int)(14.5f * kbP + 0.5f) - kbG, th = 6 * kbV - kbG;
    kbX = b.left + (b.right - b.left - tw) / 2;
    kbY = b.top + (b.bottom - b.top - th) / 2;
}

static RECT keyRect(const Key &k) {
    RECT r;
    r.left = kbX + (int)(k.x * kbP + 0.5f);
    r.right = kbX + (int)((k.x + k.w) * kbP + 0.5f) - kbG;
    r.top = kbY + k.row * kbV;
    r.bottom = r.top + kbV - kbG;
    int half = (r.bottom - r.top - px(2)) / 2;            // up / down share one key position
    if (k.half == 1) r.bottom = r.top + half;
    if (k.half == 2) r.top = r.bottom - half;
    return r;
}

static int keyZone(const Key &k) {                        // clickable keys share a zone with their setting
    if (k.kind == K_F) return Z_F + (k.vk - VK_F1);
    if (k.kind == K_CMD) return Z_CMD + (k.vk == VK_RWIN);
    return -1;
}

// Is this key part of the shortcut the mouse is resting on?
static bool keyLit(const Key &k) {
    if (hot < Z_ROW || hot >= Z_ROW + NRULES || !k.vk) return false;
    const Rule &r = RULES[hot - Z_ROW];
    WORD v = k.vk;
    if (v == r.vk) return true;
    if ((r.mods & CMD) && ((v == VK_LWIN && !g_cmdWin[0]) || (v == VK_RWIN && !g_cmdWin[1]))) return true;
    if ((r.mods & OPT) && (v == VK_LMENU || v == VK_RMENU)) return true;
    if ((r.mods & CTL) && v == VK_LCONTROL) return true;
    if ((r.mods & SHF) && (v == VK_LSHIFT || v == VK_RSHIFT)) return true;
    return false;
}

static void drawKey(int i, bool solo) {
    const Key &k = KEYS[i];
    RECT rc = keyRect(k), sh = rc;
    OffsetRect(&sh, 0, hair());
    if (solo) { RECT cl = rc; cl.bottom = sh.bottom; fill(cl, C_KBBODY); }   // redrawing one key: clear it first
    bool live = g_present && k.vk;
    bool down = live && g_keys[k.vk];
    int z = keyZone(k);
    bool hi = (z >= 0 && z == hot) || keyLit(k);
    DWORD f = C_KEY, ln = C_KEYLN, tx = C_LEG;
    if (!live) { f = C_KEYOFF; ln = C_OFFLN; tx = C_OFFTX; }
    if (hi)    { f = C_TINT; ln = C_ACC; tx = C_TEXT; }
    if (down)  { f = C_ACC; ln = C_ACCDK; tx = C_WHITE; }
    box(sh, 5, down ? C_ACCDK : C_KEYSH);                 // a sliver of shadow under the keycap
    box(rc, 5, f, 256, ln, 256);
    float cx = (rc.left + rc.right) * 0.5f, cy = (rc.top + rc.bottom) * 0.5f;

    switch (k.kind) {
    case K_CHAR:  txt(F_KEY, tx, k.label, rc, DT_CENTER); break;
    case K_SYM:   txt(F_SYM, tx, k.label, rc, DT_CENTER); break;
    case K_ARROW: txt(k.half ? F_SYMX : F_SYMS, tx, k.label, rc, DT_CENTER); break;
    case K_TXT:   txt(F_TINY, tx, k.label, rc, DT_CENTER); break;
    case K_ICON:  icon(k.ic, cx, cy, (float)px(14), tx); break;
    case K_CMD:
        if (g_cmdWin[k.vk == VK_RWIN]) txt(F_TINY, tx, L"win", rc, DT_CENTER);
        else txt(F_SYM, tx, k.label, rc, DT_CENTER);
        break;
    case K_F: {
        // Icon = the assigned action, small label = the F-number. The one in effect is darker.
        int a = g_fact[k.vk - VK_F1];
        bool act = !g_fkeys && a;
        DWORD ic = down ? C_WHITE : !g_present ? C_OFFTX : act ? C_LEG : C_OFFTX;
        DWORD lb = down ? C_WHITE : !g_present ? C_OFFTX : act ? C_TEXT3 : C_LEG;
        int h = rc.bottom - rc.top;
        if (!a) { txt(F_TINY, lb, k.label, rc, DT_CENTER); break; }
        icon(a, cx, rc.top + h * 0.40f, (float)px(14), ic);
        RECT bot = { rc.left, rc.top + h * 64 / 100, rc.right, rc.bottom - h * 6 / 100 };
        txt(F_TINY, lb, k.label, bot, DT_CENTER);
        break;
    }
    }
}

// ---------------------------------------------------------------- layout of the clickable parts

static RECT lightRect(int i)  { return R(20 + i * 20, HH / 2 - 6, 12, 12); }           // 0 close, 1 minimize, 2 (unused)
static RECT segRect(int j)    { return R(TPX + TPW - 16 - 168 + j * 84, TPY + 11, 84, 24); }
static RECT fcellRect(int i)  { float w = (TPW - 32 - 16) / 3.0f; return R(TPX + 16 + (i % 3) * (w + 8), TPY + 44 + (i / 3) * 30, w, 24); }
static RECT cmdCellRect(int j){ float w = (TPW - 32 - 8) / 2.0f; return R(TPX + 16 + j * (w + 8), TPY + 194, w, 24); }
static RECT startRect()       { return R(20, FTY, 150, 28); }
static RECT quitRect()        { return R(CW - 20 - 104, FTY, 104, 28); }
static RECT defRect()         { return R(CW - 20 - 104 - 8 - 136, FTY, 136, 28); }

static RECT listView()        { return R(LSX + 1, LSY + 40, LSW - 2, LSH - 40 - 8); }
static int  rowH()            { return px(30); }

static int listMax() {
    RECT v = listView();
    int m = NRULES * rowH() - (v.bottom - v.top);
    return m > 0 ? m : 0;
}

static RECT rowRect(int i) {
    RECT r = { px(LSX + 8), listView().top + i * rowH() - scrollY, px(LSX + LSW - 16), 0 };
    r.bottom = r.top + rowH();
    return r;
}

static RECT thumbRect() {
    RECT v = listView();
    int vh = v.bottom - v.top, total = NRULES * rowH(), th = (int)((long long)vh * vh / total);
    if (th < px(24)) th = px(24);
    if (th > vh) th = vh;
    int ty = v.top + (listMax() ? (int)((long long)(vh - th) * scrollY / listMax()) : 0);
    RECT r = { px(LSX + LSW - 11), ty + px(2), px(LSX + LSW - 6), ty + th - px(2) };
    return r;
}

static int zoneAt(int x, int y) {
    POINT p = { x, y };
    RECT r;
    for (int i = 0; i < 2; i++) { r = lightRect(i); InflateRect(&r, px(3), px(3)); if (PtInRect(&r, p)) return Z_CLOSE + i; }
    r = R(KBX, KBY, KBW, KBH);
    if (PtInRect(&r, p)) {
        for (int i = 0; i < NKEYS; i++) {
            int z = keyZone(KEYS[i]);
            if (z < 0) continue;
            r = keyRect(KEYS[i]);
            if (PtInRect(&r, p)) return z;
        }
        return -1;
    }
    for (int i = 0; i < 12; i++) { r = fcellRect(i); if (PtInRect(&r, p)) return Z_F + i; }
    for (int j = 0; j < 2; j++) {
        r = cmdCellRect(j); if (PtInRect(&r, p)) return Z_CMD + j;
        r = segRect(j);     if (PtInRect(&r, p)) return Z_SEG + j;
    }
    r = listView();
    if (PtInRect(&r, p)) {
        if (listMax() && x >= px(LSX + LSW - 15)) return Z_THUMB;
        int i = (y - r.top + scrollY) / rowH();
        if (i >= 0 && i < NRULES) { r = rowRect(i); if (PtInRect(&r, p)) return Z_ROW + i; }
        return -1;
    }
    r = startRect(); if (PtInRect(&r, p)) return Z_START;
    r = defRect();   if (PtInRect(&r, p)) return Z_DEF;
    r = quitRect();  if (PtInRect(&r, p)) return Z_QUIT;
    return -1;
}

static int zoneSecs(int z) {                              // which parts of the window show this zone
    if (z < 0) return 0;
    if (z >= Z_ROW) return SEC_LIST | SEC_KB;
    if (z < Z_SEG) return SEC_KB | SEC_TOP;
    if (z < Z_START) return SEC_TOP;
    if (z == Z_THUMB) return SEC_LIST;
    if (z == Z_CLOSE || z == Z_MIN) return SEC_HEAD;
    return SEC_FOOT;
}

// ---------------------------------------------------------------- shared bits of drawing

static void toggle(RECT t, bool on) {
    box(t, 99, on ? C_GREEN : C_TRACK);
    int in = px(2), d = t.bottom - t.top - 2 * in;
    RECT k = { on ? t.right - in - d : t.left + in, t.top + in, 0, t.bottom - in };
    k.right = k.left + d;
    box(k, 99, C_WHITE, 256, 0x000000, 22);
}

// A pop-up button: white field with up/down chevrons. The caller draws the contents.
static void cell(RECT rc, bool h) {
    box(rc, 6, C_CARD, 256, h ? C_ACC : C_KEYLN, 256);
    icon(I_CHEVUD, rc.right - px(13), (rc.top + rc.bottom) * 0.5f, (float)px(12), h ? C_ACC : C_TEXT3);
}

static void button(RECT rc, const wchar_t *label, bool h, DWORD lineCol, DWORD textCol) {
    box(rc, 7, h ? C_HOVER : C_CARD, 256, lineCol, 256);
    txt(F_UI, textCol, label, rc, DT_CENTER);
}

// Draws "Cmd+Shift+Z" as a row of keycaps, Mac-style. Returns the x after the last one.
static int chips(const wchar_t *s, int x, RECT row, bool dim) {
    static const struct { const wchar_t *name, *sym; } MAP[] = {
        { L"Cmd", L"⌘" }, { L"Shift", L"⇧" }, { L"Option", L"⌥" }, { L"Control", L"⌃" },
        { L"Left", L"←" }, { L"Right", L"→" }, { L"Up", L"↑" }, { L"Down", L"↓" },
        { L"Backspace", L"⌫" },
    };
    int ch = px(19), top = (row.top + row.bottom - ch) / 2;
    while (*s) {
        wchar_t tok[16];
        int n = 0;
        while (*s && *s != L'+' && n < 15) tok[n++] = *s++;
        tok[n] = 0;
        if (*s == L'+') s++;
        const wchar_t *out = tok;
        int f = F_UIS;
        for (auto &m : MAP) if (!lstrcmpW(tok, m.name)) { out = m.sym; f = F_SYMS; }
        if (f == F_UIS && n > 1 && tok[0] != L'F') CharLowerW(tok);   // "tab", "space", "esc"
        int w = txtW(f, out) + px(10);
        if (w < ch) w = ch;
        RECT c = { x, top, x + w, top + ch }, sh = c;
        OffsetRect(&sh, 0, hair());
        box(sh, 4.5f, C_KEYSH);
        box(c, 4.5f, C_CARD, 256, C_KEYLN, 256);
        txt(f, dim ? C_TEXT3 : C_CHIPTX, out, c, DT_CENTER);
        x += w + px(3);
    }
    return x;
}

// ---------------------------------------------------------------- the five sections of the window

static void drawHeader() {
    RECT rc = R(0, 0, CW, HH);
    setClip(&rc);
    fill(rc, C_BG);
    int mid = px(HH) / 2;

    static const DWORD LIGHT[3] = { 0xFF5F57, 0xFEBC2E, 0xD6D6DB }, EDGE[3] = { 0xE0443E, 0xDEA123, 0xC4C4C9 };
    bool over = hot == Z_CLOSE || hot == Z_MIN;
    for (int i = 0; i < 3; i++) {                         // close, minimize, and an inactive third
        RECT b = lightRect(i);
        box(b, 99, LIGHT[i], 256, EDGE[i], 256);
        if (!over || i == 2) continue;
        float cx = (b.left + b.right) * 0.5f, cy = (b.top + b.bottom) * 0.5f, q = (b.right - b.left) * 0.21f, lw = fmx(1.2f, S * 1.1f);
        if (i == 0) { seg(cx - q, cy - q, cx + q, cy + q, lw, 0x4D0000, 200); seg(cx - q, cy + q, cx + q, cy - q, lw, 0x4D0000, 200); }
        else seg(cx - q * 1.25f, cy, cx + q * 1.25f, cy, lw, 0x5C3D00, 200);
    }

    txt(F_TITLE, C_TEXT, APP, rc, DT_CENTER);

    // Status, laid out right to left.
    int x = px(CW - 20);
    const wchar_t *st = g_present ? L"Connected by USB" : L"Not connected";
    int w = txtW(F_UIS, st);
    x -= w;
    RECT r1 = { x, 0, x + w, px(HH) };
    txt(F_UIS, C_TEXT2, st, r1);
    x -= px(13);
    RECT d = { x, mid - px(3.5f), x + px(7), mid - px(3.5f) + px(7) };
    box(d, 99, g_present ? C_GREEN : C_TEXT3);
    if (g_present) {
        wchar_t b[40];
        if (g_batt >= 0) wsprintfW(b, L"%d%%", g_batt); else lstrcpyW(b, L"Battery not readable over USB");
        w = txtW(F_UIS, b);
        x -= px(18) + w;
        RECT r2 = { x, 0, x + w, px(HH) };
        txt(F_UIS, g_batt >= 0 ? C_TEXT2 : C_TEXT3, b, r2);
        if (g_batt >= 0) {                                // little battery
            x -= px(6) + px(24);
            RECT body = { x, mid - px(5.5f), x + px(21), mid - px(5.5f) + px(11) }, nub = { body.right + hair(), mid - px(2), body.right + hair() + px(1.5f), mid + px(2) };
            box(body, 3, C_BG, 256, C_TEXT2, 256);
            fill(nub, C_TEXT2);
            RECT in = body;
            InflateRect(&in, -px(2), -px(2));
            in.right = in.left + (in.right - in.left) * g_batt / 100;
            box(in, 1.5f, g_batt <= 20 ? C_RED : C_GREEN);
        }
    }
}

static void drawKeyboard() {
    RECT rc = R(KBX, KBY, KBW, KBH);
    setClip(&rc);
    fill(rc, C_BG);
    box(rc, 16, C_KBBODY, 256, C_KBLINE, 256);            // the aluminium body
    for (int i = 0; i < NKEYS; i++) drawKey(i, false);
    if (!g_present) {
        RECT p = R(KBX + KBW / 2 - 100, KBY + KBH / 2 - 15, 200, 30);
        box(p, 99, C_CARD, 244, C_KEYLN, 256);
        txt(F_UI, C_TEXT2, L"Keyboard not connected", p, DT_CENTER);
    }
}

static void drawTop() {
    RECT rc = R(TPX, TPY, TPW, TPH);
    setClip(&rc);
    fill(rc, C_BG);
    box(rc, 12, C_CARD, 256, C_LINE, 256);

    txt(F_UIB, C_TEXT, L"Top row", R(TPX + 16, TPY + 11, 80, 24));
    txt(F_UIS, C_TEXT2, L"Hold Right Option with a key for its other function", R(TPX + 76, TPY + 11, 300, 24));

    RECT seg2 = R(TPX + TPW - 16 - 168, TPY + 11, 168, 24);
    box(seg2, 7, C_TRACK);
    for (int j = 0; j < 2; j++) {
        RECT s = segRect(j);
        bool on = g_fkeys == (j == 1);
        if (on) { RECT k = s; InflateRect(&k, -px(2), -px(2)); box(k, 5.5f, C_CARD, 256, 0x000000, 28); }
        txt(F_UIS, on || hot == Z_SEG + j ? C_TEXT : C_TEXT2, j ? L"F1 – F12" : L"Actions", s, DT_CENTER);
    }

    for (int i = 0; i < 12; i++) {
        RECT c = fcellRect(i);
        int a = g_fact[i];
        wchar_t lab[4];
        wsprintfW(lab, L"F%d", i + 1);
        cell(c, hot == Z_F + i);
        RECT r1 = { c.left + px(10), c.top, c.left + px(32), c.bottom };
        RECT r3 = { c.left + px(56), c.top, c.right - px(24), c.bottom };
        txt(F_UIS, C_TEXT3, lab, r1);
        if (a) icon(a, (float)(c.left + px(41)), (c.top + c.bottom) * 0.5f, (float)px(14), C_LEG);
        txt(F_UIS, a ? C_TEXT : C_TEXT2, ACTIONS[a], r3, DT_END_ELLIPSIS);
    }

    RECT ln = R(TPX + 16, TPY + 166, TPW - 32, 1);
    ln.bottom = ln.top + hair();
    fill(ln, C_SEP);
    txt(F_UIB, C_TEXT, L"Command keys", R(TPX + 16, TPY + 171, 200, 20));
    for (int j = 0; j < 2; j++) {
        RECT c = cmdCellRect(j);
        cell(c, hot == Z_CMD + j);
        RECT r1 = { c.left + px(10), c.top, c.left + px(44), c.bottom };
        RECT r2 = { c.left + px(44), c.top, c.left + px(62), c.bottom };
        RECT r3 = { c.left + px(68), c.top, c.right - px(24), c.bottom };
        txt(F_UIS, C_TEXT3, j ? L"Right" : L"Left", r1);
        txt(F_SYMS, g_cmdWin[j] ? C_TEXT3 : C_LEG, L"⌘", r2, DT_CENTER);
        txt(F_UIS, C_TEXT, g_cmdWin[j] ? L"Windows key" : L"Command (Mac shortcuts)", r3, DT_END_ELLIPSIS);
    }
}

static void drawList() {
    RECT rc = R(LSX, LSY, LSW, LSH);
    setClip(&rc);
    fill(rc, C_BG);
    box(rc, 12, C_CARD, 256, C_LINE, 256);
    int on = 0;
    for (int i = 0; i < NRULES; i++) on += (g_rules >> i) & 1;
    wchar_t count[32];
    wsprintfW(count, L"%d of %d on", on, NRULES);
    txt(F_UIB, C_TEXT, L"Shortcuts", R(LSX + 16, LSY + 8, 120, 24));
    txt(F_UIS, C_TEXT2, count, R(LSX + 16, LSY + 8, LSW - 32, 24), DT_RIGHT);
    RECT ln = R(LSX + 1, LSY + 39, LSW - 2, 1);
    ln.bottom = ln.top + hair();
    fill(ln, C_SEP);

    RECT v = listView();
    setClip(&v);
    for (int i = 0; i < NRULES; i++) {
        RECT r = rowRect(i);
        if (r.bottom <= v.top || r.top >= v.bottom) continue;
        bool en = (g_rules >> i) & 1;
        if (hot == Z_ROW + i) { RECT hr = r; InflateRect(&hr, 0, -px(1)); box(hr, 7, C_HOVER); }
        else if (i < NRULES - 1 && hot != Z_ROW + i + 1) {
            RECT sp = { r.left + px(8), r.bottom - hair(), r.right - px(8), r.bottom };
            fill(sp, C_SEP);
        }
        chips(RULES[i].mac, r.left + px(8), r, !en);
        RECT tr = { r.left + px(108), r.top, r.right - px(46), r.bottom };
        txt(F_UI, en ? C_TEXT : C_TEXT3, RULES[i].win, tr, DT_END_ELLIPSIS);
        int cy = (r.top + r.bottom) / 2;
        RECT t = { r.right - px(38), cy - px(9), r.right - px(8), cy + px(9) };
        toggle(t, en);
    }
    setClip(&rc);
    if (listMax()) box(thumbRect(), 99, (hot == Z_THUMB || dragging) ? 0x8E8E93 : 0xC7C7CC);
}

static void drawFooter() {
    RECT rc = R(0, TPY + TPH, CW, CH - TPY - TPH);
    setClip(&rc);
    fill(rc, C_BG);

    RECT t = R(20, FTY + 5, 30, 18);
    toggle(t, g_startup);
    txt(F_UI, C_TEXT, L"Run on startup", R(58, FTY, 110, 28));

    int x = px(196);
    RECT row = { x, px(FTY), px(650), px(FTY + 28) };
    if (g_last < 0) {
        txt(F_UIS, C_TEXT3, L"Press a shortcut or a top-row key to try it out", row);
    } else {
        const wchar_t *mac, *win;
        wchar_t fk[8];
        if (g_last >= 100) { wsprintfW(fk, L"F%d", g_last - 99); mac = fk; win = ACTIONS[g_fact[g_last - 100]]; }
        else { mac = RULES[g_last].mac; win = RULES[g_last].win; }
        x = chips(mac, x, row, false);
        RECT ar = { x + px(3), row.top, x + px(21), row.bottom };
        txt(F_SYMS, C_TEXT3, L"→", ar, DT_CENTER);
        row.left = x + px(27);
        txt(F_UI, C_TEXT, win, row);
    }

    if (defArmed) button(defRect(), L"Click again to reset", true, C_ORANGE, C_ORANGE);
    else button(defRect(), L"Restore defaults", hot == Z_DEF, C_KEYLN, C_TEXT);
    button(quitRect(), L"Quit adapter", hot == Z_QUIT, C_KEYLN, hot == Z_QUIT ? C_RED : C_TEXT);
}

static void inval(RECT rc) { InvalidateRect(g_ui, &rc, FALSE); }

// Repaint parts of the off-screen bitmap and ask Windows to copy them to the screen.
static void render(int secs) {
    if (!g_ui || !sf.dc) return;
    if (secs & SEC_HEAD) { drawHeader();   inval(R(0, 0, CW, HH)); }
    if (secs & SEC_KB)   { drawKeyboard(); inval(R(KBX, KBY, KBW, KBH)); }
    if (secs & SEC_TOP)  { drawTop();      inval(R(TPX, TPY, TPW, TPH)); }
    if (secs & SEC_LIST) { drawList();     inval(R(LSX, LSY, LSW, LSH)); }
    if (secs & SEC_FOOT) { drawFooter();   inval(R(0, TPY + TPH, CW, CH - TPY - TPH)); }
    setClip(NULL);
}

// Called from the keyboard hook: repaint just the one key, so typing is never held up.
static void uiKey(DWORD vk, bool down) {
    if (g_keys[vk] == down || !g_ui || !sf.dc) return;
    g_keys[vk] = down;
    for (int i = 0; i < NKEYS; i++) {
        if (KEYS[i].vk != vk) continue;
        drawKey(i, true);
        RECT r = keyRect(KEYS[i]);
        r.bottom += hair();
        inval(r);
    }
}

static void uiLast(int what) {
    g_last = what;
    if (g_ui) render(SEC_FOOT);
}

// ---------------------------------------------------------------- temporary surfaces

static HGDIOBJ surfOld;

// Point the drawing primitives at a fresh w x h bitmap. Pair with surfEnd; the caller owns the bitmap.
static HBITMAP surfBegin(Surface &keep, int w, int h) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    DWORD *p = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&p, NULL, 0);
    if (!bmp) { DeleteDC(dc); return NULL; }
    surfOld = SelectObject(dc, bmp);
    keep = sf;
    sf.px = p; sf.w = w; sf.h = h; sf.dc = dc;
    SetBkMode(dc, TRANSPARENT);
    setClip(NULL);
    return bmp;
}

static void surfEnd(Surface &keep) {
    GdiFlush();
    SelectObject(sf.dc, surfOld);
    DeleteDC(sf.dc);
    sf = keep;
}

// ---------------------------------------------------------------- popup menus (owner-drawn to match the window)

static const wchar_t *CMDROLE[] = { L"Command (Mac shortcuts)", L"Windows key" };

// kind 0: top-row actions, kind 1: Command key role. Returns the chosen index, or -1.
static int popup(int kind, int cur, RECT anchor) {
    static const short ACT_ORDER[] = { 0, -1, 1, 2, -1, 3, 4, 5, 12, 13, 14, -2, 6, 7, 8, -1, 9, 10, 11, -1, 15, 16, 17 };
    static const short ROLE_ORDER[] = { 0, 1 };
    const short *order = kind ? ROLE_ORDER : ACT_ORDER;
    int n = kind ? 2 : (int)(sizeof ACT_ORDER / sizeof *ACT_ORDER);

    HMENU m = CreatePopupMenu();
    MENUINFO mi = { sizeof mi };
    mi.fMask = MIM_BACKGROUND | MIM_STYLE;
    mi.hbrBack = menuBrush;
    mi.dwStyle = MNS_NOCHECK;
    SetMenuInfo(m, &mi);
    UINT brk = 0;
    for (int i = 0; i < n; i++) {
        if (order[i] == -2) { brk = MF_MENUBREAK; continue; }             // start the second column
        if (order[i] == -1) AppendMenuW(m, MF_OWNERDRAW | MF_DISABLED | brk, 0, (LPCWSTR)(ULONG_PTR)0xFFFF);
        else AppendMenuW(m, MF_OWNERDRAW | brk, order[i] + 1, (LPCWSTR)(ULONG_PTR)MAKELONG(order[i], kind));
        brk = 0;
    }
    menuCur = cur;
    RECT client = anchor;
    MapWindowPoints(g_ui, NULL, (POINT *)&anchor, 2);
    TPMPARAMS tp = { sizeof tp, anchor };
    int c = TrackPopupMenuEx(m, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_VERTICAL | TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON,
                             anchor.left, anchor.bottom + px(4), g_ui, &tp);
    DestroyMenu(m);

    // Clicking the same cell again should just close the menu, not reopen it.
    MSG msg;
    if (PeekMessageW(&msg, g_ui, WM_LBUTTONDOWN, WM_LBUTTONDOWN, PM_NOREMOVE)) {
        POINT p = { GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam) };
        if (PtInRect(&client, p)) PeekMessageW(&msg, g_ui, WM_LBUTTONDOWN, WM_LBUTTONDOWN, PM_REMOVE);
    }
    return c - 1;
}

static void menuMeasure(MEASUREITEMSTRUCT *m) {
    bool sep = m->itemData == 0xFFFF;
    m->itemWidth = px(HIWORD(m->itemData) && !sep ? 226 : 178);
    m->itemHeight = sep ? px(9) : px(26);
}

static void menuDraw(DRAWITEMSTRUCT *d) {
    int w = d->rcItem.right - d->rcItem.left, h = d->rcItem.bottom - d->rcItem.top;
    if (w <= 0 || h <= 0) return;
    bool sep = d->itemData == 0xFFFF;
    int kind = HIWORD(d->itemData), idx = LOWORD(d->itemData);
    bool sel = (d->itemState & ODS_SELECTED) && !sep, cur = !sep && idx == menuCur;

    Surface keep;                                         // draw the item off-screen with our own primitives
    HBITMAP bmp = surfBegin(keep, w, h);
    if (!bmp) return;
    RECT all = { 0, 0, w, h };
    fill(all, C_MENU);
    if (sep) {
        RECT l = { px(10), h / 2, w - px(10), h / 2 + hair() };
        fill(l, C_LINE);
    } else {
        if (sel) { RECT s = { px(4), 0, w - px(4), h }; box(s, 5.5f, C_ACC); }
        DWORD fg = sel ? C_WHITE : C_TEXT, ic = sel ? C_WHITE : C_LEG;
        if (cur) icon(I_CHECK, (float)px(14), h * 0.5f, (float)px(11), fg);
        RECT ri = { px(24), 0, px(46), h }, rt = { px(50), 0, w - px(8), h };
        if (kind) { if (!idx) txt(F_SYMS, ic, L"⌘", ri, DT_CENTER); }
        else if (idx) icon(idx, (float)px(35), h * 0.5f, (float)px(14), ic);
        txt(F_UI, fg, kind ? CMDROLE[idx] : ACTIONS[idx], rt);
    }
    GdiFlush();
    BitBlt(d->hDC, d->rcItem.left, d->rcItem.top, w, h, sf.dc, 0, 0, SRCCOPY);
    surfEnd(keep);
    DeleteObject(bmp);
}

// ---------------------------------------------------------------- app icon (tray, taskbar, Alt+Tab)
// A rounded blue tile with a white Command symbol.

static HICON makeIcon(int s) {
    if (s < 16) s = 16;
    if (s > 256) s = 256;
    Surface keep;
    float keepS = S;
    HBITMAP col = surfBegin(keep, s, s);
    if (!col) return NULL;
    DWORD *p = sf.px;
    S = s / 32.0f;
    for (int y = 0; y < s; y++) {                         // soft top-to-bottom gradient
        int t = y * 256 / s;
        DWORD c = 0x3C9DFF;
        mix(&c, 0x0A63E8, t);
        for (int x = 0; x < s; x++) p[y * s + x] = c;
    }
    RECT all = { 0, 0, s, s };
    HFONT f = CreateFontW(-(s * 21 / 32), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI Symbol");
    HGDIOBJ oldFont = SelectObject(sf.dc, f);
    SetTextColor(sf.dc, RGB(255, 255, 255));
    DrawTextW(sf.dc, L"⌘", 1, &all, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    GdiFlush();
    SelectObject(sf.dc, oldFont);
    DeleteObject(f);

    float r = 7.5f * S, c = s * 0.5f, hw = c - r;         // alpha = how much of each pixel the tile covers
    for (int y = 0; y < s; y++) {
        for (int x = 0; x < s; x++) {
            float a = 0.5f - sdf(fabsf(x + 0.5f - c) - hw, fabsf(y + 0.5f - c) - hw, r);
            a = a < 0 ? 0 : a > 1 ? 1 : a;
            DWORD &q = p[y * s + x];
            q = a > 0 ? ((q & 0xFFFFFF) | ((DWORD)(a * 255) << 24)) : 0;
        }
    }
    surfEnd(keep);
    S = keepS;

    static BYTE zero[32 * 256];
    HBITMAP mask = CreateBitmap(s, s, 1, 1, zero);
    ICONINFO ii = { TRUE, 0, 0, mask, col };
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(col);
    return ic;
}

// ---------------------------------------------------------------- the window itself

static void dwmStyle(HWND h) {
    HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dwm) return;
    typedef HRESULT (WINAPI *SetAttr)(HWND, DWORD, LPCVOID, DWORD);
    SetAttr set = (SetAttr)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (!set) return;
    COLORREF line = cref(0xC4C4C9);
    set(h, 34, &line, sizeof line);                       // DWMWA_BORDER_COLOR (Windows 11)
}

static void uiFree() {
    if (sf.dc) { SelectObject(sf.dc, uiOldBmp); DeleteDC(sf.dc); }
    if (uiBmp) DeleteObject(uiBmp);
    for (HFONT &f : fnt) { if (f) DeleteObject(f); f = NULL; }
    if (menuBrush) DeleteObject(menuBrush);
    uiBmp = NULL;
    menuBrush = NULL;
    ZeroMemory(&sf, sizeof sf);
    ZeroMemory(g_keys, sizeof g_keys);
    g_ui = NULL;
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);   // hand the UI memory back
}

static void setScroll(int v) {
    int m = listMax();
    if (v < 0) v = 0;
    if (v > m) v = m;
    if (v == scrollY) return;
    scrollY = v;
    render(SEC_LIST);
}

static void setHot(int z) {
    if (z == hot) return;
    int old = hot;
    hot = z;
    render(zoneSecs(old) | zoneSecs(z));
}

static void hotFromCursor(HWND h) {
    POINT s, c;
    GetCursorPos(&s);
    c = s;
    ScreenToClient(h, &c);
    setHot(WindowFromPoint(s) == h ? zoneAt(c.x, c.y) : -1);
}

static void thumbTo(int top) {                            // drag the scrollbar thumb so its top is at `top`
    RECT v = listView(), t = thumbRect();
    int range = (v.bottom - v.top) - (t.bottom - t.top + px(4));
    if (range > 0) setScroll((int)((long long)(top - v.top) * listMax() / range));
}

static void click(HWND h, int x, int y) {
    int z = zoneAt(x, y);
    setHot(z);
    if (z != Z_DEF && defArmed) { defArmed = false; KillTimer(h, 1); render(SEC_FOOT); }
    if (z < 0) return;

    if (z >= Z_ROW) {
        g_rules ^= 1u << (z - Z_ROW);
        saveConfig();
        render(SEC_LIST);
    } else if (z < Z_SEG) {                               // a top-row key or a Command key: open its menu
        bool isCmd = z >= Z_CMD;
        int i = z - (isCmd ? Z_CMD : Z_F);
        POINT p = { x, y };
        RECT a = isCmd ? cmdCellRect(i) : fcellRect(i);
        if (!PtInRect(&a, p))                             // clicked the key on the picture instead
            for (int k = 0; k < NKEYS; k++) if (keyZone(KEYS[k]) == z) a = keyRect(KEYS[k]);
        int c = popup(isCmd, isCmd ? g_cmdWin[i] : g_fact[i], a);
        if (c >= 0) {
            if (isCmd) { resetState(); g_cmdWin[i] = c != 0; }
            else g_fact[i] = (BYTE)c;
            saveConfig();
        }
        render(SEC_KB | SEC_TOP);
        hotFromCursor(h);
    } else if (z == Z_SEG || z == Z_SEG + 1) {
        g_fkeys = z == Z_SEG + 1;
        saveConfig();
        render(SEC_KB | SEC_TOP);
    } else if (z == Z_START) {
        setStartup(!g_startup);
        g_startup = startupEnabled();
        render(SEC_FOOT);
    } else if (z == Z_DEF) {
        if (!defArmed) { defArmed = true; SetTimer(h, 1, 3000, NULL); render(SEC_FOOT); return; }
        defArmed = false;
        KillTimer(h, 1);
        resetState();
        defaults();
        saveConfig();
        render(SEC_ALL);
    } else if (z == Z_QUIT) {
        DestroyWindow(g_hwnd);                            // quits the whole app
    } else if (z == Z_THUMB) {
        RECT t = thumbRect();
        dragOff = (y >= t.top && y < t.bottom) ? y - t.top + px(2) : (t.bottom - t.top) / 2;
        dragging = true;
        SetCapture(h);
        thumbTo(y - dragOff);
        render(SEC_LIST);
    } else if (z == Z_MIN) {
        hot = -1;
        render(SEC_HEAD);
        PostMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    } else if (z == Z_CLOSE) {
        PostMessageW(h, WM_CLOSE, 0, 0);
    }
}

static LRESULT CALLBACK uiProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCALCSIZE:                                   // no system title bar: the whole window is ours
        return 0;
    case WM_NCACTIVATE:
        return DefWindowProcW(h, msg, wp, -1);            // -1: do not repaint a classic frame
    case WM_NCHITTEST: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &p);
        return (p.y < px(HH) && zoneAt(p.x, p.y) < 0) ? HTCAPTION : HTCLIENT;   // drag by the header
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (sf.dc)
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
                   sf.dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(NULL, hot >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (dragging) { thumbTo(y - dragOff); return 0; }
        setHot(zoneAt(x, y));
        if (!tracking) {
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 };
            tracking = TrackMouseEvent(&t) != 0;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking = false;
#ifndef UITEST
        if (!dragging) setHot(-1);
#endif
        return 0;
    case WM_LBUTTONDOWN:
        click(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        if (dragging) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        if (dragging) { dragging = false; render(SEC_LIST); }
        return 0;
    case WM_MOUSEWHEEL:
        setScroll(scrollY - GET_WHEEL_DELTA_WPARAM(wp) * rowH() * 3 / WHEEL_DELTA);
#ifndef UITEST
        hotFromCursor(h);
#endif
        return 0;
    case WM_TIMER:                                        // "click again to reset" timed out
        KillTimer(h, 1);
        defArmed = false;
        render(SEC_FOOT);
        return 0;
    case WM_MEASUREITEM:
        if (((MEASUREITEMSTRUCT *)lp)->CtlType == ODT_MENU) { menuMeasure((MEASUREITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_DRAWITEM:
        if (((DRAWITEMSTRUCT *)lp)->CtlType == ODT_MENU) { menuDraw((DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_SYSCOMMAND:                                   // tapping Option must not put the window in menu mode
        if ((wp & 0xFFF0) == SC_KEYMENU && lp != ' ') return 0;
        break;
    case WM_SYSCHAR:                                      // ...and Option+letter must not beep
        if (wp != ' ') return 0;
        break;
#ifdef UITEST
    case WM_APP + 20: uiKey((DWORD)wp, lp != 0); return 0;
    case WM_APP + 21: uiLast((int)wp); return 0;
    case WM_APP + 22: g_present = wp != 0; render(SEC_ALL); return 0;
#endif
    case WM_DESTROY:
        uiFree();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void openSettings() {
    if (g_ui) {
        ShowWindow(g_ui, IsIconic(g_ui) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(g_ui);
        return;
    }
    HINSTANCE inst = GetModuleHandleW(NULL);
    static bool registered;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = uiProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"MagicAdapterUI";
        RegisterClassW(&wc);
        registered = true;
    }

    HDC sdc = GetDC(NULL);
    S = GetDeviceCaps(sdc, LOGPIXELSY) / 96.0f;
    ReleaseDC(NULL, sdc);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    while (S > 0.6f && (px(CW) > wa.right - wa.left - 16 || px(CH) > wa.bottom - wa.top - 16)) S -= 0.05f;   // small screens
    int w = px(CW), h = px(CH);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    sf.dc = CreateCompatibleDC(NULL);
    uiBmp = CreateDIBSection(sf.dc, &bi, DIB_RGB_COLORS, (void **)&sf.px, NULL, 0);
    if (!uiBmp) { uiFree(); return; }
    uiOldBmp = SelectObject(sf.dc, uiBmp);
    sf.w = w; sf.h = h;
    SetBkMode(sf.dc, TRANSPARENT);
    GdiSetBatchLimit(1);                                  // we touch the pixels directly between GDI calls
    mkFonts();
    menuBrush = CreateSolidBrush(cref(C_MENU));
    kbLayout();
    scrollY = 0; hot = -1; g_last = -1;
    dragging = tracking = defArmed = false;
    g_startup = startupEnabled();

    HWND win = CreateWindowExW(0, L"MagicAdapterUI", APP, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h,
                               NULL, NULL, inst, NULL);
    if (!win) { uiFree(); return; }
    g_ui = win;
    dwmStyle(win);
    static HICON big;
    if (!big) big = makeIcon(GetSystemMetrics(SM_CXICON));
    SendMessageW(win, WM_SETICON, ICON_BIG, (LPARAM)big);
    SendMessageW(win, WM_SETICON, ICON_SMALL, (LPARAM)g_nid.hIcon);

    setClip(NULL);
    RECT all = { 0, 0, w, h };
    fill(all, C_BG);
    render(SEC_ALL);
    ShowWindow(win, SW_SHOWNORMAL);
    SetForegroundWindow(win);
}
