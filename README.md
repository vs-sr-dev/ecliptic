# Ecliptic

An emulator for the **Tapwave Zodiac** (2003-2005), the Palm OS 5 handheld game console. To our
knowledge it is the first emulator for this device.

Ecliptic is a high-level emulator. It runs the ARM code of *Tapwave Native Applications* (the
format used by the Zodiac's commercial games) on [Unicorn](https://www.unicorn-engine.org/), and
reimplements on the host the OS interface those applications use: the 612-entry `TwGlue`
function table (Palm OS data/memory/event/VFS managers, TwGfx, TwInput, TwSnd, TwMidi, TwJpg,
zlib...). Each application's 68k launch code runs on a small 68k Palm OS layer ("PACE-lite",
Unicorn m68k), so launch stubs, feature checks and `Call68KFunc` callbacks behave as on the
device. Text uses the device's own ROM fonts. See [docs/](docs/) for the research notes and the
architecture.

## Building (Windows, MSYS2 mingw64)

```
pacman -S mingw-w64-x86_64-{gcc,cmake,ninja,SDL2,SDL2_image,zlib,unicorn}
cmake -S . -B build -G Ninja
ninja -C build
```

## Running

```
build/ecliptic.exe GAME.prc --card CARD_DIR [--rom zodiac_1.1.rom]
```

- `GAME.prc`: a Tapwave Native Application.
- `--card`: a host directory used as the SD card (created empty if missing). Game data goes in
  `PALM/Programs/<name>-<CRID>/`.
- `--rom`: an optional Zodiac ROM image, which provides the system databases. The 1.1 ROM is the
  gzipped `ROMZ` resource inside Tapwave's `zodiac_1.1_update.prc`.

Controls: arrows = analog stick / 5-way, Enter = stick press, I/L/K/J = action buttons A/B/C/D,
Q/E = triggers, Tab = Function, Esc = Home, mouse = pen.

Testing options: `--headless`, `--exit-after MS`, `--screenshot FILE.bmp`,
`--keys "R,left,A"` (`--key-start`, `--key-interval`), `--trace`, `--blocks FILE`,
`--no-68k` (start the native code directly, skipping the 68k launch code).

Development helpers (expect the local `games/` layout, not part of the repository):
`tools/play.py SLUG`, `tools/smoke.py`, `tools/mkcard.py`, `tools/contact.py`.

## Bring your own assets

This repository contains only source code and documentation. No games, ROMs, SDK files or
screenshots of copyrighted software are included: you need your own legally obtained copies.

## License

MIT, see [LICENSE](LICENSE). Tapwave, Zodiac and Palm OS are trademarks of their respective owners;
this project is not affiliated with them.
