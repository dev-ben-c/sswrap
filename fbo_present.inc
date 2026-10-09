/* ------------------------------------------------------------------ FBO + post-processing
 * The game renders into g_fbo, whose color and depth are textures (so shaders can read them).
 *  - AO: when the game switches from the 3D world to the 2D HUD (its first glOrtho after a
 *    perspective projection), an ambient-occlusion pass darkens the color texture using the
 *    depth texture. The HUD is drawn afterwards, so it is never shaded.
 *  - Present: the color texture is drawn to the window, letterboxed, through a shader that does
 *    FXAA and sharpening (or a plain blit when both are off).
 * Every pass saves and restores all GL state, so the game's fixed-function rendering never
 * sees a change. */

#define GL_DEPTH_STENCIL_          0x84F9
#define GL_UNSIGNED_INT_24_8_      0x84FA
#define GL_TEXTURE0_               0x84C0
#define GL_FRAGMENT_SHADER_        0x8B30
#define GL_VERTEX_SHADER_          0x8B31
#define GL_COMPILE_STATUS_         0x8B81
#define GL_LINK_STATUS_            0x8B82
#define GL_CURRENT_PROGRAM_        0x8B8D
#define GL_CLAMP_TO_EDGE_          0x812F
#define GL_R8_                     0x8229
#define GL_RED_                    0x1903

typedef void (WINAPI *PFN_glGenTextures)(GLsizei, GLuint *);
typedef void (WINAPI *PFN_glDeleteTextures)(GLsizei, const GLuint *);
typedef void (WINAPI *PFN_glBindTexture)(GLenum, GLuint);
typedef void (WINAPI *PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *);
typedef void (WINAPI *PFN_glTexParameteri)(GLenum, GLenum, GLint);
typedef void (WINAPI *PFN_glEnable)(GLenum);
typedef void (WINAPI *PFN_glDepthMask)(GLboolean);
typedef void (WINAPI *PFN_glBlendFunc)(GLenum, GLenum);
typedef void (WINAPI *PFN_glPolygonMode)(GLenum, GLenum);
typedef void (WINAPI *PFN_glBegin)(GLenum);
typedef void (WINAPI *PFN_glEnd)(void);
typedef void (WINAPI *PFN_glVertex2f)(GLfloat, GLfloat);
typedef void (WINAPI *PFN_glTexCoord2f)(GLfloat, GLfloat);
typedef void (WINAPI *PFN_glPushClientAttrib)(GLbitfield);
typedef void (WINAPI *PFN_glPopClientAttrib)(void);
typedef void (WINAPI *PFN_glMatrixMode)(GLenum);
typedef void (WINAPI *PFN_glLoadIdentity)(void);
typedef void (WINAPI *PFN_glOrtho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
typedef void (WINAPI *PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);

static void (WINAPI *pFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static GLuint (WINAPI *pCreateShader)(GLenum);
static void (WINAPI *pShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
static void (WINAPI *pCompileShader)(GLuint);
static void (WINAPI *pGetShaderiv)(GLuint, GLenum, GLint *);
static void (WINAPI *pGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
static GLuint (WINAPI *pCreateProgram)(void);
static void (WINAPI *pAttachShader)(GLuint, GLuint);
static void (WINAPI *pLinkProgram)(GLuint);
static void (WINAPI *pGetProgramiv)(GLuint, GLenum, GLint *);
static void (WINAPI *pGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, char *);
static void (WINAPI *pUseProgram)(GLuint);
static GLint (WINAPI *pGetUniformLocation)(GLuint, const char *);
static void (WINAPI *pUniform1i)(GLint, GLint);
static void (WINAPI *pUniform1f)(GLint, GLfloat);
static void (WINAPI *pUniform2f)(GLint, GLfloat, GLfloat);
static void (WINAPI *pUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
static void (WINAPI *pActiveTexture)(GLenum);

static GLuint g_color_tex, g_depth_tex;           /* game's render targets */
static GLuint g_colonly_fbo;                      /* color texture only (AO multiply target) */
static GLuint g_ao_tex, g_ao_fbo;                 /* raw AO */
static GLuint g_tmp_tex, g_tmp_fbo;               /* copy of the scene for FXAA/sharpen */
static GLuint g_prog_ao, g_prog_aoapply, g_prog_present;
static int g_shaders_ok = -1;                     /* -1 not tried, 0 failed, 1 ok */

/* per-frame projection tracking (for AO) */
static GLenum g_matrix_mode = GL_MODELVIEW;
static int g_persp_seen, g_ao_done;
static float g_proj[4];                           /* P[0], P[5], near, far */
static LONG g_ao_at_ortho, g_ao_at_present;

static int load_gl3(void)
{
    PFN_wglGetProcAddress gpa = REAL(wglGetProcAddress);
#define L(v, n) do { *(PROC *)&v = gpa(n); if (!v) { lg(0, "ERROR: driver lacks %s; scaling disabled", n); return 0; } } while (0)
    L(pGenFramebuffers, "glGenFramebuffers"); L(pDeleteFramebuffers, "glDeleteFramebuffers");
    L(pBindFramebuffer, "glBindFramebuffer"); L(pGenRenderbuffers, "glGenRenderbuffers");
    L(pDeleteRenderbuffers, "glDeleteRenderbuffers"); L(pBindRenderbuffer, "glBindRenderbuffer");
    L(pRenderbufferStorage, "glRenderbufferStorage"); L(pFramebufferRenderbuffer, "glFramebufferRenderbuffer");
    L(pCheckFramebufferStatus, "glCheckFramebufferStatus"); L(pBlitFramebuffer, "glBlitFramebuffer");
    L(pFramebufferTexture2D, "glFramebufferTexture2D");
#undef L
    /* shader entry points are optional: without them we fall back to a plain blit */
#define O(v, n) (*(PROC *)&v = gpa(n))
    int ok = O(pCreateShader, "glCreateShader") && O(pShaderSource, "glShaderSource") &&
             O(pCompileShader, "glCompileShader") && O(pGetShaderiv, "glGetShaderiv") &&
             O(pGetShaderInfoLog, "glGetShaderInfoLog") && O(pCreateProgram, "glCreateProgram") &&
             O(pAttachShader, "glAttachShader") && O(pLinkProgram, "glLinkProgram") &&
             O(pGetProgramiv, "glGetProgramiv") && O(pGetProgramInfoLog, "glGetProgramInfoLog") &&
             O(pUseProgram, "glUseProgram") && O(pGetUniformLocation, "glGetUniformLocation") &&
             O(pUniform1i, "glUniform1i") && O(pUniform1f, "glUniform1f") && O(pUniform2f, "glUniform2f") &&
             O(pUniform4f, "glUniform4f") && O(pActiveTexture, "glActiveTexture");
#undef O
    if (!ok) { lg(0, "shaders unavailable: post-processing disabled"); g_shaders_ok = 0; }
    return 1;
}

/* ---- shaders */
static const char *VS =
    "#version 120\n"
    "void main() { gl_Position = gl_Vertex; gl_TexCoord[0] = gl_MultiTexCoord0; }\n";

/* Ambient occlusion from depth only. Positions are rebuilt in view space from the captured
 * projection (P0, P5, near, far); normals come from screen-space derivatives. */
static const char *FS_AO =
    "#version 120\n"
    "uniform sampler2D uDepth; uniform vec2 uTexel; uniform vec4 uProj;\n"
    "uniform float uRadius, uStrength, uMaxDist;\n"
    "float lin(float d) { return uProj.z * uProj.w / (uProj.w - d * (uProj.w - uProj.z)); }\n"
    "vec3 pos(vec2 uv) { float z = lin(texture2D(uDepth, uv).r); return vec3((uv * 2.0 - 1.0) * z / uProj.xy, -z); }\n"
    "void main() {\n"
    "  vec2 uv = gl_TexCoord[0].xy; float d = texture2D(uDepth, uv).r;\n"
    "  if (d >= 0.99999) { gl_FragColor = vec4(1.0); return; }\n"
    "  vec3 p = pos(uv); float z = -p.z;\n"
    "  if (z > uMaxDist) { gl_FragColor = vec4(1.0); return; }\n"
    "  vec3 n = normalize(cross(dFdx(p), dFdy(p))); if (dot(n, p) > 0.0) n = -n;\n"
    "  float rnd = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;\n"
    "  float rpx = clamp(uRadius * uProj.x / z * 0.5 / uTexel.x, 3.0, 80.0);\n"
    "  float occ = 0.0;\n"
    "  for (int i = 0; i < 16; i++) {\n"
    "    float a = rnd + float(i) * 2.39996; float r = (float(i) + 0.5) / 16.0;\n"
    "    vec3 q = pos(uv + vec2(cos(a), sin(a)) * r * rpx * uTexel) - p;\n"
    "    float l = length(q);\n"
    "    occ += max(0.0, dot(n, q / (l + 1e-4)) - 0.15) * (1.0 - smoothstep(0.0, uRadius * 1.5, l));\n"
    "  }\n"
    "  float fade = 1.0 - smoothstep(uMaxDist * 0.7, uMaxDist, z);\n"
    "  gl_FragColor = vec4(1.0 - clamp(uStrength * occ / 16.0 * fade, 0.0, 0.85));\n"
    "}\n";

/* Depth-aware 4x4 blur of the AO, output multiplied into the scene by blending. */
static const char *FS_AOAPPLY =
    "#version 120\n"
    "uniform sampler2D uAO; uniform sampler2D uDepth; uniform vec2 uTexel; uniform vec4 uProj;\n"
    "float lin(float d) { return uProj.z * uProj.w / (uProj.w - d * (uProj.w - uProj.z)); }\n"
    "void main() {\n"
    "  vec2 uv = gl_TexCoord[0].xy; float z0 = lin(texture2D(uDepth, uv).r);\n"
    "  float s = 0.0, w = 0.0;\n"
    "  for (int x = -2; x <= 1; x++) for (int y = -2; y <= 1; y++) {\n"
    "    vec2 o = (vec2(x, y) + 0.5) * uTexel;\n"
    "    float z = lin(texture2D(uDepth, uv + o).r);\n"
    "    float k = 1.0 / (1e-3 + abs(z - z0) / max(z0, 1e-3) * 50.0);\n"
    "    s += texture2D(uAO, uv + o).r * k; w += k;\n"
    "  }\n"
    "  gl_FragColor = vec4(vec3(s / w), 1.0);\n"
    "}\n";

/* Present: optional FXAA (in source texels) then contrast-adaptive sharpening. */
static const char *FS_PRESENT =
    "#version 120\n"
    "uniform sampler2D uColor; uniform vec2 uTexel; uniform float uFxaa, uSharpen;\n"
    "float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
    "vec3 fxaa(vec2 uv) {\n"
    "  vec3 nw = texture2D(uColor, uv + vec2(-1.0, -1.0) * uTexel).rgb, ne = texture2D(uColor, uv + vec2(1.0, -1.0) * uTexel).rgb;\n"
    "  vec3 sw = texture2D(uColor, uv + vec2(-1.0, 1.0) * uTexel).rgb, se = texture2D(uColor, uv + vec2(1.0, 1.0) * uTexel).rgb;\n"
    "  vec3 m = texture2D(uColor, uv).rgb;\n"
    "  float lNW = luma(nw), lNE = luma(ne), lSW = luma(sw), lSE = luma(se), lM = luma(m);\n"
    "  float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE))), lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));\n"
    "  vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));\n"
    "  float red = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128.0);\n"
    "  dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + red), -8.0, 8.0) * uTexel;\n"
    "  vec3 a = 0.5 * (texture2D(uColor, uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture2D(uColor, uv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
    "  vec3 b = a * 0.5 + 0.25 * (texture2D(uColor, uv - dir * 0.5).rgb + texture2D(uColor, uv + dir * 0.5).rgb);\n"
    "  float lB = luma(b); return (lB < lMin || lB > lMax) ? a : b;\n"
    "}\n"
    "void main() {\n"
    "  vec2 uv = gl_TexCoord[0].xy;\n"
    "  vec3 c = uFxaa > 0.5 ? fxaa(uv) : texture2D(uColor, uv).rgb;\n"
    "  if (uSharpen > 0.0) {\n"
    "    vec3 n = texture2D(uColor, uv + vec2(0.0, -uTexel.y)).rgb, s = texture2D(uColor, uv + vec2(0.0, uTexel.y)).rgb;\n"
    "    vec3 e = texture2D(uColor, uv + vec2(uTexel.x, 0.0)).rgb, w = texture2D(uColor, uv + vec2(-uTexel.x, 0.0)).rgb;\n"
    "    vec3 mn = min(c, min(min(n, s), min(e, w))), mx = max(c, max(max(n, s), max(e, w)));\n"
    "    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, 1e-4), 0.0, 1.0));\n"
    "    vec3 wgt = -amp * mix(0.125, 0.2, uSharpen);\n"
    "    c = clamp((c + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt), 0.0, 1.0);\n"
    "  }\n"
    "  gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src, const char *name)
{
    GLuint sh = pCreateShader(type); GLint ok = 0; char log[1024];
    pShaderSource(sh, 1, &src, NULL); pCompileShader(sh); pGetShaderiv(sh, GL_COMPILE_STATUS_, &ok);
    if (!ok) { pGetShaderInfoLog(sh, sizeof log, NULL, log); lg(0, "shader %s failed to compile: %s", name, log); return 0; }
    return sh;
}

static GLuint link(const char *fs, const char *name)
{
    GLuint v = compile(GL_VERTEX_SHADER_, VS, "vertex"), f = compile(GL_FRAGMENT_SHADER_, fs, name), p;
    GLint ok = 0; char log[1024];
    if (!v || !f) return 0;
    p = pCreateProgram(); pAttachShader(p, v); pAttachShader(p, f); pLinkProgram(p);
    pGetProgramiv(p, GL_LINK_STATUS_, &ok);
    if (!ok) { pGetProgramInfoLog(p, sizeof log, NULL, log); lg(0, "shader %s failed to link: %s", name, log); return 0; }
    return p;
}

static int shaders_ready(void)
{
    if (g_shaders_ok >= 0) return g_shaders_ok;
    g_prog_ao = link(FS_AO, "ao"); g_prog_aoapply = link(FS_AOAPPLY, "ao-apply"); g_prog_present = link(FS_PRESENT, "present");
    g_shaders_ok = g_prog_ao && g_prog_aoapply && g_prog_present;
    lg(1, "post-processing shaders %s", g_shaders_ok ? "ready" : "FAILED: falling back to a plain blit");
    return g_shaders_ok;
}

/* ---- state save/restore around our passes */
static GLint g_saved_prog;
static void pass_begin(void)
{
    REAL(glPushAttrib)(GL_ALL_ATTRIB_BITS);
    REAL(glPushClientAttrib)(0xFFFFFFFF);
    REAL(glGetIntegerv)(GL_CURRENT_PROGRAM_, &g_saved_prog);
    REAL(glDisable)(GL_DEPTH_TEST); REAL(glDisable)(GL_CULL_FACE); REAL(glDisable)(GL_ALPHA_TEST);
    REAL(glDisable)(GL_SCISSOR_TEST); REAL(glDisable)(GL_STENCIL_TEST); REAL(glDisable)(GL_FOG);
    REAL(glDisable)(GL_LIGHTING); REAL(glDisable)(GL_BLEND); REAL(glDisable)(GL_DITHER);
    REAL(glDepthMask)(GL_FALSE); REAL(glColorMask)(1, 1, 1, 1); REAL(glPolygonMode)(GL_FRONT_AND_BACK, GL_FILL);
}
static void pass_end(void)
{
    pUseProgram(g_saved_prog);
    REAL(glPopClientAttrib)();
    REAL(glPopAttrib)();
}
static void bind_tex(int unit, GLuint t)
{
    pActiveTexture(GL_TEXTURE0_ + unit); REAL(glBindTexture)(GL_TEXTURE_2D, t);
}
/* full-screen quad; the vertex shader ignores the game's matrices */
static void quad(void)
{
    REAL(glBegin)(GL_QUADS);
    REAL(glTexCoord2f)(0, 0); REAL(glVertex2f)(-1, -1);
    REAL(glTexCoord2f)(1, 0); REAL(glVertex2f)(1, -1);
    REAL(glTexCoord2f)(1, 1); REAL(glVertex2f)(1, 1);
    REAL(glTexCoord2f)(0, 1); REAL(glVertex2f)(-1, 1);
    REAL(glEnd)();
}

/* ---- render targets */
static GLuint make_tex(GLint ifmt, GLenum fmt, GLenum type, int w, int h, int linear)
{
    GLuint t;
    REAL(glGenTextures)(1, &t); REAL(glBindTexture)(GL_TEXTURE_2D, t);
    REAL(glTexImage2D)(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, NULL);
    REAL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    REAL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    REAL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_);
    REAL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_);
    return t;
}

static void destroy_fbo(void)
{
    if (!g_fbo) return;
    pBindFramebuffer(GL_FRAMEBUFFER, 0);
    GLuint f[4] = { g_fbo, g_colonly_fbo, g_ao_fbo, g_tmp_fbo }, t[4] = { g_color_tex, g_depth_tex, g_ao_tex, g_tmp_tex };
    pDeleteFramebuffers(4, f); REAL(glDeleteTextures)(4, t);
    g_fbo = g_colonly_fbo = g_ao_fbo = g_color_tex = g_depth_tex = g_ao_tex = g_tmp_fbo = g_tmp_tex = 0;
    g_fbo_ready = 0; g_scale = 1;
}

/* The game destroys its GL context whenever it loses focus and makes a new one when it comes
 * back. Our objects die with the old context, and their IDs mean nothing (or worse, the game's
 * own textures) in the new one, so never delete them by ID across contexts: just forget them. */
static HGLRC g_res_ctx;
static void forget_gl_resources(const char *why)
{
    if (g_fbo || g_prog_present || g_shaders_ok > 0) lg(1, "gl: forgetting our objects (%s)", why);
    g_fbo = g_colonly_fbo = g_ao_fbo = g_color_tex = g_depth_tex = g_ao_tex = 0;
    g_tmp_fbo = g_tmp_tex = 0;
    g_fbo_ready = 0; g_scale = 1;
    g_prog_ao = g_prog_aoapply = g_prog_present = 0;
    g_shaders_ok = -1;
    pBindFramebuffer = NULL;            /* re-fetch entry points for the new context */
    g_persp_seen = g_ao_done = 0;
}

static void build_fbo(void)
{
    int w = g_vactive ? g_vw : 0, h = g_vactive ? g_vh : 0, sc = cfg.render_scale < 1 ? 1 : cfg.render_scale;
    g_fbo_dirty = 0;
    if (w <= 0 || h <= 0) { destroy_fbo(); lg(1, "fbo: no virtual mode yet, drawing straight to the window"); return; }
    if (g_fbo && g_fbw == w * sc && g_fbh == h * sc) return;
    destroy_fbo();
    g_fbw = w * sc; g_fbh = h * sc;
    REAL(glPushAttrib)(GL_TEXTURE_BIT);   /* creating textures changes the game's binding */
    g_color_tex = make_tex(GL_RGBA8_, GL_RGBA, GL_UNSIGNED_BYTE, g_fbw, g_fbh, cfg.filter_linear);
    g_depth_tex = make_tex(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL_, GL_UNSIGNED_INT_24_8_, g_fbw, g_fbh, 0);
    g_ao_tex = make_tex(GL_R8_, GL_RED_, GL_UNSIGNED_BYTE, g_fbw, g_fbh, 0);
    g_tmp_tex = make_tex(GL_RGBA8_, GL_RGBA, GL_UNSIGNED_BYTE, g_fbw, g_fbh, 1);
    REAL(glPopAttrib)();
    pGenFramebuffers(1, &g_tmp_fbo); pBindFramebuffer(GL_FRAMEBUFFER, g_tmp_fbo);
    pFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_tmp_tex, 0);
    pGenFramebuffers(1, &g_ao_fbo); pBindFramebuffer(GL_FRAMEBUFFER, g_ao_fbo);
    pFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_ao_tex, 0);
    pGenFramebuffers(1, &g_colonly_fbo); pBindFramebuffer(GL_FRAMEBUFFER, g_colonly_fbo);
    pFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color_tex, 0);
    pGenFramebuffers(1, &g_fbo); pBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    pFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color_tex, 0);
    pFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g_depth_tex, 0);
    GLenum st = pCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        lg(0, "ERROR: fbo %dx%d incomplete (0x%x); drawing straight to the window", g_fbw, g_fbh, st);
        destroy_fbo(); return;
    }
    REAL(glDrawBuffer)(GL_COLOR_ATTACHMENT0);
    REAL(glReadBuffer)(GL_COLOR_ATTACHMENT0);
    g_fbo_ready = 1; g_scale = sc;
    lg(1, "fbo: game renders %dx%d (virtual %dx%d x%d) -> scaled onto %dx%d monitor",
       g_fbw, g_fbh, w, h, sc, g_rw, g_rh);
}

static int is_window_buffer(GLenum m)
{
    return m == GL_FRONT || m == GL_BACK || m == GL_FRONT_LEFT || m == GL_BACK_LEFT ||
           m == GL_LEFT || m == GL_FRONT_AND_BACK;
}

/* ---- scene effects (run between the 3D world and the HUD, so menus, HUD and text are
 * never touched): ambient occlusion, then FXAA and sharpening */
static void apply_scene_fx(const char *where)
{
    int ao = cfg.ao, aa = cfg.fxaa || cfg.sharpen > 0;
    if ((!ao && !aa) || !g_fbo_ready || !g_persp_seen || g_ao_done || !shaders_ready()) return;
    g_ao_done = 1;
    pass_begin();
    REAL(glViewport)(0, 0, g_fbw, g_fbh);
    if (ao) {
        /* 1: raw AO from depth (depth texture is read, not attached to the target) */
        pBindFramebuffer(GL_FRAMEBUFFER, g_ao_fbo);
        REAL(glDrawBuffer)(GL_COLOR_ATTACHMENT0);
        pUseProgram(g_prog_ao);
        bind_tex(0, g_depth_tex);
        pUniform1i(pGetUniformLocation(g_prog_ao, "uDepth"), 0);
        pUniform2f(pGetUniformLocation(g_prog_ao, "uTexel"), 1.0f / g_fbw, 1.0f / g_fbh);
        pUniform4f(pGetUniformLocation(g_prog_ao, "uProj"), g_proj[0], g_proj[1], g_proj[2], g_proj[3]);
        pUniform1f(pGetUniformLocation(g_prog_ao, "uRadius"), cfg.ao_radius);
        pUniform1f(pGetUniformLocation(g_prog_ao, "uStrength"), cfg.ao_strength);
        pUniform1f(pGetUniformLocation(g_prog_ao, "uMaxDist"), cfg.ao_max_dist);
        quad();
        /* 2: blur and multiply into the scene color (target holds only the color texture) */
        pBindFramebuffer(GL_FRAMEBUFFER, g_colonly_fbo);
        REAL(glDrawBuffer)(GL_COLOR_ATTACHMENT0);
        REAL(glEnable)(GL_BLEND); REAL(glBlendFunc)(GL_ZERO, GL_SRC_COLOR); REAL(glColorMask)(1, 1, 1, 0);
        pUseProgram(g_prog_aoapply);
        bind_tex(0, g_ao_tex); bind_tex(1, g_depth_tex);
        pUniform1i(pGetUniformLocation(g_prog_aoapply, "uAO"), 0);
        pUniform1i(pGetUniformLocation(g_prog_aoapply, "uDepth"), 1);
        pUniform2f(pGetUniformLocation(g_prog_aoapply, "uTexel"), 1.0f / g_fbw, 1.0f / g_fbh);
        pUniform4f(pGetUniformLocation(g_prog_aoapply, "uProj"), g_proj[0], g_proj[1], g_proj[2], g_proj[3]);
        quad();
        REAL(glDisable)(GL_BLEND); REAL(glColorMask)(1, 1, 1, 1);
    }
    if (aa) {
        /* copy the scene aside, then filter it back into the scene color */
        pBindFramebuffer(GL_READ_FRAMEBUFFER, g_colonly_fbo);
        pBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_tmp_fbo);
        REAL(glReadBuffer)(GL_COLOR_ATTACHMENT0); REAL(glDrawBuffer)(GL_COLOR_ATTACHMENT0);
        pBlitFramebuffer(0, 0, g_fbw, g_fbh, 0, 0, g_fbw, g_fbh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        pBindFramebuffer(GL_FRAMEBUFFER, g_colonly_fbo);
        REAL(glDrawBuffer)(GL_COLOR_ATTACHMENT0);
        REAL(glColorMask)(1, 1, 1, 0);
        pUseProgram(g_prog_present);
        bind_tex(0, g_tmp_tex);
        pUniform1i(pGetUniformLocation(g_prog_present, "uColor"), 0);
        pUniform2f(pGetUniformLocation(g_prog_present, "uTexel"), 1.0f / g_fbw, 1.0f / g_fbh);
        pUniform1f(pGetUniformLocation(g_prog_present, "uFxaa"), (float)cfg.fxaa);
        pUniform1f(pGetUniformLocation(g_prog_present, "uSharpen"), cfg.sharpen);
        quad();
    }
    pBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    pass_end();
    REAL(glDrawBuffer)(GL_COLOR_ATTACHMENT0);
    REAL(glReadBuffer)(GL_COLOR_ATTACHMENT0);
    if (where[0] == 'o') { if (InterlockedIncrement(&g_ao_at_ortho) == 1) lg(1, "scene effects: applied before the HUD (at glOrtho), near %.2f far %.0f", g_proj[2], g_proj[3]); }
    else if (InterlockedIncrement(&g_ao_at_present) == 1)
        lg(1, "scene effects: no HUD switch seen this frame; applied at present (HUD may be affected too)");
}

/* ------------------------------------------------------------------ present */
static BOOL present(HDC dc)
{
    LARGE_INTEGER a, b;
    BOOL r;
    g_in_swap = 1; g_last_hook = "SwapBuffers";
    QueryPerformanceCounter(&a);
    if (g_fbo_ready) {
        double s, ox, oy; int cw, ch;
        apply_scene_fx("present");
        letterbox(&s, &ox, &oy, &cw, &ch);
        /* window y is bottom-up in GL */
        int x0 = (int)(ox + 0.5), x1 = (int)(ox + g_vw * s + 0.5);
        int y0 = (int)(oy + 0.5), y1 = (int)(oy + g_vh * s + 0.5);
        int shaded = 0;   /* FXAA/sharpen now run on the 3D scene only (apply_scene_fx) */
        pass_begin();
        pBindFramebuffer(GL_FRAMEBUFFER, 0);
        REAL(glDrawBuffer)(GL_BACK);
        REAL(glClearColor)(0, 0, 0, 1);
        REAL(glClear)(GL_COLOR_BUFFER_BIT);
        if (shaded) {
            REAL(glViewport)(x0, ch - y1, x1 - x0, y1 - y0);
            pUseProgram(g_prog_present);
            bind_tex(0, g_color_tex);
            pUniform1i(pGetUniformLocation(g_prog_present, "uColor"), 0);
            pUniform2f(pGetUniformLocation(g_prog_present, "uTexel"), 1.0f / g_fbw, 1.0f / g_fbh);
            pUniform1f(pGetUniformLocation(g_prog_present, "uFxaa"), (float)cfg.fxaa);
            pUniform1f(pGetUniformLocation(g_prog_present, "uSharpen"), cfg.sharpen);
            quad();
        } else {
            pBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
            REAL(glReadBuffer)(GL_COLOR_ATTACHMENT0);
            pBlitFramebuffer(0, 0, g_fbw, g_fbh, x0, ch - y1, x1, ch - y0, GL_COLOR_BUFFER_BIT,
                             cfg.filter_linear ? GL_LINEAR : GL_NEAREST);
        }
        r = REAL(wglSwapBuffers)(dc);
        pBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        pass_end();
        REAL(glDrawBuffer)(GL_COLOR_ATTACHMENT0);
        REAL(glReadBuffer)(GL_COLOR_ATTACHMENT0);
    } else {
        r = REAL(wglSwapBuffers)(dc);
    }
    g_persp_seen = 0; g_ao_done = 0;
    QueryPerformanceCounter(&b);
    g_in_swap = 0;

    if (cfg.log_level >= 2) {
        GLenum e; int n = 0;
        while ((e = REAL(glGetError)()) != GL_NO_ERROR && n++ < 4) LG_FIRST(50, 2, "GL error 0x%x (after frame %ld)", e, g_frames);
    }

    /* timing */
    double swap_ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / g_qpf.QuadPart;
    double frame_ms = g_last_frame_qpc ? (double)(b.QuadPart - g_last_frame_qpc) * 1000.0 / g_qpf.QuadPart : 0;
    if (frame_ms > cfg.stall_ms)
        lg(1, "SLOW FRAME #%ld: %.0f ms since previous frame (swap itself %.1f ms)", g_frames, frame_ms, swap_ms);
    if (frame_ms > st_max_frame) st_max_frame = frame_ms;
    if (swap_ms > st_max_swap) st_max_swap = swap_ms;
    st_sum_frame += frame_ms; st_frames++;
    if (!st_start) st_start = b.QuadPart;
    if (cfg.stats_sec > 0 && (double)(b.QuadPart - st_start) / g_qpf.QuadPart >= cfg.stats_sec) {
        lg(1, "stats: %.1f fps, avg frame %.1f ms, worst frame %.1f ms, worst swap %.1f ms",
           st_frames / ((double)(b.QuadPart - st_start) / g_qpf.QuadPart), st_sum_frame / st_frames, st_max_frame, st_max_swap);
        st_start = b.QuadPart; st_frames = 0; st_sum_frame = st_max_frame = st_max_swap = 0;
    }
    g_last_frame_qpc = b.QuadPart;
    InterlockedIncrement(&g_frames);

    if (g_fbo_dirty && pBindFramebuffer) build_fbo();
    return r;
}

/* ---- matrix hooks: capture the 3D projection, run AO when the HUD (ortho) starts */
void WINAPI hk_glMatrixMode(GLenum m) { g_matrix_mode = m; REAL(glMatrixMode)(m); }

void WINAPI hk_glLoadIdentity(void)
{
    if (g_matrix_mode == GL_PROJECTION && (cfg.ao || cfg.fxaa || cfg.sharpen > 0) && g_fbo_ready) {
        GLfloat m[16];
        REAL(glGetFloatv)(GL_PROJECTION_MATRIX, m);
        /* a perspective projection has m[11] == -1 and m[15] == 0 */
        if (m[11] < -0.999f && m[11] > -1.001f && m[15] == 0.0f && m[10] != 1.0f && m[10] != -1.0f) {
            g_proj[0] = m[0]; g_proj[1] = m[5];
            g_proj[2] = m[14] / (m[10] - 1.0f);           /* near */
            g_proj[3] = m[14] / (m[10] + 1.0f);           /* far */
            g_persp_seen = 1;
        }
    }
    REAL(glLoadIdentity)();
}

void WINAPI hk_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    if (g_persp_seen && !g_ao_done) apply_scene_fx("ortho");
    REAL(glOrtho)(l, r, b, t, n, f);
}
