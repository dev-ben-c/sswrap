/* ------------------------------------------------------------------ HD text (Starsiege 1.004 / 1.003)
 * Every string the game draws goes through one routine (1.004: 0x65c030), reached from entry 9 of the
 * drawing-function table each surface type carries at surface+4. It lays the string out and blits
 * one glyph per character from the font's 8-bit glyph sheet through entry 6 (drawBitmap) of the
 * same table. All of the game's text lands on the OpenGL screen surface, so on that surface:
 *  - the text routine is wrapped, to know when a blit is a glyph, and
 *  - each glyph blit is replaced by a smooth, high-resolution rebuild of the same glyph, drawn into
 *    exactly the box the game chose. Layout, spacing, alignment and colours stay the game's own.
 * A glyph is rebuilt once, from its pixels read through the palette the game's OpenGL code uses
 * for that bitmap (see glyph_rebuild). The results live in mipmapped atlas pages, so they stay
 * smooth at any size and with HUD scale.
 *
 * TextTrace=1 additionally logs each distinct string once (diagnostics). */

typedef void (WINAPI *PFN_glTexEnvi)(GLenum, GLenum, GLint);
typedef void (WINAPI *PFN_glColor4f)(GLfloat, GLfloat, GLfloat, GLfloat);

/* per game build: the text routine, the OpenGL glyph blit (drawBitmap2d_rf), the drawing-function
 * tables (software, Glide, OpenGL), the surface vtables (OpenGL, memory, CDS, Glide), where the
 * OpenGL texture cache keeps its colour tables (see 0x64f788 in 1.004), and the game's
 * glTexParameteri pointer (see tp_watch). 1.003 is the Starsiege Players default; its code is the
 * same, only moved, and its texture cache keeps the colour tables 12 bytes further in. */
static const struct txt_build {
    const char *version;
    DWORD drawtext, gl_blit, tables[3], vts[4], pal_base, pal_key, tp_slot;
} g_txt_builds[] = {
    { "1.004", 0x0065c030, 0x006553c8, { 0x0072971c, 0x007298dc, 0x0072a690 },
      { 0x0072a520, 0x0072c020, 0x0072c190, 0x007299c4 }, 0x230, 0xe38, 0x008775cc },
    { "1.003", 0x0064c318, 0x006457c0, { 0x00719124, 0x007192e4, 0x0071a0a4 },
      { 0x00719f28, 0x0071ba34, 0x0071bba4, 0x007193cc }, 0x23c, 0xe44, 0x008564e8 },
};
static const struct txt_build *g_tb;            /* the running build, set by text_supported */
static const char *const g_vt_kinds[4] = { "opengl", "memory", "cds", "glide" };
DWORD g_tt_orig, g_tb_orig;
static int g_txt_depth, g_txt_on;
static DWORD g_tt_seen[4096]; static int g_tt_nseen;
static LONG st_txt_glyphs, st_txt_blits;

/* ---- glyph cache: mipmapped atlas pages filled shelf by shelf */
#define TXT_K     8                             /* rebuild resolution, times the original */
#define TXT_PAGE  2048
#define TXT_GAP   4
struct txt_glyph { DWORD key; GLuint tex; float u0, v0, u1, v1; };
static struct txt_glyph g_glyphs[8192]; static int g_nglyphs;
static GLuint g_txt_pages[16]; static int g_txt_npages, g_txt_x, g_txt_y, g_txt_row;
static int g_txt_full;

static void text_forget(void)
{
    g_nglyphs = 0; g_txt_npages = 0; g_txt_x = g_txt_y = g_txt_row = 0; g_txt_full = 0;
    memset(g_glyphs, 0, sizeof g_glyphs);
}

static struct txt_glyph *glyph_slot(DWORD key)
{
    unsigned i = key & 8191;
    for (int n = 0; n < 8192; n++, i = (i + 1) & 8191) {
        if (!g_glyphs[i].key || g_glyphs[i].key == key) return &g_glyphs[i];
    }
    return NULL;
}

/* room for a w x h rebuild; returns the page texture and position */
static GLuint atlas_alloc(int w, int h, int *x, int *y)
{
    if (w + TXT_GAP > TXT_PAGE || h + TXT_GAP > TXT_PAGE) return 0;
    if (g_txt_npages && g_txt_x + w + TXT_GAP > TXT_PAGE) { g_txt_x = 0; g_txt_y += g_txt_row; g_txt_row = 0; }
    if (!g_txt_npages || g_txt_y + h + TXT_GAP > TXT_PAGE) {
        if (g_txt_npages >= 16) { if (!g_txt_full++) lg(0, "hd text: glyph atlas full"); return 0; }
        GLuint t; REAL(glGenTextures)(1, &t);
        REAL(glBindTexture)(GL_TEXTURE_2D, t);
        REAL(glTexParameteri)(GL_TEXTURE_2D, GL_GENERATE_MIPMAP_, GL_TRUE);
        REAL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        REAL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        REAL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        REAL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        REAL(glTexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA8_, TXT_PAGE, TXT_PAGE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        g_txt_pages[g_txt_npages++] = t;
        g_txt_x = g_txt_y = g_txt_row = 0;
        lg(1, "hd text: glyph atlas page %d", g_txt_npages);
    }
    *x = g_txt_x + TXT_GAP / 2; *y = g_txt_y + TXT_GAP / 2;
    g_txt_x += w + TXT_GAP;
    if (h + TXT_GAP > g_txt_row) g_txt_row = h + TXT_GAP;
    return g_txt_pages[g_txt_npages - 1];
}

/* Scale2x on 32-bit pixels (exact colour equality, which suits palette art) */
static unsigned *s2x(const unsigned *m, int w, int h)
{
    unsigned *o = malloc((size_t)w * h * 16);
    if (!o) return NULL;
    #define M(x, y) m[((y) < 0 ? 0 : (y) >= h ? h - 1 : (y)) * w + ((x) < 0 ? 0 : (x) >= w ? w - 1 : (x))]
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        unsigned E = M(x, y), B = M(x, y - 1), D = M(x - 1, y), F = M(x + 1, y), H = M(x, y + 1);
        unsigned e0 = E, e1 = E, e2 = E, e3 = E;
        if (B != H && D != F) { e0 = D == B ? D : E; e1 = B == F ? F : E; e2 = D == H ? D : E; e3 = H == F ? F : E; }
        o[(2 * y) * 2 * w + 2 * x] = e0; o[(2 * y) * 2 * w + 2 * x + 1] = e1;
        o[(2 * y + 1) * 2 * w + 2 * x] = e2; o[(2 * y + 1) * 2 * w + 2 * x + 1] = e3;
    }
    #undef M
    return o;
}
static void box_blur(float *a, float *t, int w, int h, int r)  /* 4 channels, in place, 2 passes */
{
    for (int pass = 0; pass < 2 && r > 0; pass++) {
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < 4; c++) {
            float s = 0; int n = 0;
            for (int d = -r; d <= r; d++) { int xx = x + d; if (xx >= 0 && xx < w) { s += a[(y * w + xx) * 4 + c]; n++; } }
            t[(y * w + x) * 4 + c] = s / n;
        }
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < 4; c++) {
            float s = 0; int n = 0;
            for (int d = -r; d <= r; d++) { int yy = y + d; if (yy >= 0 && yy < h) { s += t[(yy * w + x) * 4 + c]; n++; } }
            a[(y * w + x) * 4 + c] = s / n;
        }
    }
}
/* Rebuild a w x h RGBA glyph at TXT_K times the size. Scale2x (three times for 8x) runs on the
 * colours themselves, so it straightens staircase diagonals both on the outline and where colours
 * meet inside the glyph (the menu fonts are yellow with a dark outline). Then, premultiplied:
 * the shape gets a light blur and a soft re-threshold (anti-aliased edge, square corners stay
 * square) and the colours a slightly wider blur (smooth borders between colours). */
static unsigned *glyph_rebuild(const unsigned *src, int w, int h)
{
    const int ra = TXT_K / 8, rc = TXT_K / 4;
    int W = w * TXT_K, H = h * TXT_K, P = TXT_K, pw = w + 2, ph = h + 2;
    unsigned *m = calloc((size_t)pw * ph, 4);
    if (!m) return NULL;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        unsigned c = src[y * w + x];
        m[(y + 1) * pw + x + 1] = (c >> 24) >= 128 ? (c | 0xff000000u) : 0;
    }
    int cw = pw, ch = ph;
    for (int k = 1; k < TXT_K && m; k *= 2) { unsigned *n = s2x(m, cw, ch); free(m); m = n; cw *= 2; ch *= 2; }
    size_t N = (size_t)cw * ch * 4;
    float *a = malloc(N * sizeof(float)), *c = malloc(N * sizeof(float)), *t = malloc(N * sizeof(float));
    unsigned *out = malloc((size_t)W * H * 4);
    if (!m || !a || !c || !t || !out) { free(m); free(a); free(c); free(t); free(out); return NULL; }
    for (size_t i = 0; i < (size_t)cw * ch; i++) {   /* premultiplied */
        float al = (m[i] >> 24) / 255.0f;
        a[i * 4] = (m[i] & 255) * al; a[i * 4 + 1] = (m[i] >> 8 & 255) * al; a[i * 4 + 2] = (m[i] >> 16 & 255) * al; a[i * 4 + 3] = al;
    }
    memcpy(c, a, N * sizeof(float));
    box_blur(a, t, cw, ch, ra);                 /* shape: light blur, square corners stay square */
    box_blur(c, t, cw, ch, rc);                 /* colours: a bit more, smooths borders inside */
    for (int Y = 0; Y < H; Y++) for (int X = 0; X < W; X++) {
        size_t i = ((size_t)(Y + P) * cw + X + P) * 4;
        float al = (a[i + 3] - 0.4f) / 0.2f;
        al = al < 0 ? 0 : al > 1 ? 1 : al * al * (3 - 2 * al);
        float ca = c[i + 3];
        unsigned r = 0, g = 0, b = 0;
        if (ca > 0.001f) { r = (unsigned)(c[i] / ca + 0.5f); g = (unsigned)(c[i + 1] / ca + 0.5f); b = (unsigned)(c[i + 2] / ca + 0.5f); }
        r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
        out[Y * W + X] = r | g << 8 | b << 16 | (unsigned)(al * 255 + 0.5f) << 24;
    }
    free(m); free(a); free(c); free(t);
    return out;
}
/* the colour table the game's OpenGL code converts this bitmap with (see 0x64f788) */
static const BYTE *glyph_palette(const BYTE *surf, const BYTE *bmp)
{
    const BYTE *obj = *(const BYTE *const *)(surf + 0x158);
    if (!obj) return NULL;
    DWORD key = *(const DWORD *)(bmp + 0x34);
    const BYTE *base = obj + g_tb->pal_base;
    for (int n = 0; n < 16; n++)
        if (*(const DWORD *)(obj + g_tb->pal_key + n * 0xc0c) == key) { base = obj + g_tb->pal_base + n * 0xc0c; break; }
    return base + ((*(const DWORD *)(bmp + 0x30) & 1) ? 0x400 : 0);
}

static struct txt_glyph *glyph_get(const BYTE *surf, const BYTE *bmp, const int *r)
{
    int w = r[2] - r[0] + 1, h = r[3] - r[1] + 1;
    int bw = *(const int *)(bmp + 0x10), bh = *(const int *)(bmp + 0x14), stride = *(const int *)(bmp + 0x18);
    const BYTE *bits = *(const BYTE *const *)(bmp + 0x38);
    if (w <= 0 || h <= 0 || w > 128 || h > 128 || !bits || r[0] < 0 || r[1] < 0 || r[2] >= bw || r[3] >= bh) return NULL;
    const BYTE *pal = glyph_palette(surf, bmp);
    if (!pal) return NULL;
    /* key: palette, size and the glyph's pixels, so a reloaded font can never show stale glyphs */
    DWORD key = 2166136261u;
    key = (key ^ (DWORD)pal) * 16777619u; key = (key ^ (DWORD)(w << 8 | h)) * 16777619u;
    for (int y = 0; y < h; y++) {
        const BYTE *row = bits + (r[1] + y) * stride + r[0];
        for (int x = 0; x < w; x++) key = (key ^ row[x]) * 16777619u;
    }
    if (!key) key = 1;
    struct txt_glyph *g = glyph_slot(key);
    if (!g) return NULL;
    if (g->key == key) return g->tex ? g : NULL;
    g->key = key; g->tex = 0;
    unsigned *src = malloc((size_t)w * h * 4);
    if (!src) return NULL;
    for (int y = 0; y < h; y++) {
        const BYTE *row = bits + (r[1] + y) * stride + r[0];
        for (int x = 0; x < w; x++) src[y * w + x] = *(const unsigned *)(pal + row[x] * 4);
    }
    if (cfg.tex_dump) {                         /* originals, for tuning the rebuild offline */
        static int made;
        char path[MAX_PATH];
        if (!made++) { snprintf(path, sizeof path, "%s\\glyphs", g_tex_dir); CreateDirectoryA(path, NULL); }
        snprintf(path, sizeof path, "%s\\glyphs\\g_%dx%d_%08lx.tga", g_tex_dir, w, h, (unsigned long)key);
        write_tga(path, (const unsigned char *)src, w, h);
    }
    unsigned *big = glyph_rebuild(src, w, h);
    free(src);
    if (!big) return NULL;
    int W = w * TXT_K, H = h * TXT_K, ax, ay;
    GLuint t = atlas_alloc(W, H, &ax, &ay);
    if (t) {
        REAL(glBindTexture)(GL_TEXTURE_2D, t);
        GLint al; REAL(glGetIntegerv)(GL_UNPACK_ALIGNMENT_, &al); REAL(glPixelStorei)(GL_UNPACK_ALIGNMENT_, 4);
        GLint rl; REAL(glGetIntegerv)(GL_UNPACK_ROW_LENGTH_, &rl); REAL(glPixelStorei)(GL_UNPACK_ROW_LENGTH_, 0);
        REAL(glTexSubImage2D)(GL_TEXTURE_2D, 0, ax, ay, W, H, GL_RGBA, GL_UNSIGNED_BYTE, big);
        REAL(glPixelStorei)(GL_UNPACK_ALIGNMENT_, al); REAL(glPixelStorei)(GL_UNPACK_ROW_LENGTH_, rl);
        g->tex = t;
        g->u0 = (float)ax / TXT_PAGE; g->v0 = (float)ay / TXT_PAGE;
        g->u1 = (float)(ax + W) / TXT_PAGE; g->v1 = (float)(ay + H) / TXT_PAGE;
        if (InterlockedIncrement(&st_txt_glyphs) == 1) lg(1, "hd text: first glyph rebuilt (%dx%d -> %dx%d)", w, h, W, H);
    }
    free(big);
    return g->tex ? g : NULL;
}

/* replaces one glyph blit; 0 = not ours, let the game draw it */
__attribute__((used)) int __cdecl text_blit(BYTE *surf, BYTE *bmp, int *r, DWORD flags, int *pt)
{
    if (!g_txt_depth || !cfg.text_hd || flags || !surf || !bmp || !r || !pt || *(DWORD *)surf != g_tb->vts[0]) return 0;
    /* everything changed here is restored before returning: the game caches its own GL state */
    REAL(glPushAttrib)(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TEXTURE_BIT | GL_CURRENT_BIT);
    struct txt_glyph *g = glyph_get(surf, bmp, r);
    if (!g) { REAL(glPopAttrib)(); return 0; }
    float x0 = (float)pt[0], y0 = (float)pt[1];
    float x1 = x0 + (r[2] - r[0] + 1), y1 = y0 + (r[3] - r[1] + 1);
    float u0 = g->u0, v0 = g->v0, u1 = g->u1, v1 = g->v1;
    if (surf[8] & 1) {                          /* the surface's clip rectangle, as the game applies it */
        float cx0 = (float)*(int *)(surf + 0xc), cy0 = (float)*(int *)(surf + 0x10);
        float cx1 = *(int *)(surf + 0x14) + 1.0f, cy1 = *(int *)(surf + 0x18) + 1.0f;
        if (x1 <= cx0 || x0 >= cx1 || y1 <= cy0 || y0 >= cy1) { REAL(glPopAttrib)(); return 1; }
        float du = (u1 - u0) / (x1 - x0), dv = (v1 - v0) / (y1 - y0);
        if (x0 < cx0) { u0 += (cx0 - x0) * du; x0 = cx0; }
        if (y0 < cy0) { v0 += (cy0 - y0) * dv; y0 = cy0; }
        if (x1 > cx1) { u1 -= (x1 - cx1) * du; x1 = cx1; }
        if (y1 > cy1) { v1 -= (y1 - cy1) * dv; y1 = cy1; }
    }
    float alpha = *(float *)(surf + 0xfc);
    if (!(alpha > 0.0f && alpha <= 1.0f)) alpha = 1.0f;
    REAL(glEnable)(GL_TEXTURE_2D);
    REAL(glBindTexture)(GL_TEXTURE_2D, g->tex);
    REAL(glDisable)(GL_ALPHA_TEST);
    REAL(glEnable)(GL_BLEND);
    REAL(glBlendFunc)(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    REAL(glTexEnvi)(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    REAL(glColor4f)(1, 1, 1, alpha);
    REAL(glBegin)(GL_QUADS);
    REAL(glTexCoord2f)(u0, v0); REAL(glVertex2f)(x0, y0);
    REAL(glTexCoord2f)(u1, v0); REAL(glVertex2f)(x1, y0);
    REAL(glTexCoord2f)(u1, v1); REAL(glVertex2f)(x1, y1);
    REAL(glTexCoord2f)(u0, v1); REAL(glVertex2f)(x0, y1);
    REAL(glEnd)();
    REAL(glPopAttrib)();
    InterlockedIncrement(&st_txt_blits);
    return 1;
}

static void text_trace_log(DWORD *surf, DWORD *font, int *pt, const void *str)
{
    char s[160]; int n = 0, wide = (((BYTE *)font)[0x19] & 2) != 0;
    if (wide) { const WORD *w = str; while (w[n] && n < (int)sizeof s - 1) { s[n] = w[n] < 128 ? (char)w[n] : '?'; n++; } }
    else { const BYTE *b = str; while (b[n] && n < (int)sizeof s - 1) { s[n] = b[n] >= 32 && b[n] < 127 ? (char)b[n] : '?'; n++; } }
    s[n] = 0;
    const char *kind = "?";
    for (int i = 0; i < 4; i++) if (*surf == g_tb->vts[i]) kind = g_vt_kinds[i];
    DWORD h = 2166136261u;                      /* FNV-1a over surface kind, font and text */
    for (const char *p = kind; *p; p++) h = (h ^ (BYTE)*p) * 16777619u;
    h = (h ^ (DWORD)font) * 16777619u;
    for (int i = 0; i < n; i++) h = (h ^ (BYTE)s[i]) * 16777619u;
    for (int i = 0; i < g_tt_nseen; i++) if (g_tt_seen[i] == h) return;
    if (g_tt_nseen >= 4096) return;
    g_tt_seen[g_tt_nseen++] = h;
    lg(0, "text: %-8s font %08lx h%-3d at %5d,%-5d \"%s\"", kind, (unsigned long)(DWORD)font,
       (int)font[9], pt[0], pt[1], s);
}

__attribute__((used)) void __cdecl text_pre(DWORD *surf, DWORD *font, int *pt, const void *str)
{
    if (cfg.text_trace && surf && font && pt && str) text_trace_log(surf, font, pt, str);
    g_txt_depth++;
}

__attribute__((used)) void __cdecl text_post(void)
{
    if (--g_txt_depth < 0) g_txt_depth = 0;
}

/* Borland register convention, drawText: EAX = surface, EDX = font, ECX = &point, string on the
 * stack (callee pops 4); returns the right edge in EAX, which is passed back unchanged. */
__asm__(
    ".text\n"
    ".globl _tt_thunk\n"
    "_tt_thunk:\n"
    "  pushl %ebp\n"
    "  movl  %esp, %ebp\n"
    "  pushl %eax\n"                 /* -4  surface */
    "  pushl %edx\n"                 /* -8  font    */
    "  pushl %ecx\n"                 /* -12 point   */
    "  pushl 8(%ebp)\n"              /* string */
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl %eax\n"
    "  call  _text_pre\n"
    "  addl  $16, %esp\n"
    "  pushl 8(%ebp)\n"              /* the original pops it */
    "  movl  -4(%ebp), %eax\n"
    "  movl  -8(%ebp), %edx\n"
    "  movl  -12(%ebp), %ecx\n"
    "  call  *_g_tt_orig\n"
    "  pushl %eax\n"
    "  call  _text_post\n"
    "  popl  %eax\n"
    "  movl  %ebp, %esp\n"
    "  popl  %ebp\n"
    "  ret   $4\n"
);
/* drawBitmap2d_rf: EAX = surface, EDX = bitmap, ECX = &source rect, stack: flags, &point (callee
 * pops 8). Either draws the HD glyph or falls through to the original with registers intact. */
__asm__(
    ".text\n"
    ".globl _tb_thunk\n"
    "_tb_thunk:\n"
    "  pushl %ebp\n"
    "  movl  %esp, %ebp\n"
    "  pushl %eax\n"
    "  pushl %edx\n"
    "  pushl %ecx\n"
    "  pushl 12(%ebp)\n"             /* &point */
    "  pushl 8(%ebp)\n"              /* flags  */
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl %eax\n"
    "  call  _text_blit\n"
    "  addl  $20, %esp\n"
    "  testl %eax, %eax\n"
    "  jnz   1f\n"
    "  movl  -4(%ebp), %eax\n"
    "  movl  -8(%ebp), %edx\n"
    "  movl  -12(%ebp), %ecx\n"
    "  movl  %ebp, %esp\n"
    "  popl  %ebp\n"
    "  jmp   *_g_tb_orig\n"
    "1:\n"
    "  movl  %ebp, %esp\n"
    "  popl  %ebp\n"
    "  ret   $8\n"
);
void tt_thunk(void);
void tb_thunk(void);

static int patch_slot(DWORD addr, DWORD expect, DWORD value)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)addr, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || *(DWORD *)addr != expect) return 0;
    DWORD old;
    VirtualProtect((void *)addr, 4, PAGE_READWRITE, &old);
    *(DWORD *)addr = value;
    VirtualProtect((void *)addr, 4, old, &old);
    return 1;
}

/* 1 if the text hooks fit this game build (both OpenGL table entries hold that build's functions) */
static int text_supported(void)
{
    if (g_tb) return 1;
    for (int i = 0; i < (int)(sizeof g_txt_builds / sizeof g_txt_builds[0]); i++) {
        const struct txt_build *t = &g_txt_builds[i];
        DWORD a = t->tables[2] + 6 * 4, b = t->tables[2] + 9 * 4;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void *)a, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
            *(DWORD *)a == t->gl_blit && *(DWORD *)b == t->drawtext) { g_tb = t; return 1; }
    }
    return 0;
}

/* where the game keeps its glTexParameteri pointer (see tp_watch), per build */
static void tp_init(void)
{
    if (cfg.enabled && text_supported()) g_tp_slot = (DWORD *)g_tb->tp_slot;
}

/* Installed whenever the build fits; TextHD only decides whether glyphs are replaced, so the
 * startup window can switch it either way. */
static void text_install(void)
{
    tp_init();
    if (!cfg.enabled) return;
    if (!text_supported()) { lg(1, "hd text: this game build is not 1.004 or 1.003; HD text unavailable"); return; }
    g_tt_orig = g_tb->drawtext; g_tb_orig = g_tb->gl_blit;
    for (int i = 0; i < 3; i++)
        if (cfg.text_trace || i == 2)
            patch_slot(g_tb->tables[i] + 9 * 4, g_tb->drawtext, (DWORD)tt_thunk);
    g_txt_on = patch_slot(g_tb->tables[2] + 6 * 4, g_tb->gl_blit, (DWORD)tb_thunk);
    lg(1, "hd text: %s (game build %s)%s", cfg.text_hd ? "on" : "off (TextHD=0)", g_tb->version,
       cfg.text_trace ? ", text trace on" : "");
}
