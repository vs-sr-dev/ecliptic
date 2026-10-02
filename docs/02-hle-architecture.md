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
| `0x10060000` | ~127 MB | heap: MemPtr/MemHandle chunks, TNA code, 68k globals and stack, resources of loaded PRC/PDB files |
| `0x20000000` | 8 MB | **device ROM image**, at the same address as on the Zodiac (pointers inside the ROM are absolute `0x20xxxxxx`). Our code thunks go in its unused last 4 KB. |
| `0x30000000` | 8 MB | "video memory" (Imageon W4200): framebuffer at the start, then TwGfx surfaces |
| `0x7F000000` | 64 KB | trap page: TwGlue slots, extra traps, r9 module tables (ids `0x1000+`), 68k return stop (`+0x8000`) |

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

1. Load the ROM databases, then the PRC.
2. Run the application's 68k `code` #1 on PACE-lite (below), as Palm OS does. TNA launch stubs call
   `SysAppStartup`, set features, run their compatibility checks and finally
   `TwLoadModule(0, 0, 0, 1, twLoadFlagTNA | twLoadFlagQuickRun, &entry)` (TwDispatch selector 388).
3. `TwLoadModule` gunzips `ARMZ` #1 (or takes `ARMC` #1) into the heap and calls the entry point
   with `(NULL, TwGlue*, call68KFunc)`: the application relocates itself and stores the table.
   With QuickRun it then calls `(emulState, NULL, call68KFunc)`, i.e. `PilotMain(normalLaunch)`.
   Without QuickRun the stub calls `PceNativeCall(entry, userData)` itself (Tony Hawk 4).
4. If the 68k code never starts the native code (or crashes first), the native code is started
   directly as before. `--no-68k` skips step 2.

## PACE-lite (68k)

A second Unicorn engine (`UC_ARCH_M68K`, 68040 model, big endian) maps the same host buffers as
the ARM engine, so both CPUs see one address space. `trap #15` arrives as interrupt 47 with PC on
the trap instruction; the handler reads the trap word, runs the host implementation and resumes at
PC+4. No exception frame is pushed. Results go to both D0 and A0 (scratch registers across traps).

- Arguments are read from the 68k stack in CodeWarrior order (first argument at SP, bytes pushed
  as words). Out-parameters are written big endian; handlers that reuse an ARM-side HLE function
  pass it scratch buffers and convert the results.
- `SysAppStartup` builds a 68k `SysAppInfoType`, allocates the A5 world from `code` #0 and sets A5.
  The startup code then decompresses `data` #0 itself.
- `Call68KFunc` (the callback TNA code receives) is generic: a trap number runs the host trap on a
  copy of the arguments; a function address runs 68k code with the arguments copied to the 68k
  stack and a return address that stops emulation. Nested calls (68k -> ARM -> 68k) work.
- `TwSecGetFunctions` from 68k code returns its own table: big-endian pointers to 68k stubs
  (`trap #15; dc.w 0xAF0n; rts`) handled by private pseudo-traps.
- Trap and TwDispatch selector names for logs come from `src/traps68_table.inc`
  (`tools/gen_traps68.py`, generated from the SDK headers).

Enough of Palm OS is implemented for the TNA launch stubs and small launchers (Another World's
creates folders through the VFS Manager). Applications with forms (the LJZ launcher) need a Form
Manager, which does not exist yet.

## System calls outside TwGlue

**r9 module tables.** Palm OS 5 native code reaches the system modules with
`ldr ip, [r9, #-4n]; ldr pc, [ip, #4i]`: r9 points past the dispatch tables of DAL (`[r9-4]`),
Boot (`[r9-8]`) and UI (`[r9-12]`). r9 is set to such a block before any native code runs; every
entry is a trap named `DAL#146` etc. The few entries games use are implemented in `src/dal.cpp`,
named from how the games use them (only Another World so far): DAL mutexes (146-149, sharing the
TwSemaphore state), DAL timers (169-171, fired at safe points like the audio pump), Boot 518 (queue
an event), Boot 726 (`WinEraseWindow`).

**ROM entry points.** Homebrew titles unlock TwGfx, which the 1.1 ROM restricts to signed
applications: they copy the prologue of the ROM's `TwGfxOpen` and jump past its caller check to
`0x200995F0`. A code hook there runs our `TwGfxOpen(r6, r7)` and resumes at the ROM function's
own epilogue (`0x200995E4`). See `kRomEntries` in `src/rom.cpp`.

## Window Manager and fonts

`src/win.cpp` keeps the draw state (indexed and RGB colours, draw mode, push/pop) and draws onto
the Palm display surface; coordinates follow `WinSetCoordinateSystem` (`kCoordinatesNative` = 108
dpi on the Zodiac). `src/font.cpp` uses the device's fonts: the ROM's `afti` #32000 maps FontIDs
to `afnx` resources (Latin Locale Module: std, bold, large, largeBold; Boot: symbol fonts). They
are little-endian extended fonts with a 72 and a 144 dpi strike; text on the 1.5x screen is
sampled from the 144 dpi strike. The offset/width table holds `{offset, width}` byte pairs.

## Notable HLE behaviours (all verified against games)

- `TwAppStart` queues `frmLoadEvent`, `frmOpenEvent` and **`winEnterEvent`**. X-Forge titles do not
  render until their window gets focus.
- `winDisplayChangedEvent` (0x4101) is posted after `SysSetOrientation`, `PINSetInputAreaState` and
  `StatHide`. Xploids waits for it before it starts.
- Time: `TimGetTicks` and `TwGetMicroSeconds` count from a reset one minute before the game
  starts. Zodiac Card Casino waits until `TimGetTicks() >= 5000` on its splash screen.
- The window is presented at most every 16 ms from the TwGlue dispatcher, so games that only
  write the framebuffer (Noiz2sa, Spout) or busy-wait on the clock still update the screen.
- Full-screen updates of the display (a blit covering at least 3/4 of it) are capped at 60 per
  second (`--no-frame-cap` to disable). Outpost has no working frame limiter: it mixes
  `TimGetTicks` (ms) and `TwGetMicroSeconds` (us), so on the device only the hardware held it back.
- Quitting works like the Home button: closing the window hides it at once, posts keyDown
  `vchrLaunch` (several games quit on it, e.g. Tomb Raider), then `appStopEvent` after 1 s if
  nobody passed the key to `SysHandleEvent`. The application gets 3 s to quit by itself, then
  the CPU is stopped. Games that loop without OS calls (Spout) or ignore the stop in sub-loops
  (Card Casino) end that way.
- Features: `sysFtrNumROMVersion` is `0x05273002` (Palm OS 5.2.7, from the 1.1 ROM; the 1.0 ROM
  has 5.2.6), OEM company `'Tpwv'`, device `'Rdog'`. Launch stubs check the version.
- The SD card volume always exists (the Zodiac has an internal card): a missing card directory is
  created empty. FrodoZ quits when no volume is found.
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
