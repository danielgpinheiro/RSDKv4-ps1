# ps1/ — PlayStation platform layer

Copied from `RSDKv3-ps1/ps1/` at commit `1e583ad` (2026-09-25), the finished Sonic CD port, and adapted
freely for Sonic 2 (plan `../../docs/30`; what each piece taught: `../../docs/29`). Each file's header
describes it; the Sonic CD / Nexus notes in them still apply unless changed here.

Phase 0 builds only the engine-independent part (Makefile `SRCS`): libc shims, `cdrom_fs` (load stream +
file index), `cdrom_stream`, `gpu_chain`, `spu`, `xa_music`, `mdec` / `video`, `loading`, `memcard`, and
`platform_test.cpp` (the smoke test standing in for the engine until phase 2). `render.cpp`, `tileset.cpp`,
`sprite_atlas.cpp` and `fmv.cpp` are Sonic CD's renderer pieces: they join when the engine does (phases 2–3).

Differences from the Sonic CD copy:
- `loading_icon.h` is generated from `PS1/Memcard icons/sonic-2-loading.gif` by
  `tools/loading/build_icon.py`: the first 144×144 cell of each GIF frame (8 distinct poses), reduced to
  48×48 cells, least-used colours merged to 15 + transparent, 30 ms/frame. Credit: to be given by the user.
- `memcard_icons.h` (regenerated identically by `tools/memcard/build_icons.py`) already holds
  `s_ps1McIconSonic2` from `2-mc-icon.png`.
- `platform_test.cpp` reads Sonic 2 files (`Data/Stages/Zone01/…`, `Data/SoundFX/Global/Jump.vag`).
