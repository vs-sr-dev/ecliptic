# 01 - Material survey (2026-10-02)

Source: `tapwave.net-master.zip` (a mirror of the tapwave.net community archive, 501 files, ~400 MB).
Extracted to `archive/` (game/homebrew zips) and `ref/` (SDKs, HDK, ROM images). Tools in `tools/`.

Legend: **[V]** verified by inspecting the files, **[I]** inferred.

## ROM images

| File | Source | Size | MD5 |
|---|---|---|---|
| `ref/roms/zodiac_1.1.rom` | `ROMZ` #2 (gzip) inside `zodiac_1.1_update.prc` (creator `fins`, `tver` "1.1") | 8,192,000 (0x7D0000) | 76666a023b887c371543ebbbc0cd53fb |
| `ref/roms/zodiac_1.0_revert.rom` | `ROMZ` #1 (gzip) inside SDK `ROMs/Revert.prc` (creator `twfu`, `tver` "1.1b15") | 8,257,536 (0x7E0000) | b9afd11a274625d6d555ab18269054f3 |

- [V] Both start with an ARM branch (`c80000ea`), the Palm card signature `0xFEEDBEEF`, card name
  `PalmCard`, manufacturer `Tapwave, Inc.`; the 1.1 header carries the build string `3/24/04 17:12:06`.
- [V] They contain the usual Palm OS 5 ROM databases (`SmallDAL`/`psys`, `Boot`, `DAL`, `Palm OS Data`,
  Bluetooth stack, etc.). Full directory parse: TODO.
- [I] These are the OS ("big ROM"/small ROM) images the updater flashes; the low-level bootloader may
  not be included. Not needed for an HLE approach; they are a reference for disassembling the real
  implementations of the Tapwave APIs.
- [V] The SDK also ships `Simulator.rom`/`SimulatorDbg.rom` (3.7 MB) for the x86 **Palm OS Simulator**
  (`PalmSim/PalmSim.exe` + DLLs incl. `TwMidi.dll`, `TwMultiplayer.dll`). TNA games only ship ARM code
  (`SIMC` resources, when present, are 29-byte stubs), so the simulator cannot run commercial games.

## SDKs

- [V] `Tapwave 1.1 SDK` (04/02/04, `TAPWAVE_API_VERSION 0x0205`, CodeWarrior 9.3) and
  `Tapwave R10 SDK` (pre-release, `TAPWAVE_API_VERSION 0x0100`, CodeWarrior 9.2 beta).
- [V] Headers: `Incs/TapWave/Tw*.h` (TwGfx, TwInput, TwSound, TwMidi, TwJpg, TwMp, TwHighScore,
  TwDevice, TwSecurity, TwRuntime, TwGlue...), full Palm OS 5 R2-era headers in `Incs/`.
- [V] PDF docs: TapWaveProgrammerReference, TwGfx, TwSnd, TwMidi, TwJpg, TwMp, HighPerformanceGames,
  BestPractices, DigitalRightsManagement, DeveloperOverview.
- [V] Samples with full source (FastGame, CompatibleGame, XMatrix, Zing, Rumbler, TwMpTest...),
  X-Forge engine kit (used by Stuntplane, Orbz, Firefly...).
- HDK: CAD models, connector pinout, a 5-page electrical PDF (image only, no text layer).

## Tapwave Native Application (TNA) format and ABI

- [V] A TNA is a PRC with a tiny 68k stub (`code` 0/1, ~1.4 KB) plus the ARM code in `ARMC` #1, or
  `ARMZ` #1 = gzip of `ARMC` (see `TwRuntime.h`). The stub's PilotMain calls
  `TwLoadModule(0,0,0,1,twLoadFlagTNA|twLoadFlagQuickRun,&entry)` via trap `sysTrapOEMDispatch2`
  (0xA443) with selector in D2 (`TwTraps.h`).
- [V] ARM code header (`Samples/FastGame/Src/Startup.cpp`): offset 0 = `b Startup`, 28 = `'TWNA'`
  (bytes `ANWT`), 32 = version 1, 36 = `'cdwr'`, 40..68 = CodeWarrior section/relocation markers
  (`__DataStart__`, `__RODataStart__`, `__BSSStart__`, `__BSSEnd__`, code/data reloc start/end).
- [V] Entry: `__ARMlet_Startup__(emulStateP, userData68KP, call68KFuncP)`.
  Called first with `emulStateP == NULL` and `userData68KP = struct TwGlue*`: the app stores the
  table and self-relocates. Called with non-NULL `emulStateP` it runs `PilotMain(normalLaunch)`, and
  can call 68k code through `call68KFuncP`.
- [V] **All OS calls from TNA code go through `struct TwGlue`**, a table of 612 function pointers
  (Dm*, Evt*, Exg*, Fnt*, Frm*, Mem*, Win*, Bmp*, Snd*, Sys*, VFS*, Bt*, zlib, BSD sockets, TwGfx*,
  TwInput*, TwSnd*, TwMidi*, TwJpg*, TwMp*, ...) followed by `UInt32 TwGlueReserved[64]`
  (`Incs/TapWave/TwGlue.h`). Usage is `#define DmOpenDatabase twGlue->DmOpenDatabase_`.
- [V] The R10 table (`struct TapWaveGlue`, 521 entries) is an exact prefix of the 1.1 table (only
  `close_` renamed `closesocket_` at the same index), so one ABI covers all TNA titles.
- [V] `SysTicksPerSecond()` is hard-wired to 1000 for TNA code.

## Software in the archive (`tools/survey.py`)

**TNA (ARM code with `TWNA` header)**: Tony Hawk 4 (ARMZ 2.0 MB), Spy Hunter, MTX Mototrax,
Terminator 3, Tomb Raider (tr2), Doom, Duke Nukem, Hockey Rage, Street Hoops, Orbz, Firefly,
Galactic Realms, Stuntcar Extreme, Thruster, Xploids, Zyrian, GeoPod XE, Gloopz, Firehammer,
Animated Dudes, Another World, Origo Prologue, Outpost, PaintForge, Noiz2sa, Spout, Orbital Sniper,
Zodtris, ZapEm, ZCards, Stuntplane (X-Forge sample), FrodoZ, the seven LJZ emulators.

**Generic Palm OS (68k, sometimes with ARMlets `ARMC`/`ARML`/`armc`)**: Bejeweled 2, Zuma,
Insaniquarium, SimCity, Space Trader, GTS Racing, Hellfire, Table Tennis 3D, Plazmoids, Snails,
Gamebox Classics, Cybersaurus, Head2Head, 3D Zapman, Interstellar Flames, Shippy, Edge, Bike or Die,
Madden 2005 (68k app + `dat0` resource DB), zHexen/zQuake loaders, TCPMP, utilities.
These need a 68k (PACE) path and a broader Palm OS implementation.

Large data files live on the SD card (e.g. `TonyHawk-THSL/THSL.dat` 15 MB, `spyhunter-Fspy/*.cfl`,
`Tomb-Raid/*`), accessed via VFS.

## API surface estimate (heuristic, `tools/glue_usage.py`)

- Slot names: `tools/twglue_names.txt` (612 entries, index = byte offset / 4).
- [I] Static scan of `ldr Rt,[Rb,#imm]` followed by `bx/blx Rt`. Small offsets (< ~100 slots) are
  polluted by C++ virtual calls; above that the hits look genuine. Tony Hawk 4: ~90 plausible slots
  above index 100 (VFS file I/O, SndStream*, TwInput*, TwGfx* incl. TileBlt/DrawPalmBitmap,
  TwHighScore*, BtLib* for multiplayer, TwSecGetFunctions for DRM, Win/Frm/Evt basics).
- [I] Expect ~100-150 real entries per game; a runtime `--trace` will give the exact list.
- DRM: games call `TwSecGetFunctions` (see `TwSecurity.h`, `DigitalRightsManagement.pdf`), which the
  HLE must satisfy.
