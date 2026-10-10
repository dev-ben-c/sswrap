# Starsiege engine notes

What sswrap's developers learned about how Starsiege (the Darkstar engine, 1999) works inside,
for anyone extending sswrap, memstar or other community patches. These are descriptions and
addresses in our own words, written from reverse-engineering our own copies for
interoperability. No game code is reproduced here. To look at the code itself, load your own
`Starsiege.exe` into a disassembler and import the matching symbol file:

- [`starsiege-1.004.symbols.txt`](starsiege-1.004.symbols.txt): retail 1.004 (`V 001.004r`,
  SHA-256 `a77e83e2…`; the Starsiege Players "patched 1.004" differs only in one string, so the
  same addresses apply)
- [`starsiege-1.003.symbols.txt`](starsiege-1.003.symbols.txt): 1.003, the Starsiege Players
  default executable

Both are in the format of Ghidra's `ImportSymbolsScript.py` (`name address f|l`). The tools
used are in [`tools/re`](../tools/re): `typeinfo.py` finds classes and vtables, and the Ghidra
scripts list callers, decompile functions and dump the HUD classes.

Addresses below are 1.004 / 1.003.

## Finding things in a stripped Borland executable

`Starsiege.exe` was built with Borland C++ and has no symbols, but Borland keeps a **type
descriptor** for every polymorphic class with the class name inside it (`SimGui::HudMtrRadar`,
`TS::PerspectiveCamera`, `GFXFont`, …). The u16 at descriptor+6 is the offset of the name from
the descriptor. Every vtable is preceded by a pointer to its class's descriptor (plus two more
words), so a class name leads to its vtable and from there to its virtual functions.
`tools/re/typeinfo.py` does this.

**Calling convention.** Most engine functions use Borland's register convention: the first three
arguments in EAX, EDX, ECX, the rest on the stack, and the callee pops its stack arguments. A
hook has to save EAX/EDX/ECX and return with the right `ret n`. C runtime functions such as
`tan()` are plain cdecl (argument on the stack, caller pops, result in ST0).

**Moving between builds.** 1.003 and 1.004 share most of their code, shifted. Matching the first
16–32 bytes of a function usually finds it in the other build (`typeinfo.py --find-code`), as
long as the bytes contain no absolute addresses. For call sites, match an instruction pattern
with the call target left as a wildcard.

## Graphics surfaces and the drawing table

Everything 2D (menus, HUD, text) is drawn through a **surface** object. Relevant fields:

| Offset | Meaning |
|---|---|
| +0x04 | pointer to the surface type's drawing-function table |
| +0x08 | flags; bit 0 = clip to the rectangle below |
| +0x0c..+0x18 | clip rectangle (left, top, right, bottom; inclusive) |
| +0x1c..+0x28 | bounds of what was last drawn (updated by the text routine) |
| +0xfc | float alpha for the surface (1.0 = opaque) |
| +0x120 | number of quads batched (OpenGL) |
| +0x158 | OpenGL only: the GL state / texture cache object |

Surface types and their tables (1.004 / 1.003):

| Type | vtable | Drawing table |
|---|---|---|
| `OpenGL::Surface` | `0x72a520` / `0x719f28` | `0x72a690` / `0x71a0a4` |
| `GFXMemSurface` | `0x72c020` / `0x71ba34` | `0x72971c` / `0x719124` (shared with CDS) |
| `GFXCDSSurface` | `0x72c190` / `0x71bba4` | as above |
| `Glide::Surface` | `0x7299c4` / `0x7193cc` | `0x7298dc` / `0x7192e4` |

Drawing table entries used by sswrap (index × 4 bytes):

| Entry | Function | Arguments (register convention) |
|---|---|---|
| 6 | draw bitmap (sub-rectangle) | surface, bitmap, &source rect, flags (1 = flip X, 2 = flip Y), stack: &point; pops 8 |
| 9 | draw text at a point | surface, font, &point, stack: string; pops 4; returns right edge |
| 10 | draw text in a rectangle (aligned) | surface, font, &rect, stack: string; calls entry 9 |

The text routine (entry 9, `0x65c030` / `0x64c318`) is shared by all surface types. It walks the
string, asks the font for each character's glyph (font vtable entry 8), and draws the glyph with
entry 6. **All text in the game, including menus, HUD readouts, chat and briefings, is drawn
through the OpenGL surface's table**, one glyph blit per character. sswrap's smooth text wraps
entries 9 and 6 of that table.

## Fonts (`GFXFont`)

Fonts are `.pft` resources (`console.pft`, `hud_high.pft`, `hud_low.pft`, `mefont.pft`,
`mefonthl.pft`). vtable `0x736718` / `0x72612c`; entry 8 (`0x669bc4` / `0x659eac`) returns a
character's glyph sheet bitmap and source rectangle.

| Offset | Meaning |
|---|---|
| +0x04 | table of glyph sheet bitmaps |
| +0x18 (byte) | bit 1: fixed-width font |
| +0x19 (byte) | bit 1: 16-bit characters; bit 2: characters are remapped first |
| +0x24 | height |
| +0x28 | advance for fixed-width fonts |
| +0x34 | baseline |
| +0x38, +0x3c | horizontal / vertical scale, 16.16 fixed point (0x10000 = 1) |
| +0x40 | extra spacing between characters |
| +0x4c | glyph table, 8 bytes per glyph: sheet index, x, y, width, height, baseline offset (signed), … |
| +0x50 | character → glyph index map (array of i16, -1 = none) |
| +0x54, +0x56 | first character, last index (i16) |

## Bitmaps (`GFXBitmap`) and the OpenGL texture cache

| Offset | Meaning |
|---|---|
| +0x10, +0x14 | width, height |
| +0x18 | row stride in bytes |
| +0x30 | attributes: bit 0 transparent (index 0 is see-through), bit 2 translucent |
| +0x34 | palette id |
| +0x38 | pointers to the pixel rows of each mip level (8-bit palette indices) |
| +0x5c | number of mip levels |

The OpenGL backend converts 8-bit bitmaps to RGBA when it uploads them. Its state object
(surface+0x158) keeps 16 colour tables of 0xc0c bytes each, starting at +0x230 (1.004) / +0x23c
(1.003); each table's palette id is stored 0xc08 bytes in (+0xe38 / +0xe44 for the first). For
transparent bitmaps, colour *i* is 4 bytes RGBA at table +0x400 + 4*i (opaque and translucent
bitmaps are converted by separate routines, `0x64eab8` and `0x64f390` in 1.004, which we have not
examined). Relevant functions: per-glyph blit `0x6553c8` / `0x6457c0`, quad setup
`0x651d48`+`0x651f78` / `0x6423c0`+`0x6425f0`, flush and state setting `0x651bd4` / `0x64224c`, texture download
`0x64d81c` / `0x63e714`, transparent conversion `0x64f788` / `0x6401cc`.

**Texture pages.** Many small bitmaps (HUD panels, menu art, glyph sheets) are packed into
256×256 or 256×128 texture pages and uploaded with `glTexSubImage2D`; HUD panels and their text
are often painted in software into a page and uploaded whole, unused areas included. Pages the
game repaints every frame (menu animations, video) behave like video textures.

**Large pictures** (the splash screen, menu backgrounds) are cut into equal pieces of at most 256
pixels: a 640×480 screen becomes 3 × 2 pieces of 213/213/214 × 240, each in the top-left corner of
its own 256×256 texture, uploaded one after another.

The game sets `GL_NEAREST` on 2D art, which is what keeps those pieces joining cleanly. memstar
(`mem.dll`) rewrites every `GL_NEAREST` to `GL_LINEAR` through the game's OpenGL pointer table
(the `glTexParameteri` entry is at `0x8775cc` / `0x8564e8`), which shows seams between tiles.

## The HUD

HUD elements are `SimGui` controls. Each class draws itself through vtable **entry 30
(onRender)**: register convention, EAX = the control, EDX = surface, ECX = update rectangle,
stack: x, y (screen position); pops 8. A control's size is at +0x1a4 / +0x1a8. The HUD is laid
out in absolute screen pixels and the game passes finished screen coordinates to OpenGL
(identity modelview, pixel-space orthographic projection), so a hook can scale an element by
changing the modelview around its onRender call. The 13 classes sswrap scales are listed in the
symbol files (`HudMtrRadar_onRender` and the others, with their vtable entries).

`hudLayout.prf` (format version 2) stores the HUD layout: u32 version, u32 count (18), then
29-byte records; byte 1 of a record is the anchor and the floats at file offsets 37 + 29*k are
the element's position as fractions of the screen.

## The 3D view and field of view

The game projects its 3D world on the CPU: the GL projection stays orthographic in screen
pixels, the sky is drawn first, then the world with depth testing on (`GL_GEQUAL`, depth roughly
1/distance), then the HUD with depth testing off.

The camera is a `TS::PerspectiveCamera` (vtable `0x6fc908` / `0x6ec680`). Its world viewport (the
view's extent at the near plane) is at +0x144..+0x150, half-width / half-height at +0x154 /
+0x158, centre offset at +0x15c / +0x160, near and far distances at +0x1a4 / +0x1ac; vtable entry
0 (`0x624ee8` / `0x616018`) builds the projection from them.

Three places set the half-width from the field of view as `tan(fov/2) * near` and derive the
half-height from the view's height/width, i.e. **the horizontal field of view is fixed** and wider
screens lose height. Each calls the C runtime `tan()` (`0x6d0d34` / `0x6c0cbc`):

| Where | Call site (1.004 / 1.003) | View size comes from |
|---|---|---|
| 3D view setup (`0x5d83e0` / `0x5d4b3c`) | `0x5d8b6f` / `0x5d52cb` | the view rectangle, on the caller's stack |
| a second camera control | `0x4efea6` / `0x4eda0e` | the control's size (EBX+0x1a4 / +0x1a8) |
| HUD projection of directions to the screen, for the reticle and target markers (`0x51fdfc` / `0x51d95c`) | `0x51fe4f` / `0x51d9af` | the control's size (EBX+0x1a4 / +0x1a8); it derives the vertical angle from this one |

Scaling those three `tan` results by (width/height) / (4/3) gives "Hor+" widescreen, with the HUD
markers staying on target. That is what sswrap's `WidescreenFOV` does (`fov.inc`).

The console command `fov(radians)` sets a target field of view on the controlled vehicle
(`0x474874` / `0x472ca0`: target at +0x43c, flag at +0x440, start time at +0x438); the change is
eased in like the zoom.

## Networking

- Game servers listen on UDP **29001** (`$server::UDPPortNumber` in `serverPrefs.cs`).
- Master servers are `$Inet::Master1..3`, by default `IP:ss1m1.masters.dynamix.com:29000` and
  the same for ss1m2 and ss1m3, set in `Scripts\master.cs` (inside a .vol). The community masters
  are `master1.starsiegeplayers.com` and `master2.starsiegeplayers.com`, port 29000;
  `master2.starsiegeplayers.com:29000` also serves a JSON server list at
  `/api/v1/multiplayer/servers`.
- The game resolves host names with `gethostbyname` on its **render thread**; each failed lookup
  (the old Dynamix masters and `irc.dynamix.com`) froze the game for about two seconds.

## Other behaviour worth knowing

- The game recreates its OpenGL context whenever it loses focus, and its own texture objects with
  it, so anything a wrapper creates must be rebuilt on the new context.
- `$pref::OpenGL::visDistCap` (default 750) appears, by its name, to cap the visible distance in
  OpenGL mode; we have not confirmed what it does.
- `defaultPrefs.cs` holds the video mode (`$pref::GWC::SIM_FS_WIDTH/HEIGHT/DEVICE`,
  `SIM_IS_FULLSCREEN`); the game rewrites it on exit.
