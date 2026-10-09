/*
 * sswrap: opengl32.dll proxy for Starsiege (Dynamix, 1999) under Wine.
 *
 * What it does:
 *  1. Pass-through: every opengl32 export forwards to the real opengl32 (thunks.S).
 *  2. Virtual display: the game's ChangeDisplaySettings() call is recorded instead of
 *     performed, so the real monitor never changes mode. The game window is made to
 *     cover the whole monitor, while the game is told (window rect, cursor position,
 *     mouse messages, screen metrics) that it is running at the mode it asked for.
 *  3. Off-screen rendering: the game draws into an FBO of its requested size (times
 *     RenderScale); every SwapBuffers blits that FBO to the window, scaled to fit with
 *     the aspect ratio kept (black bars), then swaps.
 *  4. Debugging: everything interesting goes to sswrap.log next to the game. A watchdog
 *     thread notices when frames stop, briefly suspends the render thread, copies its
 *     registers and stack, and logs where it is stuck (module!export+offset).
 *
 * Settings: sswrap.ini next to the game ([sswrap] section, see sswrap.ini).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "gl_names.h"

/* GL 3.0 framebuffer bits (not in the 1.1 headers) */
#define GL_FRAMEBUFFER            0x8D40
#define GL_READ_FRAMEBUFFER       0x8CA8
#define GL_DRAW_FRAMEBUFFER       0x8CA9
#define GL_RENDERBUFFER           0x8D41
#define GL_COLOR_ATTACHMENT0      0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH24_STENCIL8       0x88F0
#define GL_RGBA8_                 0x8058
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5

/* ------------------------------------------------------------------ state */
void *g_real[GL_EXPORT_COUNT];          /* used by thunks.S */
static HMODULE g_self, g_realgl;

static struct {
    int enabled, render_scale, log_level, stall_ms, filter_linear, stats_sec;
    char block_hosts[512];
    int ao, fxaa;
    float ao_radius, ao_strength, ao_max_dist, sharpen;
    char extra_modes[512];
} cfg = { 1, 1, 1, 200, 1, 10, "dynamix.com", 1, 1, 3.0f, 1.5f, 400.0f, 0.4f, "" };

static int g_vactive, g_vw, g_vh;       /* virtual (game-requested) display mode */
static int g_rw, g_rh;                  /* real primary monitor size */
static HWND g_hwnd;                     /* the game's GL window */
static HDC g_hdc;
static WNDPROC g_game_proc;             /* game's window proc (we subclass in front) */

static GLuint g_fbo;
static int g_fbw, g_fbh, g_fbo_ready, g_fbo_dirty;
static int g_scale = 1;                 /* active render scale (1 unless FBO ready) */

static LARGE_INTEGER g_qpf, g_t0;
static volatile LONG g_frames;
static volatile LONGLONG g_last_frame_qpc;
static volatile int g_in_swap;
static volatile const char *g_last_hook = "(none)";
static HANDLE g_main_thread;
static DWORD g_main_tid;

/* frame stats */
static double st_max_frame, st_max_swap, st_sum_frame;
static LONG st_frames;
static LONGLONG st_start;

/* ------------------------------------------------------------------ logging */
static CRITICAL_SECTION g_log_cs;
static HANDLE g_log = INVALID_HANDLE_VALUE;

static double now_ms(void)
{
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
}

static void lg(int level, const char *fmt, ...)
{
    char buf[1024]; int n; va_list ap; DWORD w;
    if (level > cfg.log_level || g_log == INVALID_HANDLE_VALUE) return;
    n = snprintf(buf, sizeof buf, "%10.1f [%04lx] ", now_ms(), GetCurrentThreadId());
    va_start(ap, fmt); n += vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap); va_end(ap);
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\r'; buf[n++] = '\n';
    EnterCriticalSection(&g_log_cs);
    WriteFile(g_log, buf, n, &w, NULL);
    LeaveCriticalSection(&g_log_cs);
}

/* log only the first `limit` times a given call site fires */
#define LG_FIRST(limit, level, ...) do { static int _c; if (_c < (limit)) { _c++; lg(level, __VA_ARGS__); } } while (0)

/* ------------------------------------------------------------------ real GL */
#define REAL(name) ((PFN_##name)g_real[IDX_##name])
typedef const GLubyte *(WINAPI *PFN_glGetString)(GLenum);
typedef GLenum (WINAPI *PFN_glGetError)(void);
typedef void (WINAPI *PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void (WINAPI *PFN_glScissor)(GLint, GLint, GLsizei, GLsizei);
typedef void (WINAPI *PFN_glDrawBuffer)(GLenum);
typedef void (WINAPI *PFN_glReadBuffer)(GLenum);
typedef void (WINAPI *PFN_glGetIntegerv)(GLenum, GLint *);
typedef void (WINAPI *PFN_glGetFloatv)(GLenum, GLfloat *);
typedef void (WINAPI *PFN_glLineWidth)(GLfloat);
typedef void (WINAPI *PFN_glPointSize)(GLfloat);
typedef void (WINAPI *PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *);
typedef void (WINAPI *PFN_glCopyPixels)(GLint, GLint, GLsizei, GLsizei, GLenum);
typedef void (WINAPI *PFN_glDrawPixels)(GLsizei, GLsizei, GLenum, GLenum, const GLvoid *);
typedef void (WINAPI *PFN_glBitmap)(GLsizei, GLsizei, GLfloat, GLfloat, GLfloat, GLfloat, const GLubyte *);
typedef void (WINAPI *PFN_glCopyTexImage2D)(GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint);
typedef void (WINAPI *PFN_glCopyTexSubImage2D)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
typedef void (WINAPI *PFN_glPixelZoom)(GLfloat, GLfloat);
typedef void (WINAPI *PFN_glDisable)(GLenum);
typedef void (WINAPI *PFN_glClearColor)(GLclampf, GLclampf, GLclampf, GLclampf);
typedef void (WINAPI *PFN_glClear)(GLbitfield);
typedef void (WINAPI *PFN_glColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void (WINAPI *PFN_glPushAttrib)(GLbitfield);
typedef void (WINAPI *PFN_glPopAttrib)(void);
typedef BOOL (WINAPI *PFN_wglSwapBuffers)(HDC);
typedef BOOL (WINAPI *PFN_wglMakeCurrent)(HDC, HGLRC);
typedef HGLRC (WINAPI *PFN_wglCreateContext)(HDC);
typedef BOOL (WINAPI *PFN_wglDeleteContext)(HGLRC);
typedef PROC (WINAPI *PFN_wglGetProcAddress)(LPCSTR);

/* GL 3.0 entry points, fetched from the driver after a context is current */
static void (WINAPI *pGenFramebuffers)(GLsizei, GLuint *);
static void (WINAPI *pDeleteFramebuffers)(GLsizei, const GLuint *);
static void (WINAPI *pBindFramebuffer)(GLenum, GLuint);
static void (WINAPI *pGenRenderbuffers)(GLsizei, GLuint *);
static void (WINAPI *pDeleteRenderbuffers)(GLsizei, const GLuint *);
static void (WINAPI *pBindRenderbuffer)(GLenum, GLuint);
static void (WINAPI *pRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
static void (WINAPI *pFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
static GLenum (WINAPI *pCheckFramebufferStatus)(GLenum);
static void (WINAPI *pBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

/* ------------------------------------------------------------------ real user32/gdi32/kernel32 */
static LONG (WINAPI *r_ChangeDisplaySettingsA)(DEVMODEA *, DWORD);
static BOOL (WINAPI *r_EnumDisplaySettingsA)(LPCSTR, DWORD, DEVMODEA *);
static int (WINAPI *r_GetSystemMetrics)(int);
static HWND (WINAPI *r_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static BOOL (WINAPI *r_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
static BOOL (WINAPI *r_MoveWindow)(HWND, int, int, int, int, BOOL);
static BOOL (WINAPI *r_GetClientRect)(HWND, LPRECT);
static BOOL (WINAPI *r_GetWindowRect)(HWND, LPRECT);
static BOOL (WINAPI *r_ClientToScreen)(HWND, LPPOINT);
static BOOL (WINAPI *r_ScreenToClient)(HWND, LPPOINT);
static BOOL (WINAPI *r_GetCursorPos)(LPPOINT);
static BOOL (WINAPI *r_SetCursorPos)(int, int);
static LONG (WINAPI *r_SetWindowLongA)(HWND, int, LONG);
static LONG (WINAPI *r_GetWindowLongA)(HWND, int);
static int (WINAPI *r_ShowCursor)(BOOL);
static HWND (WINAPI *r_WindowFromPoint)(POINT);
static BOOL (WINAPI *r_SwapBuffers)(HDC);
static FARPROC (WINAPI *r_GetProcAddress)(HMODULE, LPCSTR);
static HMODULE (WINAPI *r_LoadLibraryA)(LPCSTR);

/* ------------------------------------------------------------------ coordinate mapping */
static int mapping_on(void) { return cfg.enabled && g_vactive && g_hwnd && g_vw > 0 && g_vh > 0; }

/* where the virtual screen lands inside the real client area */
static void letterbox(double *s, double *ox, double *oy, int *cw, int *ch)
{
    RECT rc; int w = g_rw, h = g_rh;
    if (g_hwnd && r_GetClientRect(g_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) { w = rc.right; h = rc.bottom; }
    double sx = (double)w / g_vw, sy = (double)h / g_vh;
    *s = sx < sy ? sx : sy;
    *ox = (w - g_vw * *s) / 2.0; *oy = (h - g_vh * *s) / 2.0;
    if (cw) *cw = w;
    if (ch) *ch = h;
}

static void real_client_to_virtual(LONG *x, LONG *y)
{
    double s, ox, oy; letterbox(&s, &ox, &oy, NULL, NULL);
    double vx = (*x - ox) / s, vy = (*y - oy) / s;
    if (vx < 0) vx = 0;
    if (vy < 0) vy = 0;
    if (vx > g_vw - 1) vx = g_vw - 1;
    if (vy > g_vh - 1) vy = g_vh - 1;
    *x = (LONG)vx; *y = (LONG)vy;
}

static void virtual_to_real_client(LONG *x, LONG *y)
{
    double s, ox, oy; letterbox(&s, &ox, &oy, NULL, NULL);
    *x = (LONG)(ox + (*x + 0.5) * s); *y = (LONG)(oy + (*y + 0.5) * s);
}

/* make the game window cover the whole primary monitor */
static void force_window_geometry(const char *why)
{
    if (!mapping_on()) return;
    lg(1, "window: forcing %p to 0,0 %dx%d (%s)", (void *)g_hwnd, g_rw, g_rh, why);
    r_SetWindowPos(g_hwnd, HWND_TOP, 0, 0, g_rw, g_rh, SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

/* ------------------------------------------------------------------ window subclass */
static LRESULT CALLBACK sub_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (mapping_on() && h == g_hwnd) {
        if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL) {
            LONG x = (short)LOWORD(l), y = (short)HIWORD(l);
            real_client_to_virtual(&x, &y);
            l = MAKELPARAM(x, y);
        } else if (m == WM_SIZE) {
            lg(2, "WM_SIZE real %dx%d -> reported %dx%d", LOWORD(l), HIWORD(l), g_vw, g_vh);
            l = MAKELPARAM(g_vw, g_vh);
        } else if (m == WM_ACTIVATEAPP) {
            lg(1, "WM_ACTIVATEAPP %d", (int)w);
        }
    }
    return CallWindowProcA(g_game_proc, h, m, w, l);
}

static void subclass_window(void)
{
    if (g_game_proc || !g_hwnd) return;
    g_game_proc = (WNDPROC)r_SetWindowLongA(g_hwnd, GWL_WNDPROC, (LONG)sub_proc);
    lg(1, "window: subclassed %p (game proc %p)", (void *)g_hwnd, (void *)g_game_proc);
}

/* ------------------------------------------------------------------ user32 hooks */
static LONG WINAPI hk_ChangeDisplaySettingsA(DEVMODEA *dm, DWORD flags)
{
    g_last_hook = "ChangeDisplaySettingsA";
    if (!cfg.enabled) return r_ChangeDisplaySettingsA(dm, flags);
    if (!dm) {
        lg(1, "ChangeDisplaySettingsA(NULL, 0x%lx): restore requested -> virtual mode off", flags);
        g_vactive = 0;
        return DISP_CHANGE_SUCCESSFUL;
    }
    int w = (dm->dmFields & DM_PELSWIDTH) ? (int)dm->dmPelsWidth : g_vw;
    int h = (dm->dmFields & DM_PELSHEIGHT) ? (int)dm->dmPelsHeight : g_vh;
    lg(1, "ChangeDisplaySettingsA(%dx%d %lubpp @%luHz, flags 0x%lx) -> virtual, real monitor stays %dx%d",
       w, h, (dm->dmFields & DM_BITSPERPEL) ? dm->dmBitsPerPel : 0,
       (dm->dmFields & DM_DISPLAYFREQUENCY) ? dm->dmDisplayFrequency : 0, flags, g_rw, g_rh);
    if (flags & CDS_TEST) return DISP_CHANGE_SUCCESSFUL;
    if (w != g_vw || h != g_vh) g_fbo_dirty = 1;
    g_vw = w; g_vh = h; g_vactive = 1;
    force_window_geometry("mode change");
    return DISP_CHANGE_SUCCESSFUL;
}

/* The game builds its resolution menu from EnumDisplaySettings. Since display changes are
 * virtual, any size works, so we offer a fixed list of popular modes (plus the monitor's
 * native size and anything in ExtraModes) instead of whatever the host happens to report.
 * Each size is listed at 16 and 32 bpp, because games of this era often filter on depth. */
static struct { int w, h; } g_modes[64];
static int g_nmodes;

static void add_mode(int w, int h)
{
    if (w < 320 || h < 200 || g_nmodes >= 64) return;
    for (int i = 0; i < g_nmodes; i++) if (g_modes[i].w == w && g_modes[i].h == h) return;
    int i = g_nmodes++;
    while (i > 0 && (long)g_modes[i - 1].w * g_modes[i - 1].h > (long)w * h) { g_modes[i] = g_modes[i - 1]; i--; }
    g_modes[i].w = w; g_modes[i].h = h;
}

static void build_mode_list(void)
{
    static const int std[][2] = {
        { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 960 }, { 1280, 1024 },
        { 1400, 1050 }, { 1600, 1200 }, { 1920, 1440 },                        /* 4:3 and 5:4 */
        { 1280, 720 }, { 1366, 768 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 }, /* 16:9 */
        { 1280, 800 }, { 1440, 900 }, { 1680, 1050 }, { 1920, 1200 }, { 2560, 1600 },             /* 16:10 */
        { 2560, 1080 }, { 3440, 1440 }, { 3840, 1600 }, { 5120, 1440 },                          /* ultrawide */
    };
    char buf[512], *tok, *ctx = NULL;
    for (unsigned i = 0; i < sizeof std / sizeof std[0]; i++) add_mode(std[i][0], std[i][1]);
    add_mode(g_rw, g_rh);
    lstrcpynA(buf, cfg.extra_modes, sizeof buf);
    for (tok = strtok_r(buf, ", ", &ctx); tok; tok = strtok_r(NULL, ", ", &ctx)) {
        int w, h; if (sscanf(tok, "%dx%d", &w, &h) == 2) add_mode(w, h);
    }
    char list[1024]; int n = 0;
    for (int i = 0; i < g_nmodes; i++) n += snprintf(list + n, sizeof list - n, "%s%dx%d", i ? " " : "", g_modes[i].w, g_modes[i].h);
    lg(1, "offering the game %d resolutions: %s", g_nmodes, list);
}

static BOOL WINAPI hk_EnumDisplaySettingsA(LPCSTR dev, DWORD mode, DEVMODEA *dm)
{
    if (!cfg.enabled) return r_EnumDisplaySettingsA(dev, mode, dm);
    if (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS) {
        BOOL r = r_EnumDisplaySettingsA(dev, mode, dm);
        if (r && g_vactive) { dm->dmPelsWidth = g_vw; dm->dmPelsHeight = g_vh; }
        return r;
    }
    if (mode >= (DWORD)g_nmodes * 2) return FALSE;
    DEVMODEA base; memset(&base, 0, sizeof base); base.dmSize = sizeof base;
    r_EnumDisplaySettingsA(dev, ENUM_CURRENT_SETTINGS, &base);
    WORD size = dm->dmSize ? dm->dmSize : sizeof *dm;
    memcpy(dm, &base, size < sizeof base ? size : sizeof base);
    dm->dmSize = size;
    dm->dmPelsWidth = g_modes[mode / 2].w; dm->dmPelsHeight = g_modes[mode / 2].h;
    dm->dmBitsPerPel = (mode & 1) ? 32 : 16; dm->dmDisplayFrequency = 60;
    dm->dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    return TRUE;
}

static int WINAPI hk_GetSystemMetrics(int i)
{
    if (mapping_on() && i == SM_CXSCREEN) return g_vw;
    if (mapping_on() && i == SM_CYSCREEN) return g_vh;
    return r_GetSystemMetrics(i);
}

static HWND WINAPI hk_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                                      HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND r = r_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
    lg(1, "CreateWindowExA(class=%s title=%s style=0x%lx ex=0x%lx %d,%d %dx%d) -> %p",
       HIWORD(cls) ? cls : "#atom", title ? title : "", style, ex, x, y, w, h, (void *)r);
    return r;
}

static BOOL WINAPI hk_SetWindowPos(HWND hw, HWND after, int x, int y, int cx, int cy, UINT f)
{
    g_last_hook = "SetWindowPos";
    if (mapping_on() && hw == g_hwnd && !((f & SWP_NOMOVE) && (f & SWP_NOSIZE))) {
        lg(2, "SetWindowPos(game, %d,%d %dx%d, 0x%x) -> full monitor", x, y, cx, cy, f);
        return r_SetWindowPos(hw, after, 0, 0, g_rw, g_rh, f & ~(SWP_NOMOVE | SWP_NOSIZE));
    }
    return r_SetWindowPos(hw, after, x, y, cx, cy, f);
}

static BOOL WINAPI hk_MoveWindow(HWND hw, int x, int y, int w, int h, BOOL rp)
{
    if (mapping_on() && hw == g_hwnd) {
        lg(2, "MoveWindow(game, %d,%d %dx%d) -> full monitor", x, y, w, h);
        return r_MoveWindow(hw, 0, 0, g_rw, g_rh, rp);
    }
    return r_MoveWindow(hw, x, y, w, h, rp);
}

static BOOL WINAPI hk_GetClientRect(HWND hw, LPRECT rc)
{
    if (mapping_on() && hw == g_hwnd) { SetRect(rc, 0, 0, g_vw, g_vh); return TRUE; }
    return r_GetClientRect(hw, rc);
}

static BOOL WINAPI hk_GetWindowRect(HWND hw, LPRECT rc)
{
    if (mapping_on() && hw == g_hwnd) { SetRect(rc, 0, 0, g_vw, g_vh); return TRUE; }
    return r_GetWindowRect(hw, rc);
}

/* in the virtual world the game window sits at 0,0, so client == screen */
static BOOL WINAPI hk_ClientToScreen(HWND hw, LPPOINT p) { return (mapping_on() && hw == g_hwnd) ? TRUE : r_ClientToScreen(hw, p); }
static BOOL WINAPI hk_ScreenToClient(HWND hw, LPPOINT p) { return (mapping_on() && hw == g_hwnd) ? TRUE : r_ScreenToClient(hw, p); }

static BOOL WINAPI hk_GetCursorPos(LPPOINT p)
{
    BOOL r = r_GetCursorPos(p);
    if (r && mapping_on()) {
        r_ScreenToClient(g_hwnd, p);
        real_client_to_virtual(&p->x, &p->y);
    }
    return r;
}

static BOOL WINAPI hk_SetCursorPos(int x, int y)
{
    if (mapping_on()) {
        POINT p = { x, y };
        virtual_to_real_client(&p.x, &p.y);
        r_ClientToScreen(g_hwnd, &p);
        LG_FIRST(5, 2, "SetCursorPos(%d,%d) -> real %ld,%ld", x, y, p.x, p.y);
        return r_SetCursorPos(p.x, p.y);
    }
    return r_SetCursorPos(x, y);
}

static HWND WINAPI hk_WindowFromPoint(POINT p)
{
    if (mapping_on()) { virtual_to_real_client(&p.x, &p.y); r_ClientToScreen(g_hwnd, &p); }
    return r_WindowFromPoint(p);
}

static LONG WINAPI hk_SetWindowLongA(HWND hw, int idx, LONG v)
{
    if (hw == g_hwnd && idx == GWL_WNDPROC && g_game_proc) {
        LONG old = (LONG)g_game_proc;
        g_game_proc = (WNDPROC)v;
        lg(1, "game replaced its window proc %p -> %p (kept our subclass in front)", (void *)old, (void *)v);
        return old;
    }
    if (hw == g_hwnd && idx == GWL_STYLE) lg(1, "SetWindowLongA(game, GWL_STYLE, 0x%lx)", (DWORD)v);
    return r_SetWindowLongA(hw, idx, v);
}

static LONG WINAPI hk_GetWindowLongA(HWND hw, int idx)
{
    if (hw == g_hwnd && idx == GWL_WNDPROC && g_game_proc) return (LONG)g_game_proc;
    return r_GetWindowLongA(hw, idx);
}

static int WINAPI hk_ShowCursor(BOOL show)
{
    int r = r_ShowCursor(show);
    LG_FIRST(20, 1, "ShowCursor(%d) -> count %d", show, r);
    return r;
}

/* ------------------------------------------------------------------ kernel32 hooks (diagnostics) */
static FARPROC WINAPI hk_GetProcAddress(HMODULE m, LPCSTR name)
{
    FARPROC r = r_GetProcAddress(m, name);
    if (m == g_self && HIWORD(name)) lg(3, "game resolved %s", name);
    return r;
}

static HMODULE WINAPI hk_LoadLibraryA(LPCSTR name)
{
    HMODULE r = r_LoadLibraryA(name);
    lg(1, "LoadLibraryA(%s) -> %p%s", name ? name : "(null)", (void *)r, r == g_self ? " (= sswrap)" : "");
    return r;
}

/* ------------------------------------------------------------------ wsock32 hook: DNS lookups
 * The game resolves long-dead Dynamix servers (masters, IRC) on its render thread; each
 * failed lookup blocks ~2 s, which showed up as multi-second hitches mid-mission. Lookups of
 * hosts matching BlockHosts fail instantly; any other failed name is cached as failed. */
#define WSAHOST_NOT_FOUND_ 11001
static void *(WINAPI *r_gethostbyname)(const char *);
static void (WINAPI *r_WSASetLastError)(int);
static char g_dead[32][128]; static int g_ndead;

static int host_blocked(const char *h)
{
    char list[512], *tok, *ctx = NULL; size_t hl = strlen(h);
    lstrcpynA(list, cfg.block_hosts, sizeof list);
    for (tok = strtok_r(list, ", ", &ctx); tok; tok = strtok_r(NULL, ", ", &ctx)) {
        size_t tl = strlen(tok);
        if (hl >= tl && !lstrcmpiA(h + hl - tl, tok) && (hl == tl || h[hl - tl - 1] == '.')) return 1;
    }
    for (int i = 0; i < g_ndead; i++) if (!lstrcmpiA(g_dead[i], h)) return 2;
    return 0;
}

static void *WINAPI hk_gethostbyname(const char *name)
{
    int why;
    g_last_hook = "gethostbyname";
    if (name && (why = host_blocked(name))) {
        LG_FIRST(40, 1, "gethostbyname(%s): failed instantly (%s)", name, why == 1 ? "BlockHosts" : "failed before");
        if (r_WSASetLastError) r_WSASetLastError(WSAHOST_NOT_FOUND_);
        return NULL;
    }
    double t = now_ms();
    void *r = r_gethostbyname(name);
    double d = now_ms() - t;
    lg(1, "gethostbyname(%s) -> %s in %.0f ms%s", name ? name : "(null)", r ? "found" : "FAILED", d,
       GetCurrentThreadId() == g_main_tid ? " (on the render thread!)" : "");
    if (!r && name && g_ndead < 32) lstrcpynA(g_dead[g_ndead++], name, sizeof g_dead[0]);
    return r;
}

#include "fbo_present.inc"

/* ------------------------------------------------------------------ exported GL hooks */
BOOL WINAPI hk_wglSwapBuffers(HDC dc) { return present(dc); }
static BOOL WINAPI hk_SwapBuffers(HDC dc) { return present(dc); }   /* gdi32 import of the exe */

static void start_watchdog(void);

BOOL WINAPI hk_wglMakeCurrent(HDC dc, HGLRC rc)
{
    BOOL r = REAL(wglMakeCurrent)(dc, rc);
    lg(1, "wglMakeCurrent(%p, %p) -> %d", (void *)dc, (void *)rc, r);
    if (!r || !rc) return r;
    if (!g_main_thread) {
        const char *ven = (const char *)REAL(glGetString)(GL_VENDOR);
        const char *ren = (const char *)REAL(glGetString)(GL_RENDERER);
        const char *ver = (const char *)REAL(glGetString)(GL_VERSION);
        lg(1, "GL: %s | %s | %s", ven ? ven : "?", ren ? ren : "?", ver ? ver : "?");
        g_main_tid = GetCurrentThreadId();
        g_main_thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_main_tid);
        start_watchdog();
    }
    g_hdc = dc;
    g_hwnd = WindowFromDC(dc);
    if (cfg.enabled) {
        subclass_window();
        force_window_geometry("context made current");
        if (pBindFramebuffer || load_gl3()) { destroy_fbo(); build_fbo(); }
    }
    return r;
}

HGLRC WINAPI hk_wglCreateContext(HDC dc)
{
    HGLRC r = REAL(wglCreateContext)(dc);
    lg(1, "wglCreateContext(%p) -> %p", (void *)dc, (void *)r);
    return r;
}

BOOL WINAPI hk_wglDeleteContext(HGLRC rc)
{
    lg(1, "wglDeleteContext(%p)", (void *)rc);
    if (g_fbo && pBindFramebuffer) destroy_fbo();
    return REAL(wglDeleteContext)(rc);
}

PROC WINAPI hk_wglGetProcAddress(LPCSTR name)
{
    PROC r = REAL(wglGetProcAddress)(name);
    lg(2, "wglGetProcAddress(%s) -> %p", name, (void *)r);
    return r;
}

void WINAPI hk_glDrawBuffer(GLenum m)
{
    if (g_fbo_ready && is_window_buffer(m)) { LG_FIRST(3, 2, "glDrawBuffer(0x%x) -> fbo", m); m = GL_COLOR_ATTACHMENT0; }
    REAL(glDrawBuffer)(m);
}

void WINAPI hk_glReadBuffer(GLenum m)
{
    if (g_fbo_ready && is_window_buffer(m)) m = GL_COLOR_ATTACHMENT0;
    REAL(glReadBuffer)(m);
}

void WINAPI hk_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    LG_FIRST(10, 2, "glViewport(%d,%d %dx%d) scale %d", x, y, w, h, g_scale);
    REAL(glViewport)(x * g_scale, y * g_scale, w * g_scale, h * g_scale);
}

void WINAPI hk_glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    REAL(glScissor)(x * g_scale, y * g_scale, w * g_scale, h * g_scale);
}

void WINAPI hk_glGetIntegerv(GLenum p, GLint *v)
{
    REAL(glGetIntegerv)(p, v);
    if ((p == GL_VIEWPORT || p == GL_SCISSOR_BOX) && g_scale > 1) { v[0] /= g_scale; v[1] /= g_scale; v[2] /= g_scale; v[3] /= g_scale; }
    if ((p == GL_DRAW_BUFFER || p == GL_READ_BUFFER) && g_fbo_ready && v[0] == GL_COLOR_ATTACHMENT0) v[0] = GL_BACK;
}

void WINAPI hk_glLineWidth(GLfloat w) { REAL(glLineWidth)(w * g_scale); }
void WINAPI hk_glPointSize(GLfloat s) { REAL(glPointSize)(s * g_scale); }

/* pixel-exact operations: logged so we learn whether the game uses them, scaled where possible */
void WINAPI hk_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, GLvoid *d)
{
    LG_FIRST(5, 1, "glReadPixels(%d,%d %dx%d)%s", x, y, w, h, g_scale > 1 ? " WARNING: unscaled at RenderScale>1" : "");
    REAL(glReadPixels)(x, y, w, h, f, t, d);
}
void WINAPI hk_glCopyPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum t)
{
    LG_FIRST(5, 1, "glCopyPixels(%d,%d %dx%d)%s", x, y, w, h, g_scale > 1 ? " WARNING: unscaled at RenderScale>1" : "");
    REAL(glCopyPixels)(x, y, w, h, t);
}
void WINAPI hk_glDrawPixels(GLsizei w, GLsizei h, GLenum f, GLenum t, const GLvoid *d)
{
    LG_FIRST(5, 1, "glDrawPixels(%dx%d)", w, h);
    if (g_scale > 1) {
        GLfloat zx, zy; REAL(glGetFloatv)(GL_ZOOM_X, &zx); REAL(glGetFloatv)(GL_ZOOM_Y, &zy);
        REAL(glPixelZoom)(zx * g_scale, zy * g_scale); REAL(glDrawPixels)(w, h, f, t, d); REAL(glPixelZoom)(zx, zy);
    } else REAL(glDrawPixels)(w, h, f, t, d);
}
void WINAPI hk_glBitmap(GLsizei w, GLsizei h, GLfloat xo, GLfloat yo, GLfloat xm, GLfloat ym, const GLubyte *b)
{
    LG_FIRST(5, 1, "glBitmap(%dx%d)%s", w, h, g_scale > 1 ? " WARNING: drawn small at RenderScale>1" : "");
    REAL(glBitmap)(w, h, xo, yo, xm * g_scale, ym * g_scale, b);
}
void WINAPI hk_glCopyTexImage2D(GLenum tg, GLint lv, GLenum fmt, GLint x, GLint y, GLsizei w, GLsizei h, GLint bd)
{
    LG_FIRST(5, 1, "glCopyTexImage2D(%d,%d %dx%d)%s", x, y, w, h, g_scale > 1 ? " WARNING: unscaled at RenderScale>1" : "");
    REAL(glCopyTexImage2D)(tg, lv, fmt, x, y, w, h, bd);
}
void WINAPI hk_glCopyTexSubImage2D(GLenum tg, GLint lv, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{
    LG_FIRST(5, 1, "glCopyTexSubImage2D(%d,%d %dx%d)%s", x, y, w, h, g_scale > 1 ? " WARNING: unscaled at RenderScale>1" : "");
    REAL(glCopyTexSubImage2D)(tg, lv, xo, yo, x, y, w, h);
}

/* ------------------------------------------------------------------ watchdog */
static int addr_info(DWORD a, char *out, int outn, int need_code)
{
    HMODULE m; char path[MAX_PATH]; const char *base;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m))
        return 0;
    BYTE *img = (BYTE *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(img + ((IMAGE_DOS_HEADER *)img)->e_lfanew);
    DWORD rva = a - (DWORD)img;
    if (need_code) {
        IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt); int ok = 0;
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
            if (rva >= sec[i].VirtualAddress && rva < sec[i].VirtualAddress + sec[i].Misc.VirtualSize &&
                (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) ok = 1;
        if (!ok) return 0;
    }
    GetModuleFileNameA(m, path, sizeof path);
    base = strrchr(path, '\\'); base = base ? base + 1 : path;
    /* nearest export at or below the address */
    const char *best = NULL; DWORD best_rva = 0;
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dd->VirtualAddress && dd->Size) {
        IMAGE_EXPORT_DIRECTORY *ed = (IMAGE_EXPORT_DIRECTORY *)(img + dd->VirtualAddress);
        DWORD *fn = (DWORD *)(img + ed->AddressOfFunctions), *nm = (DWORD *)(img + ed->AddressOfNames);
        WORD *ord = (WORD *)(img + ed->AddressOfNameOrdinals);
        for (DWORD i = 0; i < ed->NumberOfNames; i++) {
            DWORD f = fn[ord[i]];
            if (f <= rva && f >= best_rva && !(f >= dd->VirtualAddress && f < dd->VirtualAddress + dd->Size)) {
                best_rva = f; best = (const char *)(img + nm[i]);
            }
        }
    }
    if (best) snprintf(out, outn, "%s!%s+0x%lx", base, best, rva - best_rva);
    else snprintf(out, outn, "%s+0x%lx", base, rva);
    return 1;
}

static void sample_main_thread(const char *tag)
{
    static DWORD stack[4096];
    CONTEXT c; DWORD eip = 0, esp = 0, ebp = 0; int n = 0;
    if (!g_main_thread) return;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    /* While the thread is suspended do nothing that could take a lock it might hold:
       only read its context and copy raw stack memory. */
    if (SuspendThread(g_main_thread) == (DWORD)-1) return;
    if (GetThreadContext(g_main_thread, &c)) {
        MEMORY_BASIC_INFORMATION mbi;
        eip = c.Eip; esp = c.Esp; ebp = c.Ebp;
        if (VirtualQuery((void *)esp, &mbi, sizeof mbi)) {
            DWORD end = (DWORD)mbi.BaseAddress + mbi.RegionSize;
            n = (end - esp) / 4; if (n > 4096) n = 4096;
            memcpy(stack, (void *)esp, n * 4);
        }
    }
    ResumeThread(g_main_thread);

    char sym[256], line[1024]; int len;
    if (!addr_info(eip, sym, sizeof sym, 0)) snprintf(sym, sizeof sym, "0x%08lx (outside any DLL: Wine unix side / kernel)", eip);
    lg(0, "%s: main thread at %s (esp %08lx ebp %08lx)", tag, sym, esp, ebp);
    len = snprintf(line, sizeof line, "%s:   return addresses on stack:", tag);
    int shown = 0; DWORD last = 0;
    for (int i = 0; i < n && shown < 14; i++) {
        if (stack[i] == last) continue;
        if (addr_info(stack[i], sym, sizeof sym, 1)) {
            last = stack[i];
            len += snprintf(line + len, sizeof line - len, " <%s>", sym);
            if (len > (int)sizeof line - 80) break;
            shown++;
        }
    }
    lg(0, "%s", line);
}

static DWORD WINAPI watchdog(LPVOID p)
{
    (void)p;
    int in_stall = 0, samples = 0; double next_sample = 0, stall_begin = 0; LONG frames_at_stall = 0;
    for (;;) {
        Sleep(20);
        if (!g_last_frame_qpc) continue;
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        double since = (double)(t.QuadPart - g_last_frame_qpc) * 1000.0 / g_qpf.QuadPart;
        if (!in_stall && since > cfg.stall_ms) {
            in_stall = 1; samples = 0; frames_at_stall = g_frames; stall_begin = now_ms() - since;
            HWND fg = GetForegroundWindow();
            if (fg != g_hwnd || !g_vactive) {
                /* not a hang: the game idles when it is in the background or out of fullscreen */
                lg(1, "paused: no frames while the game is %s", fg != g_hwnd ? "in the background" : "out of fullscreen");
                samples = 99;   /* no sampling for this gap */
            } else {
                lg(0, "STALL: no frame for %.0f ms (frame #%ld, inside swap=%d, last hook=%s)",
                   since, g_frames, g_in_swap, g_last_hook);
                sample_main_thread("STALL sample 1");
                samples = 1; next_sample = now_ms() + 250;
            }
        } else if (in_stall && g_frames != frames_at_stall) {
            lg(samples == 99 ? 1 : 0, "%s over: lasted %.0f ms", samples == 99 ? "pause" : "STALL", now_ms() - stall_begin);
            in_stall = 0;
        } else if (in_stall && samples < 12 && now_ms() >= next_sample) {
            char tag[32]; snprintf(tag, sizeof tag, "STALL sample %d", ++samples);
            sample_main_thread(tag);
            next_sample = now_ms() + (samples < 6 ? 250 : 1000);
        }
    }
    return 0;
}

static void start_watchdog(void)
{
    if (cfg.stall_ms <= 0) return;
    HANDLE h = CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    if (h) { SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL); CloseHandle(h); }
    lg(1, "watchdog: watching render thread %04lx, stall threshold %d ms", g_main_tid, cfg.stall_ms);
}

/* ------------------------------------------------------------------ IAT patching */
static int patch_iat(HMODULE mod, const char *dll, const char *fn, void *hook, void **orig)
{
    BYTE *img = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(img + ((IMAGE_DOS_HEADER *)img)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(img + dd->VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((char *)(img + d->Name), dll)) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(img + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(img + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(img + names->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, fn)) continue;
            DWORD old;
            if (orig && !*orig) *orig = (void *)iat->u1.Function;
            VirtualProtect(&iat->u1.Function, 4, PAGE_READWRITE, &old);
            iat->u1.Function = (DWORD)hook;
            VirtualProtect(&iat->u1.Function, 4, old, &old);
            return 1;
        }
    }
    return 0;
}

static void hook_exe(void)
{
    HMODULE exe = GetModuleHandleA(NULL);
    HMODULE u = GetModuleHandleA("user32.dll"), g = GetModuleHandleA("gdi32.dll"), k = GetModuleHandleA("kernel32.dll");
    int n = 0;
    /* the originals come from the DLLs themselves, so they are valid even if the exe lacks an import */
#define H(dllname, mod, fn) do { *(FARPROC *)&r_##fn = GetProcAddress(mod, #fn); n += patch_iat(exe, dllname, #fn, (void *)hk_##fn, NULL); } while (0)
    H("USER32.dll", u, ChangeDisplaySettingsA); H("USER32.dll", u, EnumDisplaySettingsA);
    H("USER32.dll", u, GetSystemMetrics);       H("USER32.dll", u, CreateWindowExA);
    H("USER32.dll", u, SetWindowPos);           H("USER32.dll", u, MoveWindow);
    H("USER32.dll", u, GetClientRect);          H("USER32.dll", u, GetWindowRect);
    H("USER32.dll", u, ClientToScreen);         H("USER32.dll", u, ScreenToClient);
    H("USER32.dll", u, GetCursorPos);           H("USER32.dll", u, SetCursorPos);
    H("USER32.dll", u, SetWindowLongA);         H("USER32.dll", u, GetWindowLongA);
    H("USER32.dll", u, ShowCursor);             H("USER32.dll", u, WindowFromPoint);
    H("GDI32.dll", g, SwapBuffers);
    H("KERNEL32.dll", k, GetProcAddress);       H("KERNEL32.dll", k, LoadLibraryA);
    HMODULE ws = GetModuleHandleA("wsock32.dll");
    if (ws) {
        H("WSOCK32.dll", ws, gethostbyname);
        *(FARPROC *)&r_WSASetLastError = GetProcAddress(GetModuleHandleA("ws2_32.dll"), "WSASetLastError");
    }
    lg(1, "DNS: lookups of [%s] fail instantly", cfg.block_hosts);
#undef H
    lg(1, "hooked %d imports of the game exe", n);
}

/* ------------------------------------------------------------------ init */
static float ini_float(const char *ini, const char *key, float def)
{
    char buf[32], dflt[32];
    snprintf(dflt, sizeof dflt, "%g", def);
    GetPrivateProfileStringA("sswrap", key, dflt, buf, sizeof buf, ini);
    return (float)atof(buf);
}

static void read_config(void)
{
    char ini[MAX_PATH], *p;
    GetModuleFileNameA(g_self, ini, sizeof ini);
    p = strrchr(ini, '\\'); strcpy(p ? p + 1 : ini, "sswrap.ini");
    cfg.enabled = GetPrivateProfileIntA("sswrap", "Enabled", cfg.enabled, ini);
    cfg.render_scale = GetPrivateProfileIntA("sswrap", "RenderScale", cfg.render_scale, ini);
    cfg.log_level = GetPrivateProfileIntA("sswrap", "LogLevel", cfg.log_level, ini);
    cfg.stall_ms = GetPrivateProfileIntA("sswrap", "StallMs", cfg.stall_ms, ini);
    cfg.filter_linear = GetPrivateProfileIntA("sswrap", "LinearFilter", cfg.filter_linear, ini);
    cfg.stats_sec = GetPrivateProfileIntA("sswrap", "StatsSeconds", cfg.stats_sec, ini);
    GetPrivateProfileStringA("sswrap", "BlockHosts", cfg.block_hosts, cfg.block_hosts, sizeof cfg.block_hosts, ini);
    cfg.ao = GetPrivateProfileIntA("sswrap", "AO", cfg.ao, ini);
    cfg.fxaa = GetPrivateProfileIntA("sswrap", "FXAA", cfg.fxaa, ini);
    cfg.ao_radius = ini_float(ini, "AORadius", cfg.ao_radius);
    cfg.ao_strength = ini_float(ini, "AOStrength", cfg.ao_strength);
    cfg.ao_max_dist = ini_float(ini, "AOMaxDistance", cfg.ao_max_dist);
    cfg.sharpen = ini_float(ini, "Sharpen", cfg.sharpen);
    GetPrivateProfileStringA("sswrap", "ExtraModes", "", cfg.extra_modes, sizeof cfg.extra_modes, ini);
}

static void open_log(void)
{
    char path[MAX_PATH], *p;
    GetModuleFileNameA(g_self, path, sizeof path);
    p = strrchr(path, '\\'); strcpy(p ? p + 1 : path, "sswrap.log");
    g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    g_self = inst;
    DisableThreadLibraryCalls(inst);
    QueryPerformanceFrequency(&g_qpf); QueryPerformanceCounter(&g_t0);
    InitializeCriticalSection(&g_log_cs);
    read_config();
    open_log();
    lg(0, "sswrap starting: Enabled=%d RenderScale=%d LogLevel=%d StallMs=%d LinearFilter=%d",
       cfg.enabled, cfg.render_scale, cfg.log_level, cfg.stall_ms, cfg.filter_linear);
    lg(0, "effects: AO=%d (radius %.2f strength %.2f maxdist %.0f) FXAA=%d Sharpen=%.2f",
       cfg.ao, cfg.ao_radius, cfg.ao_strength, cfg.ao_max_dist, cfg.fxaa, cfg.sharpen);

    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, sizeof sys);
    lstrcatA(sys, "\\opengl32.dll");
    g_realgl = LoadLibraryA(sys);
    lg(0, "real opengl32: %s -> %p (sswrap itself is %p)", sys, (void *)g_realgl, (void *)g_self);
    if (!g_realgl || g_realgl == g_self) { lg(0, "FATAL: could not load the real opengl32"); return FALSE; }
    int missing = 0;
    for (int i = 0; i < GL_EXPORT_COUNT; i++)
        if (!(g_real[i] = (void *)GetProcAddress(g_realgl, g_names[i]))) { missing++; lg(1, "real opengl32 lacks %s", g_names[i]); }
    lg(1, "forwarding %d GL exports (%d missing)", GL_EXPORT_COUNT, missing);

    DEVMODEA dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
    g_rw = dm.dmPelsWidth; g_rh = dm.dmPelsHeight;
    lg(0, "real primary monitor: %dx%d", g_rw, g_rh);
    build_mode_list();
    hook_exe();
    return TRUE;
}
