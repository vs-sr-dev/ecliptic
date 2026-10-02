# 03 - Status (2026-10-02, end of session 2)

Measured with `tools/smoke.py 10` (10 s headless per game, virtual SD card from `tools/mkcard.py`)
plus a few manual runs with scripted keys. "Menu" means the title screen or main menu renders.
Deeper gameplay has only been checked where noted.

## Tapwave Native Applications

| Game | State | Notes |
|---|---|---|
| Zodtris | **playable** | confirmed by hand on 2026-10-02, MIDI music included |
| Spy Hunter | **playable** | X-Forge; confirmed by hand on 2026-10-02 |
| Terminator 3 | **playable** | X-Forge; confirmed by hand on 2026-10-02 |
| Tony Hawk's Pro Skater 4 | **playable** | confirmed by hand on 2026-10-02; needed realigned `bx pc` veneers and low memory |
| MTX Mototrax | **playable** | confirmed by hand on 2026-10-02 (streamed music during races); hardened DRM passes; this copy is a leaked early dev build ("NO DRM ACTIVE!" banner; the Readme says one level, no AI) |
| Doom II | **playable** | confirmed by hand on 2026-10-02. Turn acceleration is authentic. Reads the stick through `twInput4Way` and handles only cardinal values, so no diagonals (a limit of the port). Stream audio underruns fixed by deferring the mix while a variable-size callback has no data. |
| Duke Nukem Mobile | **playable** | confirmed by hand on 2026-10-02 |
| Firefly | menu | X-Forge |
| Firehammer | **playable** | confirmed by hand on 2026-10-02; vertical shmup in portrait mode, music + SFX OK |
| Galactic Realms (both versions) | menu | BtLib calls stubbed |
| Hockey Rage 2004 | menu | leaked unfinished build (the most complete of three, per the Readme) |
| Street Hoops | runs (prototype) | leaked unfinished early build per the bundled Readme: boots straight into a match with no menu/UI. Authentic. |
| Stuntcar Extreme | **playable** | confirmed by hand on 2026-10-02 |
| Stunt Plane (X-Forge sample) | menu | |
| GeoPod XE | menu | |
| Orbz | logo | |
| Origo Prologue | menu | |
| Zyrian | menu | |
| Xploids | **playable** | confirmed by hand on 2026-10-02; needed `winDisplayChangedEvent`, TwJpg, TwHighScore and the polar stick inputs (twInputNavTheta is in screen orientation, contrary to the docs) |
| ZapEm | menu | |
| Orbital Sniper | title | |
| PaintForge | **works** | 2003 preview by Jari Komppa; pen drawing OK. Tools on L trigger (Q), undo/redo on R trigger (E) |
| Tomb Raider (tr2) | **playable** | confirmed by hand on 2026-10-02 (PS1-style affine texturing/vertex jitter is authentic) |
| Zodiac Card Casino | menu | data goes straight into `/PALM/Programs/ZCards/`; the distributed copy has no `.wav` files. Slow to react to `appStopEvent` (the smoke test's watchdog stops it) |
| Gloop Zero | menu | data path is hard-coded as `GloopZero-GLPZ` (the creator is `GPZZ`); its 68k stub needs ROM version >= 5.2.7 |
| Animated Dudes | title | its DRM check (`Dirty.cpp`) wants `TwSecGetFunctions` to leave `*codeSize` alone when `codeStart` is NULL |
| FrodoZ | C64 BASIC | C64 emulator with built-in replacement ROMs; quits if no card volume exists |
| LJZ emulators (7) | message | draw "error at opening" with the ROM fonts: they need the `'LJZZ'` 6000+ features set by the 68k LJZ launcher, which needs a Form Manager. LJZ-NGP crashes after the message |
| Noiz2sa | title | 68k stub (features 1111/1112, Call68KFunc into a 68k `MemChunkNew` wrapper) + TwGfx unlock |
| Spout | title | same port family as Noiz2sa; its `Twdp` points at Noiz2sa's folder for sounds (a bug of the port) |
| Thruster | menu | TwGfx unlock (jump into the ROM's `TwGfxOpen` past its signed-app check) |
| Outpost | gameplay | TwGfx unlock |
| Another World | title | TwGfx unlock, 68k launcher with VFS calls, r9 DAL/Boot calls (mutexes, timers, `WinEraseWindow`) |

## Known gaps / next steps

1. **Form Manager** for 68k applications: forms (`tFRM`), controls, lists, fields, menus, with
   the 68k event loop. This runs the LJZ launcher (which sets the `'LJZZ'` features the LJZ
   emulators read) and is the next step towards generic 68k Palm games (Bejeweled 2, Zuma...).
2. More of the Window Manager: offscreen windows, bitmaps (`WinPaintBitmap`), lines, frames,
   `WinDrawChars` for 68k code; 68k `BmpGetBits` on real bitmaps.
3. r9 module tables: only the 9 entries Another World uses are known. A complete map would come
   from PACE's trap-to-ARM table in the ROM.
4. Confirm uncertain semantics against the ROM implementation (TAL): the
   `TwGetDBDataDirectory` naming rule, input axis signs, `TwGfxBlendBlt` rounding.
5. Bluetooth (BtLib*) and TwMp multiplayer are stubs.
6. Real queued TwInput (`TwInputRead`/`Peek` return EAGAIN today; only polling works).
7. MIDI fidelity: music currently plays through the Windows GS synth. Side project: find and
   rebuild the original instrument samples (the "Tapwave Midi Library" ROM database, `twMD`).
8. Frame pacing: games that do not wait on VBlank can run faster than the real device.
