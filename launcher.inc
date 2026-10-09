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

/* HUD size presets: the game lays its HUD out in pixels, so running it at native / k makes
 * the HUD k times bigger; RenderScale = k x quality keeps the 3D world at native or above. */
static const float g_huds[] = { 1.0f, 1.25f, 1.5f, 2.0f, 3.0f };
#define NHUDS 5
static const int g_quals[] = { 1, 2, 3 };
#define NQUALS 3

static void hud_res(float k, int *w, int *h) { *w = ((int)(g_rw / k + 0.5f)) & ~1; *h = ((int)(g_rh / k + 0.5f)) & ~1; }

/* the game resolution and RenderScale the current choices add up to */
static void launcher_choice(HWND d, int *gw, int *gh, float *scale, float *hud)
{
    int hi = (int)SendDlgItemMessageA(d, ID_HUD, CB_GETCURSEL, 0, 0);
    int qi = (int)SendDlgItemMessageA(d, ID_SCALE, CB_GETCURSEL, 0, 0);
    int ri = (int)SendDlgItemMessageA(d, ID_RES, CB_GETCURSEL, 0, 0);
    float q = (float)g_quals[qi < 0 ? 0 : qi];
    if (ri > 0) {                      /* advanced: exact resolution */
        *gw = g_res_items[ri - 1].w; *gh = g_res_items[ri - 1].h;
        float sx = (float)g_rw / *gw, sy = (float)g_rh / *gh;
        *hud = sx < sy ? sx : sy;
    } else {
        *hud = g_huds[hi < 0 ? 0 : hi];
        hud_res(*hud, gw, gh);
    }
    *scale = (*hud < 1.0f ? 1.0f : *hud) * q;
    if (*scale > 8.0f) *scale = 8.0f;
}

static void update_hint(void)
{
    int gw, gh; float sc, hud; char t[240];
    launcher_choice(g_dlg, &gw, &gh, &sc, &hud);
    EnableWindow(GetDlgItem(g_dlg, ID_HUD), SendDlgItemMessageA(g_dlg, ID_RES, CB_GETCURSEL, 0, 0) == 0);
    snprintf(t, sizeof t, "The game runs at %d x %d (HUD %.2gx). The 3D world renders at %d x %d and is shown on your %d x %d monitor.",
             gw, gh, hud, (int)(gw * sc + 0.5f), (int)(gh * sc + 0.5f), g_rw, g_rh);
    SetDlgItemTextA(g_dlg, ID_HINT, t);
}

static LRESULT CALLBACK launcher_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_COMMAND) {
        int id = LOWORD(w);
        if ((id == ID_RES || id == ID_SCALE || id == ID_HUD) && HIWORD(w) == CBN_SELCHANGE) update_hint();
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

static void show_launcher(void)
{
    static int shown;
    if (shown || !cfg.enabled || !cfg.ask_on_launch) return;
    shown = 1;

    int cw, ch; read_game_res(&cw, &ch);
    for (int i = 0; i < g_nmodes; i++) add_res(g_modes[i].w, g_modes[i].h, "%d x %d");
    if (cw > 0 && ch > 0) add_res(cw, ch, "%d x %d  (current)");

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = launcher_proc; wc.hInstance = g_self; wc.lpszClassName = "sswrapLauncher";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);
    int W = 580, H = 376;
    g_dlg = CreateWindowExA(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, "sswrapLauncher", "Starsiege - display settings (sswrap)",
                            WS_POPUP | WS_CAPTION | WS_SYSMENU, (r_GetSystemMetrics(SM_CXSCREEN) - W) / 2,
                            (r_GetSystemMetrics(SM_CYSCREEN) - H) / 2, W, H, NULL, NULL, g_self, NULL);
    if (!g_dlg) { lg(0, "launcher: could not create the settings window"); return; }

    char t[160];
    snprintf(t, sizeof t, "Detected monitor: %d x %d", g_rw, g_rh);
    ctl("STATIC", t, 0, 16, 14, 540, 18, 0);

    /* HUD size, preselected from the game's current resolution */
    ctl("STATIC", "HUD size:", 0, 16, 46, 130, 18, 0);
    HWND hud = ctl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP, 150, 42, 406, 200, ID_HUD);
    int hsel = 3; float best = 1e9f;
    for (int i = 0; i < NHUDS; i++) {
        int w, h; hud_res(g_huds[i], &w, &h);
        snprintf(t, sizeof t, "%.2gx%s   (game runs at %d x %d)", g_huds[i],
                 g_huds[i] == 1.0f ? " - smallest" : g_huds[i] == 2.0f ? " - recommended" : "", w, h);
        SendMessageA(hud, CB_ADDSTRING, 0, (LPARAM)t);
        if (cw > 0) { float d = (float)(w - cw) * (w - cw); if (d < best) { best = d; hsel = i; } }
    }
    SendMessageA(hud, CB_SETCURSEL, hsel, 0);

    /* render quality, preselected from the current RenderScale */
    ctl("STATIC", "Render quality:", 0, 16, 82, 130, 18, 0);
    HWND sc = ctl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP, 150, 78, 406, 200, ID_SCALE);
    SendMessageA(sc, CB_ADDSTRING, 0, (LPARAM)"Native  (fastest)");
    SendMessageA(sc, CB_ADDSTRING, 0, (LPARAM)"2x supersampling");
    SendMessageA(sc, CB_ADDSTRING, 0, (LPARAM)"3x supersampling  (sharpest, smoothest)");
    float q = cfg.render_scale / g_huds[hsel];
    SendMessageA(sc, CB_SETCURSEL, q < 1.5f ? 0 : q < 2.5f ? 1 : 2, 0);

    /* advanced: an exact resolution instead of a HUD size */
    ctl("STATIC", "Exact resolution:", 0, 16, 118, 130, 18, 0);
    HWND res = ctl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 150, 114, 406, 300, ID_RES);
    SendMessageA(res, CB_ADDSTRING, 0, (LPARAM)"Automatic (from HUD size)");
    for (int i = 0; i < g_nres_items; i++) SendMessageA(res, CB_ADDSTRING, 0, (LPARAM)g_res_items[i].label);
    SendMessageA(res, CB_SETCURSEL, 0, 0);

    ctl("STATIC", "", 0, 150, 144, 406, 34, ID_HINT);
    HWND ao = ctl("BUTTON", "Ambient occlusion (soft contact shadows)", BS_AUTOCHECKBOX | WS_TABSTOP, 150, 184, 406, 20, ID_AO);
    HWND aa = ctl("BUTTON", "Edge smoothing and sharpening (FXAA)", BS_AUTOCHECKBOX | WS_TABSTOP, 150, 208, 406, 20, ID_AA);
    HWND uw = ctl("BUTTON", "Ultrawide HUD: keep the HUD within a 16:9 area in the centre", BS_AUTOCHECKBOX | WS_TABSTOP, 150, 232, 406, 20, ID_UWHUD);
    HWND ask = ctl("BUTTON", "Show this window every time the game starts", BS_AUTOCHECKBOX | WS_TABSTOP, 150, 264, 406, 20, ID_ASK);
    SendMessageA(ao, BM_SETCHECK, cfg.ao ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(aa, BM_SETCHECK, (cfg.fxaa || cfg.sharpen > 0) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(ask, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageA(uw, BM_SETCHECK, cfg.ultrawide_hud ? BST_CHECKED : BST_UNCHECKED, 0);
    ctl("BUTTON", "Start game", BS_DEFPUSHBUTTON | WS_TABSTOP, 320, 300, 116, 30, ID_START);
    ctl("BUTTON", "Keep current", WS_TABSTOP, 440, 300, 116, 30, ID_KEEP);
    update_hint();

    ShowWindow(g_dlg, SW_SHOW); SetForegroundWindow(g_dlg); SetFocus(hud);
    lg(1, "launcher: settings window shown (monitor %dx%d, game currently %dx%d, RenderScale %.2f)", g_rw, g_rh, cw, ch, cfg.render_scale);
    MSG msg; HWND dlg = g_dlg;
    while (!g_launcher_done && GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    }
    if (g_launcher_result != ID_START) lg(1, "launcher: kept current settings");
}

/* Save the choices (called on "Start game", while the controls still exist). */
static void launcher_capture_and_apply(HWND h)
{
    int gw, gh; float sc, hud; char v[16];
    launcher_choice(h, &gw, &gh, &sc, &hud);
    int ao = SendDlgItemMessageA(h, ID_AO, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int aa = SendDlgItemMessageA(h, ID_AA, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int ask = SendDlgItemMessageA(h, ID_ASK, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int uw = SendDlgItemMessageA(h, ID_UWHUD, BM_GETCHECK, 0, 0) == BST_CHECKED;
    write_game_res(gw, gh);
    cfg.ultrawide_hud = uw; ini_set("UltrawideHud", uw ? "1" : "0");
    apply_hud_layout(uw, gw, gh);
    add_mode(gw, gh);                                   /* so the game's own menu can show it too */
    cfg.render_scale = sc; snprintf(v, sizeof v, "%.4g", sc); ini_set("RenderScale", v);
    cfg.ao = ao; ini_set("AO", ao ? "1" : "0");
    cfg.fxaa = aa; ini_set("FXAA", aa ? "1" : "0");
    if (aa && cfg.sharpen <= 0) cfg.sharpen = 0.4f;
    if (!aa) cfg.sharpen = 0;
    snprintf(v, sizeof v, "%.2f", cfg.sharpen); ini_set("Sharpen", v);
    cfg.ask_on_launch = ask; ini_set("AskOnLaunch", ask ? "1" : "0");
    lg(1, "launcher: game %dx%d (HUD %.2fx), RenderScale %.2f, AO %d, FXAA/sharpen %d, ask next time %d",
       gw, gh, hud, sc, ao, aa, ask);
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
