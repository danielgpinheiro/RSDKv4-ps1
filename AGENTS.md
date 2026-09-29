# AGENTS.md — RSDKv4-ps1 (Sonic 2 and Sonic 1 on PlayStation)

Operating manual for this repo. Workspace rules: `../AGENTS.md`. Knowledge base: `../docs/` (start at
`../docs/README.md`). **Plans and phase tables: `../docs/30-rsdkv4-sonic2-port-plan.md` (Sonic 2, done),
`../docs/37-rsdkv4-sonic1-port-plan.md` (Sonic 1, `GAME=1`). Lessons from the earlier ports: `../docs/29`.**

**Two games, one engine**: `make GAME=1` / `GAME=1 tools/data_disc.sh` / `GAME=1 tools/<test>.sh` for Sonic 1
(`../Sonic1_Extracted`; its own natives, save file, loading icon, menus, BG strip format 'BGS3'; its own PCSX-Redux
instance, `tools/redux_env.sh`); the default `GAME=2` is Sonic 2 and must stay byte-identical to the published EXE unless
Sonic 2 itself changes: Sonic 1-only code goes behind `#if PS1_GAME == 1` with Sonic 2's text verbatim in `#else`, and
Sonic 1-only opcodes after Sonic 2's (docs/37 choices 9-10).

## What this is
RSDKv4-Decompilation (Retro Engine v4, Sonic 1/2 2013) ported to the PS1 with **psyqo** (C++20) on
branch `ps1`: **Sonic 2** and **Sonic 1** (`GAME=1`, docs/37), both done, run on a PSone and published as ISO builders. Remotes: `origin` =
upstream RSDKModding (read-only), `fork` = github.com/danielgpinheiro/RSDKv4-ps1 (**private**; push here).

## First checks (every session)
- `git status` / `git log --oneline -10` (uncommitted work may be another agent's).
- Phase + status in `../docs/30` (and the `rsdkv4-ps1-port` skill).
- Game data: `../Sonic2_Extracted/` (the user's Android/iOS `Data.rsdk` "RSDKvB", extracted `Data/`,
  `Bytecode/`). Never commit game data.
- `../nugget` stays plain upstream. Don't read or use the Xbox RSDKv4 port (incomplete).

## User decisions (2026-09-25)
- Fresh upstream clone + private fork; the `ps1/` layer and tools come from `RSDKv3-ps1` (`1e583ad`).
- All characters: Sonic, Tails, Sonic & Tails, Knuckles.
- Icons: `../Memcard icons/sonic-2-loading.gif` (loading), `2-mc-icon.png` (save); credit to be given by the user.
- Carried over: 320 px, 2 MB, golden rule, **no frame skip**, byte-exact evidence per step, plans approved
  before implementing.

## Build / run / debug
```bash
make -j8                        # rsdkv4.ps-exe (+ .elf, .map): the engine; must fit 2 MB
make -j8 BOOT=Zone01 [BOOT_ID=2] [BOOT_PLAYER=0..3]   # start in a stage (0 Sonic, 1 Tails, 2 Knuckles, 3 Sonic & Tails);
                                # touch RSDKv4/main.cpp RSDKv4/RetroEngine.cpp when switching
make PLATFORM_TEST=1            # the phase 0 smoke-test app (ps1/platform_test.cpp)
tools/data_disc.sh              # = tools/build_assets.py --src ../Sonic2_Extracted (the builder's pipeline) + make disc;
                                # game disc from ../Sonic2_Extracted (Data/ + Bytecode/, no sources / menus / ZoneM)
tools/export_builder.sh ../Sonic-2-PS1-ISO-Builder   # phase 11: the public builder's builder/ + bin/ (natural build)
make dist                       # natural build only: licensed (../LICENSE.DAT by path, LICENSE=none for none), verified
                                # build/dist/Sonic2-PS1.bin/.cue + SHA256SUMS (reproducible: same hashes on a rebuild)
                                # + manifests (tools/rsdkmanifest), atlases per variant, tile pages (phase 3.1)
tools/vram_test.sh Zone01_st [OUT] [F]        # BOOT= build: tile pages + atlas + palette in VRAM == the files
[MASK=8 FROM=240] tools/frame_capture.sh N OUT                  # both framebuffers as PNGs at frame N + renderer counters
tools/input_test.sh [OUT] [F]   # every pad-1 button through the real pad path (tools/pad.lua) -> its engine input
tools/boot_test.sh [OUT] [F1] [F2]            # headless boot over GDB: loads, fps, heap, VM/array guards
tools/logic_test.sh [OUT] [S0 S1 S2 S3]       # BOOT= build: run right, jump, land (g_ps1PadTestMask) + dumps (guards and
                                              # audio counters as files: OUT_guards.bin / OUT_audio.bin)
GAME=1 tools/zone_sweep.sh OUT Zone01:0 ...   # per FOLDER:PLAYER a BOOT= build + logic test, one line each (restores make)
make BOOT=X SAMPLER=1 && tools/pc_profile.sh OUT [START END]   # CPU profile by PC sampling (root counter 0 IRQ records
                                              # EPC, ps1/pc_sampler.cpp): share per function; not for make dist
GAME=1 tools/export_builder.sh ../Sonic-1-PS1-ISO-Builder   # the Sonic 1 builder (published: github.com/danielgpinheiro/Sonic-1-PS1-ISO-Builder)
tools/load_profile.sh [OUT] [STOP]            # BOOT= build: load-time profile, stages switched by GDB (SCHEDULE=,
                                              # PRE='set var g_ps1FsFastLoad = 0' / g_ps1ScriptImageOn = 0 for A/B)
python3 tools/tiles_check.py ../Sonic2_Extracted/Data/Stages/Zone01 OUT     # packed chunk tiles == file
python3 tools/script16_check.py ../Sonic2_Extracted/Bytecode Zone01 OUT     # int16 script code == bytecode
tools/scene3d/sort3d_check.sh   # host: the PS1 3D draw-list merge sort == upstream's bubble sort
tools/smoke_disc.sh             # phase 0 test disc (Sonic 2 subset: Zone01 + Jump.vag) + make disc
tools/smoke_test.sh [OUT]       # phase 0 smoke test over GDB (file CRCs, SPU, loading icon, pad)
python3 tools/loading/check_icon.py VRAM FRAME Y     # loading icon pixel check on a VRAM dump
python3 tools/loading/build_icon.py                   # regenerate ps1/loading_icon.h
python3 tools/memcard/build_icons.py                  # regenerate ps1/memcard_icons.h
```
PCSX-Redux in the background with a timeout, GDB for evidence (`../docs/21`); start it **without `-run`**
when GDB sets breakpoints before the first `continue`. Use line breakpoints (`file.cpp:N if cond`) rather
than watchpoints. GDB convenience variables must not be register names (`$v0`, `$s0`…).

## Layout
| Path | Role |
|---|---|
| `RSDKv4/` | the engine (upstream decompilation) + PS1 code behind `RETRO_PLATFORM == RETRO_PS1` (`main.cpp` = the psyqo app, `RetroEngine::RunFrame`; arrays sized from the data, `../docs/30` phase 2 appendix) |
| `RSDKv4/NativeObjects/` | the 2013 menus as native C++ objects (drawn by `RSDKv4/Renderer.cpp`): phase 1 research |
| `ps1/` | PS1 platform layer (see `ps1/README.md`); `render.cpp` = the GPU renderer (phase 3, from Sonic CD; tiles from the packed chunk table), `tileset` / `sprite_atlas` = stage-load VRAM uploads; `audio.cpp` = RSDKv4/Audio.cpp on the SPU (per-stage SFX banks, phase 5.1) + music (5.2); `platform_test.cpp` = phase 0 app |
| `tools/disc/` | mkpsxiso BIN/CUE builder (file index, two mastering passes) + byte-level verifier |
| `tools/loading/`, `tools/memcard/` | loading / save icon builders and checks |
| `tools/smoke_*.sh`, `vramshot.py`, `pad.lua`, `frame_capture.sh`, `boot_test.sh` | harness |

## Invariants (from the Nexus and Sonic CD ports)
- **2 MB RAM** (`RAM_SIZE=2M`); track heap high-water incl. load peaks.
- **Golden rule**: no runtime asset decoding; converters decode their output back and compare.
- Per-frame drawing is **DMA-chained** (`ps1/gpu_chain.hh`); fragments ≤ 14 words; the chain is sent after
  `flip()`, so immediate VRAM writes go to the other buffer (`../docs/29` §3).
- Clip primitives beyond the GPU limits (±1024, 1023×511).
- **Pads = `psyqo::AdvancedPad`**. **One CD action at a time** (stop the load stream first).
- CLUT index 0 = transparent (0x0000); visible black = 0x8000.
- Hardware access in `start()` / `frame()`, not `prepare()`.
- **v4 RAM layout (phase 2)**: script code is **int16 slots + a sorted big-value table** (`Script.hpp`
  `PS1ScriptCode`): read `scriptCode[i]` freely, write only through `PS1ScriptWrite`. Script entity accesses go through
  `PS1_OBJ(i)` (blank scratch past `ENTITY_COUNT` 0x380, counted). `tiles128x128` fields are packed views
  (`Scene.hpp`: assign/read only, no pointers). Type groups share one pool (`Object.cpp PS1BuildTypeGroups`).
  Scene3D vertex indices 0xFF0-0xFFF map to the buffer's tail. Every clamp has a `g_ps1*` counter that tests read.
- **Scene3D arrays are a heap block** (~85 KB: faces, vertices, draw list, sort scratch, projections, exact cache), allocated
  only while a stage listed in `Data/Game/PS1Scene3D.bin` is loaded (`tools/scripts/scene3d_stages.py`: stages whose
  bytecode touches Scene3D; Sonic 2: Special). `PS1Scene3DPrepare` runs first in each stage load's bracket (the chain
  arenas are freed there: the block goes under the load's buffers and the arenas). Index through `g_ps1VtxCap` /
  `g_ps1FaceCap` (0 without the block), never `VERTEXBUFFER_SIZE` / `FACEBUFFER_SIZE`.
- No native objects on PS1: `RetroEngine::RunFrame` does `RetroGameLoop`'s work (scripted Title path).
- **Stage loads (phase 9)**: every stage's scripts come from `Data/Stages/<X>/Scripts.ps1` (the host engine's
  arrays right after the stage's `LoadBytecode`, written by `tools/rsdkmanifest`; `g_ps1ScriptImageOn` = 0 decodes the
  bytecode for A/B): regenerate the disc (`tools/data_disc.sh`) after any bytecode patch. Shared files a load reads
  (GameConfig, the players' .ani, the palettes) are copied into `Data/Stages/<X>/PS1Local/` (`tools/disc/stage_local.py`)
  and read from there during that stage's load; re-opened small files are pinned in RAM for the load. Profile loads with
  `tools/load_profile.sh` (seeks are the model-independent cost).

## Definition of done for a step
Builds cleanly · fits 2 MB · verified in PCSX-Redux with evidence · `../docs/30` status updated · commit
(prefixes `feature:` `fix:` `chore:` `housekeeping:` `FMV:`) and push to `fork ps1` only when the user asks.
