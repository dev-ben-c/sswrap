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
- **Debugging built in.** `sswrap.log` records display/window/context events, fps summaries, and
  slow frames. A watchdog thread notices when frames stop, briefly suspends the render thread,
  and logs where it is stuck as `module!function+offset`. That is how the DNS freeze was found.

## Install

1. Build `opengl32.dll` (below) or download it from the releases page.
2. Copy `opengl32.dll` and `sswrap.ini` into the Starsiege folder (next to `Starsiege.exe`).
3. **Wine only:** make Wine use it for Starsiege:
   ```sh
   wine reg add 'HKCU\Software\Wine\AppDefaults\Starsiege.exe\DllOverrides' /v opengl32 /d native,builtin /f
   ```
4. In the game, pick the **OpenGL** renderer, fullscreen, and any resolution you like.

`contrib/linux/starsiege.sh` is an example launcher that also keeps the game on OpenGL/fullscreen
(the game sometimes saves itself into windowed Software mode) and pins it to one CPU core, which
removes stutter in this 1999 engine.

## Settings (`sswrap.ini`)

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | 1 | 0 = pure pass-through (logging and watchdog still work), for comparisons |
| `RenderScale` | 1 | Render at N x the requested size (experimental supersampling) |
| `LinearFilter` | 1 | Smooth (1) or sharp pixel (0) scaling |
| `AO`, `AORadius`, `AOStrength`, `AOMaxDistance` | 1, 3.0, 1.5, 400 | Ambient occlusion and its reach, darkness, fade distance |
| `FXAA` | 1 | Edge smoothing |
| `Sharpen` | 0.4 | Sharpening amount, 0 = off |
| `ExtraModes` | | Extra resolutions for the menu, e.g. `1720x720,2880x1200` |
| `BlockHosts` | `dynamix.com` | Hostnames whose DNS lookups fail instantly |
| `LogLevel` | 1 | 0 errors/stalls only, 1 normal, 2 verbose, 3 everything |
| `StallMs` | 200 | Frame gap that counts as a stall (0 disables the watchdog) |
| `StatsSeconds` | 10 | fps/worst-frame summary interval (0 = off) |

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

or natively with `i686-w64-mingw32-gcc` (see the two commands in `build-in-docker.sh`).

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
