# AGENTS.md — RSDKv4-ps1 (Sonic 2 on PlayStation)

Operating manual for this repo. Workspace rules: `../AGENTS.md`. Knowledge base: `../docs/` (start at
`../docs/README.md`). **Plan and phase table: `../docs/30-rsdkv4-sonic2-port-plan.md`. Lessons from the
Sonic CD port and what carries over: `../docs/29`.**

## What this is
RSDKv4-Decompilation (Retro Engine v4, Sonic 1/2 2013) being ported to the PS1 with **psyqo** (C++20) on
branch `ps1`, focused on **Sonic 2** (Sonic 1 comes last, after the v5 port). Remotes: `origin` =
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
make -j8                        # rsdkv4.ps-exe (+ .elf, .map); must fit 2 MB. Phase 0: the smoke-test app
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
| `RSDKv4/` | the engine (upstream decompilation); PS1 code arrives in phase 2 behind `RETRO_PLATFORM == RETRO_PS1` |
| `RSDKv4/NativeObjects/` | the 2013 menus as native C++ objects (drawn by `RSDKv4/Renderer.cpp`): phase 1 research |
| `ps1/` | PS1 platform layer (see `ps1/README.md`); `ps1/platform_test.cpp` = phase 0 app |
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

## Definition of done for a step
Builds cleanly · fits 2 MB · verified in PCSX-Redux with evidence · `../docs/30` status updated · commit
(prefixes `feature:` `fix:` `chore:` `housekeeping:` `FMV:`) and push to `fork ps1` only when the user asks.
