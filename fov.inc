/* ------------------------------------------------------------------ widescreen field of view
 * Starsiege keeps the horizontal field of view fixed: the 3D view's half-width at the near plane
 * is tan(fov/2) * near and its half-height follows from the view's height/width. On anything
 * wider than 4:3 that crops the top and bottom (on 21:9 the view is 45 degrees tall instead of
 * 74), so it looks zoomed in. WidescreenFOV=1 widens it instead ("Hor+"): the tan(fov/2) the
 * game computes is scaled by (width/height) / (4/3), so the vertical view is the one 4:3 gets and
 * the sides open up.
 *
 * Three places compute that tan, each a call to the C library's tan():
 *  - the 3D view (SimGui TS control): the view rectangle's width/height are on its stack,
 *  - a second camera control: width/height in the control's extent (EBX+0x1a4/0x1a8),
 *  - the HUD's screen projection for reticle and target markers (EBX+0x1a4/0x1a8), so markers
 *    stay on their targets; it derives the vertical angle from this one.
 * They are found by their instruction patterns, so 1.004 and 1.003 are both covered; nothing is
 * patched unless every pattern occurs exactly once and all three call the same tan(). */

DWORD g_fov_tan;                                /* the game's tan() */
static int g_fov_ok;                            /* the camera code was recognised and patched */

__attribute__((used)) float __cdecl fov_k(int w, int h)
{
    float k = 1.0f;
    if (cfg.wide_fov && w > 0 && h > 0) {
        float a = (float)w / (float)h;
        if (a > 4.0f / 3.0f + 0.01f) k = a * 0.75f;
    }
    return k;
}
__attribute__((used)) float __cdecl fov_k_view(int w, int h) { return g_fov_k = fov_k(w, h); }

/* replacements for the three tan(double) calls (cdecl; the caller pops the argument). Each calls
 * the original, keeps the result, gets k for its own view size and returns k * tan. */
__asm__(
    ".text\n"
    ".globl _fov_tan_view\n"
    "_fov_tan_view:\n"
    "  subl  $8, %esp\n"
    "  fldl  12(%esp)\n"                /* the argument */
    "  subl  $8, %esp\n"
    "  fstpl (%esp)\n"
    "  call  *_g_fov_tan\n"
    "  addl  $8, %esp\n"
    "  fstpl (%esp)\n"
    "  pushl 0x40(%esp)\n"              /* view height (caller's [esp+0x2c]) */
    "  pushl 0x40(%esp)\n"              /* view width  (caller's [esp+0x28]) */
    "  call  _fov_k_view\n"
    "  addl  $8, %esp\n"
    "  fmull (%esp)\n"
    "  addl  $8, %esp\n"
    "  ret\n"
    ".globl _fov_tan_ebx\n"
    "_fov_tan_ebx:\n"
    "  subl  $8, %esp\n"
    "  fldl  12(%esp)\n"
    "  subl  $8, %esp\n"
    "  fstpl (%esp)\n"
    "  call  *_g_fov_tan\n"
    "  addl  $8, %esp\n"
    "  fstpl (%esp)\n"
    "  pushl 0x1a8(%ebx)\n"             /* control height */
    "  pushl 0x1a4(%ebx)\n"             /* control width  */
    "  call  _fov_k\n"
    "  addl  $8, %esp\n"
    "  fmull (%esp)\n"
    "  addl  $8, %esp\n"
    "  ret\n"
);
void fov_tan_view(void);
void fov_tan_ebx(void);

/* find a byte pattern (-1 = any byte) in the exe's code; returns the address if it occurs once */
static BYTE *code_find_once(const short *pat, int n)
{
    BYTE *img = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(img + ((IMAGE_DOS_HEADER *)img)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    BYTE *found = NULL; int hits = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *p = img + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize - n;
        for (; p < end; p++) {
            int i = 0;
            while (i < n && (pat[i] < 0 || p[i] == pat[i])) i++;
            if (i == n) { found = p; hits++; }
        }
    }
    return hits == 1 ? found : NULL;
}

static void fov_install(void)
{
    if (!cfg.enabled) return;                   /* patched whenever recognised; WidescreenFOV is read live */
    static const short view[] = { 0xd9,0x86,0xf8,0x01,0x00,0x00, 0xdd,0x1c,0x24, 0xe8,-1,-1,-1,-1, 0xd8,0x8e,0xf0,0x01,0x00,0x00 };
    static const short cam[]  = { 0xd9,0x83,0xe4,0x01,0x00,0x00, 0xdd,0x1c,0x24, 0xe8,-1,-1,-1,-1, 0xd8,0x8b,0xdc,0x01,0x00,0x00 };
    static const short hud[]  = { 0xe8,-1,-1,-1,-1, 0x83,0xc4,0xf8, 0xdd,0x1c,0x24, 0xe8,-1,-1,-1,-1,
                                  0x83,0xc4,0x08, 0xd9,0x5c,0x24,0x08, 0xd9,0x44,0x24,0x08, 0xd8,0x1d };
    struct { BYTE *call; void *with; } site[3] = {
        { NULL, (void *)fov_tan_view }, { NULL, (void *)fov_tan_ebx }, { NULL, (void *)fov_tan_ebx } };
    BYTE *v = code_find_once(view, sizeof view / 2), *c = code_find_once(cam, sizeof cam / 2), *h = code_find_once(hud, sizeof hud / 2);
    if (!v || !c || !h) { lg(0, "widescreen FOV: this game build's camera code was not recognised; FOV left as is"); return; }
    site[0].call = v + 9; site[1].call = c + 9; site[2].call = h + 11;
    DWORD tan_fn = 0;
    for (int i = 0; i < 3; i++) {
        DWORD t = (DWORD)site[i].call + 5 + *(int *)(site[i].call + 1);
        if (i && t != tan_fn) { lg(0, "widescreen FOV: camera code calls differ; FOV left as is"); return; }
        tan_fn = t;
    }
    g_fov_tan = tan_fn;
    for (int i = 0; i < 3; i++) {
        DWORD old;
        VirtualProtect(site[i].call + 1, 4, PAGE_EXECUTE_READWRITE, &old);
        *(int *)(site[i].call + 1) = (int)((DWORD)site[i].with - ((DWORD)site[i].call + 5));
        VirtualProtect(site[i].call + 1, 4, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    g_fov_ok = 1;
    lg(1, "widescreen FOV: %s (3D view, camera and HUD projection at %p, %p, %p)", cfg.wide_fov ? "on" : "off",
       site[0].call, site[1].call, site[2].call);
}
