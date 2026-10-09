/* ------------------------------------------------------------------ UI texture upscaling
 * Starsiege packs its HUD/menu bitmaps and fonts into shared texture pages at runtime: it
 * creates a page with glTexImage2D (unused areas are uninitialised memory) and copies each
 * bitmap into its slot with glTexSubImage2D. Those individual copies are clean and stable.
 *
 * With UiUpscale = k (2 or 4) the first time a texture receives a sub-image copy we recreate it
 * k times larger (its existing contents read back and enlarged), and every copy into it is
 * enlarged k times on the way in: by an HD replacement from sswrap_textures/load/ if one exists
 * ("sub_<w>x<h>_<hash>.png"), otherwise by Scale2x (applied twice for 4x). Texture coordinates
 * are fractions of the texture, so the game's drawing still lines up. With TextureDump=1 each
 * copied bitmap is also saved to dump/ as sub_<w>x<h>_<hash>.tga for building HD versions. */

#define GL_TEXTURE_WIDTH_   0x1000
#define GL_TEXTURE_HEIGHT_  0x1001
#define GL_PACK_ALIGNMENT_  0x0D05
typedef void (WINAPI *PFN_glGetTexImage)(GLenum, GLint, GLenum, GLenum, GLvoid *);
typedef void (WINAPI *PFN_glGetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint *);
typedef void (WINAPI *PFN_glPixelStorei)(GLenum, GLint);
typedef void (WINAPI *PFN_glGetTexParameteriv)(GLenum, GLenum, GLint *);

struct ui_page { GLuint id; int w, h; };          /* original size; stored k times larger */
/* Only UI pages are touched. The terrain texture cache uses the same sub-image copies (hundreds
 * a second, with mip levels), so a texture is eligible only if it is not mipmapped. Decisions
 * are cached per texture id: g_ui_no holds ids we decided to leave alone. */
static GLuint g_ui_no[4096]; static int g_nui_no;
static struct ui_page g_pages[1024]; static int g_npages;
static LONG st_ui_pages, st_ui_subs, st_ui_hd, st_ui_dumped;

static int ui_k(void) { return (cfg.ui_upscale == 2 || cfg.ui_upscale == 4) ? cfg.ui_upscale : 1; }
static struct ui_page *ui_page_find(GLuint id)
{
    for (int i = 0; i < g_npages; i++) if (g_pages[i].id == id) return &g_pages[i];
    return NULL;
}
static int ui_is_no(GLuint id) { for (int i = 0; i < g_nui_no; i++) if (g_ui_no[i] == id) return 1; return 0; }
static void ui_page_forget(GLuint id)
{
    for (int i = 0; i < g_npages; i++) if (g_pages[i].id == id) { g_pages[i] = g_pages[--g_npages]; break; }
    for (int i = 0; i < g_nui_no; i++) if (g_ui_no[i] == id) { g_ui_no[i] = g_ui_no[--g_nui_no]; break; }
}
static void ui_forget_all(void) { g_npages = 0; g_nui_no = 0; }

/* is the bound texture a UI page? cached per texture id. Not UI:
 *  - mipmapped textures: the terrain cache (hundreds of tile copies a second);
 *  - a copy that fills a whole small texture: Herc shadows and distant-vehicle sprites, which
 *    the engine redraws every frame. UI art is copied into part of a larger page. */
static int ui_eligible(GLenum tg, GLuint id, GLint lv, GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (ui_page_find(id)) return 1;
    if (ui_is_no(id)) return 0;
    GLint minf = 0, tw = 0, th = 0;
    REAL(glGetTexParameteriv)(tg, GL_TEXTURE_MIN_FILTER, &minf);
    REAL(glGetTexLevelParameteriv)(tg, 0, GL_TEXTURE_WIDTH_, &tw);
    REAL(glGetTexLevelParameteriv)(tg, 0, GL_TEXTURE_HEIGHT_, &th);
    int whole_small = x == 0 && y == 0 && w == tw && h == th && tw <= 128 && th <= 128;
    if (lv > 0 || whole_small || (minf != GL_NEAREST && minf != GL_LINEAR)) {
        if (g_nui_no < 4096) g_ui_no[g_nui_no++] = id;
        return 0;
    }
    return 1;
}

/* Scale2x / EPX on packed RGBA pixels: 2x, keeps exact colours, rounds diagonal edges */
static unsigned *scale2x(const unsigned *s, int w, int h)
{
    unsigned *d = malloc((size_t)w * h * 16);
    if (!d) return NULL;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        unsigned P = s[y * w + x];
        unsigned A = s[(y > 0 ? y - 1 : y) * w + x], D = s[(y < h - 1 ? y + 1 : y) * w + x];
        unsigned C = s[y * w + (x > 0 ? x - 1 : x)], B = s[y * w + (x < w - 1 ? x + 1 : x)];
        unsigned *o = d + (size_t)(2 * y) * (2 * w) + 2 * x;
        o[0]         = (C == A && C != D && A != B) ? A : P;
        o[1]         = (A == B && A != C && B != D) ? B : P;
        o[2 * w]     = (D == C && D != B && C != A) ? C : P;
        o[2 * w + 1] = (B == D && B != A && D != C) ? D : P;
    }
    return d;
}

/* k-times enlargement of tightly packed RGBA: Scale2x once (2x) or twice (4x) */
static unsigned *ui_enlarge(const unsigned *s, int w, int h, int k)
{
    unsigned *a = scale2x(s, w, h);
    if (k == 4 && a) { unsigned *b = scale2x(a, w * 2, h * 2); free(a); a = b; }
    return a;
}

/* nearest-neighbour enlargement, used for the page's existing (mostly unused) contents */
static unsigned *ui_nearest(const unsigned *s, int w, int h, int k)
{
    unsigned *d = malloc((size_t)w * h * k * k * 4);
    if (!d) return NULL;
    for (int y = 0; y < h * k; y++) for (int x = 0; x < w * k; x++) d[(size_t)y * w * k + x] = s[(y / k) * w + x / k];
    return d;
}

static void ui_upload(GLenum tg, int x, int y, int w, int h, const void *rgba)
{
    GLint al, rl;
    REAL(glGetIntegerv)(GL_UNPACK_ALIGNMENT_, &al); REAL(glGetIntegerv)(GL_UNPACK_ROW_LENGTH_, &rl);
    REAL(glPixelStorei)(GL_UNPACK_ALIGNMENT_, 4); REAL(glPixelStorei)(GL_UNPACK_ROW_LENGTH_, 0);
    REAL(glTexSubImage2D)(tg, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    REAL(glPixelStorei)(GL_UNPACK_ALIGNMENT_, al); REAL(glPixelStorei)(GL_UNPACK_ROW_LENGTH_, rl);
}

/* make the bound texture a k-times page (once), keeping what it already holds */
static struct ui_page *ui_promote(GLenum tg, GLuint id, int k)
{
    struct ui_page *p = ui_page_find(id);
    if (p) return p;
    if (g_npages >= 1024) return NULL;
    GLint w = 0, h = 0, minf = 0;
    REAL(glGetTexLevelParameteriv)(tg, 0, GL_TEXTURE_WIDTH_, &w);
    REAL(glGetTexLevelParameteriv)(tg, 0, GL_TEXTURE_HEIGHT_, &h);
    if (w <= 0 || h <= 0 || w * k > 4096 || h * k > 4096) return NULL;
    unsigned *cur = malloc((size_t)w * h * 4), *big = NULL;
    if (!cur) return NULL;
    GLint pa; REAL(glGetIntegerv)(GL_PACK_ALIGNMENT_, &pa); REAL(glPixelStorei)(GL_PACK_ALIGNMENT_, 4);
    REAL(glGetTexImage)(tg, 0, GL_RGBA, GL_UNSIGNED_BYTE, cur);
    REAL(glPixelStorei)(GL_PACK_ALIGNMENT_, pa);
    big = ui_nearest(cur, w, h, k); free(cur);
    if (!big) return NULL;
    REAL(glGetTexParameteriv)(tg, GL_TEXTURE_MIN_FILTER, &minf);
    if (minf != GL_NEAREST && minf != GL_LINEAR) REAL(glTexParameteri)(tg, GL_GENERATE_MIPMAP_, GL_TRUE);
    GLint al; REAL(glGetIntegerv)(GL_UNPACK_ALIGNMENT_, &al); REAL(glPixelStorei)(GL_UNPACK_ALIGNMENT_, 4);
    GLint rl; REAL(glGetIntegerv)(GL_UNPACK_ROW_LENGTH_, &rl); REAL(glPixelStorei)(GL_UNPACK_ROW_LENGTH_, 0);
    REAL(glTexImage2D)(tg, 0, GL_RGBA8_, w * k, h * k, 0, GL_RGBA, GL_UNSIGNED_BYTE, big);
    REAL(glPixelStorei)(GL_UNPACK_ALIGNMENT_, al); REAL(glPixelStorei)(GL_UNPACK_ROW_LENGTH_, rl);
    free(big);
    p = &g_pages[g_npages++]; p->id = id; p->w = w; p->h = h;
    if (InterlockedIncrement(&st_ui_pages) <= 10) lg(1, "ui upscale: texture page %u %dx%d -> %dx%d", id, w, h, w * k, h * k);
    return p;
}

/* an HD replacement for a sub-image, resized (box filter, integer ratio) to exactly k times */
static unsigned *ui_replacement(const char *base, int w, int h, int k)
{
    const char *rep = g_nload ? find_replacement(base) : NULL;
    if (!rep) return NULL;
    char path[MAX_PATH]; int rw, rh, n;
    snprintf(path, sizeof path, "%s\\load\\%s", g_tex_dir, rep);
    unsigned char *img = stbi_load(path, &rw, &rh, &n, 4);
    if (!img) return NULL;
    int tw = w * k, th = h * k;
    unsigned *out = malloc((size_t)tw * th * 4);
    if (out && rw == tw && rh == th) memcpy(out, img, (size_t)tw * th * 4);
    else if (out && rw % tw == 0 && rh % th == 0 && rw / tw == rh / th) {
        int f = rw / tw;
        for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) {
            unsigned acc[4] = { 0, 0, 0, 0 };
            for (int j = 0; j < f; j++) for (int i = 0; i < f; i++) {
                const unsigned char *px = img + ((size_t)(y * f + j) * rw + (x * f + i)) * 4;
                acc[0] += px[0]; acc[1] += px[1]; acc[2] += px[2]; acc[3] += px[3];
            }
            unsigned char *o = (unsigned char *)&out[(size_t)y * tw + x];
            for (int c = 0; c < 4; c++) o[c] = (unsigned char)(acc[c] / (f * f));
        }
    } else { lg(1, "ui upscale: %s is %dx%d, need %dx%d or an integer multiple; ignored", rep, rw, rh, tw, th); free(out); out = NULL; }
    stbi_image_free(img);
    return out;
}

/* returns 1 if the sub-image was handled (enlarged and uploaded) */
static int ui_sub_image(GLenum tg, GLint lv, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const GLvoid *px)
{
    int k = ui_k();
    if ((k == 1 && !cfg.tex_dump) || tg != GL_TEXTURE_2D || !px || w <= 0 || h <= 0) return 0;
    GLuint id = bound_tex();
    if (!ui_eligible(tg, id, lv, x, y, w, h)) return 0;   /* terrain, shadows, sprites: untouched */
    int bpp = tex_bpp(fmt, type);
    if (!bpp) return 0;
    int stride = row_stride(w, bpp);
    unsigned long long v = tex_hash(px, w, h, bpp, stride, fmt);
    char base[48]; snprintf(base, sizeof base, "sub_%dx%d_%016llx", w, h, v);
    unsigned *rgba = (unsigned *)to_rgba(px, w, h, stride, fmt);
    if (!rgba) return 0;
    if (cfg.tex_dump && !already_dumped(v)) {
        char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\dump\\%s.tga", g_tex_dir, base);
        write_tga(path, (unsigned char *)rgba, w, h);
        if (g_ndumped == g_cap_dumped) { g_cap_dumped = g_cap_dumped ? g_cap_dumped * 2 : 1024; g_dumped = realloc(g_dumped, g_cap_dumped * sizeof *g_dumped); }
        g_dumped[g_ndumped++] = v;
        InterlockedIncrement(&st_ui_dumped);
    }
    if (k == 1) { free(rgba); return 0; }
    if (lv > 0) { free(rgba); return 1; }   /* our page has generated mipmaps */
    struct ui_page *p = ui_promote(tg, id, k);
    if (!p) { free(rgba); return 0; }
    unsigned *big = ui_replacement(base, w, h, k);
    if (big) InterlockedIncrement(&st_ui_hd); else big = ui_enlarge(rgba, w, h, k);
    free(rgba);
    if (!big) return 0;
    ui_upload(tg, x * k, y * k, w * k, h * k, big);
    free(big);
    if (InterlockedIncrement(&st_ui_subs) % 500 == 1)
        lg(1, "ui upscale: %ld bitmaps enlarged %dx so far (%ld from HD replacements, %ld pages)", st_ui_subs, k, st_ui_hd, st_ui_pages);
    return 1;
}
