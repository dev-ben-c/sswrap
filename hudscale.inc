/* ------------------------------------------------------------------ HUD scale (Starsiege 1.004 / 1.003)
 * Starsiege draws its HUD with SimGui control objects; each class renders itself through vtable
 * slot 30 (onRender). Found with Ghidra: the class names survive in Borland type descriptors,
 * which lead to each vtable. The game passes finished screen coordinates to OpenGL (identity
 * modelview, pixel-space ortho), so to enlarge one HUD element we wrap its onRender and, for the
 * duration of the call, scale the modelview around the element's anchor (the screen edge or
 * corner it sits nearest to). Children render inside the call, so they scale with it.
 *
 * onRender uses Borland's register convention: EAX = this, EDX = surface, ECX = update rect,
 * then the element's screen position (x, y) on the stack; the callee pops those 8 bytes.
 * The element's size is at this+0x1a4 / this+0x1a8.
 *
 * Only display meters are scaled; buttons, dialogs and pop-ups are left alone so mouse clicks
 * still land where they are drawn. Nothing is patched unless every vtable slot still holds the
 * exact function address of a known build, so other builds are never touched. The 1.003 table
 * (the community's Starsiege Players default) was found the same way and its functions verified
 * to be the same code as 1.004's; the object layout is unchanged. */

typedef void (WINAPI *PFN_glTranslatef)(GLfloat, GLfloat, GLfloat);
typedef void (WINAPI *PFN_glScalef)(GLfloat, GLfloat, GLfloat);
typedef void (WINAPI *PFN_glPushMatrix)(void);
typedef void (WINAPI *PFN_glPopMatrix)(void);
typedef void (WINAPI *PFN_glMatrixMode)(GLenum);

struct hud_class { DWORD slot; DWORD orig; const char *name; };
#define N_HUD_CLASSES 13
static const struct hud_build { const char *version; struct hud_class c[N_HUD_CLASSES]; } g_hud_builds[] = {
    { "1.004", {
        { 0x0070269c, 0x005185b4, "HudMtrCtrl" },
        { 0x00703548, 0x00529568, "HudMtrChatRecieve" },
        { 0x007013e0, 0x0050fcb4, "HudBitmapCtrl" },
        { 0x00701d04, 0x0051188c, "HudSimpleText" },
        { 0x00702880, 0x0051c028, "HudMtrRadar" },
        { 0x00702a5c, 0x0051f154, "HudMtrShields" },
        { 0x00702c3c, 0x0051f898, "HudMtrAimReticle" },
        { 0x00702e34, 0x0052469c, "HudMtrWeapDisplay" },
        { 0x00702ff4, 0x005273c8, "HudMtrDamage" },
        { 0x007031a0, 0x00527fb8, "HudMtrTarget" },
        { 0x00703364, 0x0052886c, "HudMtrIntDamage" },
        { 0x007039dc, 0x0052ace0, "HudMtrTimer" },
        { 0x00703d04, 0x0052baf4, "HudMtrCommand" } } },
    { "1.003", {
        { 0x006f2414, 0x00516118, "HudMtrCtrl" },
        { 0x006f32c0, 0x005270c8, "HudMtrChatRecieve" },
        { 0x006f1158, 0x0050d81c, "HudBitmapCtrl" },
        { 0x006f1a7c, 0x0050f3f4, "HudSimpleText" },
        { 0x006f25f8, 0x00519b8c, "HudMtrRadar" },
        { 0x006f27d4, 0x0051ccb4, "HudMtrShields" },
        { 0x006f29b4, 0x0051d3f8, "HudMtrAimReticle" },
        { 0x006f2bac, 0x005221fc, "HudMtrWeapDisplay" },
        { 0x006f2d6c, 0x00524f28, "HudMtrDamage" },
        { 0x006f2f18, 0x00525b18, "HudMtrTarget" },
        { 0x006f30dc, 0x005263cc, "HudMtrIntDamage" },
        { 0x006f3754, 0x00528840, "HudMtrTimer" },
        { 0x006f3a78, 0x00529634, "HudMtrCommand" } } },
};
static const struct hud_class *g_hud_classes;   /* the running build's table, set by hud_supported */
#define HUD_EXTENT_X 0x1a4
#define HUD_EXTENT_Y 0x1a8

static int g_hud_installed, g_hud_depth, g_hud_active;
static float g_hud_ax, g_hud_ay;          /* anchor of the element being drawn, game pixels */
static LONG g_hud_calls;

void hud_thunk(void);                      /* assembly entry, below */

static DWORD hud_orig_for(void *self)
{
    DWORD vt = *(DWORD *)self;              /* vtable pointer */
    for (int i = 0; i < N_HUD_CLASSES; i++)
        if (g_hud_classes[i].slot - 30 * 4 == vt) return g_hud_classes[i].orig;
    return 0;
}

static void hud_apply_matrix(void)
{
    float k = cfg.hud_scale;
    REAL(glTranslatef)(g_hud_ax, g_hud_ay, 0);
    REAL(glScalef)(k, k, 1);
    REAL(glTranslatef)(-g_hud_ax, -g_hud_ay, 0);
}

/* called before the original onRender; returns the function to call */
__attribute__((used)) DWORD __cdecl hud_pre(void *self, int x, int y)
{
    DWORD orig = hud_orig_for(self);
    if (++g_hud_depth != 1 || cfg.hud_scale == 1.0f || !g_fbo_ready || !g_vactive) return orig;
    int w = *(int *)((char *)self + HUD_EXTENT_X), h = *(int *)((char *)self + HUD_EXTENT_Y);
    if (w <= 0 || h <= 0 || w > g_vw * 2 || h > g_vh * 2) return orig;
    /* anchor: a shared screen point for the region the element is in (corner, edge centre or
     * screen centre). Every element in, say, the bottom-right scales around the bottom-right
     * corner, so neighbouring elements (a panel and its label) keep their relative layout
     * instead of each growing from its own edge and drifting apart. */
    float cx = x + w * 0.5f, cy = y + h * 0.5f;
    g_hud_ax = cx < g_vw / 3.0f ? 0.0f : cx > g_vw * 2.0f / 3.0f ? (float)g_vw : g_vw * 0.5f;
    g_hud_ay = cy < g_vh / 3.0f ? 0.0f : cy > g_vh * 2.0f / 3.0f ? (float)g_vh : g_vh * 0.5f;
    GLint mode; REAL(glGetIntegerv)(GL_MATRIX_MODE, &mode);
    REAL(glMatrixMode)(GL_MODELVIEW); REAL(glPushMatrix)(); hud_apply_matrix(); REAL(glMatrixMode)(mode);
    g_hud_active = 1;
    InterlockedIncrement(&g_hud_calls);
    return orig;
}

/* called after the original onRender returns */
__attribute__((used)) void __cdecl hud_post(void)
{
    if (--g_hud_depth == 0 && g_hud_active) {
        GLint mode; REAL(glGetIntegerv)(GL_MATRIX_MODE, &mode);
        REAL(glMatrixMode)(GL_MODELVIEW); REAL(glPopMatrix)(); REAL(glMatrixMode)(mode);
        g_hud_active = 0;
    }
    if (g_hud_depth < 0) g_hud_depth = 0;
}

/* Borland register-convention trampoline: keep EAX/EDX/ECX for the original, pass this/x/y to
 * hud_pre, call the original with the same registers and stack arguments, then hud_post. */
__asm__(
    ".text\n"
    ".globl _hud_thunk\n"
    "_hud_thunk:\n"
    "  pushl %ebp\n"
    "  movl  %esp, %ebp\n"
    "  pushl %eax\n"                 /* -4  this    */
    "  pushl %edx\n"                 /* -8  surface */
    "  pushl %ecx\n"                 /* -12 rect    */
    "  pushl 12(%ebp)\n"             /* y */
    "  pushl 8(%ebp)\n"              /* x */
    "  pushl %eax\n"                 /* this */
    "  call  _hud_pre\n"
    "  addl  $12, %esp\n"
    "  pushl %eax\n"                 /* -16 original onRender */
    "  pushl 12(%ebp)\n"             /* stack args for the original (it pops them) */
    "  pushl 8(%ebp)\n"
    "  movl  -4(%ebp), %eax\n"
    "  movl  -8(%ebp), %edx\n"
    "  movl  -12(%ebp), %ecx\n"
    "  call  *-16(%ebp)\n"
    "  call  _hud_post\n"
    "  movl  %ebp, %esp\n"
    "  popl  %ebp\n"
    "  ret   $8\n"
);

/* 1 if this is a game build the HUD hooks were made for (every slot holds that build's address) */
static int hud_supported(void)
{
    if (g_hud_classes) return 1;
    for (int b = 0; b < (int)(sizeof g_hud_builds / sizeof g_hud_builds[0]); b++) {
        const struct hud_class *c = g_hud_builds[b].c;
        int i;
        for (i = 0; i < N_HUD_CLASSES; i++) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void *)c[i].slot, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
                *(DWORD *)c[i].slot != c[i].orig)
                break;
        }
        if (i == N_HUD_CLASSES) {
            g_hud_classes = c;
            lg(1, "hud scale: game build %s recognised", g_hud_builds[b].version);
            return 1;
        }
    }
    return 0;
}

static void hud_install(void)
{
    if (g_hud_installed || !cfg.enabled) return;
    if (cfg.hud_scale == 1.0f) { lg(1, "hud scale: off (HudScale=1)"); return; }
    if (!hud_supported()) {
        lg(0, "hud scale: this game build is not 1.004 or 1.003; HUD scale disabled");
        return;
    }
    for (int i = 0; i < N_HUD_CLASSES; i++) {
        DWORD old;
        VirtualProtect((void *)g_hud_classes[i].slot, 4, PAGE_READWRITE, &old);
        *(DWORD *)g_hud_classes[i].slot = (DWORD)hud_thunk;
        VirtualProtect((void *)g_hud_classes[i].slot, 4, old, &old);
    }
    g_hud_installed = 1;
    lg(1, "hud scale: %.2fx on %d HUD element classes", cfg.hud_scale, N_HUD_CLASSES);
}

/* a scissor rectangle (GL coords, y up) set while an element is being drawn scales with it */
static void hud_scissor(GLint *x, GLint *y, GLsizei *w, GLsizei *h)
{
    if (!g_hud_active) return;
    float k = cfg.hud_scale, ax = g_hud_ax, ay = g_vh - g_hud_ay;
    float x0 = ax + (*x - ax) * k, y0 = ay + (*y - ay) * k;
    float x1 = ax + (*x + *w - ax) * k, y1 = ay + (*y + *h - ay) * k;
    *x = (GLint)(x0 + 0.5f); *y = (GLint)(y0 + 0.5f); *w = (GLsizei)(x1 - x0 + 0.5f); *h = (GLsizei)(y1 - y0 + 0.5f);
}
