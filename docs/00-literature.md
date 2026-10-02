# 00 - Literature check (2026-10-02)

Web research done on 2026-10-02. Reddit and the Emulation General wiki returned 403 and could not be
checked directly.

**Conclusion: no Zodiac emulator exists, released or in development.** The closest work is the
Zodiac DAL inside Dmitry Grinberg's rePalm, which is not an emulator.

| Project | Status | Zodiac relevance |
|---|---|---|
| PersianTree (meepingsnesroms) | 7 commits, Jul 2016, dead | `palmos/astrology.cpp`: Tw* trap numbers, `TwLoadModule` is a stub that aborts. ARM emulation broken. `Notes.txt`: "Tapwave zodiac uses CFL3 as application compression". |
| NewPalmEmu2017 (meepingsnesroms) | last push 2019-08 | `zodiacapi.h`; repo mirrors the Tapwave 1.1 SDK |
| PumpkinOS (migueletto) | active (push 2026-10-02) | no Tapwave code, no issues mentioning it |
| CloudpilotEmu | active (2.2.2, 2026-07; commits 2026-10-01) | OS5 = Tungsten E2/"E3" on uARM. Unreleased: "paravirtualized rePalm OS5 platform". No Tapwave work. |
| Mu / libretro Mu | Dragonball only | none |
| MAME | no Tapwave driver; MC9328MX1 only in a skeleton `sony/librie.cpp` | none |
| QEMU | no i.MX1 | none |
| **rePalm** (dmitry.gr, sources tarball 2023-09) | Palm OS 5 port with its own DAL | `myrom/dal/zodiac/` (~3.5k lines: zodGfx.c, zodInput.c, zodSound, zodSecurity, ...), off by default (`ZODIAC_SUPPORT=no`). Writeup: the TAL/TwMidi/Multiplayer libraries are thin wrappers; the real work is in a DAL with 373 entrypoints (all but 14 identified). The GPU is 2D-only RGB565 with 8 MB (blit, alpha, bilinear scaling, lines, rects, points), reimplemented in software. "A few games ran". TwMidi is a stub. |

## Hardware documentation
- i.MX1 reference manual (MC9328MX1RM.pdf, 17.6 MB) is public on nxp.com.
- No public datasheet or register reference for the ATI Imageon W4200. It may be related to the W100
  (Linux `w100fb.c`), which is unconfirmed.
- No public writeup on the Zodiac boot process or memory map.

## ROMs
- palmdb's device ROM collection has no Zodiac ROM. palmdb hosts the x86 simulator build
  (`Tapwave_Zodiac.zip`).
- **The real device ROM was in plain sight**: `zodiac_1.1_update.prc` (tapwave.net) and the SDK's
  `Revert.prc` each embed a gzipped 8 MB ARM ROM image (`ROMZ` resource). See
  [01-material-survey.md](01-material-survey.md).

## Open-source Zodiac code (test cases and reference)
- spiffcode/hostile-takeover (Warfare Incorporated; has `inc/TwGfx.h`, `TwTraps.h`)
- ScummVM PalmOS backend `zodiac_gfx.cpp`
- jarikomppa/paintforge (X-Forge)
- duhone/BladeofBetrayal
