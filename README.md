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
- **Online multiplayer server list works again.** The game's lookups of the old Dynamix master
  servers are answered with the community-run replacements from
  [Starsiege Players](https://starsiegeplayers.com/), so the in-game server browser lists the
  live community servers without editing any game files. Games you host are listed there too
  (other players can only join if UDP port 29001 reaches your PC, e.g. through a port forward).
- **Visual effects** (each switchable), applied to the 3D world only, *before* the HUD is drawn, so
  menus, HUD and text stay crisp: screen-space ambient occlusion, FXAA, and sharpening.
- **A real HUD scale.** Found with Ghidra: Starsiege draws its HUD with C++ GUI controls, so
  sswrap hooks each HUD element's render call in memory and enlarges it around its screen
  corner or edge, at full native resolution (versions 1.004 and 1.003; checked before patching).
- **Sharper HUD and menu art.** The HUD/menu bitmaps the game assembles at runtime are enlarged
  2x or 4x with a pixel-art filter (Scale2x) on their way to the GPU.
- **Proper supersampling.** At render quality above native, every screen pixel averages all the
  pixels rendered under it, so thin lines and small text stay clean.
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
3. Start the game. A settings window appears first: pick a **HUD scale**, **render quality**,
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

### With the Starsiege Players build

The community build from [starsiegeplayers.com](https://starsiegeplayers.com/) works too. Its
default executable is version 1.003, and HUD scale and smooth text support both 1.003 and its
alternative patched 1.004 executable. Keep its `mem.dll` (crash and server fixes; sswrap undoes
only its forced texture smoothing, which shows seams between tiles), but remove its DxWnd files,
`dinput.dll` and `dxwnd.dll`: DxWnd does the same job as sswrap and the two get in each other's
way. In the game's video options, set the 3D hardware type to "Other" (OpenGL).

### The settings window

| Option | What it does |
|---|---|
| Resolution | **Native** (your monitor's resolution) is what you want; a specific resolution can be picked if needed |
| HUD scale | Enlarges the HUD meters (radar, shields, weapons, reticle, target, timer, chat) 1.25x-2x |
| Render quality | The 3D world renders at your monitor's resolution (Native) or 2x/3x above it and is averaged down (supersampling) |
| Sharper HUD and menu art | Runs the HUD, menu and font bitmaps through a 4x pixel-art filter (or HD replacements) before they are enlarged (`UiUpscale=4`) |
| Smooth text | Redraws all game text (menus, briefings, chat, HUD readouts) from smoothed, high-resolution rebuilds of the game's own fonts, keeping the game's layout and colours (1.004 and 1.003; `TextHD`) |
| Ambient occlusion, edge smoothing | The visual effects; applied to the 3D world only, never the HUD |
| Ultrawide HUD | Pulls HUD elements anchored to the screen edges in to a 16:9-wide area in the middle (rewrites `hudLayout.prf`, keeping the original as `hudLayout.prf.sswrap-orig`) |

The window shows the resulting game and render resolutions for whatever you pick. Supersampling
is demanding: 3x on a 3440x1440 screen is about 45 million pixels per frame (around 90-150 fps on
an RTX 4080 SUPER, versus a locked 165 at 2x).

### The HUD

**HUD scale (1.004 and 1.003).** Starsiege lays its HUD out in absolute screen pixels from fixed-size
pixel art, with no size setting of its own. sswrap found the HUD's GUI classes in the game with
Ghidra and wraps each one's render call, scaling it around a shared screen anchor (the corner, edge
centre or screen centre of its region) so neighbouring elements stay aligned while the game runs
at your native resolution. Buttons and dialogs are left alone so clicks still land where they are
drawn. Many HUD panels are painted by the game in software at their original resolution, text
included, so leave **Sharper HUD and menu art** on: the pixel-art filter smooths them before they
are enlarged.

**Other game versions.** HUD scale only activates if the game's code is exactly version 1.004 (the retail CD's final
patch) or 1.003 (the default in the [Starsiege Players](https://starsiegeplayers.com/) build). On
any other build the window instead offers **HUD size**, the older method: the game runs at your
monitor's resolution divided by the HUD size (so it lays the whole HUD out bigger) while the 3D
world is still rendered at full sharpness.

**Moving HUD elements.** HUD positions are stored in `hudLayout.prf` as fractions of the screen, so
they *can* be moved. **Ultrawide HUD** uses this: on screens wider than 16:9 it pulls the elements
anchored to the left and right edges in to a 16:9-wide area in the middle, so you don't have to
look to the far corners. Heights and centred elements (reticle, compass) are unchanged. Your
original layout is kept as `hudLayout.prf.sswrap-orig`; unticking the option restores it, and if
you rearrange the HUD in the game, that arrangement becomes the new original.

**Known limits.**
- HUD text and panels are enlarged from the game's own pixel art, smoothed but not redrawn with
  more detail.
- Menus are fixed 640x480 artwork; they are scaled to the screen height with bars at the sides.

## Settings (`sswrap.ini`)

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | 1 | 0 = pure pass-through (logging and watchdog still work), for comparisons |
| `RenderScale` | 1 | Render at this multiple of the game's resolution, fractions allowed (set by the settings window) |
| `AskOnLaunch` | 1 | Show the settings window at startup |
| `UltrawideHud` | 0 | Keep the HUD within a centred 16:9 area (set by the settings window) |
| `HudScale` | 1 | Enlarge the HUD meters 1.25x-2x at any game resolution (1.004 and 1.003; set by the settings window) |
| `TextHD` | 1 | Redraw game text from smoothed rebuilds of the game's fonts (1.004 and 1.003; set by the settings window) |
| `TextTrace` | 0 | Diagnostics: log every distinct string the game draws |
| `UiUpscale` | 1 | Enlarge HUD/menu/font bitmaps 2x or 4x with Scale2x, or with `sub_*.png` HD replacements |
| `LinearFilter` | 1 | Smooth (1) or sharp pixel (0) scaling |
| `AO`, `AORadius`, `AOStrength`, `AOMaxDistance` | 1, 3.0, 1.5, 400 | Ambient occlusion and its reach, darkness, fade distance |
| `FXAA` | 1 | Edge smoothing |
| `Sharpen` | 0.4 | Sharpening amount, 0 = off |
| `ExtraModes` | | Extra resolutions for the menu, e.g. `1720x720,2880x1200` |
| `BlockHosts` | `dynamix.com` | Hostnames whose DNS lookups fail instantly |
| `Masters` | `master1.starsiegeplayers.com,master2.starsiegeplayers.com` | Community master servers that answer the game's lookups of the old Dynamix ones, so the in-game server list works |
| `LogLevel` | 1 | 0 errors/stalls only, 1 normal, 2 verbose, 3 everything |
| `StallMs` | 200 | Frame gap that counts as a stall (0 disables the watchdog) |
| `StatsSeconds` | 10 | fps/worst-frame summary interval (0 = off) |
| `TextureDump`, `TextureReplace` | 0, 1 | Save every texture to `sswrap_textures/dump/`; load replacements from `sswrap_textures/load/` |
| `FOV`, `DepthScale`, `MinWorldDraws` | 90, 1.0, 50 | How the game draws: used to place the effects before the HUD and to rebuild 3D positions for AO |

## HD textures

**Ready-made pack:** [sswrap-hd-textures](https://github.com/dev-ben-c/sswrap-hd-textures) has
about 4,700 textures upscaled 4x (terrain, skies, Herc skins, buildings, effects) for the retail
1.004 release. Extract it into the Starsiege folder and start the game. It is kept in a separate
repository because it is derived from the game's original artwork; this repository contains no
game assets.

**Build your own:** sswrap can dump every texture the game loads and swap in replacements, so you
can build or extend a pack yourself, e.g. with an AI upscaler.

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

## CD music

Starsiege plays its soundtrack as **CD audio** tracks on the game disc. sswrap doesn't touch audio;
here is how to keep the music.

**With the original disc in the drive:** nothing to do. Make sure CD music is on in the game's sound
options.

**With a disc image, or no disc:** a mounted ISO (including Windows' built-in ISO mounting) holds
only the data track, so the music is missing. [ogg-winmm](https://github.com/ayuanx/ogg-winmm) (a
separate project) fixes that: it replaces the game's CD-audio calls and plays the tracks from OGG
files instead.

### Windows

1. **Rip the music tracks** from your disc to OGG Vorbis. [fre:ac](https://www.freac.org/) (free,
   open source) can rip straight to OGG; any ripper works if you convert to OGG afterwards.
   Track 1 of the disc is the game data; the music starts at track 2.
2. In the Starsiege folder (next to `Starsiege.exe`) create a folder named `Music` and put the
   tracks in it, named by their **track number on the disc**: `Track02.ogg`, `Track03.ogg`, ...
   (no spaces, no gaps in the numbering).
3. Download `ogg-winmm_binary.zip` from its [releases page](https://github.com/ayuanx/ogg-winmm/releases) and
   copy `winmm.dll` and `winmm.ini` into the Starsiege folder.
4. Start the game with CD music turned on in its sound options. `winmm.ini` has a separate music
   volume (`CDDAVolume`).

The tracks differ between editions (the original 2-CD release, the 1-CD re-releases), so always
rip from your own disc. To undo, delete `winmm.dll`, `winmm.ini` and the `Music` folder.

### Linux / Wine

The same steps, plus a Wine setting so the game uses ogg-winmm's `winmm.dll` instead of Wine's:

```sh
wine reg add 'HKCU\Software\Wine\AppDefaults\Starsiege.exe\DllOverrides' /v winmm /d native,builtin /f
```

Rip with `cdparanoia` and encode with `ffmpeg -i track02.wav -c:a libvorbis -q:a 8 Track02.ogg`
(note the capital T in the final names). ogg-winmm also builds from source with MinGW if you'd
rather not use a prebuilt DLL.

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
