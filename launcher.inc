/* ------------------------------------------------------------------ launch settings window
 * Shown once at startup (before the game sets up its display) when AskOnLaunch=1: detects
 * the monitor, lets the player pick the game resolution, render quality and effects, and
 * writes them to sswrap.ini and to the game's own defaultPrefs.cs (Starsiege keeps its
 * resolution there). Plain Win32, so it works the same on Windows and under Wine. */

#define ID_RES     101
#define ID_SCALE   102
#define ID_AO      103
#define ID_AA      104
#define ID_ASK     105
#define ID_HINT    106
#define ID_START   107
#define ID_KEEP    108
#define ID_HUD     109
#define ID_UWHUD   110
#define ID_HSCALE  111
static void apply_hud_layout(int ultrawide, int gw, int gh);

static struct { int w, h; char label[96]; } g_res_items[80];
static int g_nres_items, g_launcher_done, g_launcher_result;
static HWND g_dlg;
static void launcher_capture_and_apply(HWND h);

static void game_file(char *out, int n, const char *name)
{
    GetModuleFileNameA(g_self, out, n);
    char *p = strrchr(out, '\\'); lstrcpynA(p ? p + 1 : out, name, n - (int)(p ? p + 1 - out : 0));
}

/* read the game's current fullscreen size from defaultPrefs.cs (0 if unknown) */
static void read_game_res(int *w, int *h)
{
    char path[MAX_PATH], line[256]; FILE *f;
    *w = *h = 0;
    game_file(path, sizeof path, "defaultPrefs.cs");
    if (!(f = fopen(path, "rb"))) return;
    while (fgets(line, sizeof line, f)) {
        sscanf(line, "$pref::GWC::SIM_FS_WIDTH = \"%d\"", w);
        sscanf(line, "$pref::GWC::SIM_FS_HEIGHT = \"%d\"", h);
    }
    fclose(f);
}

/* rewrite the display lines of defaultPrefs.cs, keeping everything else as it is */
static int write_game_res(int w, int h)
{
    char path[MAX_PATH]; FILE *f; long n; char *buf, *out, *line, *ctx = NULL; int changed = 0;
    game_file(path, sizeof path, "defaultPrefs.cs");
    if (!(f = fopen(path, "rb"))) { lg(0, "launcher: %s not found; resolution not saved", path); return 0; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc(n + 1); out = malloc(n + 512);
    if (!buf || !out || fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); free(out); return 0; }
    fclose(f); buf[n] = 0; out[0] = 0;
    for (line = strtok_r(buf, "\n", &ctx); line; line = strtok_r(NULL, "\n", &ctx)) {
        char rep[128] = "";
        if (!strncmp(line, "$pref::GWC::SIM_FS_WIDTH ", 25)) snprintf(rep, sizeof rep, "$pref::GWC::SIM_FS_WIDTH = \"%d\";\r", w);
        else if (!strncmp(line, "$pref::GWC::SIM_FS_HEIGHT ", 26)) snprintf(rep, sizeof rep, "$pref::GWC::SIM_FS_HEIGHT = \"%d\";\r", h);
        else if (!strncmp(line, "$pref::GWC::SIM_FS_DEVICE ", 26)) strcpy(rep, "$pref::GWC::SIM_FS_DEVICE = \"OpenGL\";\r");
        else if (!strncmp(line, "$pref::GWC::SIM_IS_FULLSCREEN ", 30)) strcpy(rep, "$pref::GWC::SIM_IS_FULLSCREEN = \"True\";\r");
        if (rep[0]) changed++;
        strcat(out, rep[0] ? rep : line); strcat(out, "\n");
    }
    if ((f = fopen(path, "wb"))) { fwrite(out, 1, strlen(out), f); fclose(f); }
    free(buf); free(out);
    lg(1, "launcher: wrote %dx%d to defaultPrefs.cs (%d lines updated)", w, h, changed);
    return changed;
}

static void ini_set(const char *key, const char *val)
{
    char ini[MAX_PATH]; game_file(ini, sizeof ini, "sswrap.ini");
    WritePrivateProfileStringA("sswrap", key, val, ini);
}

static void add_res(int w, int h, const char *fmt)
{
    if (g_nres_items >= 80 || w <= 0 || h <= 0) return;
    for (int i = 0; i < g_nres_items; i++) if (g_res_items[i].w == w && g_res_items[i].h == h) return;
    g_res_items[g_nres_items].w = w; g_res_items[g_nres_items].h = h;
    snprintf(g_res_items[g_nres_items].label, sizeof g_res_items[0].label, fmt, w, h);
    g_nres_items++;
}

/* The window, two layouts:
 *  - 1.004 or 1.003 (HUD hooks available): the game runs at the monitor's native resolution and
 *    HUD scale enlarges the HUD meters; resolution is a separate, normally untouched choice.
 *  - other builds: no HUD hooks, so a bigger HUD means running the game at native / k
 *    ("HUD size"), with RenderScale keeping the 3D world sharp. */
static const float g_huds[] = { 1.0f, 1.25f, 1.5f, 2.0f, 3.0f };      /* HUD size (other builds) */
#define NHUDS 5
static const float g_hscales[] = { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };  /* HUD scale (1.004/1.003) */
#define NHSCALES 5
static const int g_quals[] = { 1, 2, 3 };
static int g_hud_ok;
#define ID_UIUP 112
#define ID_TEXT 113

static void hud_res(float k, int *w, int *h) { *w = ((int)(g_rw / k + 0.5f)) & ~1; *h = ((int)(g_rh / k + 0.5f)) & ~1; }
static int sel(HWND d, int id) { int i = (int)SendDlgItemMessageA(d, id, CB_GETCURSEL, 0, 0); return i < 0 ? 0 : i; }
static int checked(HWND d, int id) { return SendDlgItemMessageA(d, id, BM_GETCHECK, 0, 0) == BST_CHECKED; }

/* the game resolution and RenderScale the current choices add up to */
static void launcher_choice(HWND d, int *gw, int *gh, float *scale)
{
    int ri = sel(d, ID_RES);
    if (ri > 0) { *gw = g_res_items[ri - 1].w; *gh = g_res_items[ri - 1].h; }
    else if (g_hud_ok) { *gw = g_rw; *gh = g_rh; }
    else hud_res(g_huds[sel(d, ID_HUD)], gw, gh);
    /* render at (at least) the monitor's resolution, times the chosen quality */
    float sx = (float)g_rw / *gw, sy = (float)g_rh / *gh, k = sx < sy ? sx : sy;
    *scale = (k < 1.0f ? 1.0f : k) * g_quals[sel(d, ID_SCALE)];
    if (*scale > 8.0f) *scale = 8.0f;
}

static void update_hint(void)
{
    int gw, gh; float sc; char t[240];
    launcher_choice(g_dlg, &gw, &gh, &sc);
    if (!g_hud_ok) EnableWindow(GetDlgItem(g_dlg, ID_HUD), sel(g_dlg, ID_RES) == 0);
    snprintf(t, sizeof t, "The game runs at %d x %d; the 3D world is rendered at %d x %d and shown on your %d x %d monitor.",
             gw, gh, (int)(gw * sc + 0.5f), (int)(gh * sc + 0.5f), g_rw, g_rh);
    SetDlgItemTextA(g_dlg, ID_HINT, t);
}

static LRESULT CALLBACK launcher_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_COMMAND) {
        int id = LOWORD(w);
        if ((id == ID_RES || id == ID_SCALE || id == ID_HUD || id == ID_HSCALE) && HIWORD(w) == CBN_SELCHANGE) update_hint();
        if (id == ID_START || id == ID_KEEP) {
            if (id == ID_START) launcher_capture_and_apply(h);   /* before the controls are destroyed */
            g_launcher_result = id; g_launcher_done = 1; DestroyWindow(h);
        }
        return 0;
    }
    if (m == WM_CLOSE) { g_launcher_result = ID_KEEP; g_launcher_done = 1; DestroyWindow(h); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static HWND ctl(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_dlg, (HMENU)(INT_PTR)id, g_self, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}
static HWND combo(const char *label, int y, int id)
{
    ctl("STATIC", label, 0, 16, y + 4, 130, 18, 0);
    return ctl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 150, y, 406, 300, id);
}
static HWND check(const char *label, int y, int id, int on)
{
    HWND c = ctl("BUTTON", label, BS_AUTOCHECKBOX | WS_TABSTOP, 150, y, 406, 20, id);
    SendMessageA(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    return c;
}

static void show_launcher(void)
{
    static int shown;
    if (shown || !cfg.enabled || !cfg.ask_on_launch) return;
    shown = 1;
    g_hud_ok = hud_supported();

    int cw, ch; read_game_res(&cw, &ch);
    for (int i = 0; i < g_nmodes; i++) add_res(g_modes[i].w, g_modes[i].h, "%d x %d");
    if (cw > 0 && ch > 0) add_res(cw, ch, "%d x %d  (current)");

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = launcher_proc; wc.hInstance = g_self; wc.lpszClassName = "sswrapLauncher";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);
    int W = 580, H = 400;
    g_dlg = CreateWindowExA(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, "sswrapLauncher", "Starsiege - display settings (sswrap)",
                            WS_POPUP | WS_CAPTION | WS_SYSMENU, (r_GetSystemMetrics(SM_CXSCREEN) - W) / 2,
                            (r_GetSystemMetrics(SM_CYSCREEN) - H) / 2, W, H, NULL, NULL, g_self, NULL);
    if (!g_dlg) { lg(0, "launcher: could not create the settings window"); return; }

    char t[160]; int y = 14;
    snprintf(t, sizeof t, "Detected monitor: %d x %d", g_rw, g_rh);
    ctl("STATIC", t, 0, 16, y, 540, 18, 0); y += 30;

    /* resolution: native unless the player picks something else */
    HWND res = combo("Resolution:", y, ID_RES); y += 34;
    snprintf(t, sizeof t, g_hud_ok ? "Native  %d x %d  (recommended)" : "Automatic (from HUD size)", g_rw, g_rh);
    SendMessageA(res, CB_ADDSTRING, 0, (LPARAM)t);
    for (int i = 0; i < g_nres_items; i++) SendMessageA(res, CB_ADDSTRING, 0, (LPARAM)g_res_items[i].label);
    int rsel = 0;
    if (cw > 0 && !(cw == g_rw && ch == g_rh) && g_hud_ok)
        for (int i = 0; i < g_nres_items; i++) if (g_res_items[i].w == cw && g_res_items[i].h == ch) rsel = i + 1;
    SendMessageA(res, CB_SETCURSEL, rsel, 0);

    if (g_hud_ok) {
        HWND hs = combo("HUD scale:", y, ID_HSCALE); y += 34;
        int hsel = 0;
        for (int i = 0; i < NHSCALES; i++) {
            snprintf(t, sizeof t, i == 0 ? "Off  (the game's own size)" : "%.3gx", g_hscales[i]);
            SendMessageA(hs, CB_ADDSTRING, 0, (LPARAM)t);
            if (cfg.hud_scale > g_hscales[i] - 0.01f) hsel = i;
        }
        SendMessageA(hs, CB_SETCURSEL, hsel, 0);
    } else {
        /* other builds: a bigger HUD comes from a smaller game resolution */
        HWND hud = combo("HUD size:", y, ID_HUD); y += 34;
        int hsel = 0; float best = 1e9f;
        for (int i = 0; i < NHUDS; i++) {
            int w, h; hud_res(g_huds[i], &w, &h);
            snprintf(t, sizeof t, "%.3gx   (game runs at %d x %d)", g_huds[i], w, h);
            SendMessageA(hud, CB_ADDSTRING, 0, (LPARAM)t);
            if (cw > 0) { float dd = (float)(w - cw) * (w - cw); if (dd < best) { best = dd; hsel = i; } }
        }
        SendMessageA(hud, CB_SETCURSEL, hsel, 0);
    }

    HWND sc = combo("Render quality:", y, ID_SCALE); y += 30;
    SendMessageA(sc, CB_ADDSTRING, 0, (LPARAM)"Native  (fastest)");
    SendMessageA(sc, CB_ADDSTRING, 0, (LPARAM)"2x supersampling");
    SendMessageA(sc, CB_ADDSTRING, 0, (LPARAM)"3x supersampling  (sharpest, smoothest)");
    {
        int gw = cw > 0 ? cw : g_rw, gh = ch > 0 ? ch : g_rh;
        float sx = (float)g_rw / gw, sy = (float)g_rh / gh, k = sx < sy ? sx : sy; if (k < 1) k = 1;
        float q = cfg.render_scale / k;
        SendMessageA(sc, CB_SETCURSEL, q < 1.5f ? 0 : q < 2.5f ? 1 : 2, 0);
    }
    ctl("STATIC", "", 0, 150, y, 406, 32, ID_HINT); y += 38;

    check("Sharper HUD and menu art (pixel-art upscaling)", y, ID_UIUP, cfg.ui_upscale > 1); y += 24;
    HWND tx = check("Smooth text (redraws the game's fonts at full resolution)", y, ID_TEXT, cfg.text_hd); y += 24;
    if (!text_supported()) { EnableWindow(tx, FALSE); SendMessageA(tx, BM_SETCHECK, BST_UNCHECKED, 0); }
    check("Ambient occlusion (soft contact shadows)", y, ID_AO, cfg.ao); y += 24;
    check("Edge smoothing and sharpening (FXAA)", y, ID_AA, cfg.fxaa || cfg.sharpen > 0); y += 24;
    HWND uw = check("Ultrawide HUD: keep the HUD within a 16:9 area in the centre", y, ID_UWHUD, cfg.ultrawide_hud); y += 24;
    EnableWindow(uw, (float)g_rw / g_rh > 1.85f);
    check("Show this window every time the game starts", y, ID_ASK, 1); y += 32;
    ctl("BUTTON", "Start game", BS_DEFPUSHBUTTON | WS_TABSTOP, 320, y, 116, 30, ID_START);
    ctl("BUTTON", "Keep current", WS_TABSTOP, 440, y, 116, 30, ID_KEEP);
    SetWindowPos(g_dlg, NULL, 0, 0, W, y + 72, SWP_NOMOVE | SWP_NOZORDER);
    update_hint();

    ShowWindow(g_dlg, SW_SHOW); SetForegroundWindow(g_dlg); SetFocus(res);
    lg(1, "launcher: settings window shown (monitor %dx%d, game currently %dx%d, RenderScale %.2f, HUD hooks %s)",
       g_rw, g_rh, cw, ch, cfg.render_scale, g_hud_ok ? "available" : "not available for this build");
    MSG msg; HWND dlg = g_dlg;
    while (!g_launcher_done && GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    }
    if (g_launcher_result != ID_START) lg(1, "launcher: kept current settings");
    hud_install();
}

/* Save the choices (called on "Start game", while the controls still exist). */
static void launcher_capture_and_apply(HWND h)
{
    int gw, gh; float sc; char v[16];
    launcher_choice(h, &gw, &gh, &sc);
    float hs = g_hud_ok ? g_hscales[sel(h, ID_HSCALE)] : 1.0f;
    int ui = checked(h, ID_UIUP), ao = checked(h, ID_AO), aa = checked(h, ID_AA);
    int tx = IsWindowEnabled(GetDlgItem(h, ID_TEXT)) ? checked(h, ID_TEXT) : cfg.text_hd;  /* unavailable: keep the setting */
    int uw = checked(h, ID_UWHUD) && IsWindowEnabled(GetDlgItem(h, ID_UWHUD)), ask = checked(h, ID_ASK);
    write_game_res(gw, gh);
    add_mode(gw, gh);                                   /* so the game's own menu can show it too */
    cfg.render_scale = sc; snprintf(v, sizeof v, "%.4g", sc); ini_set("RenderScale", v);
    cfg.hud_scale = hs; snprintf(v, sizeof v, "%.4g", hs); ini_set("HudScale", v);
    cfg.ui_upscale = ui ? 4 : 1; ini_set("UiUpscale", ui ? "4" : "1");
    cfg.text_hd = tx; ini_set("TextHD", tx ? "1" : "0");
    cfg.ao = ao; ini_set("AO", ao ? "1" : "0");
    cfg.fxaa = aa; ini_set("FXAA", aa ? "1" : "0");
    if (aa && cfg.sharpen <= 0) cfg.sharpen = 0.4f;
    if (!aa) cfg.sharpen = 0;
    snprintf(v, sizeof v, "%.2f", cfg.sharpen); ini_set("Sharpen", v);
    cfg.ultrawide_hud = uw; ini_set("UltrawideHud", uw ? "1" : "0");
    apply_hud_layout(uw, gw, gh);
    cfg.ask_on_launch = ask; ini_set("AskOnLaunch", ask ? "1" : "0");
    lg(1, "launcher: game %dx%d, RenderScale %.2f, HudScale %.2f, UiUpscale %d, TextHD %d, AO %d, FXAA %d, ultrawide HUD %d, ask next time %d",
       gw, gh, sc, hs, cfg.ui_upscale, tx, ao, aa, uw, ask);
}

/* ---- ultrawide HUD layout
 * hudLayout.prf (version 2): u32 version, u32 count, then 29-byte records. At record offset 29
 * (i.e. file offsets 37 + 29k) a "positioned" byte is followed by the element's anchor as two
 * floats, x and y as fractions of the screen (0.5, 0.5 = centre). For wide screens we pull
 * every x towards the centre so the HUD spans a 16:9-wide area in the middle; y is unchanged.
 * The original layout is kept in hudLayout.prf.sswrap-orig; if the player rearranges the HUD
 * in game (the file then differs from what we last wrote), that becomes the new original. */
static int read_all(const char *path, unsigned char **buf)
{
    FILE *f = fopen(path, "rb"); long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    *buf = malloc(n > 0 ? n : 1);
    if (!*buf || fread(*buf, 1, n, f) != (size_t)n) { fclose(f); free(*buf); return -1; }
    fclose(f); return (int)n;
}
static void write_all(const char *path, const unsigned char *b, int n)
{
    FILE *f = fopen(path, "wb"); if (f) { fwrite(b, 1, n, f); fclose(f); }
}

static void apply_hud_layout(int ultrawide, int gw, int gh)
{
    char cur[MAX_PATH], orig[MAX_PATH], last[MAX_PATH];
    unsigned char *c = NULL, *o = NULL, *l = NULL; int nc, no, nl;
    game_file(cur, sizeof cur, "hudLayout.prf");
    snprintf(orig, sizeof orig, "%s.sswrap-orig", cur); snprintf(last, sizeof last, "%s.sswrap-last", cur);
    if ((nc = read_all(cur, &c)) < 8) { if (ultrawide) lg(0, "hud layout: %s not found, nothing to adjust", cur); free(c); return; }
    no = read_all(orig, &o); nl = read_all(last, &l);
    /* the base layout: the original, unless the player changed the HUD since we last wrote it */
    int ours = nl == nc && !memcmp(l, c, nc) && no > 0;
    unsigned char *base = ours ? o : c; int nb = ours ? no : nc;
    if (!ours) { write_all(orig, c, nc); lg(1, "hud layout: saved the current layout as the original"); }
    if (*(unsigned *)base != 2) { lg(0, "hud layout: unknown version %u, left alone", *(unsigned *)base); goto done; }

    unsigned char *out = malloc(nb); memcpy(out, base, nb);
    float aspect = gh > 0 ? (float)gw / gh : 16.0f / 9.0f, f = (16.0f / 9.0f) / aspect;
    int moved = 0;
    if (ultrawide && f < 0.999f) {
        for (int i = 37; i + 9 <= nb; i += 29) {
            float x, y;
            if (out[i] != 1) continue;
            memcpy(&x, out + i + 1, 4); memcpy(&y, out + i + 5, 4);
            if (!(x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f)) continue;
            x = 0.5f + (x - 0.5f) * f;
            memcpy(out + i + 1, &x, 4); moved++;
        }
    }
    write_all(cur, out, nb); write_all(last, out, nb);
    lg(1, "hud layout: %s (%d elements moved inward, factor %.3f for a %.2f:1 screen)",
       moved ? "ultrawide, kept within a centred 16:9 area" : "game's own layout", moved, f, aspect);
    free(out);
done:
    free(c); free(o); free(l);
}

/* ---- diagnostics: which script/prefs files the game opens, and when */
static HANDLE (WINAPI *r_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE WINAPI hk_CreateFileA(LPCSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t)
{
    if (n) {
        size_t l = strlen(n);
        if (l > 3 && !lstrcmpiA(n + l - 3, ".cs")) LG_FIRST(40, 1, "game opens %s (%s)", n, (a & GENERIC_WRITE) ? "write" : "read");
    }
    return r_CreateFileA(n, a, s, sa, c, f, t);
}
