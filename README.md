# sr-recomp

Static recompilation of one's own Switch 2 dump of Splatoon Raiders 1.1.0 into a Windows program: analysis tools, the recompiler, the runtime and a builder that turns a plaintext NSP into a game folder. Nothing of the game is included: it needs the player's own dump.

## Rules

- No game data in this repository. Inputs are passed by path; `.gitignore` blocks dump formats; both tools refuse to
  write reports or work files inside the repository.
- No keys. Only plaintext dumps are read (NCA section encryption type "None", or NUP). Encrypted NCAs are refused.
- No game content in logs or reports: counts, ISA mnemonics, SDK/middleware identifiers, system service names and
  hashes only. Messages from vendored tools have instruction words and addresses stripped.

## Layout

```
SR_RECOMP/
  dump/          game data (not in git)
  vendor/        pinned clones, see THIRD_PARTY.md (not in git)
  .toolchains/   Python venv, local .NET 10 SDK, dmsh_xlate build
  reports/       JSON reports (not in git)
  sr-recomp/     this repository
```

## Tools

```
python tools/exefs_inventory.py <exefs dir | .nsp | .nup> --vendor ../vendor --out ../reports/exefs_inventory.json
```
Modules (NSO segments, MOD0, DT_NEEDED, .eh_frame_hdr function count), SDK and middleware versions, dynamic symbol
counts by namespace, npdm (allowed SVCs, services), SVC call sites, and lifter coverage: every unique `.text` word is
decoded with capstone and lifted with each vendored lifter; words it rejects are grouped by mnemonic.

```
python tools/shader_census.py <.nsp | .nup> --vendor ../vendor --xlate ../.toolchains/dmsh_xlate/dmsh_xlate.dll \
    --dotnet ../.toolchains/dotnet/dotnet.exe --work ../out/shader_work --out ../reports/shader_census.json
```
Finds every shader in RomFS and ExeFS main with PR #88's scanners, deduplicates, and counts how many the Ampere
frontend translates to SPIR-V. Every module gets a structural check (`srrecomp/spirv_check.py`); pass
`--spirv-val <path>` to also run SPIRV-Tools' validator, and `--infer-txq-array` to retranslate textures whose size
query reads a component the default 2D type lacks as `2darray` (an inference, reported separately: without the
game's texture types the translator types unknown textures as 2D). PR #88's `sarc_members` is replaced at run time by a linear
version (the original is quadratic and never finishes on large layout archives).

`exefs_inventory.py` also measures `totk-c+ext` / `totk-asm+ext`: the totk lifter behind `srrecomp/lifter_ext.py`,
which adds PAuth (RETAA/RETAB, BRAA/BLRAA, PAC/AUT/XPAC as identity, PACGA via a runtime `rt_pacga`), LDAPR* and FP16
vector FABS/FNEG. Coverage is reported for a linear sweep of `.text` and for the words inside `.eh_frame` function
ranges.

Build the translator from the pinned PR source:
```
dotnet build -c Release ../vendor/nx2android-pr88/nxnative/dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj -o ../.toolchains/dmsh_xlate
```

Generate and compile C (output under `../out`, never in the repository):
```
python tools/extract_exefs.py <.nsp | .nup> --out ../out/exefs
python tools/gen_c.py --vendor ../vendor -- --exefs ../out/exefs --out ../out/gen --modules main,subsdk0,sdk
python tools/compile_gen.py --gen ../out/gen --vendor ../vendor \
    --clang ../.toolchains/llvm-mingw-20261020-ucrt-x86_64/bin/clang.exe --obj ../out/obj --out ../reports/compile_gen.json
```
`gen_c.py` runs totk-pc-recomp's `recomp/gen.py` unmodified, with `lifter_ext.ExtLifter` wrapped around each module's
lifter (also in its spawned workers). `compile_gen.py` uses totk's flags for generated code and force-includes
`runtime/srrecomp_ext.h`; it compiles only, nothing is linked yet.

Link the runtime (`runtime/core`: totk-pc-recomp's runtime, see its `NOTICE.md`; `runtime/sr`: ours) with those
objects into `../out/build/sr.exe`, null graphics profile, RomFS read in place from the NSP:
```
python tools/build_runtime.py --gen ../out/gen --obj ../out/obj --exefs ../out/exefs --nsp <.nsp> \
    --vendor ../vendor --clang ../.toolchains/llvm-mingw-20261020-ucrt-x86_64/bin/clang.exe --out ../out/build
../out/build/sr.exe --selftest
../out/build/sr.exe --boot
```
A NUP ([a55uka/nup](https://github.com/a55uka/nup) `SPEC.md` v1) works in place of the NSP everywhere `<.nsp>` appears:
pass `--nup <.nup>` to `build_runtime.py` (repeat it for the files of a split pack, `nup pack --split`). The program is
the newest `update` package (else the `base`); `extract_exefs.py` / `exefs_inventory.py` take `--nup-package base` to
read the base instead. Its RomFS is the update's tree alone, as the console treats a patch NCA (`--nup-overlay shadow`
lays it over the base by path instead, which also keeps files only the base has). Raw files are read in place from the
`.nup`; zstd files are decompressed once, SHA-256 checked, into `romfs-side.bin` next to `sr.exe` (1.5 GiB for
Splatoon Raiders 1.1.0; game data, outside the repository), rebuilt only when they change. The display version comes
from the package meta (TLV 0x0004).
With `--profile vulkan --deps ../.toolchains --out ../out/build-vk` it links the real Vulkan back end, the Win32 window
and SDL3 instead (dependencies in `THIRD_PARTY.md`); `sr.exe --probe-vkdevice` checks the GPU, `sr.exe --seconds 5`
runs the game for S seconds of real time (`--shot <png>` saves the last frame, outside the repository).

System fonts (`nn::pl`) come from the user's own firmware, never from this repository:
```
python tools/firmware_fonts.py --vendor ../vendor --keys <prod.keys> --firmware <firmware NCA dir> --out ../out/fonts
```
The keys file is read by path inside the process; key values are never printed, logged or copied. Add
`--fonts ../out/fonts` to `build_runtime.py` to place them next to `sr.exe`.

Shader translator (runtime, `--serve`): totk's `Program.cs` built against nx2android PR #88's Ryujinx (Ampere frontend).
Two fixes to that tree live in `tools/sr_xlate/patches/` (see `THIRD_PARTY.md`); apply them to a fresh checkout first:
```
git -C ../vendor/nx2android-pr88 apply ../../sr-recomp/tools/sr_xlate/patches/nx2android-pr88-translator.patch
../.toolchains/dotnet/dotnet.exe build -c Release tools/sr_xlate/sr_xlate.csproj -o ../.toolchains/sr_xlate
```
Add `--xlate ../.toolchains/sr_xlate --dotnet ../.toolchains/dotnet` to `build_runtime.py` (vulkan profile).

NVIDIA DLSS (RTX GPUs) can replace the game's own neural upscaler: `--dlss <nvngx_dlss.dll>` copies NVIDIA's DLSS
model next to `sr.exe` (github.com/NVIDIA/DLSS, NVIDIA RTX SDK license; never in this repository), and `dlaa = 1` in
`sr-config.ini` turns it on at start. F8 switches it off and on in game, F10 cycles the performance overlay
(`perf_overlay = off | fps | full`). See `runtime/sr/vk_dlss.c`.

In game, F1, Esc, the controller's Home or Minus and Plus together open the settings menu (`runtime/core/ui/`, totk-pc-recomp's, Dear ImGui):
screen, DLSS, HDR, language, sound, controls and button remapping, the crash ring and a screenshot to `screenshots/`.
F9 saves a report of what is on screen, for a picture that looks wrong: `reports/<date>-<time>/` next to `sr.exe`
with a screenshot, the frame's render passes as pictures, the shaders that drew them (and which translator made each)
and the picture settings; a line at the top says when it is saved (about 350 MB, a few seconds). `report = 0` turns
the key off. See `runtime/core/vk_dump.c`.
Changes are written to `sr-config.ini`.
The Ink tab sets the player's and the enemies' ink colour (`ink_color`, `enemy_ink_color` in `sr-config.ini`: `rrggbb`,
unset for the game's own): a colour wheel, a hex code or three stepper rows. The game reads its colours at start, so a
change shows after a restart; the runtime serves the game's team colour table with those colours written in
(`runtime/sr/ink_color.c`), from the profile folder, and the game's own file is not touched.
The Amiibo tab loads 540-byte figure dumps (`amiibo/` next to `sr.exe` by default) and puts one on the emulated reader;
the game's own data on the figure is kept in `<profile>/amiibo/`, never in the dump.

Self-tests: `python tools/probe_census.py ../out/build-vk/shader-cache` (once, after playing: a shader census of SR's
own shaders, outside the repository), then `python tools/run_probes.py ../out/build-vk [--amiibo <dump.bin>]` runs every
probe in a clean setting (an empty settings file and profile) and says which pass.

DLSS Frame Generation (RTX 40/50, up to 4x): `--streamline <sdk>` compiles in `runtime/sr/vk_dlssg.cpp` against NVIDIA
Streamline 2.14 and copies its signed plugins next to `sr.exe`; `dlss_fg = 2 | 3 | 4` in `sr-config.ini` turns it on at
start and F7 cycles it in game. It needs DLSS (`dlaa = 1`), whose inputs it reuses.

AMD FSR 3 frame generation (any GPU with Vulkan, 2x): `--fidelityfx <sdk>` compiles in `runtime/sr/vk_fsrfg.cpp`
against the FidelityFX SDK 2.4 and copies AMD's signed loader and frame generation DLLs next to `sr.exe`; `fsr_fg = 1`
in `sr-config.ini` (with `dlss_fg = off`) turns it on at start, F7 toggles it, F6 cycles AMD's debug views. It also needs
DLSS (`dlaa = 1`) for its inputs.

Frame generation is ruled out for this game: with either SDK, shadows jump and moving things smear in camera turns,
whatever inputs they get, so both stay off by default (DLSS without frame generation is the playable setup). The code
is kept for reference.

Frame interpolation (any GPU with Vulkan, a monitor of at least 120 Hz; `runtime/core/vk_interp.c`): `frame_interp = off | 2 |
3 | 4`, the Screen tab, applies at once. Unlike the two above it needs nothing of the engine: it makes frames from the game's
finished picture, as Hookshot does. The game still runs at 60. More than the monitor can show is not applied (the log says so).
`fullscreen_exclusive = 1` (the Screen tab, after restart) takes the monitor in fullscreen while the window has the focus, so
variable refresh (G-SYNC, FreeSync) follows the game instead of the desktop's rate.

The game's per-draw uniform updates no longer end the render pass: each becomes a small version in the replay's uniform
snapshot, and the copy to memory waits for the next barrier. In the ink map's pass this was most of a frame's GPU time, about
23 of 25 ms in one large area (`ub_inpass`, `ub_reach`, `ub_carry`, `ub_known` under `[diagnostics]` turn the pieces off). For
GPU time by pass, set `pass_timing = 1` under `[diagnostics]`; the log lists the costliest render passes and dispatches every 5 s.

`sr.exe` without arguments opens a launcher in the game's own window, before anything of the game loads: Play, Precompile
shaders (`--warm-shaders`, with a progress bar and Cancel; worth running after a driver update), Settings (the tabs of the
in-game menu) and Quit. Mouse, keyboard and controller work. A setting read at start that changed there makes Play restart
the process first. `sr.exe --play` skips the launcher. Its look, and the menu's fonts, come from the game's own files, read
from the dump at run time (`runtime/core/game_art.c`); nothing of them is stored in this repository or next to `sr.exe`. Without
the dump the launcher and the menu fall back to a plain look.

Translate every shader ahead of play (once per build; the seed files it writes next to `sr.exe` can be deleted after):
```
python tools/prepare_shaders.py <.nsp | .nup> --vendor ../vendor --build ../out/build-vk
```
It seeds every RomFS and `main` shader with its default answers and runs `sr.exe --prepare-shaders`, which fills the
build's shader cache and the GPU driver's pipeline cache. Variants the defaults do not cover (another texture type in a
slot the shader queries, integer vertex inputs) and shaders found only at run time are still translated in play. Frames can
be saved with `SR_SHOTS=60-1500:120 SR_SHOTS_DIR=<dir>` (every 120th present); they stay outside the repository.

## Builder

`builder/` turns one's own plaintext NSP into a portable game folder in one command, with every step above in order:
check, copy of the RomFS, ExeFS, lift, compile, system fonts, link, the .NET runtime the shader translator needs, the
shaders prepared ahead of play, shortcuts. It uses this workspace's `vendor/` and `.toolchains/`; the game folder needs
neither afterwards (romfs/romfs.bin, exefs/, dotnet/, shared-fonts/ and the shader cache sit next to `sr.exe`).

```
python -m builder --game <plaintext .nsp> --keys <prod.keys> --firmware <firmware NCA folder> --dest <game folder> \
    [--desktop-shortcut] [--start-menu] [--delete-original] [--clean]
python -m builder --dest <game folder>                          # resume a cancelled or failed build, or rebuild
python -m builder restore --dest <game folder> --out <file.nsp>  # the original NSP again, byte for byte
python -m builder tools --list                                   # the tools of builder/tools.json: what is there
python -m builder tools                                          # download and unpack the ones that are not
```

Run it from the repository folder with the workspace's Python (`..\.toolchains\venv\Scripts\python.exe`). Keys and
firmware are read by `tools/firmware_fonts.py` only, for the system fonts, and only on the first build; no key value is
stored. Each step records a fingerprint of its inputs in `<game folder>/sr-build.json` and is skipped when it matches:
a resume starts where it stopped, a rebuild after a change of the port's code redoes only what depends on it. The C,
objects and logs stay in `../out/builder-temp/<folder>-<hash>` for quicker rebuilds (`--clean` drops them).

The RomFS copy also keeps, in `<game folder>/original/` (about 75 MB), every byte of the NSP outside its RomFS and the
NSP's SHA-256: `restore` writes the NSP again from the folder and checks it. `--delete-original` uses that: once the
folder is built, the NSP is rebuilt in memory, checked against its SHA-256 and sent to the Recycle Bin (never deleted
outright). A later rebuild that needs the NSP rebuilds it into the temp folder for that run.

`--game` also takes a NUP: the program that runs in it (its newest update, else its base) must be the version the port
expects. Its files are written one after another to `romfs/romfs.bin`, each checked against the SHA-256 the NUP records,
with their own index (`romfs/romfs-index.bin`, `build_runtime.py --romfs-indexed`). A NUP is never removed, since it
cannot be made again exactly from the folder, and a later rebuild needs it where it was.
Measured here (24 threads): about 5 minutes of compile, 2 for the shaders, 15 GB of game folder.

The window (`builder/window/`, WPF on the workspace's .NET) is that command line with pickers and a progress view; it
decides nothing itself: it runs `python -m builder`, reads the events (`builder/events.py`) and shows the texts of
`builder/text/en.json`.
```
../.toolchains/dotnet/dotnet.exe build builder/window -c Release      # builder/window/bin/Release/net10.0-windows/SrBuilder.exe
SrBuilder.exe [--game F --firmware D --keys F --dest D [--go]]        # fill the setup; --go builds at once
SrBuilder.exe --shots <dir>                                           # each screen with sample values as PNG: a look check
```
Three views: the setup (game file, firmware and keys, game folder, the shortcuts and the removal of the original), the
build (the steps, the current one's bar, STOP: Ctrl+Break to the builder, which keeps the finished steps) and the end
(PLAY, the folder, restore). A failed or stopped build shows the engine's message and resumes with TRY AGAIN. The last
paths are kept in `%LOCALAPPDATA%/SrBuilder/settings.json` (paths only). The look is its own: the launcher's palette,
no game asset; its fonts are Bowlby One and Barlow Condensed (OFL, `builder/window/fonts/`, built into the exe).
The setup checks the game file as soon as it is chosen (`python -m builder check-game`, the check step's own test of
title and version) and BUILD stays off for a file the port does not take.

The tools (`builder/tools.json`, `builder/tools.py`): the compiler, .NET, Streamline, FidelityFX, DLSS, SDL3, the
Vulkan headers, VMA and libvpx, each with its official address, size and SHA-256. A tool is there when its probe path
exists, so a workspace set up by hand downloads nothing; `--fetch-tools` lets a build's check step get the missing
ones (the window always passes it and says on its setup how much that is). Downloads are kept in
`../out/builder-downloads`. The pinned clones of `../vendor` are fetched the same way, as GitHub's archive of each
commit, checked by a hash over their files and patched by `tools.py` itself (no git), and the shader translator is
built from them with the downloaded .NET.

The package (`builder/package.py`): `python -m builder package --out <folder>` makes the builder as one folder that
needs nothing installed (154 MB): `SrBuilder.exe` with its own .NET inside, `app/` with the sources the builder runs
and `python/`, the embeddable Python with the wheels of `tools.json`. That folder is a workspace of its own: the first
build fills its `.toolchains/`, `vendor/` and `out/`.


## Licence

GNU General Public License, version 3 (`LICENSE`). The runtime holds GPL-3.0 code of others (the frame generation
shaders of Hookshot in `runtime/core/interp/` and the file that drives them, and the virtual clock adapted from
last-shot-1), so the whole is under that licence. The other third-party parts keep their own, all compatible with it
(MIT, BSD, Apache-2.0, SIL OFL): `THIRD_PARTY.md` names each with its origin. Nothing of the game, of console keys or of
firmware is part of this repository, and nothing of NVIDIA's: a build fetches those libraries from their source on the
PC it runs on, and no built `sr.exe` is distributed.

Tests use synthetic data only: `python -m unittest discover tests`.
In-game checks (runs with an input script and pictures, comparisons between runs, A/B, translator comparisons):
`tools/testing/README.md`.
