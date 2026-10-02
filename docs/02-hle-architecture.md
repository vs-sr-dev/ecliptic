# 02 - HLE architecture

Ecliptic runs the ARM code of Tapwave Native Applications (TNA) on Unicorn and reimplements the OS
on the host, at the `struct TwGlue` boundary (see [01-material-survey.md](01-material-survey.md)).

## Emulated address space

| Range | Size | Contents |
|---|---|---|
| `0x00000000` | 64 KB | zero-filled "low memory". Palm OS 5 devices map it, and games read through NULL (Tony Hawk 4). |
| `0x0FE00000` | 2 MB | ARM stack |
| `0x10000000` | 128 KB | "system area": tables that hardened DRM checks expect in `0x10000000-0x1001FFFF` |
| `0x10020000` | 256 KB | master pointer table (MemHandle = address of a slot, `*h` = chunk) |
| `0x10060000` | ~127 MB | heap: MemPtr/MemHandle chunks, TNA code, resources of loaded PRC/PDB files |
| `0x20000000` | 8 MB | **device ROM image**, at the same address as on the Zodiac (pointers inside the ROM are absolute `0x20xxxxxx`). Our code thunks go in its unused last 4 KB. |
| `0x30000000` | 8 MB | "video memory" (Imageon W4200): framebuffer at the start, then TwGfx surfaces |
| `0x7F000000` | 64 KB | trap page |

Verified on the ROM: database headers and resource pointers are absolute `0x20xxxxxx` addresses.
Verified on MTX Mototrax: its hardened DRM code checks that `TwSecGetFunctions()` returns a table
in `0x10000000-0x1001FFFF`, with function pointers in `0x20000000-0x207FFFFF`.

## Calling the OS

- Each of the 612 `TwGlue` slots points at `TRAP_BASE + 4*index`. Every trap holds `bx lr`.
- A `UC_HOOK_CODE` hook limited to the trap page runs `glue_dispatch(index)` before the `bx`
  executes. The handler reads its arguments AAPCS-style (r0-r3, then the stack, via `Args`) and
  writes r0.
- The dispatcher **must also set PC = LR**. A handler that calls back into ARM code (`cpu_call`,
  which is a nested `uc_emu_start`) makes Unicorn resume at the trap address. Without the explicit
  return, the hook would fire a second time with r0 overwritten. The earlier `svc` design suffered
  from the same problem in another form: a stale PC in the interrupt hook.
- Extra traps (`glue_extra_trap`) provide host callbacks that are not glue slots: Call68KFunc and
  the TwSec table functions.
- Handlers are registered with the `HLE(Name)` macro, which matches the slot name generated from
  the SDK header into `src/glue_table.inc` (`tools/gen_glue.py`).

## Application start

1. Load the ROM databases, load the PRC, then gunzip `ARMZ` #1 (or take `ARMC` #1) into the heap.
2. Call the entry point with `(NULL, TwGlue*, call68KFunc)`. The application relocates itself and
   stores the table.
3. Call the entry point with `(emulState, NULL, call68KFunc)`. This runs `PilotMain(normalLaunch)`.

The 68k stub (`code` #1) is **not** executed (see "Known gaps").

## Notable HLE behaviours (all verified against games)

- `TwAppStart` queues `frmLoadEvent`, `frmOpenEvent` and **`winEnterEvent`**. X-Forge titles do not
  render until their window gets focus.
- `winDisplayChangedEvent` (0x4101) is posted after `SysSetOrientation`, `PINSetInputAreaState` and
  `StatHide`. Xploids waits for it before it starts.
- `TwGetDBDataDirectory(NULL, ...)` refers to the running application. It returns
  `/PALM/Programs/<dbname>-<CRID>` with **no trailing slash**, because callers append "/". A name
  that already ends in `-<CRID>` is not suffixed again (StreetHoops-STRT). A `Twdp` #0 resource
  overrides the path.
- `VFSVolumeEnumerate` writes volRefNum 0 when it runs out of volumes. `VFSDirEntryEnumerate`
  reports the end of the list one call late, as the FAT driver does. X-Forge's file index depends
  on both.
- SndStream callbacks are ARM functions. They are pulled on the emulation thread, but only when no
  TwSemaphore is held, typically right after the game releases its last mutex.
- `SysSetOrientation(portrait)` switches the framebuffer to 320x480 (Firehammer).
- Thumb `bx pc` veneers at addresses = 2 mod 4: the ARM920T masks the misaligned ARM target and
  QEMU does not. Each such target gets a one-address code hook that realigns PC.
- DRM (`TwSecGetFunctions`): every signature check passes. The table and the function thunks are
  placed in the address ranges that hardened games check.

## Host side

SDL2 provides the window, keyboard, mouse and audio. The window scales the 480x320 (or 320x480)
RGB565 framebuffer.

Audio is mixed at 44.1 kHz stereo from SndStreams and SndPlayResource WAVs. TwMidi/TwSmf use the
Windows GS synth through winmm, sequenced on a host thread. TwJpg uses SDL2_image. The game's
zlib calls go to the host zlib, mirroring the ARM `z_stream`.

Keyboard: arrows = stick and 5-way, Enter = stick press, I/L/K/J = action A/B/C/D (also Z = C,
X = B), Q/E = L/R triggers, Tab = Function, Esc = Home. The mouse acts as the pen.
