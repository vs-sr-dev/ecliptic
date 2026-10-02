# 03 - Status (2026-10-02, end of day 1)

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
| Zodiac Card Casino | black | loops opening files from its `Install/` data, layout to be checked |
| Gloopz | error | "Could not find data installed on SD/Internal Card" |
| Animated Dudes | error | `Dirty.cpp:127` |
| FrodoZ | exits | quits right after start, to investigate |
| LJZ emulators (7) | partial | need Palm Win/Fnt drawing (WinPaintChars, FntSetFont...) and features set by the 68k LJZ launcher |
| Noiz2sa, Spout | blocked | need the 68k stub: `FtrGet(crid, 1111/1112)` and Call68KFunc into a 68k `MemChunkNew` wrapper |
| Thruster, Outpost, Another World | blocked | call DAL ROM functions directly through R9 (`ldr r9,[r9]; ... ldr pc,=0x200995f0`) |

## Known gaps / next steps

1. **PACE-lite (68k)**: run the 68k `code` #1 stub with Unicorn m68k and HLE Palm traps. This
   unlocks the "PD" ports (Noiz2sa, Spout), the LJZ launcher flow and, later, generic 68k Palm
   games (Bejeweled 2, Zuma, SimCity...).
2. **R9 system globals**: work out the Palm OS 5 ARM dispatch tables (`[r9-4]`, `[r9-8]`...) from
   the ROM and HLE the DAL/Boot/UI entry points that games call directly.
3. **Palm Window Manager + fonts**: Win* drawing, FntSetFont/WinPaintChars using the ROM fonts.
   Needed for LJZ and Another World, and generally for homebrew UIs.
4. Confirm uncertain semantics against the ROM implementation (TAL): the
   `TwGetDBDataDirectory` naming rule, input axis signs, `TwGfxBlendBlt` rounding.
5. Bluetooth (BtLib*) and TwMp multiplayer are stubs.
6. Real queued TwInput (`TwInputRead`/`Peek` return EAGAIN today; only polling works).
7. MIDI fidelity: music currently plays through the Windows GS synth. Side project: find and
   rebuild the original instrument samples (the "Tapwave Midi Library" ROM database, `twMD`).
8. Frame pacing: games that do not wait on VBlank can run faster than the real device.
