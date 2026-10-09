# sswrap: an OpenGL wrapper for Starsiege (1999)

`sswrap` is a drop-in `opengl32.dll` for **Starsiege**, Dynamix's 1999 HERC combat sim. It makes
the game's OpenGL renderer usable on modern systems, especially Linux/Wine on high-resolution
and ultrawide monitors, and it logs enough to find out *why* the game misbehaves when it does.

Status: **alpha**. Developed and tested on Linux (Wine 11, KDE Plasma Wayland, NVIDIA RTX 4080
SUPER, 3440x1440). It should also work on Windows, but that is untested.

## What it does

- **Any resolution, scaled to your screen.** The game's display-mode changes become virtual: your
  monitor never switches mode. The game renders off-screen at the size it asked for, and every
  frame is scaled onto the whole monitor with the aspect ratio kept (black bars). Mouse
  coordinates, the cursor, and window sizes are translated so the game never notices.
- **A full resolution menu.** The game's options list common 4:3, 16:9, 16:10, 21:9 and 32:9
  sizes plus your native size, and you can add your own. Tip for ultrawides: half your native size
  (e.g. 1720x720 on 3440x1440) fills the screen and keeps the HUD readable.
- **Fixes the multi-second freezes.** Starsiege looks up the long-dead Dynamix master and IRC
  servers on its render thread; each failed DNS lookup froze the game for ~2 s, repeatedly when
  hosting multiplayer. Those lookups now fail instantly.
- **Visual effects** (each switchable), applied to the 3D world only, *before* the HUD is drawn, so
  menus, HUD and text stay crisp: screen-space ambient occlusion, FXAA, and sharpening.
- **HD texture packs.** Dump every texture the game loads, upscale them (a Real-ESRGAN script
  is included), and sswrap swaps the HD versions in.
- **Debugging built in.** `sswrap.log` records display/window/context events, fps summaries, and
  slow frames. A watchdog thread notices when frames stop, briefly suspends the render thread,
  and logs where it is stuck as `module!function+offset`. That is how the DNS freeze was found.

## Install

### Windows

1. Download the latest `sswrap-*.zip` from the [Releases page](https://github.com/dev-ben-c/sswrap/releases)
   (built from this source by GitHub Actions).
2. Copy `opengl32.dll` and `sswrap.ini` into the Starsiege folder, next to `Starsiege.exe`.
3. Start the game. A settings window appears first: pick a **HUD size**, **render quality**,
   effects and (on 21:9 and wider screens) **Ultrawide HUD**, then click *Start game*.

That's it. Details, uninstalling and notes are in `INSTALL-WINDOWS.txt` inside the zip.

### Linux / Wine

1. Same files as above into the Starsiege folder (or build them yourself, see *Building*).
2. Make Wine use the DLL for Starsiege:
   ```sh
   wine reg add 'HKCU\Software\Wine\AppDefaults\Starsiege.exe\DllOverrides' /v opengl32 /d native,builtin /f
   ```
3. Start the game; the same settings window appears.

`contrib/linux/starsiege.sh` is an example launcher that also pins the game to one CPU core,
which removes stutter in this 1999 engine.

### The settings window

| Option | What it does |
|---|---|
| HUD size | The game lays its HUD out in pixels, so it runs at your monitor's size divided by this; 2x on a 3440x1440 screen means the game runs at 1720x720 with a 2x HUD |
| Render quality | The 3D world renders at your monitor's resolution (Native) or 2x/3x above it and is scaled down, independent of HUD size |
| Exact resolution | Advanced: pick a specific game resolution instead of a HUD size |
| Ambient occlusion, edge smoothing | The visual effects; applied to the 3D world only, never the HUD |
| Ultrawide HUD | Pulls HUD elements anchored to the screen edges in to a 16:9-wide area in the middle (rewrites `hudLayout.prf`, keeping the original as `hudLayout.prf.sswrap-orig`) |

### The HUD: what you can and can't change

**Why HUD size is tied to the game's resolution.** Starsiege draws its HUD from fixed-size pixel art
and bitmap fonts, and it places every HUD element in absolute screen pixels (sswrap traced a full
HUD frame: no per-element positioning, no scaling, just finished screen coordinates). There is no
font-size or HUD-scale setting in the game, and a wrapper cannot tell where one HUD element ends and
the next begins, so it cannot scale them individually. What it *can* do is run the game at a smaller
resolution, which makes the game lay the whole HUD out bigger, while rendering the 3D world at full
sharpness separately. That is what **HUD size** does.

**HUD size and render quality are independent.** HUD size picks the game's resolution (your monitor
divided by the HUD size); render quality picks how sharply the 3D world is drawn. A bigger HUD never
costs sharpness:

| Monitor | HUD size | Game runs at | Render quality | 3D world drawn at |
|---|---|---|---|---|
| 1920x1080 | 1.5x | 1280x720 | Native | 1920x1080 |
| 2560x1440 | 2x | 1280x720 | 2x | 5120x2880 |
| 3440x1440 | 1.5x | 2293x960 | Native | 3440x1440 |
| 3440x1440 | 2x | 1720x720 | 3x | 10320x4320 |

The settings window shows the resulting numbers for whatever you pick. Supersampling is
demanding: 3x on a 3440x1440 screen is about 45 million pixels per frame (around 90 fps on an
RTX 4080 SUPER, versus a locked 165 at 2x).

**Moving HUD elements.** HUD positions are stored in `hudLayout.prf` as fractions of the screen, so
they *can* be moved. **Ultrawide HUD** uses this: on screens wider than 16:9 it pulls the elements
anchored to the left and right edges in to a 16:9-wide area in the middle, so you don't have to
look to the far corners. Heights and centred elements (reticle, compass) are unchanged. Your
original layout is kept as `hudLayout.prf.sswrap-orig`; unticking the option restores it, and if
you rearrange the HUD in the game, that arrangement becomes the new original.

**Known limits.**
- HUD sizes are steps of the game resolution, so text and lines are drawn at the game's pixel size
  and scaled up: they stay clean (they are drawn at the render quality's resolution) but are not
  redrawn with more detail.
- The game may pick slightly simpler models and terrain a little sooner at lower game resolutions
  (level of detail is chosen from the game's resolution, not the render resolution). If distant
  objects look blocky at large HUD sizes, raise the shape and terrain detail sliders in the game's
  video options.
- Menus are fixed 640x480 artwork; they are scaled to the screen height with bars at the sides.

## Settings (`sswrap.ini`)

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | 1 | 0 = pure pass-through (logging and watchdog still work), for comparisons |
| `RenderScale` | 1 | Render at this multiple of the game's resolution, fractions allowed (set by the settings window) |
| `AskOnLaunch` | 1 | Show the settings window at startup |
| `UltrawideHud` | 0 | Keep the HUD within a centred 16:9 area (set by the settings window) |
| `LinearFilter` | 1 | Smooth (1) or sharp pixel (0) scaling |
| `AO`, `AORadius`, `AOStrength`, `AOMaxDistance` | 1, 3.0, 1.5, 400 | Ambient occlusion and its reach, darkness, fade distance |
| `FXAA` | 1 | Edge smoothing |
| `Sharpen` | 0.4 | Sharpening amount, 0 = off |
| `ExtraModes` | | Extra resolutions for the menu, e.g. `1720x720,2880x1200` |
| `BlockHosts` | `dynamix.com` | Hostnames whose DNS lookups fail instantly |
| `LogLevel` | 1 | 0 errors/stalls only, 1 normal, 2 verbose, 3 everything |
| `StallMs` | 200 | Frame gap that counts as a stall (0 disables the watchdog) |
| `StatsSeconds` | 10 | fps/worst-frame summary interval (0 = off) |
| `TextureDump`, `TextureReplace` | 0, 1 | Save every texture to `sswrap_textures/dump/`; load replacements from `sswrap_textures/load/` |
| `FOV`, `DepthScale`, `MinWorldDraws` | 90, 1.0, 50 | How the game draws: used to place the effects before the HUD and to rebuild 3D positions for AO |

## HD textures

sswrap can dump every texture the game loads and swap in replacements, so you can build an HD
texture pack, e.g. with an AI upscaler.

1. Set `TextureDump=1` in `sswrap.ini` and play: every texture the game loads is saved once to
   `sswrap_textures/dump/` (named `<width>x<height>_<fingerprint>.tga`). Play the maps and
   vehicles you care about; textures only load when something is actually drawn.
2. Build the pack with [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN/releases)
   (`realesrgan-ncnn-vulkan`, any Vulkan GPU):
   ```sh
   GAME_DIR=/path/to/Starsiege REALESRGAN=/path/to/realesrgan-ncnn-vulkan tools/hd-textures/build.sh
   ```
   It skips junk (Starsiege packs its HUD/menu bitmaps into texture pages full of uninitialised
   memory, which never fingerprint the same twice), pads each texture so tiling surfaces stay
   seamless while skies and skins don't bleed, upscales 4x, and writes the results to
   `sswrap_textures/load/`. Re-running only processes new textures.
3. Set `TextureDump=0` again. With `TextureReplace=1` (default) any texture with a matching file
   in `load/` (PNG, TGA or JPG, any size) is replaced, with mipmaps generated by the driver.

Terrain, sky, vehicle skins, buildings and effects are covered; HUD and menu art is not.

## CD music without the CD

Starsiege plays its soundtrack as CD audio. To keep the music when running from a disc image or
without the disc, rip the audio tracks to `Music/Track02.ogg` ... and use
[ogg-winmm](https://github.com/ayuanx/ogg-winmm) (a separate project) as `winmm.dll`. Under Wine
also set `winmm` to `native,builtin` for `Starsiege.exe`.

## Building

Needs a 32-bit MinGW-w64 toolchain and Python 3. The easiest way is a throwaway container:

```sh
docker run --rm -v "$PWD":/src -w /src debian:stable-slim sh build-in-docker.sh
```

or natively with `sh build.sh` once `i686-w64-mingw32-gcc` is installed. Pushing a `v*` tag makes
GitHub Actions build and publish a release zip.

`gen.py` generates the pass-through layer from `exports.txt` (the export list of Wine's
`opengl32.dll`): every export becomes a one-instruction jump to the real OpenGL, except the
handful `sswrap.c` intercepts.

## How it works

- `opengl32.dll` loads the real system `opengl32.dll` and forwards all 361 exports to it.
- On load it patches the game's import table for the display, window, cursor, `SwapBuffers`,
  and DNS calls it needs to intercept.
- When the game creates its GL context, sswrap creates an off-screen framebuffer (color + depth
  textures) at the game's requested size and binds it in place of the window.
- Ambient occlusion runs when the game switches from its 3D projection to the 2D HUD (its first
  `glOrtho` after a perspective projection), using the depth texture.
- On `SwapBuffers` the frame is drawn to the real window through the FXAA/sharpen shader,
  letterboxed, then swapped. All GL state is saved and restored around every pass.

## License

MIT, see [LICENSE](LICENSE). Starsiege is a trademark of its respective owners; this project
contains no game code or assets and is not affiliated with Dynamix, Sierra, or their successors.
