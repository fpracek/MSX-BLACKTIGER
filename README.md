# MSX-BLACKTIGER

Porting di **Black Tiger** (Capcom, 1987) per **MSX2+**, basato su un motore
triple-buffer SCREEN5 a 60fps con sprite software compilati (Z80) e ripristino
del fondale via comandi VDP.

## Stato

Prototipo giocabile del round 1 (finestra 120 colonne):

- Scrolling 8-way triple buffer (pagine 0-2, cache tile in pagina 3), streaming
  colonne level-triggered, flip di pagina nell'ISR (niente tearing)
- Eroe completo: idle, camminata 6 fasi, salto ad arco (long-jump aereo +
  coyote time), attacco in 3 tempi con mazza chiodata, colpito, perdita
  armatura (frantumazione stile GnG), morte animata fino allo scheletro;
  l'arma corrente penzola dalla mano in ogni posa
- Orchi: pattuglia / carica / guardia a distanza, attacco con l'ascia,
  drop di zenny alla morte, draw split sulla cucitura delle pagine
- Item: vasi rompibili (animazione a 3 fasi), monete zenny, armatura
  raccoglibile — tutti sprite compilati Z80
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

## Requisiti

- [MSXgl](https://github.com/aoineko-fr/MSXgl) (il progetto vive in `projects/`)
- Python 3 + Pillow + NumPy per la pipeline
- openMSX per il test (macchina MSX2+), MAME per i rip

Grafica e materiale originale © Capcom. Progetto amatoriale senza fini di lucro.
