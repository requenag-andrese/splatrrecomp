# Origin of this directory

Copied from [valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) `runtime/` at commit
`0f65b4ee07e934e99b0c937d059f6bb6ae72e1c3`, MIT License (`LICENSE-totk-pc-recomp.txt`). That runtime itself adapts MIT
code from LAS Recomp and MacNativeGames, with the attributions kept in each file's header and listed in
`THIRD_PARTY_NOTICES-totk-pc-recomp.md`.

Left out as specific to *Tears of the Kingdom* or to its optional features: the nfp IPC service (`nfp.c`; amiibo.c came
in on 2026-11-08 with its menu page, see its header), boot checkpoints of TotK
versions (boot.c is adapted, not copied as is), camera speed, cheats, contact shadows, Discord presence, DLAA and the
motion-vector tools, flight recorder, mods, RenderDoc hook, the game-resolution code, the
TotK zstd entry points, and the TotK window resources. DLAA keeps only its header `vk_dlaa.h` (and `vk_mvobj_fs.h`, a SPIR-V array vk_exec_draw.c includes), answered as "off" by `runtime/sr/no_dlaa.c`. Also left out: `third_party/` (except Dear ImGui, now in `runtime/third_party/imgui/`) and `shaders/`. The menu `ui/`
was left out at first and came in on 2026-11-08 without its Mods, Cheats and Amiibo pages (`amiibo_page.cpp`,
`cheats_tab.cpp`) and without `menu_sdl.cpp` (no SDL window here); the null profile still answers `ui/menu.h` as "no
menu" (`runtime/sr/no_menu.c`).

Files changed after the copy say so in their header with a line starting `sr-recomp:`; every other file is byte-identical
to the commit above.
