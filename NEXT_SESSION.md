# Ecliptic - prompt per la sessione 2

Copia il testo qui sotto come primo messaggio della nuova sessione.

---

Ciao! Riprendiamo **Ecliptic**, l'emulatore HLE del **Tapwave Zodiac** (`D:\Homebrew6\Ecliptic`).

## Stato a fine sessione 1 (2026-10-02)

- Documentazione in `docs/`: `00-literature`, `01-material-survey`, `02-hle-architecture` e
  `03-status`, quest'ultimo con la tabella di compatibilità e i limiti noti. Leggila prima di tutto.
- **11 giochi confermati a mano**: Zodtris, Doom II, Duke Nukem Mobile, Spy Hunter, Terminator 3,
  Tony Hawk 4, MTX Mototrax, Firehammer, Stuntcar Extreme, Tomb Raider, Xploids. PaintForge funziona.
- Altri ~15 titoli arrivano al menu e devono ancora essere provati a mano (vedi `docs/03-status.md`).
- Strumenti:
  - `tools/play.py SLUG` lancia un gioco (`--list` per l'elenco);
  - `tools/smoke.py [secondi]` fa un test headless di tutti i TNA;
  - `tools/contact.py` crea il provino degli screenshot;
  - `tools/mkcard.py` costruisce le SD virtuali in `cards/`.
- Debug: `--trace` (chiamate TwGlue), `--blocks FILE` (ultimi basic block eseguiti),
  `ECLIPTIC_DUMP_SURFACES=1`, `tools/disasm.py`, `tools/xref.py`, `tools/dis68k.py`.

## TODO della sessione 2

1. **Repository GitHub**: crearlo e fare il push, in **due passaggi separati** (prima la creazione
   del remote, poi il push). Il `.gitignore` esclude già giochi, ROM, SDK e `cards/`. Prima del push
   ricontrolla che nel repo non ci sia materiale protetto da copyright.
2. **PACE-lite (68k)**: eseguire lo stub 68k (`code` #1) con Unicorn m68k, con HLE delle trap Palm
   (FtrSet/FtrGet, WinGetDisplayWindow/WinGetBitmap/BmpGetBits, MemChunkNew, SysAppStartup...).
   Sblocca Noiz2sa e Spout (feature `PDxx` 1111/1112 più Call68KFunc), il flusso del launcher LJZ
   (feature `LJZZ` 6000+) e, più avanti, i giochi Palm 68k generici (Bejeweled 2, Zuma, SimCity...).
3. **Chiamate R9 al DAL in ROM**: Thruster, Outpost e Another World fanno
   `ldr r9,[r9]; ldr r9,[r9]; ldr pc,=0x200995f0` e saltano dentro la ROM. Bisogna capire le tabelle
   di dispatch di Palm OS 5 ARM (`[r9-4]`, `[r9-8]`...) e agganciare in HLE gli indirizzi chiamati.
4. **Window Manager e font Palm**: WinPaintChars, FntSetFont, WinPaintRectangle,
   Push/PopDrawState, usando i font della ROM (database "Boot"/"UI"). Serve agli LJZ.
5. Casi singoli: Gloopz (dati non trovati), Animated Dudes (`Dirty.cpp:127`), FrodoZ (esce subito),
   Zodiac Card Casino (cicla aprendo file da `Install/`).
6. Da verificare sulla ROM (TAL/DAL): la regola dei nomi di `TwGetDBDataDirectory`, le code
   TwInput (`TwInputRead`/`Peek`), il significato reale dei selettori 296/297 (`CircX`/`CircY`).

## Progetti collaterali (non urgenti)

- Fedeltà del MIDI: trovare e ricostruire i campioni originali dal database ROM
  "Tapwave Midi Library" (`twMD`).
- Bluetooth e multiplayer (BtLib*, TwMp*) oggi sono stub.

## Preferenze (invariate)

Si parla in italiano; la documentazione del repo e i commit sono in inglese. Niente commit se non
richiesti. Se l'utente è fuori casa, gli screenshot si mandano con SendUserFile.
