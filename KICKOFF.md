# Ecliptic - prompt di avvio

Prima sessione della pipeline **Ecliptic**: un emulatore per il **Tapwave Zodiac** (2003-2005).
Copia il testo qui sotto come primo messaggio della nuova sessione.

---

Ciao! Inizia oggi una nuova pipeline: **Ecliptic**, un emulatore per il **Tapwave Zodiac**,
la console portatile basata su Palm OS del 2003-2005.

## Da dove arriviamo

- Una ricerca fatta con Claude Web cercava console **mai emulate** (nemmeno tentativi, o poco più) ma
  con **dump di giochi disponibili** per poter testare. Ne sono usciti tre candidati: **Gizmondo**,
  **ZAPiT Game Wave** e **Tapwave Zodiac** (quest'ultimo indicato come "meno sicuro").
  Zeebo e XaviX erano stati esclusi perché hanno già emulatori funzionanti o in sviluppo.
- **Gizmondo**: scartato. Una verifica del 2 ottobre 2026 ha trovato PocketHLE
  (github.com/j92580498-max/PocketHLE). È un emulatore HLE (Unicorn più le API di Windows CE
  reimplementate) che dalla v0.3.0 di agosto 2026 fa girare 8 giochi Gizmondo ed è in sviluppo attivo.
- **Game Wave**: fatto, nella pipeline gemella **Undertow** (D:\Homebrew6\pc-gizwiz, pubblicata su
  https://github.com/vs-sr-dev/undertow). In una giornata siamo arrivati a Sudoku e Gemz giocabili,
  con un approccio HLE: il bytecode Lua originale gira su Lua 5.0.2 e le librerie del motore sono
  reimplementate con SDL2 e FFmpeg.
- **Tapwave Zodiac**: è questa pipeline.

## Cosa sappiamo già del Zodiac

Da quella sessione, **da riverificare con una ricerca aggiornata prima di decidere qualsiasi cosa**:

- Si tratta di un PDA e console con **Palm OS 5** (la variante dovrebbe essere la 5.2T) e un
  **Motorola i.MX1** (ARM9 a 200 MHz). La GPU dovrebbe essere un ATI Imageon (W4200?): da
  verificare. I giochi usano **API proprietarie Tapwave** per grafica, input e audio, oltre a
  quelle di Palm OS; i nomi delle API vanno verificati.
- **Tentativi di emulazione noti**:
  - **PersianTree** (github.com/meepingsnesroms/PersianTree): emulatore Palm OS 5 con l'obiettivo
    dichiarato di supportare le API Tapwave. È **fermo dal 2016** (ultimo push 10/07/2016, 7 stelle).
  - **CloudpilotEmu** (github.com/cloudpilot-emu/cloudpilot-emu): emula i Palm con Dragonball
    (OS 1-4) e, come unico dispositivo Palm OS 5, il **Tungsten E2**. **Niente Zodiac** e niente API
    Tapwave. È comunque un riferimento prezioso per la parte Palm OS 5 su ARM.
  - **PumpkinOS** (reimplementazione di Palm OS): non controllato, da verificare.
- **Comunità e archivi**: tapwave.net, con giochi, homebrew ed emulatori *per* lo Zodiac (LJZ,
  PalmMAME, ScummVM, UAE...). Sono cose che girano *sul* dispositivo, non emulatori *del* dispositivo.
- **Dump**: sulla reperibilità dei giochi commerciali e di una ROM o firmware non abbiamo certezze.
  Il Zodiac era il candidato "meno sicuro" proprio per questo. Va chiarito come primo passo.

## Cosa ha funzionato con Undertow (da riapplicare)

1. **Verificare prima la letteratura esistente** (repo GitHub, wiki emulatori, issue, forum). Per il
   Gizmondo la premessa "nessun tentativo" si è rivelata falsa.
2. **Guardare subito com'è fatto il codice dei giochi.** Sul Game Wave i giochi erano bytecode su un
   runtime di alto livello, e questo ha reso l'HLE la strada vincente. Sul Zodiac la domanda chiave è
   se i giochi siano applicazioni Palm OS (PRC con codice ARM nativo o 68k) che chiamano le API
   Tapwave. In quel caso un HLE "alla PocketHLE" (CPU ARM più API reimplementate) potrebbe evitare
   di emulare tutto il dispositivo a basso livello.
3. **Strumenti di ricerca in Python** (estrattori, disassemblatori annotati con capstone, tracciatori
   di riferimenti) e **note verificate** in `docs/`, distinguendo sempre i fatti verificati da quelli
   dedotti.
4. **Emulatore testabile in automatico**: `--trace` delle chiamate API, tasti simulati
   (`--keys`/`--key-start`/`--key-interval`), `--exit-after` con `--screenshot`.
5. **Toolchain** già pronta: MSYS2 mingw64 (gcc, CMake, Ninja, SDL2, FFmpeg, zlib, gdb); Python 3.14
   con PIL e capstone in `C:\Python314`.

## Preferenze di lavoro

- Si parla in italiano; documentazione del repo e commit in inglese.
- Git: niente commit se non richiesti. Quando si pubblica, creazione del remote e push vanno fatti
  in **due passaggi separati**.
- Se l'utente è fuori casa, mandargli gli screenshot dei progressi con SendUserFile.
- LBA ovunque è un meme ricorrente, non un compito 😄

## Primo passo

Fai una ricerca aggiornata (ottobre 2026) per confermare che non esista nulla di funzionante o in
sviluppo attivo per il Tapwave Zodiac. Poi verifica cosa è reperibile: dump dei giochi, firmware o
ROM, SDK Tapwave e documentazione delle API. Infine proponi l'approccio (HLE oppure LLE).
