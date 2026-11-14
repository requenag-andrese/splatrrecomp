# Pinned references

Checked 2026-11-04. Clones live in `../vendor/` (outside this repository) at exactly these commits. No code from them is copied
into this repository; the tools import or run them from `--vendor`.

| Source | Commit | License | Used for |
|---|---|---|---|
| [valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) `main` | `0f65b4ee07e934e99b0c937d059f6bb6ae72e1c3` | MIT (root `LICENSE`) | `recomp/lifter.py` + `simd_c.py`: lifter coverage (`totk-c`, `totk-asm`) |
| [peiikai/LASRecomp](https://github.com/peiikai/LASRecomp) `main` | `a6b4f03844ecfb4012f18deff96da8b410aeafcd` | MIT (root `LICENSE`; GitHub reports NOASSERTION because of an added scope paragraph) | `tools/lifter.py` + `simd_c.py`: lifter coverage (`las-c`) |
| [a55uka/nup](https://github.com/a55uka/nup) `meow` | `92089de3bf98e04f02d8b14f3b91dddc88e4cd80` | MIT | `SPEC.md` v1.0-draft: NUP input reader in `srrecomp/sources.py` (written from the spec) |
| [drakolordx7/switch2-recomp](https://github.com/drakolordx7/switch2-recomp) `main` (cloned 2026-11-05) | `5339bf85b05e84c34a7f18d8a43db1de1f1692b2` | GPL-3.0 (see note below) | reference: NVN2 ABI notes (`runtime/docs/nvn2_abi.md`), NVN2 front end, Switch 2 services |
| [drakolordx7/nxrecomp](https://github.com/drakolordx7/nxrecomp) `main` (cloned 2026-11-05) | `de23ddc8e2447fe6b4ec626807a31dab7ab4b807` | GPL-3.0 | reference: recompiler, runtime and tools switch2-recomp is built on |
| [BluetufieBugado/SuperMarioWonderRecomp](https://github.com/BluetufieBugado/SuperMarioWonderRecomp) `main` (cloned 2026-11-05) | `9ecad0cef164008a474b5ff0fa50e6ef55edba60` | MIT | reference: NVN replayed into Vulkan with the Ryujinx translator |
| [shirai91/nx2android](https://github.com/shirai91/nx2android) PR #88 head (`knlnks/switch-2`) | `3089bb229510075db9b048403192f21a860071fe` (base `main` `b352b9f4879d730c6cf68aab45c0d13849764073`) | MIT, stated in `README.md` and `nxnative/LICENSE`; no root `LICENSE` file, so GitHub shows none | `gfx/tools/shader_split.py` scanners, `dmsh_xlate` + vendored Ryujinx shader translator with the Ampere frontend |

GPL-3.0 sources (switch2-recomp, nxrecomp and the recomps they credit) may be read and copied: this project is under
GPL-3.0 as well (`LICENSE`). Code taken from them is marked with its origin where it lands.

Third-party code inside those repositories keeps its own license. The parts this evaluation touches:

- Ryujinx `Ryujinx.Graphics.Shader`, `Ryujinx.Common`, `Spv.Generator` (inside nx2android and totk-pc-recomp): MIT.
- `dmsh_xlate` is built from source (`nxnative/dm/gfx/tools/dmsh_xlate`), not from the prebuilt `dmsh_xlate_bin/` in the PR.

Validator: SPIRV-Tools `v2026.4-90-gb570f00a`, Khronos CI Windows release build (continuous/280, 2026-11-03) linked
from SPIRV-Tools `docs/downloads.md`; archive MD5 `QZ3wa86W9M31D9G5PabFLQ==` (base64) matched the bucket's hash.
Apache-2.0 (the archive ships no LICENSE file; license per the upstream repository). Unpacked to
`../.toolchains/spirv-tools/`.

Compiler: llvm-mingw `20261020` (`llvm-mingw-20261020-ucrt-x86_64.zip`, GitHub release of mstorsjo/llvm-mingw,
SHA-256 `fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd`, matched), clang 23.1.4. LLVM: Apache-2.0
with LLVM exceptions; mingw-w64 runtime: its own permissive licenses. Unpacked to `../.toolchains/`.

Vulkan profile dependencies (under `../.toolchains/`, downloaded 2026-11-04):
- Vulkan-Headers `v1.4.367` (KhronosGroup, git tag, commit `9fbc2a782ee3f8a4db64e7d991e23da75e893f44`): Apache-2.0 / MIT.
- Vulkan Memory Allocator `v3.4.0` `vk_mem_alloc.h` (GPUOpen): MIT; SHA-256
  `8487b7995ad3b263eb73bc5b9a77d71aa69b6bef5d58a715c02d2663afd81f1a`, the hash totk-pc-recomp pins.
- SDL3 `3.4.20` (`SDL3-devel-3.4.20-mingw.zip`, libsdl-org/SDL release): Zlib; SHA-256
  `5d321d3d2d499cfabad3023921e4b56bbec03558060f4e195ce626d200126442`. `SDL3.dll` is copied next to `sr.exe`.
- libvpx `1.17.0` (`mingw-w64-ucrt-x86_64-libvpx-1.17.0-1-any.pkg.tar.zst`, MSYS2 ucrt64 repository, downloaded
  2026-11-04): BSD-3-Clause (the package ships no licence file; licence per the MSYS2 package database and upstream);
  SHA-256 `8ff6919dbb88ea26ce28d22fd81d6f0d6e200cf56a4dd81ffc2d33124fe98024`, matched against that repository's
  `ucrt64.db`. Unpacked to `libvpx/`; `libvpx.a` is linked statically, with llvm-mingw's winpthreads.
- RenderDoc `1.47` (`RenderDoc_1.47_64.zip`, renderdoc.org/stable/1.47, downloaded 2026-11-05; 101 729 763 bytes,
  SHA-256 `b51712e58d40066b34ca30c7869d15f72e4b5c001b2d92232f6783a636d5a3c1`): MIT. The site publishes no hash; its
  `.sig` verified good with Baldur Karlsson's key `1B039DB9A4718A2D699DE031AC612C3120C34695` (keys.openpgp.org).
  Unpacked to `renderdoc/`; used only to capture frames of the reference emulator for comparison, not linked.
- `vulkan-implib/libvulkan-1.a`: import library generated locally with llvm-dlltool from the exports of the system's
  `vulkan-1.dll` (no file downloaded).

`tools/firmware_fonts.py` imports totk-pc-recomp's vendored `tools/vendor/macnativegames` scripts (MIT, MacNativeGames
contributors) from `--vendor` for NCA decryption and RomFS verification; nothing from them is copied here.

`tools/sr_xlate/Program.cs` is totk-pc-recomp's `tools/xlate/Program.cs` (MIT, its license in
`tools/sr_xlate/LICENSE-totk-pc-recomp.txt`) with the changes listed in its header; it builds against the Ryujinx tree of
nx2android PR #88 (`nxnative/dm/third_party/ryujinx`, MIT) from `../vendor`, nothing of which is copied here. Two
local fixes are made in that vendored tree (marked `sr-recomp`), so a fresh checkout of the PR needs them again:
`StructuredIr/AstOptimizer.cs` (an atomic is not propagated into its uses: it ran twice) and
`Instructions/InstEmitAmpere.cs` `EmitAmpereBlock` (a guarded write no longer drops another instruction's result still
in flight to the same register). Both are in `tools/sr_xlate/patches/nx2android-pr88-translator.patch`.

Per-object motion vectors: `runtime/sr/vk_mvprev.c` and `runtime/sr/vk_mvsurvey.c` are totk-pc-recomp's
`runtime/vk_mvprev.c` and `runtime/vk_mvsurvey.c` (MIT, the commit pinned above; the changes are in their headers), and
`tools/sr_xlate/patches/0015-motion-previous-position-nvn2.patch` is its `tools/xlate/patches/0015-motion-previous-position.patch`
adapted to nx2android PR #88's Ryujinx (slots bank + 22, a 39-entry slot table), applied to the vendored tree like the
other patch.

`runtime/sr/vk_dlss.c` adapts the NGX half and the camera reprojection of totk-pc-recomp `runtime/vk_dlaa.c` (MIT,
Copyright (c) 2026 Valiantsin Dzerakh, the commit pinned above); `runtime/sr/shaders/dlaa_mv.comp` and its SPIR-V
`runtime/sr/dlaa_mv_spv.h` are copied unchanged from the same tree. NVIDIA's DLSS model `nvngx_dlss.dll` 310.9.1
(github.com/NVIDIA/DLSS tag v310.9.1, `lib/Windows_x86_64/rel`, NVIDIA RTX SDK license, downloaded 2026-11-06;
58 956 912 bytes, SHA-256 `3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983`, the hash totk-pc-recomp's
installer pins; Authenticode signature valid, NVIDIA Corporation) lives in `../.toolchains/dlss-dl/` with its LICENSE
and is copied next to `sr.exe` by `build_runtime.py --dlss`; it is never part of this repository. NGX itself is the
NVIDIA driver's.

NVIDIA Streamline SDK `v2.14.16` (`streamline-sdk-v2.14.16.zip`, github.com/NVIDIA-RTX/Streamline release of
2026-10-16, downloaded 2026-11-06; 276 016 170 bytes, SHA-256
`32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b`, matching the digest GitHub publishes for the asset;
every DLL in `bin/x64` carries a valid NVIDIA Corporation Authenticode signature): unpacked to
`../.toolchains/streamline/` for DLSS Frame Generation and Reflex. Its core is MIT (`license.txt`); the plugin and NGX
binaries are under NVIDIA's licences shipped beside them. Nothing of it is in this repository.

AMD FidelityFX SDK `v2.4.0` (source archive `v2.4.0.zip` of github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK,
downloaded 2026-11-07 as `FidelityFX-SDK-2.4.0.zip`; 254 235 565 bytes, SHA-256
`27120e3bbe0d0b30213825ad40f19077563f04f04572e252109a392d5fb522cb`; `amd_fidelityfx_loader_vk.dll` and
`amd_fidelityfx_framegeneration_vk.dll` carry valid Advanced Micro Devices Authenticode signatures): unpacked to
`../.toolchains/fidelityfx/` for FSR 3 frame generation. Its headers are compiled into `runtime/sr/vk_fsrfg.cpp`; the
two signed DLLs are copied next to `sr.exe` by `tools/build_runtime.py --fidelityfx`. Licence: AMD's
`docs/license.md` (binary redistribution with notice; no reverse engineering of the DLLs); the samples are MIT.
Nothing of it is in this repository.

In-game menu (2026-11-08): `runtime/core/ui/` is totk-pc-recomp's `runtime/ui/` (MIT, the commit pinned above):
`menu.cpp` with the tabs cut to the settings SR reads and SR's own rows added, `option_rows.*`, `binds_page.*` and
`menu_win.cpp` with only the setting names changed (each says so in its header). Inside it, as totk-pc-recomp credits
them: the Vulkan set-up after LAS Recomp's `imgui_glue.cpp` and the look and option rows after SavageWindRecomp's
`menu.mm` / `option_rows.mm` (both MIT). `runtime/third_party/imgui/` is Dear ImGui v1.91.9 (MIT, Omar Cornut, its
`LICENSE.txt`) with its Vulkan back end, copied unchanged from the same tree. `runtime/core/ui/fonts/Roboto-Medium.ttf`
is the Roboto font (Apache-2.0, `LICENSE-Roboto.txt` beside it), embedded into `sr.exe`.

`runtime/third_party/zstd/zstddeclib.c` is the official single-file Zstandard decompressor (BSD, Meta Platforms, its
`LICENSE`), copied unchanged from `vendor/switch2-recomp/runtime/third_party/zstd_zbic`; only `runtime/core/game_art.c` uses
it. The launcher's fonts and pictures are the game's own, read from the player's dump at run time: none are in this
repository. Three behaviours of the shader start-up (driver stamp, stall guard, cache save threshold) were reimplemented after
Hookshot (https://github.com/NotRexed/Hookshot, GPL-3.0); no code was copied.

`runtime/core/interp/` holds the compute shaders of Hookshot's frame generation (https://github.com/NotRexed/Hookshot, GPL-3.0,
`gpu/vk/vk_framegen.cpp`), and `runtime/core/vk_interp.c` follows that file's passes and pacing. `fg_luma`, `fg_median` and
`fg_synth` are unchanged; `fg_flow.comp` has our two-step search. Their `_comp_spv.h` are `glslangValidator -V --target-env vulkan1.3
--vn <NAME>` output. GPL-3.0 applies to these files and to a build containing them: the project as a whole is under
GPL-3.0 since 2026-11-12 (`LICENSE`).

Amiibo (2026-11-08): `runtime/core/amiibo.c` and `runtime/core/ui/amiibo_page.*` are totk-pc-recomp's (MIT, the
commit pinned above; SR's changes in their headers). `runtime/sr/ov_nfp.c` and `runtime/sr/vp9_probe.c` are ours; the
first takes its function signatures from nx2android PR #88 `dm/host/hle_sdk16.c` (MIT) and its result codes from
Switchbrew's error code list (yuzu's `nfc_result.h` names the same values), the second copies the WebM demuxer of
totk-pc-recomp's `runtime/movie_probe.c` (MIT). `runtime/sr.manifest` is totk-pc-recomp's `runtime/totk.manifest`.

Virtual time (2026-11-11): `rt_ticks` in `runtime/core/main.c` (`SR_VIRTUAL_TIME`) adapts `rt_ticks` of last-shot-1's
`host/sp_main.c` (https://github.com/portsofbigN/last-shot-1 at `c8e077500891335ef04a1c2e424f315fe10eb36c`, GPL-3.0, read on
the web); ours starts at a fixed value and also drives the GPU report timestamps. GPL-3.0 applies as above.

Builder (2026-11-11): `builder/runner.py`, `builder/state.py` and `builder/events.py` adapt totk-pc-recomp's
`installer/runner.py`, `state.py` and `events.py` (MIT, the commit pinned above; changes in their headers), and
`builder/steps.py` its `fingerprint` and `run_tool`; the steps themselves are this project's.

Builder window fonts (2026-11-12), in `builder/window/fonts/` with their licence texts: Bowlby One (Copyright (c) 2011
vernon adams) and Barlow Condensed Regular, SemiBold and Bold (Copyright 2017 The Barlow Project Authors), both under
the SIL Open Font License 1.1, taken unchanged from https://github.com/google/fonts (`ofl/bowlbyone`,
`ofl/barlowcondensed`) and built into SrBuilder.exe as resources.

Translator study: `tools/eden_xlate` builds against PokeDinner's shader recompiler
(https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork, an Eden fork, GPL-3.0-or-later; shallow clone at
`23f383cab5` in `../vendor/pokedinner`) and its dependencies in `../vendor/pokedinner-deps` (sirit v1.0.5, BSD-3; fmt
12.1.0, MIT; SPIRV-Headers vulkan-sdk-1.4.341.0, MIT; boost-1.91.0 libraries, BSL-1.0). None of them is copied here;
`eden-xlate.exe` links them, so GPL-3.0 applies to it.

Ideas without code: the crash ring (`runtime/core/ring.c`) and the viewport checks of `runtime/core/vk_exec_draw.c`
follow SwitchRecomp v190 by MZ Labs (github.com/SwitchRECOMP-Dev/SwitchRECOMP, AGPL-3.0): its `vk_trace.txt` ring in
a memory-mapped file and its viewport checks before a draw. No SwitchRecomp code is in this repository.

Python packages (venv, see `requirements.txt`): capstone (BSD-3), lz4 (BSD-3), numpy (BSD-3), zstandard (BSD-3), backports.zstd (PSF/BSD), cryptography (Apache-2.0 / BSD-3).
