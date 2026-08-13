# MSX-BLACKTIGER

Porting di **Black Tiger** (Capcom, 1987) per **MSX2+** su cartuccia
**Yamanooto** (mapper Konami-SCC esteso: SCC, OFFR/ENAR, flash per i
salvataggi), basato su un motore triple-buffer SCREEN5 a 60fps con sprite
software compilati (Z80) e ripristino del fondale via comandi VDP.

## Stato

Prototipo giocabile del round 1 (finestra 120 colonne):

- Scrolling 8-way triple buffer (pagine 0-2, cache tile in pagina 3), streaming
  colonne level-triggered, flip di pagina applicato dall'ISR dentro il vblank
- Eroe completo: idle, camminata 6 fasi, salto ad arco (long-jump aereo +
  coyote time), attacco in 3 tempi con mazza chiodata E CATENA, colpito,
  perdita armatura (frantumazione stile GnG), morte animata fino allo
  scheletro, arrampicata a 4 fasi sui pilastri; l'arma penzola dalla mano
  in ogni posa con la sua maglia di catena
- Orchi (frame TSR nativi): pattuglia / carica / guardia a distanza, attacco
  con l'ascia, drop di zenny alla morte, draw split sulla cucitura delle
  pagine e clipping parziale ai bordi dello schermo
- Item: vasi rompibili (animazione a 3 fasi, mezzo secondo), monete zenny,
  armatura raccoglibile con "pop" arcade — tutti sprite compilati Z80
- Scorebar con split screen vero: banda HUD (pagina 3) alle linee 0-15 via
  interrupt IM2 (vblank -> banda HUD, line int raster 15 -> banda gioco);
  BIOS fuori dal percorso interrupt, niente sprite hardware
- Pilastri scalabili (aggancio automatico al contatto, balzo direzionale)
- Palette 16 colori ispirata al porting Atari ST

## Struttura

| Cartella | Contenuto |
|---|---|
| `game/` | progetto MSXgl: va copiato in `MSXgl/projects/blacktiger` e compilato con `node ../../engine/script/js/build.js` |
| `tools/` | pipeline asset in Python (rip MAME → sheet MSX → SpriteEncoder) e script Lua per MAME/openMSX |
| `rip/` | dump grezzi da MAME (tilemap, palette, log sprite) e metadati JSON |
| `gfx/` | sheet generati e riferimenti (mappa id degli sprite TSR) |
| `assets/tsr/` | fogli sprite nativi (The Spriters Resource) usati come sorgente dei frame |

## Pipeline asset

```
make_level.py      rip arcade → level1_data.h + bin/tiles.bin (+ g_Solid/g_Climb)
compose_hero.py    comp MAME + frame TSR → hero_msx_sheet.png + hero_meta.h
compose_enemies.py comp MAME + affondo TSR → orc_msx_sheet.png + orc_meta.h
SpriteEncoder.py   (dal progetto soccerlgMSX2) sheet → sprite compilati Z80
                   NB: usare sempre --FN 2 per le celle 48x48
```

La palette (`rip/level1_pal.json`) è curata a mano: **non** rigenerarla.

## Note tecniche importanti

- **Limite 24KB**: il codice fisso vive a 0x4000-0x9FFF; `l__CODE` nel .map
  deve restare sotto 0x6000 — oltre, il linker trabocca in silenzio nella
  finestra bancata e la ROM non parte. Dati corposi → segmenti raw.
- La build va lanciata verificando l'exit code (Dropbox può bloccare la
  rimozione di `package/` → EXIT 1 con ROM comunque scritta).
- Test in openMSX: `-romtype Yamanooto`.
- **Trappola Yamanooto**: il crt0 lascia ENAR.REGEN attivo e con REGEN attivo le
  LETTURE di 0x7FFC-0x7FFF restituiscono i registri della cartuccia al posto
  della ROM — qualsiasi byte di codice/dati piazzato lì dal linker viene
  corrotto al fetch. Il gioco spegne ENAR come prima istruzione di `main()`
  (OFFR resta 0: tutti i segmenti sono < 256).
- SpriteEncoder: sempre `--FN 2` per le celle 48x48.

## Requisiti

- [MSXgl](https://github.com/aoineko-fr/MSXgl) (il progetto vive in `projects/`)
- Python 3 + Pillow + NumPy per la pipeline
- openMSX per il test (macchina MSX2+), MAME per i rip

Grafica e materiale originale © Capcom. Progetto amatoriale senza fini di lucro.
