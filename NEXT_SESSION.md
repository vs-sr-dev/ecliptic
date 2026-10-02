# Ecliptic - prompt per la sessione 3

Copia il testo qui sotto come primo messaggio della nuova sessione.

---

Ciao! Riprendiamo **Ecliptic**, l'emulatore HLE del **Tapwave Zodiac** (`D:\Homebrew6\Ecliptic`).

## Stato a fine sessione 2 (2026-10-02)

- Leggi prima `docs/02-hle-architecture.md` (sezioni nuove: PACE-lite, chiamate fuori da TwGlue,
  Window Manager e font) e `docs/03-status.md`.
- **PACE-lite**: lo stub 68k (`code` #1) di ogni TNA gira su Unicorn m68k (`src/m68k.cpp`) e lancia
  il codice ARM con `TwLoadModule`. Se non lo fa, si ripiega sul lancio diretto (`--no-68k` per forzarlo).
- **Sbloccati**: Noiz2sa, Spout, Thruster, Outpost, Another World (bypass di `TwGfxOpen` in ROM e
  tabelle r9), Gloop Zero, Animated Dudes, FrodoZ, Zodiac Card Casino. Tony Hawk 4 passa dal suo
  stub con DRM 68k. Nessuno di questi è ancora stato provato a mano.
- **Font della ROM** (`src/font.cpp`) e primo nucleo del Window Manager (`src/win.cpp`).
- Correzioni: versione ROM 5.2.7 (0x05273002), heap che si sovrapponeva alla tabella degli handle,
  il trace che non stampava mai i valori di ritorno, il volume SD che esiste sempre.
- `tools/mkcard.py` ora legge gli INI di Tapwave SimpleInstaller e le cartelle citate dal codice.

## TODO della sessione 3

1. **Repository**: verificare che il push su `vs-sr-dev/ecliptic` sia andato, poi committare il
   lavoro della sessione 2 (su richiesta).
2. **Prove a mano** dei titoli sbloccati (Noiz2sa, Spout, Thruster, Outpost, Another World,
   Gloop Zero, Animated Dudes, FrodoZ, Card Casino) e degli altri ~15 che arrivano al menu.
3. **Form Manager per il 68k**: form (`tFRM`), controlli, liste, campi, menu e il loop eventi 68k.
   Serve per il launcher LJZ (`ljZ.prc`, 1276 trap), che imposta le feature `'LJZZ'` 6000+
   lette dagli emulatori LJZ, e più avanti per i giochi Palm 68k generici.
4. Window Manager: finestre offscreen, `WinPaintBitmap`, linee e cornici, `WinDrawChars` per il 68k.
5. Zodiac Card Casino reagisce lentamente ad `appStopEvent`: da capire.
6. Da verificare sulla ROM: la regola dei nomi di `TwGetDBDataDirectory`, le code TwInput,
   i selettori 296/297, la mappa completa delle tabelle r9 (dalla tabella trap→ARM di PACE).

## Progetti collaterali (non urgenti)

- Fedeltà del MIDI: campioni originali dal database ROM "Tapwave Midi Library" (`twMD`).
- Bluetooth e multiplayer (BtLib*, TwMp*) sono stub.

## Preferenze (invariate)

Si parla in italiano; la documentazione del repo e i commit sono in inglese. Niente commit se non
richiesti. Repo BYOA: solo codice, nessun asset né screenshot. I comandi git e gh che i permessi
automatici bloccano (amend, creazione del repo, push) li lancia l'utente: dagli il comando esatto.
Se l'utente è fuori casa, gli screenshot si mandano con SendUserFile.
