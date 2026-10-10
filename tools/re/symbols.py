#!/usr/bin/env python3
"""Write the symbol files in docs/ (Ghidra ImportSymbolsScript.py format: name address f|l).

Each entry: name, kind (f = function, l = label/data), 1.004 address, 1.003 address (None if not
known). See docs/engine-notes.md for what they are.
"""
import os

HUD = [  # class, onRender function 1.004, its vtable entry 1.004, function 1.003, vtable entry 1.003
    ('HudMtrCtrl',        0x5185b4, 0x70269c, 0x516118, 0x6f2414),
    ('HudMtrChatRecieve', 0x529568, 0x703548, 0x5270c8, 0x6f32c0),
    ('HudBitmapCtrl',     0x50fcb4, 0x7013e0, 0x50d81c, 0x6f1158),
    ('HudSimpleText',     0x51188c, 0x701d04, 0x50f3f4, 0x6f1a7c),
    ('HudMtrRadar',       0x51c028, 0x702880, 0x519b8c, 0x6f25f8),
    ('HudMtrShields',     0x51f154, 0x702a5c, 0x51ccb4, 0x6f27d4),
    ('HudMtrAimReticle',  0x51f898, 0x702c3c, 0x51d3f8, 0x6f29b4),
    ('HudMtrWeapDisplay', 0x52469c, 0x702e34, 0x5221fc, 0x6f2bac),
    ('HudMtrDamage',      0x5273c8, 0x702ff4, 0x524f28, 0x6f2d6c),
    ('HudMtrTarget',      0x527fb8, 0x7031a0, 0x525b18, 0x6f2f18),
    ('HudMtrIntDamage',   0x52886c, 0x703364, 0x5263cc, 0x6f30dc),
    ('HudMtrTimer',       0x52ace0, 0x7039dc, 0x528840, 0x6f3754),
    ('HudMtrCommand',     0x52baf4, 0x703d04, 0x529634, 0x6f3a78),
]

SYMBOLS = [
    # 2D drawing
    ('GFX_drawText_p',                 'f', 0x65c030, 0x64c318),
    ('GFX_drawText_r',                 'f', 0x65c2b4, 0x64c59c),
    ('GFX_drawtable_software',         'l', 0x72971c, 0x719124),
    ('GFX_drawtable_glide',            'l', 0x7298dc, 0x7192e4),
    ('GFX_drawtable_opengl',           'l', 0x72a690, 0x71a0a4),
    ('OpenGL_Surface_vtable',          'l', 0x72a520, 0x719f28),
    ('GFXMemSurface_vtable',           'l', 0x72c020, 0x71ba34),
    ('GFXCDSSurface_vtable',           'l', 0x72c190, 0x71bba4),
    ('Glide_Surface_vtable',           'l', 0x7299c4, 0x7193cc),
    ('OpenGL_drawBitmap2d_rf',         'f', 0x6553c8, 0x6457c0),
    ('OpenGL_quad_clip_positions',     'f', 0x651d48, 0x6423c0),
    ('OpenGL_quad_texcoords',          'f', 0x651f78, 0x6425f0),
    ('OpenGL_flush_bitmap_quads',      'f', 0x651bd4, 0x64224c),
    ('OpenGL_texcache_lookup',         'f', 0x64dd10, 0x63ea20),
    ('OpenGL_texcache_download',       'f', 0x64d81c, 0x63e714),
    ('OpenGL_convert_opaque',          'f', 0x64eab8, 0x63f6c4),
    ('OpenGL_convert_transparent',     'f', 0x64f788, 0x6401cc),
    ('OpenGL_convert_translucent',     'f', 0x64f390, 0x63fdd8),
    ('game_glTexParameteri_ptr',       'l', 0x8775cc, 0x8564e8),
    # fonts
    ('GFXFont_vtable',                 'l', 0x736718, 0x72612c),
    ('GFXFont_getCharInfo',            'f', 0x669bc4, 0x659eac),
    # 3D view and field of view
    ('TS_PerspectiveCamera_vtable',    'l', 0x6fc908, 0x6ec680),
    ('TS_PerspectiveCamera_buildProjection', 'f', 0x624ee8, 0x616018),
    ('SimGui_TSView_setupCamera',      'f', 0x5d83e0, 0x5d4b3c),
    ('fov_tan_call_3dview',            'l', 0x5d8b6f, 0x5d52cb),
    ('fov_tan_call_camera_control',    'l', 0x4efea6, 0x4eda0e),
    ('Hud_projectDirectionToScreen',   'f', 0x51fdfc, 0x51d95c),
    ('fov_tan_call_hud_projection',    'l', 0x51fe4f, 0x51d9af),
    ('Vehicle_setTargetFov',           'f', 0x474874, 0x472ca0),
    ('crt_tan',                        'f', 0x6d0d34, 0x6c0cbc),
]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, '..', '..', 'docs')
    for col, ver in ((0, '1.004'), (1, '1.003')):
        lines = []                  # no comment lines: the importer reads every line
        for cls, f4, s4, f3, s3 in HUD:
            fn, slot = (f4, s4) if col == 0 else (f3, s3)
            lines.append(f'SimGui_{cls}_onRender {fn:08x} f')
            lines.append(f'SimGui_{cls}_vtable {slot - 30 * 4:08x} l')
        for name, kind, a4, a3 in SYMBOLS:
            a = a4 if col == 0 else a3
            if a is not None:
                lines.append(f'{name} {a:08x} {kind}')
        with open(os.path.join(out, f'starsiege-{ver}.symbols.txt'), 'w') as f:
            f.write('\n'.join(lines) + '\n')
        print(ver, len(lines), 'symbols')


if __name__ == '__main__':
    main()
