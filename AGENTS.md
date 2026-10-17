# AGENTS.md — RSDKv4-ps1 (Sonic 2 and Sonic 1 on PlayStation)

Operating manual for this repo. Workspace rules: `../AGENTS.md`. Knowledge base: `../docs/` (start at
`../docs/README.md`). **Plans and phase tables: `../docs/30-rsdkv4-sonic2-port-plan.md` (Sonic 2, done),
`../docs/37-rsdkv4-sonic1-port-plan.md` (Sonic 1, `GAME=1`, done). Lessons from the earlier ports: `../docs/29`.**
Performance at HEAD: `../docs/43`.

**Two games, one engine**: `make GAME=1` / `GAME=1 tools/data_disc.sh` / `GAME=1 tools/<test>.sh` for Sonic 1
(`../Sonic1_Extracted`; its own natives, save file, loading icon, menus, BG strip format 'BGS3'; its own PCSX-Redux
instance, `tools/redux_env.sh`); the default `GAME=2` is Sonic 2 and must stay byte-identical to the published EXE unless
Sonic 2 itself changes: Sonic 1-only code goes behind `#if PS1_GAME == 1` with Sonic 2's text verbatim in `#else`, and
Sonic 1-only opcodes after Sonic 2's (docs/37 choices 9-10).

## What this is
RSDKv4-Decompilation (Retro Engine v4, Sonic 1/2 2013) ported to the PS1 with **psyqo** (C++20) on
branch `ps1`: **Sonic 2** and **Sonic 1** (`GAME=1`, docs/37). Remotes: `origin` = upstream RSDKModding (read-only),
`fork` = github.com/danielgpinheiro/RSDKv4-ps1 (public on GitHub as of 2026-10-16; push here).

**Status (2026-10-16): both done**, both run on a PSone from a CD-R (Sonic 2 2026-09-28, Sonic 1 2026-09-29). Public
builders: https://github.com/danielgpinheiro/Sonic-2-PS1-ISO-Builder (head `f05e07b`) and
https://github.com/danielgpinheiro/Sonic-1-PS1-ISO-Builder (head `c5fdb3f`); both executables from `75ddf41`, update
notes in each `CHANGELOG.md`. The discs boot `SONIC2.EXE` / `SONIC1.EXE` (`9a45d1d`): `PS1_EXE_NAME` from the Makefile
for `GAME=1`, else `tools/disc/exe_name.txt` (written into each builder by the exporter), else `SONIC2.EXE`.

## First checks (every session)
- `git status` / `git log --oneline -10` (uncommitted work may be another agent's).
- Status in `../docs/30` / `../docs/37` (and the `rsdkv4-ps1-port` skill).
- Game data: `../Sonic2_Extracted/` (the user's Android/iOS `Data.rsdk` "RSDKvB", extracted `Data/`,
  `Bytecode/`). Never commit game data.
- `../nugget` stays plain upstream. Don't read or use the Xbox RSDKv4 port (incomplete).

## User decisions (2026-09-25)
- Fresh upstream clone + private fork; the `ps1/` layer and tools come from `RSDKv3-ps1` (`1d1cd68`; older docs cite
  `1e583ad`, the same commit before a history rewrite).
- All characters: Sonic, Tails, Sonic & Tails, Knuckles.
- Icons: `../Memcard icons/sonic-2-loading.gif` (loading; Sonic 1 `sonic-1-loading.gif`), `2-mc-icon.png` (save); the
  builders credit them as fan-made pixel art, author unknown.
- Carried over: 320 px, 2 MB, golden rule, **no frame skip**, byte-exact evidence per step, plans approved
  before implementing.

## Build / run / debug
```bash
make -j8                        # rsdkv4.ps-exe (+ .elf, .map): the engine; must fit 2 MB
make -j8 BOOT=Zone01 [BOOT_ID=2] [BOOT_PLAYER=0..3]   # start in a stage (0 Sonic, 1 Tails, 2 Knuckles, 3 Sonic & Tails);
                                # the flags stamp (build/flags.txt: PROFILE, SAMPLER, BOOT, BOOT_ID, BOOT_PLAYER, GAME,
                                # COMBO, GAP, PERF_SPLIT) deletes the objects when they change: no touch needed (the
                                # Makefile's BOOT comment still says to touch them). The EXE is always re-made (.PHONY)
make -j8 BOOT=Special BOOT_ID=1..8              # Sonic 2's special stages 1-8 (GameConfig entries)
make BOOT=<stage> COMBO=1|2 [GAP=n]             # 1-up + Sign Post (1) / Super (2) test without a debugger (RetroArch
                                                # cores); make dist refuses it
make PERF_SPLIT=1 ...                           # docs/43 frame-split counters (GPU tail, flip gap, sections); harness only
make PLATFORM_TEST=1            # the phase 0 smoke-test app (ps1/platform_test.cpp)
tools/data_disc.sh              # = tools/build_assets.py --src ../Sonic2_Extracted (the builder's pipeline) + make disc;
                                # game disc from ../Sonic2_Extracted (Data/ + Bytecode/, no sources / menus / ZoneM)
tools/export_builder.sh ../Sonic-2-PS1-ISO-Builder   # the public builder's builder/ + bin/ (natural build; writes
                                # bin/README.md, SHA256SUMS and tools/disc/exe_name.txt)
make dist                       # natural build only: licensed (../LICENSE.DAT by path, LICENSE=none for none), verified
                                # build/dist/Sonic2-PS1.bin/.cue + SHA256SUMS (reproducible: same hashes on a rebuild)
                                # (data_disc.sh also builds manifests, atlases per variant, tile pages)
# build/iso holds ONE game's data: GAME=1 tools/data_disc.sh before a Sonic 1 disc, GAME=2 tools/data_disc.sh after it
# (leave the tree on Sonic 2). make dist GAME=1 -> Sonic1-PS1.bin booting SONIC1.EXE
tools/vram_test.sh Zone01_st [OUT] [F]        # BOOT= build: tile pages + atlas + palette in VRAM == the files
[MASK=8 FROM=240] tools/frame_capture.sh N OUT                  # both framebuffers as PNGs at frame N + renderer counters
tools/input_test.sh [OUT] [F]   # every pad-1 button through the real pad path (tools/pad.lua) -> its engine input
tools/boot_test.sh [OUT] [F1] [F2]            # headless boot over GDB: loads, fps, heap, VM/array guards
[GAME=1] tools/special_entry_test.sh OUT [POS] [SP]   # natural build: menus -> a zone -> the special stage, as a player
                                              # gets there (a BOOT=Special build starts on a clean heap); fails on an
                                              # arena-less frame. Run it after any RAM / code-size change
tools/logic_test.sh [OUT] [S0 S1 S2 S3]       # BOOT= build: run right, jump, land (g_ps1PadTestMask) + dumps (guards and
                                              # audio counters as files: OUT_guards.bin / OUT_audio.bin)
GAME=1 tools/zone_sweep.sh OUT Zone01:0 ...   # per FOLDER:PLAYER a BOOT= build + logic test, one line each (restores make)
make BOOT=X SAMPLER=1 && tools/pc_profile.sh OUT [START END]   # CPU profile by PC sampling (root counter 0 IRQ records
                                              # EPC, ps1/pc_sampler.cpp): share per function; not for make dist;
                                              # CHASE=1 = perf_scene.sh's input, PRE='gdb lines' before the game
[EMU=ds] [GPU="f.."] [KEEP_RAM=1] [JUMP=0] tools/perf_scene.sh OUT [FROM TO]   # BOOT= build: scripted chase, window CPU
                                              # hblanks / vsyncs; GPU= prices the chain (tools/gpu_cost.py), KEEP_RAM=1
                                              # keeps the RAM dumps, JUMP=0 = Right only (Sonic 1's special stage,
                                              # window 300-1500); a PERF_SPLIT=1 build adds the SPLIT line
python3 tools/gpu_cost.py RAM.bin HEAD [TAIL ...]   # DuckStation's GPU model on a frame's chain (clipped to the screen)
make BOOT=X PROFILE=1 && tools/obj_profile.sh X [OUT F1 F2]   # per-object-type CPU ticks + VM instruction counts
tools/menu/menu_run.sh "F:MASK,..." "F,..." OUT  # menu runs: pad masks at frames, both framebuffers captured
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
**Emulators** (user 2026-10-16):
- Default: the **stock PCSX-Redux app** (`/Applications/PCSX-Redux.app`), `-2mb`, on this port's own portable instances
  via `tools/redux_env.sh` (`7b6d59b`): Sonic 2 `../pcsx-redux-s2perf` (GDB 3342, web 8102), `GAME=1` Sonic 1
  `../pcsx-redux-s1perf` (3343 / 8103); OpenBIOS, muted, fresh cards each run. 2026-10-09..16 it was the patched
  `../pcsx-redux-16mb/redux16mb` copy in stock mode (now the GTA III project's); never `-8mb` / `-16mb` / `-2mbvram`.
  `redux_busy` refuses to start while the ports are taken; scripts kill only the PID they started, never `pkill`.
  Launch emulators only from bash scripts (the Bash tool is zsh). Background + timeout; check the run really started.
- DuckStation (`tools/emu_ds.sh`, `EMU=ds`; SCPH-101, GDB 2345): allowed whenever no other session runs it (`pgrep`
  first, exit 2; never a second instance), muted, own PID only, `settings.ini` backed up and restored by the script.
  Pass the build's flags to `make disc` before an `EMU=ds` run.
- GDB: Homebrew gdb 18.1 can't attach to PCSX-Redux's stub ("invalid remote ptid: p1.t1"): every harness script runs it
  through `tools/gdbw` + `tools/gdbrelay.py` (`eecb654`, `fa85339`; `GDB=` overrides). New `-x` scripts: a plain
  `target remote localhost:N` line (the one `gdbw` rewrites). Evidence rules: `../docs/21`. Start PCSX-Redux **without
  `-run`** when GDB sets breakpoints before the first `continue`. Use line breakpoints (`file.cpp:N if cond`) rather
  than watchpoints. GDB convenience variables must not be register names (`$v0`, `$s0`…).

## docs/43 fix pass (2026-10-16/17)
Results per issue: `docs/perf-43-report.md` (log: `docs/perf-43-tracker.md`; hardware facts: `docs/ps1-perf-findings.md`).
New in RSDKv4-ps1: `ps1/pad.cpp` (psyqo's AdvancedPad driver for port 1 only), flip-bit tile sprites after a boot
self-test (`PS1DetectGpuVersion`; GDB `g_ps1FlipRectsOn` / `g_ps1FlipRectsForce`), the special-stage Bomb native
(`PS1SpecialBomb`), the ordering table's rare paths out of line. Harness: `tools/perf_scene.sh` `SHOTS=` (VRAM + entity
table per frame) with `tools/shots_cmp.py`, `RUN_EXE` / `RUN_ELF`, `DISC=` (reference on the pre-patch disc), `HOLD=`,
`PAD="f:mask ..."`, `MID=` / `POST=` (GDB lines at the window's start / end), a retry when GDB loses the race.

## Layout
| Path | Role |
|---|---|
| `RSDKv4/` | the engine (upstream decompilation) + PS1 code behind `RETRO_PLATFORM == RETRO_PS1` (`main.cpp` = the psyqo app, `RetroEngine::RunFrame`; arrays sized from the data, `../docs/30` phase 2 appendix) |
| `RSDKv4/NativeObjects/` | the 2013 menus as native C++ objects (drawn by `RSDKv4/Renderer.cpp`): not run on the PS1 (phase 1 decision: menus rebuilt in 2D, `ps1/menu.cpp`); its headers are copied into the builders (`tools/export_builder.sh`) for `tools/rsdkmanifest` |
| `ps1/` | PS1 platform layer (see `ps1/README.md`); `render.cpp` = the GPU renderer (phase 3, from Sonic CD; tiles from the packed chunk table), `tileset` / `sprite_atlas` = stage-load VRAM uploads; `audio.cpp` = RSDKv4/Audio.cpp on the SPU (per-stage SFX banks, phase 5.1) + music (5.2); `menu.cpp` = the 2D menus; `platform_test.cpp` = phase 0 smoke test (`make PLATFORM_TEST=1`) |
| `tools/disc/` | mkpsxiso BIN/CUE builder (file index, two mastering passes) + byte-level verifier |
| `tools/loading/`, `tools/memcard/` | loading / save icon builders and checks |
| `tools/smoke_*.sh`, `vramshot.py`, `pad.lua`, `frame_capture.sh`, `boot_test.sh`, `perf_scene.sh`, `pc_profile.sh`, `obj_profile.sh`, `menu/` | harness |
| `tools/redux_env.sh`, `tools/emu_ds.sh`, `tools/gdbw`, `tools/gdbrelay.py` | emulator instances and the GDB 18 relay |

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
Builds cleanly · fits 2 MB (and `tools/special_entry_test.sh` after RAM / code growth) · `GAME=2` EXE byte-identical
unless Sonic 2 changes · verified in the emulator with evidence · `../docs/30` / `../docs/37` updated · commit (prefixes
`feature:` `fix:` `chore:` `housekeeping:` `FMV:`) and push to `fork ps1` only when the user asks; no Claude / AI
attribution, no `Co-Authored-By` line, here or in the builders.
