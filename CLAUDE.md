# CLAUDE.md – Pokémon Saphir/Rubin (DE), nativer PS-Vita-Port

Diese Datei ist die Übergabe aus der bisherigen Arbeit (Claude-App/Cowork, Okt.–Nov. 2026)
an Claude Code. Sie beschreibt Projekt, Arbeitsweise, Stand, Technik und Testwerkzeuge.
Stand: **Version 0.3.16** (Commit „0.3.16: shadows only under the battlers …“).

## 1. Projekt in einem Satz
Nativer Vita-Port der deutschen Pokémon-Saphir-Version (später Rubin) auf Basis der
pret/pokeruby-Decomp: der C-Code des Spiels läuft direkt auf der Vita, die GBA-Hardware
(Grafik, Ton, DMA, Flash, RTC) wird im Port-Layer nachgebildet. Kein Emulator.

## 2. Arbeitsweise mit Mike (wichtig)
- **Sprache: Deutsch**, knapp, ehrlich, ohne Moralisieren. Widerspruch und eigene Einschätzung
  sind ausdrücklich erwünscht (kein Ja-Sagen). Technische Begriffe sparsam, Ergebnis zuerst.
- **Claude schreibt den Code (80–100 %)**, Mike liefert Ideen, Use Cases und testet jede Version
  **selbst auf der Vita** (PS Vita Slim PCH-2016, `ur0:data/libshacccg.suprx` vorhanden) und gibt
  Feedback mit Fotos. Vor neuen Änderungen erst sein Feedback zur letzten Version abwarten.
- Sagt Mike „nichts bauen / erst prüfen / nur sammeln“, dann **nur einschätzen bzw. Liste führen**,
  keinen Code ändern. Bei „bauen“: Liste abarbeiten, eine VPK ausliefern.
- Nach jeder Version: kurze Liste **„Neu, bitte testen“** (nummeriert, damit er punktweise
  antworten kann) und **„Nicht geschafft / offen“**. Ehrlich sagen, was nur am PC geprüft wurde
  und was ein Verdacht ist.
- Tokens/Kontingent sparsam einsetzen; nicht doppelt und dreifach testen; bei
  Hintergrundarbeit erst melden, wenn sie fertig ist.
- **Nur in Mikes eigene Repos pushen** (nie zu pret oder anderen Forks). Keine privaten Daten
  (E-Mail, Passwörter, Accounts, NAS-Adressen) in Repo/Verlauf.
- Versionsnummer muss sichtbar sein (LiveArea-Hintergrund und Titelbild) – ist umgesetzt
  über `platform/vita/VERSION`.

## 3. Repo, Git, Commits
- Fork: `https://github.com/bit-wmike/pokeruby-vita`. Lokaler Arbeitsbranch **`port`**.
  Pushen immer: `git push origin port:main` **und** `git push origin port`
  (der Remote-Branch `port` existiert versehentlich mit und wird synchron gehalten).
- Commits als `bit-wmike <295828+bit-wmike@users.noreply.github.com>`, z. B.
  `git -c user.name=bit-wmike -c user.email=295828+bit-wmike@users.noreply.github.com commit ...`
- Commit-Nachricht: `0.3.x: kurze Zusammenfassung` + Stichpunkte. Pro ausgelieferter VPK ein
  Commit mit erhöhter Version.
- Nintendo-Grafiken für die LiveArea liegen nur lokal in `platform/vita/sce_sys_local_<game>/`
  (git-ignoriert); ohne sie wird eine Standard-LiveArea erzeugt.

## 4. Bauen
Voraussetzungen (Linux): `gcc` mit 32-Bit-Multilib, `python3` mit Pillow, VitaSDK unter
`$VITASDK` (Standard `/usr/local/vitasdk`), für den ARM-Test `arm-linux-gnueabihf-gcc-14` und
`qemu-arm`. Für die Decomp-Assets die Tools aus `INSTALL.md` (agbcc usw.).

```
# einmalig: Assets der deutschen Version erzeugen (Decomp-eigener Build)
make clean && sh de_before.sh && make sapphire_de CPP=cpp
# Port
make -f Makefile.port -j16                    # Host-Testbuild  -> build/port-host-sapphire/pokemon_host
make -f Makefile.port TARGET=armhost -j16     # ARM-Testbuild   -> build/port-armhost-sapphire/pokemon_armhost
make -f Makefile.port TARGET=vita vpk -j16    # Vita            -> build/port-vita-sapphire/PokemonSaphir_Vita_<VERSION>.vpk
```
- Version: `platform/vita/VERSION` (z. B. `0.3.16`) vor dem Vita-Build erhöhen; zu jeder VPK bleibt
  `pokemon_sapphire_vita-<version>.elf` liegen (für Savestates/Crash-Analyse).
- Header-Änderungen: keine automatischen Abhängigkeiten – betroffene `.o` löschen bzw. bei
  Zweifel `build/port-*/` neu bauen.
- ASAN-Build stürzt in m4a ab – unbrauchbar.
- `GAME=ruby` existiert als Schalter, Rubin ist aber noch nicht gebaut/getestet.

## 5. Aufbau (wo was liegt)
- `src/` – Decomp-Spielcode, Port-Eingriffe mit `#ifdef PORT` bzw. `Port…`-Funktionen.
- `port/host_core.c` – Frame-Ablauf: Rendern, Durchgänge für Kampf (Feld, Intro, BG-Fill, UI, HUD),
  `ReplayLine`/`sLineIo` (Zeilenregister für zweite Durchgänge), Menü-Fade, Backdrop hinter
  GBA-großen Bildschirmen, schwarzer Bildschirm am Ende von Kampfübergängen.
- `port/ppu/` – VirtuaPPU (Software-Renderer, `src/mode1.c`), Port-Knöpfe `virtuappu_mode1_*`
  (Regionen, Clips, Fenster-Versatz `win_dx/dy` + `win_port`, Backdrop-Key, Alt-Tileset).
- `port/port_view.c` – **Weitsicht** der Oberwelt (480×272): Welt aus Kartendaten füllen
  (`FillWorld`), Räume zentriert/geklemmt (`FrameOrigin`), Textfenster platzieren
  (`PortView_PlaceWindows`), Nachbarkarten-Tilesets (Alt-Tiles), VM-Einblendung mittig.
- `port/port_stage.c` – **Kampfbühne**: Pseudo-3D aus der echten Karte (Boden, aufrechte Karten
  für Bäume/Häuser/Felsen, Höhlenwände bzw. Höhlenfelsen-Reihen), Kampfstart-Szenerie in Ebenen,
  Wetter, Schatten, Bühnen-Fade, Trainer-Markierung für Scale3x, CPU-Compose.
- `port/port_config.c`, `port_cheats.c`, `port_hoenn.c` („Nur Hoenn“), `port_state.c` (Savestate),
  `port_test.c` (alle Host-Test-Hooks).
- `platform/vita/source/main_vita.c` – Vita: Port-Menü (START+SELECT), Audio-Thread, Darstellung
  (vita2d), Kampf-Layer auf der GPU; `vita_hq.c` – HQ-Shader und **Scale3x-Shader** fürs Kampffeld.
- `platform/host/main_host.c` – Headless-Testläufer (Screenshots als PPM, Eingaben per Env).
- Vita-Daten: `ux0:data/pokemon_saphir/` (Flash-Save `saphir.sav`, Emulator-kompatibel, `settings.ini`).

## 6. Festgelegte Entscheidungen (nicht ohne Mike ändern)
- Oberwelt Weitsicht 480×272 bei 2×, alles was geht **pixelgenau**. Räume zentriert, Höhlen wie
  offene Welt. GBA-große Bildschirme (Beutel, Team, Dex, Shop …) 2× über weichgezeichneter,
  abgedunkelter Szene. Titelbild/Intro bleiben vergrößert (3×).
- **Kampf 3×** (Feld 320×160 bei 1,5× in 480 → auf der Vita 3×). 2× wurde verglichen und verworfen.
  Auf der Vita **Scale3x** auf dem Kampffeld (Pokémon, Attacken, Boden), **Trainerbilder
  ausgenommen**. Kein HQ und kein Zoom im Kampf.
- **Einzelkampf auf den äußeren Doppelkampf-Positionen**; Doppelkampf unverändert.
- Kampfbühne blickt nur N/W/O (nie Süd). Aufrecht nur Bäume, Sträucher, Häuser, Schilder, Zäune,
  Felsen, unerreichbare Außenwelt; flach: Gras, Sprungkanten, Ufer, dünne Kanten. Karten nur aus der
  oberen Kachelebene (kein Wasser um Felsen).
- Höhlen-Bühne „Variante B“: zwei Reihen Felsen aus der Höhlen-Intro-Grafik über dem Horizont,
  Farbe aus den Höhlenwänden, Boden bleibt die Karte.
- Kampfstart-Szenerie: drei Ebenen, vorne zuerst rein/zuletzt raus, ungleichmäßige Versätze,
  nie Unterkanten sichtbar.
- Kampfübergänge: Originale in Weitsicht (kein Zoom). Volle Attacken-Hintergründe seitlich
  **gespiegelt**. Textfenster in GBA-Breite, Kampf-Boxen ohne grauen Rand.
- Darstellung nur „Pixelgenau“ und „HQ“. Cheats: EP ×1/2/4/8 (bei Bonus volle EP für jeden
  Beteiligten), Geld, Begegnungsrate, Shiny 1:32, Zufalls-Pokémon (inkl. Legendäre, fangbar),
  immer fliehen, Rennen in Gebäuden (gespeichert), wilde Doppelkämpfe (Normal/30 %/50 %),
  Item-Editor (nur Anzahl vorhandener Items). Bewusst **nicht**: sicher fangen, volle KP, durch Wände.
- Ein Savestate-Platz mit Ja/Nein-Rückfrage; Tasten im Spiel frei belegbar.

## 7. Roadmap
- Jetzt: **nur noch Fehler, Leistung (Frames sicher < 16 ms), Aufräumen** → erstes echtes Release.
- Geparkt: Rubin-Build, README aktualisieren, eigene Breitbild-Kampfübergänge.
- Idee für nach dem Release: 2,5D-/Voxel-Oberwelt als eigener Branch/zuschaltbarer Modus
  (erst Prototyp Wurzelheim). Bühnen-Klassifizierung (`ClassifyBlock`) wäre die Grundlage.

## 8. Stand 0.3.16 – von Mike bestätigt
Kampf läuft flüssig mit Scale3x; äußere Positionen, Schatten, Zielauswahl-HUD im Doppelkampf,
Kartengrenzen in der Bühne, Ballwurf im Doppel, Flash, Leitern, Musik, Item-Editor in Ordnung.

### Von Mike noch zu testen (Feedback steht aus)
- 0.3.15/0.3.16: Trainer ohne Scale3x, gespiegelte Attacken-Hintergründe (Nachtnebel, Anziehung,
  Psychokinese, Knirscher), VM-Einblendung (Zerschneider usw.) mittig/voll breit ohne Stottern,
  kein zweiter Schatten beim Ein-/Auswechseln, Kampfstart komplett schwarz statt schwarzer Kasten,
  Tür-Ab-/Aufblenden wieder da, Schatten nur unter Pokémon/Trainer, keine Silhouetten am Kampfende,
  kein Blitz in der Werft, Hafengebäude-Gespräch normal.
- Später im Spiel: Surfer beim Gegner (Bühne scheint bläulich durch), Höhlenfarben (Metallbohr-
  tunnel wirkt grünlich), Regen/Hagel/Asche/Sand im Kampf und draußen, Lavastadt-Dampf, Tauchen,
  Himmelssäule.

### Bekannte Grenzen / offene Punkte
- NPCs erscheinen in vollen Städten (Graphitport) spät am Rand: das Spiel verwaltet max. 16 aktive
  Personen (auch im Spielstand). Bewusst nicht geändert.
- Attacken 253 (Aufruhr) und 258 (Hagel) hängen im automatischen Attacken-Test (Testskript, kein
  Absturz); Attacke ~250 beendet den Testkampf → Test ab 254/259 neu starten.
- VM-Einblendung: beim Aufgehen des Fensters ein kurzer Ein-Frame-Rest links (Übergang).
- Leistung: Doppelkampf war zuletzt 16–18 ms auf der Vita → muss runter (Release-Ziel).
- Testkonzept (Sweeps, mGBA-Vergleich, Auto-Zustand/„Fehler melden“) siehe Projekt-Doku
  `testkonzept.md`; Karten/NPC/Trainer/Arten/Attacken-Sweeps liefen zuletzt bei 0.2.8 grün,
  Menü-Sweep, Story-Sprünge, Savestate-Test, mGBA-Vergleich fehlen noch.

## 9. Host-Tests (ohne Vita)
Der Host-Build ist derselbe Spielcode; x86 und ARM liefern bitgleiche Bilder. Bilder 480×272 PPM.
Skripte in `tools/port/test/` (Ausgabe nach `$PKR_TEST_DIR`, Standard `/tmp/pkr`), Spielstand
`tools/port/test/test.sav` (Testspielstand, Team mit Moorabbel Lv. 22):
```
F=2600 tools/port/test/battle.sh r104 0:19:3:9 25 2450,2500 [VAR=..]   # wilder Kampf (Pikachu) auf Route 104
F=2200 tools/port/test/field.sh yard 9:0:4:8 "1760:16:120"               # Oberwelt, Eingaben ab 1760
python3 tools/port/test/sheet.py out.png 8 240 /tmp/pkr/r104/frame_0*.ppm # Kontaktbogen
HOSTBIN=tools/port/test/qarm.sh tools/port/test/battle.sh ...              # ARM-Build unter qemu
python3 tools/port/test/gen_sweep_plans.py 16                              # Karten/NPC-Sweep-Pläne
```
Wichtige Env-Variablen (alle in `port/port_test.c`, `platform/host/main_host.c`, `port/*.c`):
- Ablauf: `PKR_FRAMES, PKR_OUT, PKR_SHOT=a,b, PKR_SHOT_EVERY=k, PKR_INPUT="frame:keys:len,..."`
  (GBA-Bits A 1, B 2, SELECT 4, START 8, RIGHT 16, LEFT 32, UP 64, DOWN 128), `PKR_AUTO_A=frame`,
  `PKR_SAVE, PKR_TIME, PKR_VIEW=0|1, PKR_NOAUDIO, PKR_HOENN, PKR_EXP, PKR_DOUBLES=prozent`.
- Szenen: `PKR_TEST_WARP=frame:g:n:x:y`, `PKR_TEST_BATTLE=frame:species:level`, `PKR_TEST_TRAINER`,
  `PKR_TEST_MOVES=erste` (beide Seiten benutzen jede Attacke ab Nummer), `PKR_TEST_PLAN=datei`
  (Schritte: warp, face, mash, back, walk, keys, party, note, setvar, flag, style, trainer, savegame),
  `PKR_TEST_DEX`, `PKR_WEATHER=frame:id`, `PKR_TERRAIN=n`, `PKR_SHOWMON=frame` (VM-Einblendung),
  `PKR_STATE_SAVE/LOAD`.
- Diagnose: `PKR_VIEW_LOG, PKR_CB_LOG` (callback2 je Frame), `PKR_KNOB_LOG=frame` (PPU-Knöpfe),
  `PKR_FW_LOG=frame` (FillWorld), `PKR_FADE_LOG, PKR_MFADE_LOG` (Bühnen-Fade), `PKR_SHADOW_LOG,
  PKR_MARK_LOG, PKR_HUD_LOG, PKR_FIELD_DUMP=frame, PKR_STAGE_DUMP, PKR_STAGE_OAM=frame,
  PKR_INTRO_LOG/DUMP/DUMPI, PKR_OAM_WX, PKR_BAG_DUMP, PKR_STAGE_TIME`.
- Testorte (g:n:x:y): Route 104 hohes Gras `0:19:3:9`, Graphitport Werft `9:0:4:8`, Hafen
  `9:8:2:12`, Granithöhle `24:7:20:10`, Meteorfälle `24:0:20:12`, Metallbohrtunnel `24:4:10:8`,
  Metarost Mart-Tür `0:3:16:46`, Regen `0:34:24:7`, Asche `0:28:33:7`, Wüste `0:26:20:60`,
  unter Wasser `0:50:10:10`. Karten-Indizes aus `data/maps/map_groups.json`.
- Kampfzeitpunkte im Standardtest: Kampf ab 1900, Pokémon draußen ~2450, Kampfende ~2930–2970.
- Bewährtes Vorgehen bei Mikes Fotos: Szene nachstellen, Frame-Folge als Kontaktbogen ansehen,
  mit `PKR_VIEW=0` (GBA-Ansicht) bzw. vorherigem Binary (`HOSTBIN=`) vergleichen, Ursache per
  Diagnose-Env eingrenzen, erst dann ändern. Vita-eigene Teile (GPU, Shader) lassen sich am Host
  nicht prüfen – das ehrlich so sagen.

## 10. Technik-Fallstricke (gelernt)
- Kampf-Backdrop ist Alpha 0 (Bühne scheint durch). Aufhellen/Abdunkeln/Blend auf Alpha-0 nur,
  wenn `virtuappu_mode1_backdrop_key` gesetzt ist (Kampf) – sonst gingen Tür-Fades kaputt.
- Fensterkanten in Weitsicht: `virtuappu_mode1_win_port=1` setzen, sonst werden Fenster bei
  Versatz 0 nicht umgerechnet (Werft-/Hafen-Blitz).
- Bühnen-Fade: normale Fades aus `gPaletteFade`, Schnell-Fades/beendete Fades aus den Paletten
  gemessen (`FadeAmount`), sonst schwarze Silhouetten.
- Schatten nur für Sprites mit Pokémon-Anim-Tabelle (`gSpriteAnimTable_81E7C64`) oder Trainerbild;
  `gBattlerSpriteIds` kann nach K.O. auf einen wiederverwendeten Slot zeigen.
- Scale3x-Shader (vita_hq.c): eigene Matrix = vita2d-Ortho, Tiefentest aus (sonst verschwanden
  Textbox/HUD), Shader beim Start kompilieren (sonst schwarze Pause beim 1. Kampf).
  Trainerpixel tragen Alpha 0xFE → Shader filtert sie nicht.
- Battler auf BG (Psychokinese u. a., `gPortBgMon`): BG dann GBA-breit, sonst Doppelbild am Rand.
- Copyvar mit Konstante (`GetVarPointer`) muss `id` liefern (Granithöhle-Leitern).
- Audio: 2400-Sample-Puffer mit Strecken/Stauchen gegen Hänger bei späten Frames.
- Weitere Historie: Projekt-Doku `pokemon-rubin-saphir-vita/technik.md` (claude.ai-Projekt
  „3ds/psp/vita modding“) und die Commit-Nachrichten 0.2.x–0.3.16.
