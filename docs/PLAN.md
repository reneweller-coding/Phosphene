# Phosphene: Plan für den Psytrance-Set-Generator

Stand 15.09.2026, nach den Entscheidungen des Nutzers (Abschnitt 14). Name **Phosphene**, Repo
`G:\Tools\VRAudio\PsytranceGenerator`, Namensraum `phos::`, Präfix `PHOS_`, Werkzeuge `phos_render`,
`phos_selftest`. Code-Kommentare im Doxygen-Format (`docs/Doxyfile`).

## Stand der Umsetzung

**15.09.2026: Phase 0 und Phase 1 fertig.** Gemessen auf dem i9-12900K, ein Kern, 48 kHz:

| Prüfstein | Ergebnis |
|---|---|
| Selbsttest `phos_selftest` | 66 von 66 Prüfungen bestanden |
| Vektortests AVX2 / NEON-Shim / skalar | je 5 von 5; Leiter, Halbband und 12 Operationen bitgleich zum skalaren Pfad |
| Kick + Rolling Bass, 256 Takte | 165-fache Echtzeit, 0,6 % eines Kerns |
| Halbband-Dezimierer (6 Koeffizienten) | Durchlass flach bis 17 kHz (±0,0001 dB), Sperrdämpfung 110 dB |
| PolyBLEP-Säge bei 2 kHz, Aliasing unter 18 kHz | naiv −14 dB, PolyBLEP −35 dB, 2× überabgetastet −57 dB |
| Engine-Ausgabe bei Blöcken 1 / 64 / 1000 / 4096 | bitgleich |
| MIDI-Rundlauf mit Tempo-Rampe | alle Noten zurück; Tempo-Stufen weichen um höchstens 2 µs von der Rampe ab |

Gebaut: `Vec.h` (Lane-SIMD mit fusioniertem Multiply-Add auf allen drei Pfaden), Parametersystem in
Modulblöcken, Tempo-Karte mit geschlossener Integralform, Partitur und lock-freier Ereignisring,
Engine mit absolutem 32-Sample-Raster, SMF-Export und -Leser, streamender WAV-Schreiber (RF64 über
4 GiB), BS.1770-Messgerät (aus Noctuary, ohne Zwicker), Kick mit Sweep- und Resonanz-Engine, Bass mit
PolyBLEP, nichtlinearer ZDF-Leiter und 2× Halbband, ereignisgesteuerter Ducker, Komponist mit fünf
Bass-Mustern und phrasenweisen Figuren, Conductor, `phos_render`, `Tools/inspect_wav.py`.

Abweichungen vom Plan, bewusst:
- **Resonanz-Kick** ist ein gedämpfter Phasor-Resonator nach Mathews und Smith (2003) mit dem
  Retrigger-Verhalten der TR-808 laut Werner et al. (2014), keine Bauteil-Simulation der
  Bridged-T-Schaltung. Ein SVF-Resonator verlor unter dem Tonhöhen-Sweep 8,2 dB statt der 4 dB
  des Decays; der Test dafür schlug gegen die SVF-Fassung an.
- **Leiter-Nichtlinearität** ist die algebraische Sigmoide x/√(1+x²) statt tanh, gelöst mit der
  "cheap nonlinear ZDF"-Methode (Voipio 2012). Grund: Wurzel und Division sind einzelne
  IEEE-Operationen, damit bleiben die Lane-Pfade bitgleich.
- **Halbband** ist ein polyphasiges IIR-Allpass-Paar (Valenzuela und Constantinides 1983) statt
  eines FIR: 6 Koeffizienten für 110 dB, kaum Latenz. Die Phase ist nahe Nyquist nicht linear, was
  für Bass und Acid keine Rolle spielt.
- **`SourceSlot` von der Noctuary-Engine lösen** war zunächst nach Phase 4 verschoben und ist seit
  dem Review vom 15.09. gestrichen: Pads werden Wavetables in der `Poly`-Engine (5.7).
- **Composer-Thread** existiert noch nicht als Thread: der `Conductor` füllt den Ring, im
  Offline-Render synchron vor jedem Block. Der Thread kommt mit dem Plugin (Phase 6).
- **Kick-Defaults** verkürzt (Hold 12 ms, Decay 150 ms): mit den ersten Werten (35/280 ms) lag die
  Kick bei der ersten Bass-Sechzehntel erst 6 dB unter ihrem Spitzenwert, jetzt 27 dB. Die
  Bass-Startphase steht auf 0,5 (Nulldurchgang), weil Phase 0 bei jeder Note einen DC-Sprung erzeugte.

**15.09.2026, zweite Runde: Phasen-Grundlagen von Kick und Bass, Variation über die Nacht.**
Anlass war die Prüfung gegen sechs signaltheoretische Anforderungen des Nutzers (Startphase, Chirp,
Raster, Überlappung, Perioden, Filterphase). Umgesetzt und gemessen:

| Anforderung | Umsetzung | Messung |
|---|---|---|
| Sub-Oszillator | Sinus auf dem Grundton statt eine Oktave tiefer, hinter der Leiter, unmoduliert; Split-Modus mit Linkwitz-Riley-Hochpass 4. Ordnung bei 2·f0 auf dem gefilterten Zweig | Grundton-Phase driftet über 140 ms um 3,8° (vorher −34°, Mixed-Modus 23°) |
| Gleiche Grundton-Phase | Sägezahn und Sub starten mit derselben Grundton-Phase; Puls invertiert, weil sein Grundton gegenphasig zum Sägezahn lag und die Mischung bei Wave ⅓ den Grundton auslöschte | Einsatzphase auf 0,02° genau |
| Kick-Chirp | τ1 (Punch) eigener Parameter; Phase in geschlossener Form integriert; Einstellungen pro Schlag eingefroren | Ausgabe folgt der analytischen Phase inklusive Kettenphase auf 0,09° |
| Sub-Sample-Einsätze | Kick, Bass, Hüllkurven und Ducker beginnen um den Bruchteil eines Samples versetzt | Schwankung der Einsatzphase von Beat zu Beat 0,013° (vorher 0,33°) |
| Überlappung | Ausklang-Grenze am ersten Bass-Slot unter Einrechnung der Sättigungsanhebung g/tanh(g); Körper-Untergrenze von zwei Grundton-Perioden über −20 dB | Ausklang −26 bis −27 dB bei allen Sättigungsarten |
| Phasenbedingung Δϕ = 2πk | `bass.kick_lock`: "Kick follows bass" trimmt τ2 der Kick per Bisektion, "Bass follows kick" setzt die Bass-Startphase auf die analytische Kick-Phase | Δϕ am Einsatz +2,9° bis +5,1° bei 138 bis 148 BPM; ohne Kopplung −157° bis +165° |
| Perioden-Disziplin | Release nie kürzer als eine halbe Grundton-Periode | Release bei F#1 mindestens 10,8 ms |
| Identische Noten | Leiter, Dezimierer und Hochpass beginnen neu, wenn die vorige Note unter −60 dB liegt | Korrelation zweier Sechzehntel 0,999995 |

Variation (Wunsch des Nutzers: Sets, die eine ganze Nacht laufen und nie langweilig werden):
- **Track-Ebene:** Länge, Tonart (Quinten, Ganztöne), Modus, Tempo als mittelwertstabiler Zufallsweg
  innerhalb `compose.tempo_range` mit 16-Takt-Rampen, Primär- und Sekundärmuster, Gate innerhalb der
  Release-Grenze. Der erste Track spielt exakt die Knöpfe.
- **Klang-Rezepte:** fünf Wahrnehmungsrichtungen je Instrument (Kick: Länge, Punch, Körper, Grit,
  Klick; Bass: Helligkeit, Pluck, Squelch, Grit, Gewicht) nach den Timbre-Dimensionen von Grey (1977)
  und McAdams et al. (1995), gewählt mit Mitchells Best-Candidate-Verfahren (1991) gegen die letzten
  vier Tracks. Gemessen über 60 Tracks: engster Abstand aufeinanderfolgender Rezepte 1,17 bei
  mittlerem Abstand 1,80; 11 Tonarten, alle 5 Muster.
- **Innerhalb eines Tracks:** Filterbögen über 32 Takte, Sekundärmuster in manchen 16-Takt-Blöcken,
  zweischlägige Bass-Pausen, Figuren mit steigender Häufigkeit.
- **Pegelangleich:** Messlauf von zwei Takten je Track in einer privaten Engine, BS.1770, Verstärkung
  gegen Track 1. Über 30 Minuten Lautheitsspanne 1,0 LU statt 3,7 LU.
- **Steuerereignisse:** zweiter Ereignisring; die Engine spielt Knopf + normierter Offset bzw.
  Override. Tonart- und Track-Marker gehen in den MIDI-Export.

Bekannte Grenzen: Die Klang-Rezepte stehen noch nicht als MIDI-CCs im Export. Die Pegelmessung nutzt
die Knöpfe zum Zeitpunkt der Planung; ein live verstellter Knopf ändert bereits geplante Tracks nicht.
Die Resonanz-Kick trifft die Phasenkopplung etwas ungenauer als die Sweep-Kick, weil ihre Phase eine
Summe statt eines Integrals ist.

**15.09.2026, Review-Runde zum Plan.** Ein externes Review schlug sechs Änderungen vor; fünf sind
übernommen und in die Abschnitte eingearbeitet, eine ist nach Messung abgelehnt:

| Vorschlag | Entscheidung | Wo |
|---|---|---|
| Supersaw als Standard-Lead, dynamischer Detune, Key-Tracking-Hochpass | übernommen, Hochpass als Tiefenregel für alles außer Kick und Bass | 5.5 |
| Wavetables statt Noctuary-`SourceSlot` für Pads | übernommen, `SourceSlot` gestrichen | 5.7, Tabelle Abschnitt 4 |
| Psy-Squelch als eigener Modus | übernommen, Zahlen werden an Hallucinogen kalibriert | 5.4 |
| Bass-Slot-Hüllkurve "Note 2 kürzer und leiser" | **abgelehnt als Regel**: neun Referenztracks innerhalb ±1,2 dB; als Stilprofil-Parameter mit flachem Standard | 6.6 |
| Trance-Gate als Kanaleffekt | übernommen | 5.9 |
| Pre-Drop-Vakuum in der Grammatik | übernommen als feste Regel mit Varianten | 6.2, 5.8 |

Phase 4 wird dadurch um etwa einen Tag kürzer (keine Entkopplung).

**15.09.2026, Phase 2 fertig: Percussion-Kit mit zwölf Lanes, Rhythmus, Variation.**

*Erst gemessen, dann gebaut.* `Tools/ref_perc_profile.py` faltet die Einsatzstärke (positiver
spektraler Fluss) von zwölf Referenztracks ins Sechzehntel-Raster, ausgerichtet an der
Sechzehntel-Gruppe, die im Kick-Band am stärksten heraussticht (eine einzelne Slice-Spitze war
mehrdeutig, weil der rollende Bass auf jeder Sechzehntel ebenfalls Tiefton-Einsätze setzt). Ergebnis
im Hat-Band 6 bis 14 kHz: in sieben von zwölf Tracks ist die Achtel-Offbeat-Position die stärkste
(31 bis 49 % der Einsätze), die beiden anderen Sechzehntel tragen je 10 bis 25 %. Das ist das
Standardmuster: Offbeat-Hat laut, Sechzehntel-Schicht bei etwa der halben Stärke.
`Tools/ref_band_balance.py` misst die Bandbalance gegenüber dem Kick-Bass-Band 40 bis 140 Hz;
Referenz-Median: Präsenz 1,5 bis 6 kHz −12,5 dB, Luft 6 bis 16 kHz −15,2 dB.

| Baustein | Umsetzung | Messung |
|---|---|---|
| Lane-Kernel (`PercKernel.h`) | eine universelle Stimme für alle Rollen: Ton mit Tonhöhen-Hüllkurve und FM (Taylor-Rotation 7. Ordnung + Newton-Renormierung), vier Moden (Membran, Balken, Tabla nach Fletcher/Rossing), sechs 808-Rechtecke mit maskenbasiertem PolyBLEP, Rauschen mit Clap-Bursts, SVF, Low-Cut 24 dB/Okt ab 150 Hz, Drive, Choke, Panorama | zwölf Lanes bitgleich über AVX2, NEON-Shim und skalar; AVX2 5,6× schneller als skalar (0,5 % gegen 3,0 % eines Kerns, alle Lanes dauerhaft beschäftigt) |
| Kit (`Perc.h`) | Standard-Kit aus Instanz-Standardwerten (neu im Parametersystem), Stimmen auf die Tonart, Tonhöhen-Shift pro Schlag, Sub-Sample-Einsätze, `DenormalGuard` statt Klemmen | Tom-Moden bei 1 : 1,589 (Soll 1,593); Clap 4 Bursts im Abstand 9,0 ms; Choke −92 dB in 10 ms; Ton-Amplitude über 2 s auf 0,01 dB genau |
| Tiefenregel | Low-Cut nie unter 150 Hz, Tom auf 220 Hz, Low-Cuts für Tom und Conga höher | jede Lane unter −30 dB Leistungsanteil unter 140 Hz (vorher Tom −22,9 dB) |
| Pegel | kalibriert auf die Referenz-Bandbalance, knapp darunter, damit die Leads in Phase 3 Platz haben | drei Seeds, Median Präsenz −18,0 dB, Luft −17,2 dB (vorher −27,3 und −31,9) |
| Rhythmus (`Rhythm.h`) | Euklid (Bresenham-Form, gleiche Ketten wie Bjorklund), LHL-Synkopenmaß, Rotation der euklidischen Lanes auf mittlere Synkope (Sioros et al. 2014), Entscheidungen pro Vier-Takt-Phrase, Fills (Snare-Roll mit 32teln, Tom-Lauf in der Skala, Zap-Burst, Clap-Triole, Hat-Drop), Crash nach 16-Takt-Fill, Schichten-Aufbau pro 16 Takte | Euklid gegen Toussaints Tabelle; LHL: 4-on-the-floor 0, Offbeat-Achtel 7, Son-Clave 4; euklidische Lanes nie auf der Kick |
| Variation | Hat-Modus, Clap-Backbeat, Schichten-Reihenfolge und -Zahl, Klang-Rezept (Helligkeit, Kürze, Grit) für das ganze Kit, Engine- und Moden-Wechsel einzelner Lanes; Pegelmessung schließt die Percussion ein | über 40 Tracks alle drei Hat-Modi, Backbeat in 22 von 40, 4 bis 7 Schichten; Pegelspanne zwischen Tracks 0,39 LU |
| MIDI | Percussion-Spur auf Kanal 10, General-MIDI-Noten je Rolle, Tom-Lauf als tiefere Noten | Rundlauf geprüft |

Gesamt: 98 Selbsttest-Prüfungen, Vektortests mit Kit in drei Pfaden. Kick, Bass und Percussion
zusammen 1,6 % eines Desktop-Kerns. Auf der Quest noch nicht gemessen.

**15.09.2026, Phase 3 fertig: Acid, Lead, Arp, Harmonie, Korpus-Melodik.**

*Erst gemessen, dann gebaut.* Die Korpus-Analyse (`Tools/corpus/build_corpus.py`, nur Zählwerte in
`Core/src/CorpusTables.cpp`) ergab: Acid 62 melodische Loops mit 3834 Noten, Lead 183 mit 7055, Arp
421 mit 26185. **Akzente und Slides stehen im Korpus praktisch nicht** (MIDI-Loops speichern sie selten)
und sind deshalb Entwurfswerte. Akkorde: in 88 % der Takte bleibt der Akkord; Wechsel vor allem
0↔5, 0↔7, 0↔8. Der Squelch ließ sich an den Mischungen nicht messen (`Tools/ref_sweeps.py`,
Negativbefund: ein Sweep über mehrere Kilohertz verschmiert im Kurzzeitspektrum wie ein Transient);
seine Zahlen sind Entwurfswerte mit Stellbereich. Die Supersaw-Konstanten sind aus Szabos Arbeit selbst
abgelesen (Tabellen 1 bis 3, Abschnitte 3.2 bis 3.4), nicht aus Nachbauten übernommen.

| Baustein | Umsetzung | Messung |
|---|---|---|
| Diodenleiter (`DiodeLadder.h`) | Zavalishin rev. 2.1.2, Abschnitt 5.10, Gl. 5.18: gekoppelte Stufen, ZDF von innen nach außen gelöst, algebraische Sigmoide pro Stufe nach Voipio; Cutoff = Resonanzspitze bei ωc/√2 | Kleinsignal gegen Gl. 5.29 auf 0,019 dB (k = 0, 8, 16); Selbstoszillation ab k = 17 genau bei der Spitze (k = 17,5: +73 dB bei 1000,0 Hz; 16,5 und die 4 der Transistorleiter klingen ab); DC-Verstärkung 1/(1+k) |
| Acid (`Acid.h`) | PolyBLEP 2× → Diodenleiter 2× → Halbband → Kamm (Squelch) → VCA → ADAA-tanh → 24-dB-Low-Cut ≥ 150 Hz; Akzent mit Sweep-Kondensator (Tiefe ∝ Resonanz), Slide als exponentielles Legato-Glide, Squelch als Cutoff-Startwert × Faktor plus auf die Periode gestimmter Kamm; Tempo-Delay | Glide erreicht nach der Slide-Zeit 1 − 1/e (61,425 von 61,425); Akzent +4,0 dB, Sweep nach 1/2/3 Akzenten 0,22/0,32/0,38; Squelch-Anschlag −23,8 dB Leistung über 1,5 kHz, 60 ms später −62,8 dB; D3 mit voller Resonanz und Drive −40,6 dB unter 140 Hz |
| `Poly` (`Poly.h`, `PolyKernel.h`) | 8 Stimmen × 7 Oszillatoren = 56 Lanes, Säge/Puls/FM-Sinus in einem zweiglosen Kernel; Supersaw nach Szabo (Detune-Polynom 11. Grades, lineare Mitte, parabolische Seiten, Zufallsphase je Note, Hochpass auf dem Grundton), VA, 2-Op-FM; dynamischer Detune über log2 der Notenlänge; SVF-Tiefpass mit Hüllkurve, LR4-Hochpass bei max(Floor, Track × f0); Stimmfilter auf absolutem 16-Sample-Raster | Detune-Kurve gegen Szabos Tabelle 2 höchstens 0,004 daneben, Mix gegen Tabelle 3 0,006; sieben Spektrallinien auf 0,25 Hz, Seiten-zu-Mitte-Verhältnis auf 0,15 dB; FM-Seitenbänder gegen J1/J0 und J2/J0 auf 0,26 dB; Korrelation zweier gleicher Anschläge −0,20; Lead und Arp auf ihren tiefsten Noten −60 dB unter 140 Hz; acht Supersaw-Stimmen 0,9 % eines Kerns (AVX2, 5,9× schneller als skalar) |
| Tempo-Delay (`TempoDelay.h`) | Stereo, Zeiten in Beats, HP und LP in der Rückkopplung, 15 % Übersprechen, Lesen in double | ohne Leerlauf-Abkürzung, damit die Ausgabe unabhängig von Host-Blöcken bleibt |
| Constraint-Markov (`Corpus.h`) | Witten-Bell-interpoliertes VMM Ordnung 2 über Intervalle zum Grundton, exakte Stichprobe unter Positions-Constraints nach Pachet und Roy; Rückwärtsschritt nur über erreichbare Zustandspaare | gegen vollständige Aufzählung Totalvariation 0,006 (T = 1) und 0,005 (T = 0,5); ein gieriger Schritt-für-Schritt-Sampler liegt bei 0,20 und 0,27; Modelle summieren in jedem Kontext auf 1 |
| Melodik (`Melody.h`) | Akkorde: vier Akkorde zu 2 oder 4 Takten, Wechsel nach den Korpus-Übergängen ohne Verbleib, Verbleib mit 0,35; Acid-Muster A/B (B zieht 2 bis 3 Noten bei festen Nachbarn neu); Lead-Phrase A A′ B A″ mit Akkordtönen auf starken Sechzehnteln und Schluss auf Akkordton; Arp auf Akkordtönen (auf, ab, auf-ab oder Korpus-Modell); Schichten pro 16-Takt-Block; Maskierungsregel: Arp oktaviert, bis die Tonräume von Lead und Arp sich höchstens zwei Halbtöne überlappen | 24587 Noten, alle in der Skala; alle 624 starken Lead-Noten und 12416 Arp-Noten Akkordtöne; 1304 Slides, alle überlappend; 18 gemeinsame Blöcke, keiner maskiert; 24 verschiedene Acid-Riffs in 24 Tracks, 23 bewegte Akkordfolgen, alle vier Arp-Stile |
| Pegelangleich | Fundament (Kick, Bass, Percussion) wie bisher; jede Melodiestimme einzeln geprobt und an dieselbe Stimme im ersten Track angeglichen, Track-Verstärkung herausgerechnet | — |
| Tiefenregel | Acid ab D3, Lead ab B3, Arp ab G3; LR4-Hochpass bei f0 in `Poly`; Delay-Hochpass ≥ 150 Hz | gerenderte Melodiestimmen zusammen −33,5 dB Leistung unter 140 Hz |
| MIDI | Slide als überlappende Noten plus CC 65, Akzent als Velocity 120 | Rundlauf geprüft |
| Memorisierung | `build_corpus.py --memorisation`, jeder Takt gegen jeden Korpustakt der Rolle, transpositionsinvariant | eine Stunde mit allen Stimmen: 0 von 969 Acid-, 0 von 627 Lead-, 0 von 633 Arp-Takten identisch; Positivkontrolle mit transponierten Korpus-Loops: 100 % gefunden |

*Gegenprobe.* Sechs Fehler absichtlich eingebaut, jeder von seiner Prüfung gefunden: halbierte erste
Diodenstufe (25 dB Abweichung), feste Startphase (Korrelation 1,000), abgeschaltete Maskierungsregel
(10 von 18 Blöcken maskiert), kein Legato (Tonhöhe springt), Sweep ohne Kondensator (0,178 dreimal),
Stimmfilter-Koeffizienten pro Segment statt auf dem absoluten Raster (Ausgabe hängt von der
Blockgröße ab; die bisherige Blockgrößen-Prüfung rendert nur vier Takte ohne Melodik und hätte das
nicht bemerkt, deshalb gibt es sie jetzt auch mit Acid, Lead und Arp).

*Bandbalance.* Referenz neu über 39 der 40 Tracks gemessen (einer ist in der Mitte stumm), relativ
zum Band 40 bis 140 Hz: Low-Mid −7,3, Mitten −8,8, Präsenz −9,8, Luft −13,3 dB. Phosphene mit den
Standardwerten, fünf Seeds über je 512 Takte ganz gemessen: Low-Mid −3,7, Mitten −9,8, Präsenz
−13,3, Luft −18,9 dB. Die Melodiestimmen sind auf die Mitten kalibriert (Pegel, Acid-Cutoff und
-Hüllkurve, Hochpass auf dem Grundton wie bei Szabo). **Offen:** Präsenz 3,5 dB und Luft 5,6 dB unter
der Referenz, Low-Mid 3,6 dB darüber; der Überschuss kommt aus dem Fundament (Bass-Obertöne), nicht
aus der Melodik. Pads, SFX und die Mischpult-Kalibrierung (Phasen 4 und 5) nehmen das auf.

*Abweichungen vom Plan, bewusst:* Supersaw-Summe auf inkohärente Leistung normiert (der JP-8000 wird
mit dem Mix-Regler 4 dB lauter). Wavetable-Oszillator mit den Pads in Phase 4; 4-Op-FM und Phase
Distortion noch nicht gebaut, `Poly` hat 2-Op-FM. Dynamischer Detune folgt vorerst nur der
Notenlänge, der Sektionstyp kommt mit der Form-Grammatik (Phase 4). Die Schichten pro 16-Takt-Block
sind ein Platzhalter für die Instrumentierungs-Matrix der Form (6.1, 6.2).

Gesamt: 126 Selbsttest-Prüfungen, Vektortests 9 von 9 in AVX2, NEON-Shim und skalar (Diodenleiter
und `Poly` bitgleich). Eine Stunde Render mit allen Stimmen und Komposition 3,9 % eines Kerns.

**16.09.2026, Phase 4 fertig: Fläche, Effekte, Sends, Sidechain und Master.**

Reihenfolge nach der Phasentabelle (Abschnitt 12): Phase 4 ist "Fläche + FX". Der vorige Satz an dieser
Stelle nannte Form-Grammatik, Energiebogen und Übergänge als Phase 4; das war falsch, sie gehören zu
Phase 5.

*Erst gemessen, dann gebaut.* Die Referenzlautheit der 40 Tracks (ffmpeg `ebur128`, ganze Tracks) liegt
im Median bei **−13,3 LUFS** (Quartile −14,1 bis −12,8), True Peak −1,8 dBTP, LRA 6,3 LU. Die Dateien
tragen nur ReplayGain-Tags, die beim Dekodieren nicht angewendet werden; die Sammlung ist also leiser
gemastert als heutige Psytrance-Veröffentlichungen (−8 bis −6 LUFS). Das Lautheitsziel ist deshalb ein
Entwurfswert: **−9 LUFS**, einstellbar. Die Bandbalance-Referenz ist über 39 Tracks neu gemessen
(einer ist in der Mitte stumm): Low-Mid −7,3, Mitten −8,8, Präsenz −9,8, Luft −13,3 dB zum Band 40 bis
140 Hz. Solo-Messungen zeigten, dass der Low-Mid-Überschuss aus Phase 3 von der **Kick** kommt (Pitch
Start 330 Hz: −3,6 dB Low-Mid im Kick-Solo), nicht vom Bass (−11,1 dB).

| Baustein | Umsetzung | Messung |
|---|---|---|
| Wavetables (`WaveTable.h`) | Noctuarys `CycleTable`-Schema (zehn Oktav-Stufen, acht Samples je Oberton, Catmull-Rom, Hysterese), sechs Tabellen **im Code aus Spektren erzeugt**: Classic, Vocal (Formanten nach Peterson und Barney), Glass, PWM, Sync (65536-fach analysiert), Formant Saw | Sägezahn-Frame 1/h bis zur 256. Harmonischen exakt; A6 aus der gewählten Stufe −61 dB unharmonisch, aus Stufe 0 −16 dB; über die Pad-Engine −57,7 dB; Vocal: 2. Harmonische +13 dB, 6. −28 dB von a nach i |
| `Poly` Wavetable + Pad-Instanz | vierter Oszillatortyp (sieben Oszillatoren mit Szabos Detune und Mix), Tabellenposition mit Hüllkurve und LFO in Beats; Tabellenlesen skalar (Gather), Kernel bitgleich | Vektortests mit Wavetable 9 von 9 in drei Pfaden |
| Pads (`Melody.h`) | Vier-Stimmen-Voicings G3 bis G5 aus allen Akkordton-Kombinationen mit allen drei Tonklassen und minimaler Stimmbewegung; Pads tragen die Blöcke ohne Lead und Acid; Gate pro Block | 200 Zufallsfälle gleich der Brute-Force-Suche; 592 Pad-Noten, alle Akkordtöne |
| Trance-Gate (`TranceGate.h`) | Kanaleffekt für Lead, Arp, Pad: Öffnung als Funktion der Beat-Position (sechs Muster), Raised-Cosine-Flanken, Tone-Duck gegen 700-Hz-Tiefpass; Komponist schaltet es pro Block | halb offen genau in der Flankenmitte; Tiefe 0,9 → −19,9 dB (Soll −20,0) |
| Sidechain-Matrix | ereignisgesteuerte Ducker (`Ducker.h`) auf Acid, Lead, Arp, Pad, SFX und den Hall-Returns, Tiefe je Kanal | Pad bei Tiefe 0,5: −5,8 dB während des Holds |
| Sends (`Reverb.h`) | Noctuarys FDN (acht Linien, Streuung, farblose Linienlängen), als Raum und Halle; Pre-Delay in Beats; Return-Low-Cut ≥ 150 Hz | T60 1,78 s bei Einstellung 2,0 s; Rauschen durch den Return −30,2 dB unter 140 Hz |
| Master (`Dynamics.h`) | Bus-Kompressor nach Giannoulis, Massberg, Reiss 2012 (Soft Knee, Detektor im Log-Bereich), Mono-Bass (Seite mit LR4-Hochpass), **2×-Knie-Clipper** (unter 0,7 T unberührt), Lookahead-True-Peak-Limiter (Kaiser-Sinc 4×, gleitendes Minimum + gleitender Mittelwert, 1,5 ms), Sicherheits-Clip, BS.1770-Meter mit demselben Interpolator | Kennlinie exakt; True Peak bis 0,25 fs innerhalb 0,10 dB (Grenze der 4×-Abtastung 0,17 dB); 12 dB zu heißes Programm → −0,95 dBTP (Sample-Clip allein +1,57 dBTP); Gain als Rampe über 72 Samples (größte Änderung 0,0098 je Sample) |
| Auto-Gain | Master-Probe über acht gleichmäßig verteilte Takte des Tracks mit allen Korrekturen durch die Masterkette, Sekantenschritt | fünf Seeds −9,1 bis −9,6 LUFS bei Ziel −9 (der dichteste Block allein las 0,8 LU zu laut) |
| SFX (`Sfx.h`) | Riser, Downlifter, Impact (ohne Sub: Tiefenregel), Sweep, Formant-Schuss (Pre-Drop-Abriss), Reverse Swell (vorwärts synthetisiert, gleiches Leistungsspektrum wie umgekehrtes Rauschen), Zap; 24-dB-Low-Cut | Riser +44,9 dB und Schwerpunkt 670 → 4448 Hz bis zum Ziel, danach still; Swell am lautesten in den letzten 100 ms; alle Typen < −32 dB unter 140 Hz |
| SFX-Platzierung | wo ein Block eine Stimme bringt: Riser über 8 Takte oder Swell über 2, Formant-Schuss auf dem letzten Beat davor, Impact auf der Eins; Downlifter wo Stimmen gehen; Sweep in den letzten 8 Takten | 30 Impacts, 24 Formant-Schüsse, 20 Riser, alle am richtigen Ort |
| Prüfstein | kompletter Track, 288 Takte (7:57), Standardwerte | −9,95 LUFS bei Ziel −9, True Peak −0,99 dBTP, LRA 2,2 LU |

*Kalibrierung der Bandbalance* (Solos ohne Masterdynamik, Pegel per Kleinste-Quadrate gegen die
Referenz, dann gemastert an fünf Seeds nachgemessen): Kick Pitch Start 330 → 220 Hz, Lead −8 → −4 dB und
Cutoff 7,5 → 10 kHz, Arp −10 → −5 dB und Cutoff 2,2 → 3,5 kHz, Pad −16 dB bei 5 kHz, Acid −9 dB,
Percussion +1 dB. Ergebnis im Median: **Low-Mid −7,0 (Ref. −7,3), Mitten −9,2 (−8,8), Präsenz −12,2
(−9,8), Luft −13,1 (−13,3)**. Offen bleibt die Präsenz mit 2,4 dB (vorher 3,5 dB).

*Gegenprobe.* Sechs Fehler eingebaut, jeder gefunden: Limiter ohne gleitenden Mittelwert (Gain-Sprung
0,67 je Sample; die True-Peak-Prüfung allein hätte ihn nicht bemerkt), Voicing ohne Stimmführung (93 von
200 falsch), Wavetable immer aus Stufe 0 (−12 dB), Gate-Flanke als Sprung, Hall-Gains für die doppelte
Nachhallzeit (T60 3,53 s), Auto-Gain-Probe nur aus der Trackmitte (−10,7 LUFS).

*Laufzeit.* Render mit Standardwerten 5,4 % eines Kerns, mit allen Stimmen in jedem Track 7,3 %. Der
Selbsttest dauerte durch die neuen Proben 392 s; Tests, die nicht den Pegelangleich prüfen, schalten
die Proben jetzt ab (155 s), `PHOS_ONLY` wählt einzelne Abschnitte.

*Abweichungen vom Plan, bewusst:* Die 608 Tabellen aus Noctuarys WavetableLib werden nicht kopiert (keine
Dateien, keine Samples, Quest-Speicher); sechs Tabellen entstehen im Code. Der Clipper vor dem Limiter
ist neu: ohne ihn erreichte der Mix bei +8 dB nur −9,9 LUFS, mit ihm −8,6. Nicht gebaut: Stutter,
Tape-Stop, Pitch-Delay, Phaser, Bitcrush (Bus-Effekte), Kanal-EQ, Convolver; das Pre-Drop-Vakuum als
Regel (nur der Abriss wird gesetzt) und die Platzierung an echten Sektionsgrenzen kommen mit der
Form-Grammatik in Phase 5. Der Limiter hat 77 Samples Latenz (`Engine::latencySamples()`).

Gesamt: 147 Selbsttest-Prüfungen, Vektortests 9 von 9 in drei Pfaden.

**16.09.2026, Literaturrunde vor Phase 5.** Der Nutzer brachte eine Zusammenstellung musikwissenschaftlicher
Quellen und ein SOTA-Review. Primärquellen gelesen, soweit erreichbar; Ergebnis nach Verlässlichkeit:

| Quelle | Gelesen | Befund für Phosphene |
|---|---|---|
| Grosz, Solberg, Katz, Vu, Jensenius, Patel-Grosz, "An outline of the narrative grammar of electronic dance music", Musicae Scientiae 2025 | ja (Volltext) | Kategorien Intro/Breakdown → Buildup → **Pre-Drop Break (PDB)** → Core → **Cut**/Outro; Notwendigkeit Core > {Buildup, Cut/Outro} > {Breakdown/Intro, PDB}; Zwei-Drop-Standardform; PDB 1,5 bis 2,5 s (Snare-Roll auf jeder dritten Sechzehntel, Uplifter, Bass-Slide); Cut 1 bis 3 s am Ende eines Core. **Übernehmen als Form-Grammatik.** |
| Solberg und Dibben, "Peak experiences with EDM", Music Perception 2019 | ja (Volltext) | Break-Routine 32 bis 97 s; U-förmige Amplitude (Breakdown tief, Build steigend, Drop = Maximum); Entzug von Bass und Kick im Breakdown; aufsteigende Riser, dann absteigender Sweep als Drop-Marker; Hautleitwert im Drop am höchsten; der beliebteste Track hatte nach dem Drop **mindestens die Lautheit und den spektralen Fluss von vor dem Break**. **Übernehmen als messbare Regeln** (Amplitude, Fluss, Bassband je Sektion). |
| Easwaran, "Psytrance and the Spirituality of Electronics", 2004 | ja (Nachdruck) | 6 bis 12 min, meist 7 bis 8; etwa 30 s atmosphärische Einleitung; zwei Hälften mit je einem Höhepunkt; neue Klänge alle 4 oder 8 Takte; eine Melodie wiederholt ein bis zwei Viertakter, bevor sie sich wandelt; Drone-Grundton, implizierte Skala, ♭2 und übermäßige Sekunde; 135 bis 145 BPM. **Übernehmen.** |
| Butler, "Unlocking the Groove", 2006 | Sekundärzitat | Hypermetrik in Zweierpotenzen, Core als "the track in its most essential form". Bereits im Plan (2.1, 6.2). |
| Cole und Hannan, "Goa Trance", Perfect Beat 3(3), 1997 (mit Chans Kritik) | nicht erreichbar | Modale Ostinati statt Kadenzen: deckt sich mit dem Korpus (88 % Akkordverbleib). |
| Farrell, Diss. Sussex 2019; "Musical Psychedelia", Routledge 2023 | nur Abstract | Klangfarbenmodulation als Narrativ. Deckt sich mit den Filterbögen; Phase 5 weitet sie auf Lead-Cutoff und Pad-Position je Sektion aus. |
| "Studie der Universität Helsinki" zur Mikro-Evolution | nicht gefunden | Gefunden ist nur eine kulturhistorische Arbeit zu Goa in Finnland. Die Regel "kein identischer Loop" ist plausibel und schon Bauprinzip (Phrasen-Figuren, Bögen), gilt aber **ohne Quelle**. |
| "9,6 Hz Alpha-Resonanz / ASSR" | keine Primärquelle | 144 BPM × 4 / 60 = 9,6 Hz ist Arithmetik, kein Befund. **Nicht übernehmen**, keine Behauptung in Doku oder GUI. |
| Pendel-Harmonik i↔♭II, i↔♭VII | keine Korpus-Zahl in der Zusammenstellung | Unser Korpus zeigt 0↔5, 0↔7, 0↔8 (IV, V, ♭VI) als häufigste Wechsel. ♭II und ♭VII kommen als **Stilprofil-Gewichte** (Goa) dazu, ersetzen die Korpus-Übergänge nicht. |

SOTA-Review, bewertet: (1) Filter/ADAA/Halbband und Kick–Bass: bestätigt, nichts zu tun. (2) Supersaw per
gemipmappter Wavetable statt PolyBLEP: seit Phase 4 vorhanden (`WaveTable` Classic, Sägezahn-Frame) und
**messbar** — Aufgabe: Aliasing der PolyBLEP-Supersaw bei hohen Noten gegen den Tabellen-Sägezahn messen und
den saubereren Weg zum Standard machen. (3) Velvet Noise für Hats: das Review schreibt es dem Plan zu, gebaut
ist weißes Rauschen; als Option prüfen (Välimäki, Alary, Politis 2017), Entscheidung nach Messung an den
Referenz-Hats. (4) Phase 8: Transformer mit 512 Tokens gegen Selective State Space Model (Gu und Dao, Mamba
2023/2024) mit hierarchischen Tokens (Meta-Token je 4 Takte, Mikro-Token je Sechzehntel). Entscheidung
**nach Held-out-NLL beider Modelle auf demselben Token-Raum**, nicht nach Reputation; der SSM-Inferenzzustand
ohne KV-Cache passt zur Quest. Kein Bau vor Phase 7.

**16.09.2026, Phase 5 fertig: Form-Grammatik, Energiebogen, Sektionsregeln, Stilprofile, Sperren,
`.phosset`.** Der Komponist baut einen Track nicht mehr aus 16-Takt-Blöcken, sondern aus Sektionen.

*Erst gemessen, dann gebaut.* `Tools/ref_style.py` liest den Künstler-Tag der 40 Referenzaufnahmen
(Auswahl über den Album-Tag) und schätzt das Tempo aus der Autokorrelation der Einsatzhüllkurve im
Kick-Band, wie `ref_slot_profile.py` es tut. Ergebnis: **Goa 142,8 BPM Median** über 13 Tracks
(Quartile 137,9 / 145,0; Spanne 128,0 bis 151,9), **Full-On 144,6** über 18 (Quartile 140,4 / 145,7;
14 der 18 sind 1200 Micrograms, der Median ist also von einem Projekt geprägt), Länge im Median 8,6
bzw. 7,7 Minuten. Neun weitere Künstler (Afgin, Cwithe, Electric Universe, Kingpink, S.U.N. Project,
Spectral) sind bewusst **nicht** zugeordnet; für Progressive, Dark/Forest und Hi-Tech gibt es weiter
keine Referenzen, deren Tempofenster stehen aus der Literatur (2.1).

Dieselbe Messung mit `--form` bestimmt die Gestalt einer Break-Routine in den Referenzen: kurzzeitige
Lautheit (3 s, 1 s Schritt) und Band 40 bis 140 Hz über den ganzen Track, tiefste 8 s in den mittleren
80 % als Breakdown genommen, alles gegen die 20 s Core davor. **Median: Breakdown −7,4 dB, Bassband
−20,6 dB, nach dem Drop +0,1 dB; 33 von 40 Tracks sind nach dem Drop innerhalb 1 dB des Core vor dem
Break oder lauter.** Das bestätigt Solberg und Dibben an diesem Material und liefert die Zahlen, gegen
die die Sektionsregeln unten prüfen. (Ein Track ist in der Mitte stumm und liest −184 dB; er verzerrt
den Median nicht.)

| Baustein | Umsetzung | Messung |
|---|---|---|
| Form-Grammatik (`Form.h`, `Form.cpp`) | gewichtete kontextfreie Grammatik mit den drei Körpern aus 6.2; Längen als **Constraint-Problem** gelöst statt per Reparaturschleife: Intro, Outro, Builds und Breaks aus ihren erlaubten Mengen aufgezählt, die Cores tragen den Rest als Summe aus {16, 32, 64} (aus 16a+32b+64c = R und a+b+c = n folgt b+3c = R/16 − n), gewählt wird die Kombination, die dem gewürfelten Wunsch am nächsten liegt | 1400 Formen (5 Stile × 7 Ziellängen × 40 Seeds): 0 verletzen eine Regel, 0 verfehlen die Ziellänge, Break-Anteil 0,17 bis 0,29, alle drei Körper kommen vor |
| Kategorien nach Grosz et al. | `SectionType` um **Pdb** und **Cut** erweitert (angehängt, damit die MIDI-Marker der alten Typen stehen bleiben); PDB = letzter Takt eines Builds mit vier Varianten (ganzer Takt, halber Takt, nur Beat 4, Kick allein auf 4), Cut = 1 oder 2 Beats am Kopf eines Breakdowns, in denen außer der Hallfahne nichts steht | 200 Tracks: alle haben ≥ 2 Cores (Zwei-Drop-Standard), Intro und Outro immer 8 oder 16 Takte, alle vier PDB-Varianten gezogen (103/82/74/73 von 332 Builds), 234 Cuts |
| Energiebogen | E(t) über das Set aus fünf Dramaturgien (Warm-up, Peak-Time, Morning, Closing, Flat) als Raised-Cosine-Segmente; Sektionsenergie = Typenergie × (0,55 + 0,45·E), Builds laufen von der Energie davor zur Energie des Drops | größter Sprung über ein Tausendstel des Sets 0,0013, Wertebereich 0,20 bis 1,00, Ordnung Drop > Groove > Breakdown an jedem Punkt jedes Bogens; über ein 60-Minuten-Set: Peak-Time 0,64 → 0,68, Closing 0,61 → 0,52 |
| Farbood-Größen | Lautheit als Rampe auf `mix.track_gain` (höchstens ±2 dB, der Pegelangleich bleibt), Dichte über Percussion-Schichten und anwesende Stimmen, Register über Arp- und Lead-Oktave (immer innerhalb der Tiefenregel), Dissonanz als **Gewicht** auf ♭2 und übermäßige Sekunde in den Constraint-Mengen des Leads | Farbanteil im Lead 7,4 % bei Farbe 0, 46,0 % bei Farbe 1 |
| Constraint-Sampler mit Gewichten | `allowed[i][s]` ist nicht mehr Flagge, sondern **relatives Gewicht** (0 verbietet); ein Faktor, der an einer Position für alle Symbole gleich ist, kürzt sich in beiden Normierungen, deshalb ist das alte Verhalten (überall 1) bitgleich | Sampler-Prüfungen gegen die vollständige Aufzählung unverändert (Totalvariation 0,006 / 0,005) |
| Instrumentierungs-Matrix | ersetzt `blockParts`: Intro schichtet Percussion von null auf und lässt die Kick zwischen Takt 5 und 9 einsetzen, Groove ist der Kern ohne Lead in der ersten Hälfte, Build holt pro 4 Takte eine Schicht zurück mit dichteren Hats und Snare-Roll in den letzten 4 Takten, **Drop bringt alles auf einmal**, Breakdown nimmt Kick und Bass heraus und dünnt Lead/Arp aus, Outro nimmt pro 4 oder 8 Takte eine Schicht weg | im Score über 4 Tracks: 0 Kicks und 0 Bassnoten in Breakdowns, Intro-Kick in 4 von 4 Fällen zwischen Takt 5 und 9, 9 von 9 Drops mit Kick, Bass und Melodik, 0 Noten auf Beat 4 von 8 PDBs |
| Alle 4 oder 8 Takte etwas Neues | jede Achttaktgruppe einer Sektion zieht ihre Änderung (Schicht, Figur, Fill, Register) und nie dieselbe wie die Gruppe davor; die Bassfigur am Gruppenende kommt aus einer Menge, deren Glieder sich **in der letzten Note für jedes Bassmuster** unterscheiden, damit die Garantie auch für die einnotigen Muster gilt | 91 aufeinanderfolgende Gruppenpaare in Cores, 0 identisch -- geprüft mit zwei Hashes, über alles und **nur über den Bass**; der zweite deckte auf, dass ein Paar von sechzehn sich wiederholte (siehe Gegenprobe) |
| Sektionsregeln, gerendert (Solberg und Dibben 2019) | ganzer Track gerendert und zurückgelesen | U-Form: Core −13,3 dB, Breakdown −23,3 dB (**10,0 dB tiefer**), nach dem Drop −13,2 dB (**+0,1 dB**); Build steigt über alle vier Viertakt-Fenster (−15,6 → −14,3 dB, 0 fallende); Band 40 bis 140 Hz im Breakdown **65,3 dB** unter dem Core; Beat 4 des PDB **57,9 dB** unter einem Core-Beat; Präsenz 1,5 bis 6 kHz nach dem Drop **+0,6 dB** gegen vor dem Break |
| Stilprofile (`compose.style`) | fünf Profile als Gewichtsvektoren: Tempomitte und -spanne (gemessen für Goa und Full-On), Skalengewichte der Tonartenreise, Körpergewichte der Grammatik, zusätzliche Akkordzüge (Goa i↔♭II und i↔♭VII **zusätzlich** zu den Korpus-Übergängen), Multiplikatoren auf Acid/Lead/Arp/Pad, Squelch-Chance, Hat-Dichte, PDB-Varianten, Bass-Slot-Hüllkurve (flach als Standard, 6.6), Break-Anteil, Farbe, Introlänge | Full-On ist der Standard mit lauter Einsen, deshalb spielen die Knöpfe unverändert, was sie sagen; `compose.style_tempo` schaltet auf die Profil-Tempi um |
| Set-Walk gegen Track-Entscheidungen | Länge, Tonart, Modus, Tempo und die beiden Klang-Rezepte kommen aus einem Walk, der **nur** am Set-Seed hängt; Form, Percussion, Melodik, Bassmuster und Gate hängen am Seed des Tracks. Ohne diese Trennung würde ein neu gewürfelter Track alle folgenden verschieben | Tracklängen sind Vielfache von 32 Takten, jede Trackgrenze liegt auf dem 32er-Raster (5 von 5 Übergängen) |
| Sperren und Neuwürfeln (6.8) | `setLock`/`reroll`/`variation` für Set, Track, Sektion und Pattern-Lane; ein gesperrter Baustein ist auf seinem ursprünglichen Seed eingefroren, ein neu gewürfelter mischt seinen Zähler in seinen Seed | Track 3 neu gewürfelt: 4 von 4 anderen Tracks bitgleich in Noten und Steuerereignissen (gemessen außerhalb der 16-Takt-Überblendfenster, die ein Übergang absichtlich mit dem Nachbarn teilt), Track 3 geändert; gesperrte Sektion behält ihren Seed |
| `.phosset` (7) | Kopfzeile `phosset 1`, dann `seed=`, `style=`, `arc=`, die geänderten Knöpfe aus `ParamStore::toText(true)` und `lock.<einheit>.<index>` / `reroll.<einheit>.<index>`; `phos_render --set-file` und `--save-set`, dazu `--lock`/`--reroll` auf der Kommandozeile | Rundlauf: 0 Knopfunterschiede, gleiche Sperren und Zähler, gleiche Partitur in 4 Tracks |
| Übergänge zwischen Tracks (6.7) | Überblendfenster von 16 Takten: die Hats des nächsten Tracks laufen über das Outro des vorigen ein, dessen Pads bleiben 16 Takte stehen — aber nur, wenn die Tonarten verwandt sind (Prim, Quart, Quint; harmonisches Mixen nach Ishizaki et al. 2009), sonst hören sie auf; der Tonartwechsel liegt auf der 32er-Grenze und wird vom Sweep des Outros, der genau dort endet, und einem Impact verdeckt | 5 von 5 Übergängen mit vorgezogenen Hats, 3 von 3 verwandten Tonarten mit bleibenden Pads, 5 von 5 Tonartwechseln maskiert |
| SFX an Formgrenzen | Riser über die letzten 8 Takte eines Builds, endet auf dem Drop; Formant-Schuss auf dem letzten Beat des PDB; absteigender Sweep in den Drop (Solberg und Dibben: der Sweep ist die Drop-Marke); Impact auf der Eins; Downlifter und Reverse Swell am Breakdown; Sweep über die letzten 8 Takte des Tracks | 16 Impacts, 14 Formant-Schüsse, 14 Riser, 20 Sweeps über 8 Tracks, 0 am falschen Ort |
| Auto-Gain nachgeschärft | die Mix-Probe nimmt jetzt **vier zusammenhängende Vier-Takt-Fenster**, das erste auf Takt 0 und das letzte am Trackende, und hört die Lautheitsseite des Energiebogens mit | vorher 8 Einzeltakte aus der Mitte: fünf Seeds −10,3 bis −9,9 LUFS bei Ziel −9; jetzt **−9,9 bis −8,7**, Median −9,5 |

*Gegenprobe (Mutationsrunde).* Sieben Fehler einzeln eingebaut, jeder von seiner
Prüfung gefunden -- zwei davon erst, nachdem Prüfung oder Regel geschärft wurden:

| Mutation | Wer merkt es |
|---|---|
| Längenlöser ignoriert den Break-Anteil | 343 von 1400 Formen verletzen die Regeln, Anteil 0,11 bis 0,40 |
| jede Achttaktgruppe endet auf derselben Figur | 91 von 91 Paaren mit gleichem Bass |
| Breakdown behält Kick und Bass | 987 Kicks und 2963 Bassnoten in Breakdowns; U-Form nur noch 2,1 dB tief; Bassband nur 3,3 dB unter dem Core |
| PDB behält Kick und Bass auf allen vier Beats | 18 Noten auf Beat 4; Bassband auf Beat 4 **1,6 dB über** einem Core-Beat statt 30 dB darunter |
| Neuwürfeln erreicht den Track-Seed nicht | Track 3 ändert sich nicht |
| Energie bewegt die Verstärkung nicht | Build fällt in 2 von 4 Fenstern |
| `.phosset` schreibt Sperren und Zähler nicht | Rundlauf verliert Sperren und Zähler |

Zwei Befunde aus der Gegenprobe sind echte Korrekturen: (1) Die Gruppenprüfung *bestand* mit
ausgebauter Regel, weil zufällig ein Percussion-Fill verschieden war; sie hasht jetzt zusätzlich **nur
den Bass**, und das deckte sofort ein echtes Loch auf -- eine Gruppe, deren Figur schon verschoben
worden war, wurde gegen ihren *rohen* Wurf verglichen, also wiederholte sich ein Paar von sechzehn (7
von 91). Die Figur wird jetzt von der ersten Gruppe der Sektion an durchgelaufen. (2) Die erste
PDB-Mutation traf nur die Kick-Maske, die das Standard-Kickmuster "Four + Fills" auf Beat 4 ohnehin
verdeckt; erst mit der Bass-Maske schlagen beide Prüfungen an.

*Prüfstein: ein Set aus einem Seed.* `phos_render --minutes 60 --seed 20260916 --tracks --sections --report --midi --save-set` mit
`compose.style=Goa compose.style_tempo=On compose.arc=Peak-Time compose.set_minutes=60`:

- **9 Tracks**, 2272 Takte, 139,5 bis 144,5 BPM (Goa-Profil, Mitte 143), Tonarten F#, E, B (Quinten
  und Ganztöne), Körper Full-On und Goa gemischt, 8 bis 9 Sektionen je Track.
- **Laufzeit 234 s für 3600 s Audio: 15,4-fache Echtzeit, 6,5 % eines Kerns** (Phase 4: 5,4 % ohne
  Form, 7,3 % mit allen Stimmen in jedem Track).
- **Lautheit −9,5 LUFS integriert** bei Ziel −9, True Peak −0,98 dBTP, **LRA 6,7 LU** (Referenz-Median
  6,3 LU -- die Form bringt den Dynamikumfang echter Tracks). Je Track gespielt: −8,1 bis −10,6 LUFS,
  Spanne 2,5 LU; der Pegelangleich hält den *Klang* zusammen, der Energiebogen darf die Tracks
  bewusst um ±2 dB auseinanderziehen.
- **MIDI: 76550 Ereignisse**, 676 kB. Unabhängig nachgelesen (eigener Parser in Python, nicht der
  Leser des Projekts): SMF **Format 1**, PPQ 960, 9 benannte Spuren (Phosphene, Kick, Bass, Perc,
  Acid, Lead, Arp, Pad, Sfx), **99 Sektionsmarken** in allen acht Kategorien (15 Intro, 3 Groove,
  15 Build, 15 PDB, 23 Drop, 10 Cut, 10 Break, 8 Outro), 5 Tonartwechsel, 586 Tempo-Ereignisse,
  76342 Note-Ons.
- **Memorisierung 0,00 %**: 0 von 669 Acid-, 0 von 136 Lead- und 0 von 253 Arp-Takten stimmen
  transpositionsinvariant mit einem Korpustakt überein.
- **`.phosset`**: die gespeicherte Datei (7 Zeilen) rendert dieselben 64 Takte **byteweise identisch**
  wie die Kommandozeile, aus der sie entstand.

*Abweichungen vom Plan, bewusst:*
- **Tracklängen liegen zwischen 128 und 320 Takten** (3:32 bis 8:50 bei 145 BPM). Der Knopf
  `compose.track_bars` wird in dieses Fenster geklemmt. Grund: die Grammatik muss jede Ziellänge mit
  **jedem** Körper exakt treffen, weil die Länge zum Set-Walk gehört und der Körper zum Track; das
  gemeinsame Fenster der drei Körper ist genau dieses. Die Referenzaufnahmen laufen im Median 7,7 bis
  8,6 Minuten, liegen also darin.
- **Breakdowns sind nie 8 Takte lang.** Solberg und Dibben messen Break-Routinen von 32 bis 97 s; 8
  Takte sind 13 s. Erlaubt sind 16, 32 und 64.
- **Der Standard-Bogen ist "Flat"** und `compose.style_tempo` steht auf Aus, damit die Knöpfe ohne
  weitere Einstellung genau das spielen, was sie sagen. Ein Set mit Dramaturgie braucht
  `compose.arc=Peak-Time` (o. ä.) und `compose.set_minutes`.
- **Register und Maskierungsregel sind Sektions-, nicht Taktentscheidungen.** Innerhalb eines Builds
  steigt die Energie; eine Lead-Oktave, die auf halber Strecke springt, hätte die Maskierungsregel
  gebrochen, mit der die Sektion geplant wurde.
- **Der Pegelangleich misst weiter den Klang, nicht die Form:** die Proben je Stimme spielen alles, was
  der Track hat, nur die Mix-Probe folgt der Instrumentierungs-Matrix.
- **Nicht gebaut:** die Profil-Reise (Morph zwischen Stilprofilen über das Set), der Ranker aus 6.9,
  das Ausrollen der Partitur in die `.phosset`-Datei, und Stems je Erzeuger. Die Bass-Slot-Hüllkurve
  ist als Profilparameter da, aber in allen fünf Profilen flach, weil die Messung vom 15.09. keine
  andere rechtfertigt.

Gesamt: 170 Selbsttest-Prüfungen (24 neue in vier Abschnitten: `testForm`, `testSectionRules`,
`testCuration`, `testTransitions`), Vektortests 9 von 9 in AVX2, NEON-Shim und skalar. Der Selbsttest
dauert 231 s statt 155 s: 17 s kostet der gerenderte Track der Sektionsregeln, den Rest die beiden
Prüfungen, die jetzt in einem Core statt in Takt 0 messen müssen (Phasenkopplung, Pegelangleich) und
deshalb bis dorthin rendern. Beide sind so eingestellt, dass sie nur rendern, was sie messen: ohne
Melodik, und die Phasenkopplung sucht sich den ersten Seed mit einem achttaktigen Intro.

Nächster Schritt (Stand nach Phase 5): Phase 6 und 7 sind in derselben Nacht in eigenen Arbeitsbäumen
entstanden und gemergt (Blöcke unten); danach Phase 8 (Transformer) und Phase 9 (Qualität). Offen aus Phase 5: die Profil-Reise (Morph
zwischen Stilprofilen über das Set), der Ranker aus 6.9, Stems je Erzeuger und das Ausrollen der
Partitur in die `.phosset`-Datei.

**16.09.2026, Phase 7: Quest-Build, Qualitätsstufen, Performer-App.** Gebaut in einem eigenen
Arbeitsbaum, parallel zu Phase 5 und 6. **Es war kein Headset angeschlossen**; alle Gerätezahlen
sind offen, die Toolchain steht fertig gebaut bereit.

| Baustein | Umsetzung | Messung |
|---|---|---|
| arm64-Toolchain | NDK r27c, `cmake -G "Unix Makefiles"` mit dem `make.exe` des NDK (dieses SDK hat kein ninja), `ANDROID_ABI=arm64-v8a`, `ANDROID_PLATFORM=android-29`; Android-Zweig im Wurzel-`CMakeLists.txt`, `add_test` fällt auf Android weg (ctest kann arm64 nicht auf dem Host starten), Testziele mit `-Wall -Wextra` wie der Kern | `libPhospheneCore.a` 10,5 MB, `phos_selftest` 8,82 MB, `phos_vectest` 1,78 MB, `phos_render` 5,91 MB, alle ELF aarch64 PIE für Android 29, ungestrippt mit Debug-Info |
| Ein Compile-Fehler des NDK | `DenormalGuard` (`Dsp.h`) schrieb das FZ-Bit über `fenv_t::__fpcr`; **bionic nennt das Feld `__control`**, glibc `__fpcr`. Jetzt `mrs`/`msr fpcr` direkt — das, was `fesetenv` ohnehin tut, und auf jeder AArch64-libc gleich | sonst fand clang nichts, was MSVC hatte durchgehen lassen; zwei Warnungen bleiben (`Dynamics.h::minFilled_`, `selftest.cpp::engineSwitches`, beide vorbestehend und in fremden Dateien), `Acid.h::combOn_` war tot und ist raus |
| Qualitätsstufen (`Quality.h`) | Struktur mit Desktop und Quest, gewählt in `Engine::prepare(sr, block, quality)` mit Desktop als Vorgabe, damit Selbsttest, Vektortests und jeder bisherige Aufrufer unverändert rendern. Quest: Acid-Oversampling 2× → 1×, Bass bleibt 2×, Unisono 7 → 3, Pad-Polyphonie 8 → 4. Neu: `phos_render --quality quest|desktop` | Selbsttest 147/147 und Vektortests 9/9 in drei Pfaden unverändert; die Ausgaben der beiden Stufen unterscheiden sich, die Lautheit nicht (beide −9,7 LUFS, True Peak −0,99 dBTP über 64 Takte) |
| Ersparnis der Stufe Quest | 8 min, alle Stimmen (`compose.{pad,acid,lead,arp}_amount=1`), 48 kHz, Block 256, i9-12900K, AVX2, je drei Läufe | Desktop **7,19 / 7,21 / 7,21 %** eines Kerns, Quest **6,05 / 5,98 / 5,96 %**: **16,9 % weniger**. Die Aufteilung auf die drei Schalter ist offen — ein Knopf wie `acid_amount=0` ändert das ganze Arrangement, also lässt sie sich über die Knöpfe nicht isolieren. Der Gerätewert (Ziel ≤ 30 % eines großen Kerns) ist offen |
| Oversampling-Schalter | `Bass::setOversampling` / `Acid::setOversampling`: bei 1× ein Oszillator- und ein Leiterschritt je Ausgabesample, kein Halbband, und die Koeffizienten (Filterhüllkurve, Squelch, Akzent-Kondensator) rechnen auf der Basisrate. Die Cutoff-Obergrenze kommt bei 2× aus dem Durchlassband des Dezimierers (0,2 der hohen Rate), bei 1× aus der Stabilität (0,45 fs) — bei 48 kHz liegt beides über der 18-kHz-Kappe, die Stufe verliert also keinen Stellbereich | der Bass behält 2×: seine Leiter wird von einer schnellen Hüllkurve über einen Grundton gefahren, auf den die Kick phasengekoppelt ist, und genau dort landen seine Aliasprodukte |
| `Poly`-Grenzen ohne `Poly.cpp` | öffentlicher Setter `setQuality(unison, voices)` und `noteOnLimited()`, beides inline im Header, `Engine::dispatch` ruft es statt `noteOn`. **Stimmen:** `noteOn` nimmt die erste freie Stimme und stiehlt erst die älteste, wenn keine frei ist — es genügt also, die oberen Stimmen nie belegen zu lassen: sind alle unter der Grenze aktiv, wird hier die älteste davon stillgelegt und ist damit die erste freie. Eine stumme Stimme kostet nichts, weil `renderSegment` eine Achtergruppe überspringt, wenn keine ihrer Stimmen klingt. **Unisono:** die äußeren Oszillatorpaare bekommen Gain 0, die übrigen werden auf gleiche inkohärente Leistung hochskaliert (Mitte und Szabos engstes Paar bleiben) | Gegenprobe mit acht sich überlappenden Pad-Noten: Spitzenzahl klingender Stimmen 8 (Desktop) gegen 4 (Quest), also greift die Grenze; 4 von 8 Stimmen lassen 28 der 56 Oszillator-Slots und eine der beiden Filter-Achtergruppen ungerechnet. Eine einzelne Note ist auf beiden Stufen gleich laut (−27,56 gegen −27,49 dB RMS): die Renormierung des verkürzten Unisonos stimmt. **Das Unisono-Limit spart dagegen nichts**: der Kernel rechnet die Slots ohnehin. Es ist heute eine Klangentscheidung (erledigt am 16.09., siehe *Nachtrag Unisono-Begrenzung* am Ende der DSP-Qualitätsrunde) |
| Quest-App (`Quest/`) | NativeActivity + `android_native_app_glue`, OpenXR mit `XR_EXT_hand_tracking`, EGL/GLES 3, Oboe Low-Latency-Float-Stream. Drei Threads: Audio (`Engine::process` plus Blende, ein Compare-and-Exchange, kein Lock, keine Allokation), Komponist (plant, komponiert, füllt die Ringe acht Takte voraus, veröffentlicht die Anzeige), Render (OpenXR-Schleife, Hände, Bild) | APK 3,5 MB, `libphosquest.so` 10,3 MB, mit Debug-Schlüssel signiert, ohne Warnung gebaut |
| Track-Sprung ohne Kerneingriff | `SetPlayer` ist der `Conductor` mit einem Takt-Versatz: die Engine spielt immer ab ihrem eigenen Beat 0, jedes komponierte Ereignis und die Tempo-Karte werden um den Beat des ersten zu spielenden Takts zurückgeschoben. Damit kann Takt 700 des Sets der erste Takt der Engine sein — `Composer.h` musste nicht angefasst werden | am Trackanfang exakt: dort hält die Quell-Karte das Tempo des neuen Tracks; ein Sprung mitten in eine Rampe verlöre deren Steigung im ersten Segment |
| Performer-Oberfläche (8.2) | kopffeste Punkte-Tafel (nur Gier, nicht Nicken): Track und Takt, Tonart, Tempo, 16-Takt-Block mit seinen Stimmen, Lautheit, beide Makrowerte, vier Beat-Lampen und eine Lautheitsreihe. Linker Pinch Play/Stop (15-ms-Blende, die Musik hält an, wo sie ist), rechter Pinch nächster Track, linke Handhöhe `mix.track_gain` (−12…+12 dB), rechte Handhöhe Acid-Cutoff (±2 Oktaven um den komponierten Wert) | Höhe wird am Kopf gemessen, nicht am Boden, also gleich im STAGE- und im LOCAL-Raum und für jede Körpergröße; ein Makro folgt nur der *offenen* Hand, damit der Pinch nicht zugleich die Verstärkung mitzieht; 0,15-s-Einpol-Glättung, beide Makros mittig — nichts springt. Beat-Lampen als Raised-Cosine über den Beat-Abstand, kein Blitz |
| Sektion auf der Tafel | Die Form-Grammatik ist Phase 5 und liegt im Nachbar-Arbeitsbaum. Die Tafel zeigt deshalb den 16-Takt-Block und seine Stimmen (`MelodyPlan::blockParts`, `padGate`) als Platzhalter | tauschen, sobald `Form.*` gemerged ist |
| OSC-Cue-Brücke (8.3) | gebaut statt weggelassen: `/phos/bar f f` (Takt, BPM) je Takt, `/phos/track f f f` (Track, Tonart, Skala) bei jedem Trackwechsel, Ziel aus `phos.cfg` | aus der Partitur, nie aus einer Audioanalyse |
| Stumm starten | `phos.cfg mute=1` schaltet den Ausgang stumm, lässt aber Engine, Komponist und Bild laufen, damit ein Testlauf alles durchläuft, ohne zu klingen | Regel „Synth stumm starten" |

*Was `Poly.cpp` für ein echtes Unisono-Limit braucht* (damals nicht geändert, weil die Datei einem
anderen Arbeitsbaum gehörte; am 16.09. so gebaut und gemessen, siehe *Nachtrag Unisono-Begrenzung*
am Ende der DSP-Qualitätsrunde): ein Feld `unison_` mit Setter; in `noteOn` die Schleife über die sieben
Oszillatoren auf die mittleren `unison_` beschränken und die übrigen Slots auf Gain 0 und `dt = 0`
setzen; in `renderSegment` die Gruppenentscheidung `on = on || voiceOn[s / kPolyUnison]` zusätzlich
prüfen lassen, ob der Slot innerhalb des Limits liegt, und die Summenschleife über `u` auf `unison_`
verkürzen. Erst dann spart Unisono 3 die vier Oszillatoren wirklich. Mit einer dichteren
Slot-Belegung (Stimme × `unison_` statt Stimme × 7) wäre mehr zu holen, das verschiebt aber die
Lane-Zuordnung und damit die Bitgleichheit der Vektortests.

*Grenze der Pad-Polyphonie.* In derselben Gegenprobe war die Stufe Quest mit acht gleichzeitigen
Pad-Noten 3,5 dB leiser — genau das, was das Halbieren inkohärent addierter Stimmen kostet. In der
echten Musik fällt das kaum an: der Komponist schreibt vierstimmige Pad-Voicings (`Melody.h`), acht
gleichzeitige Pad-Noten gibt es nur beim Wechsel von Akkord zu Akkord, solange die alten noch
ausklingen. Auf dem Gerät nachhören.

*Bewusste Abweichungen.* „Reverb-Modus Classic" und „Convolver aus" aus Abschnitt 9 brauchen keinen
Schalter: die Sends sind ein FDN ohne Modi, einen Convolver gibt es in Phosphene nicht. Der Komponist
plant beim Start und nach jedem Sprung 1024 Takte (gut eine halbe Stunde) im Voraus, weil die
Tempo-Karte so weit reichen muss; die Tafel zeigt währenddessen PLANNING, und der Audiostream startet
erst danach. Die Lautheitsanzeige liest `Engine::meter()` vom Renderthread — im schlimmsten Fall ein
zerrissener Wert, für eine Anzeige egal.

*Offen und nächster Schritt.* Sobald ein Headset da ist, in dieser Reihenfolge: `phos_vectest` muss
`path neon` und alle Lanes bitgleich melden; `phos_render --bench` je Qualitätsstufe auf den großen
Kernen (`taskset f0`) gegen die 30-%-Grenze; dann Session-Zustandsfolge, Swapchain-Format,
Handtracking-Abfrage, Oboe-Start und die Pinch-Schwellen (22 mm zu, 38 mm auf — aus Noctuary geraten).
Die genauen Befehle stehen in `Quest/README.md`.

**16.09.2026, DSP-Qualitätsrunde: Supersaw-Aliasing, Velvet Noise, Lead bei 2×, Kostentabelle.**
Anlass sind die drei messbaren Punkte des SOTA-Reviews (Absatz Literaturrunde). *Erst gemessen, dann
entschieden;* die Zahlen stehen unten auch dort, wo das Ergebnis "nichts tun" ist.

*Das Maß.* `inharmonicDb` zählt alles, was keine Harmonische von f0 ist, als Aliasing — bei einer
verstimmten Supersaw sind das sechs von sieben Oszillatoren. Bei Detune 1 liest es **+7,9 dB, egal
welcher Oszillator spielt**, und sagt über Aliasing nichts. Die Runde misst deshalb mit
`supersawAliasDb`: die erlaubten Linien einer Supersaw sind {h · (1 + a_u y) · f0}; alles andere
zwischen 100 Hz und 18 kHz ist Aliasing. Die Bänder um die erlaubten Linien decken bei den gemessenen
Noten 4 bis 25 % der Bins ab, das ist unter einem dB Verzerrung des Ergebnisses.

**1. Supersaw: PolyBLEP gegen gemipmappten Tabellen-Sägezahn.** Gehaltene Note, 65536-Fenster 0,2 s
nach dem Einsatz, 48 kHz, Lead- und Pad-Instanz gleich eingestellt (Cutoff 18 kHz, `hp_track` 0, keine
Positions-Hüllkurve, kein LFO, Mix 0,75).

| Note | Detune | PolyBLEP 1× | Tabellen-Säge | PolyBLEP 2× + Halbband | `inharmonicDb` (alle drei) |
|---|---|---|---|---|---|
| C4 | 1,00 | −49,6 dB | **−76,7 dB** | −71,5 dB | +7,3 dB |
| C5 | 1,00 | −46,6 dB | **−72,4 dB** | −68,4 dB | +7,9 dB |
| C6 | 1,00 | −43,3 dB | **−68,9 dB** | −65,1 dB | +7,9 dB |
| A6 | 1,00 | −40,9 dB | **−61,6 dB** | −62,3 dB | +7,9 dB |
| C4 | 0,55 | −49,9 dB | **−80,8 dB** | −71,5 dB | −5,6 / −6,7 dB |
| C5 | 0,55 | −45,8 dB | **−76,0 dB** | −67,6 dB | +0,6 dB |
| C6 | 0,55 | −43,1 dB | **−73,2 dB** | −64,9 dB | +3,3 dB |
| A6 | 0,55 | −40,8 dB | **−61,3 dB** | −62,4 dB | +4,0 dB |

Die Tabelle ist ab C5 um 26 bis 30 dB sauberer, bei A6 um 20 dB — weit über der Schwelle von 10 dB.
Sie ist dabei **nicht dunkler**: Leistung über 8 kHz bei Detune 0,55 PolyBLEP −22,1 / −18,8 / −15,9 /
−13,6 dB, Tabelle −21,6 / −17,5 / −14,5 / −12,7 dB (C4 bis A6), obwohl die gewählte Mipmap-Stufe bei
A6 nur acht Harmonische bis 14 kHz führt. **Die Alternative 2× mit dem vorhandenen `HalfbandDown`
gewinnt nicht:** sie bleibt 3 bis 10 dB hinter der Tabelle und kostet einen zweiten Oszillator-Durchlauf.
Entscheidung: **Supersaw liest die Säge der Classic-Tabelle** (Frame 2), jeder der sieben Oszillatoren
auf seiner eigenen Stufe, mit Szabos Detune, Mix, Zufallsphasen und Hochpass unverändert. **PolyBLEP
bleibt der VA-Typ** (dort wird die Säge in einen Puls geblendet, was eine Tabelle nicht kann).

Ein Tabellen-Frame ist auf `kTargetRms` = 1/(2√2) normiert, die Rampe 2t−1 hat 1/√3; die
Schacht-Verstärkung trägt deshalb `kSawTableGain` = 2√(2/3). Gemessen bleibt der Pegel des Leads gleich
(**+0,24 dB** gegen die Rampe), die Phase-4-Kalibrierung gilt weiter.

*Nebenbefund, mitgenommen:* `polySlotKernel` rechnete für **jede** Stimme beide gefalteten
Taylor-Sinus der Frequenzmodulation, auch für Supersaws und Pads, die `wFm` mit 0 gewichten. Der Kernel
lässt eine Quelle jetzt aus, wenn die ganze Achtergruppe sie mit 0 gewichtet — dieselbe Entscheidung pro
Gruppe wie das vorhandene "spielt hier eine Stimme", also auf allen Vektorpfaden gleich und bitgleich.

**2. Velvet Noise für Hats: abgelehnt, Negativbefund.** `Tools/ref_hat_texture.py` misst im Band
6 bis 16 kHz die spektrale Flachheit (geometrisches durch arithmetisches Mittel, Median über
1024er-Hann-Fenster), den Crest-Faktor ganz und über 100-ms-Fenster, die Kurtosis und den Anteil der
Samples innerhalb 20 dB unter dem Spitzenwert. 40 Referenztracks (Album-Tag "Psytrance Collection",
je 60 s ab 2:30) gegen Phosphene-Renders über 210 s:

| Quelle | Flachheit | Crest | Crest 100 ms | Kurtosis | Duty |
|---|---|---|---|---|---|
| **Referenz, Median von 40** | **0,299** | **22,4 dB** | **15,2 dB** | **9,4** | **0,45** |
| Referenz, Quartile | 0,237–0,336 | 21,3–23,5 | 14,4–16,1 | 6,7–12,3 | 0,36–0,59 |
| Phosphene, ganzer Mix, weiß | 0,275 | 26,2 dB | 19,0 dB | 18,6 | 0,37 |
| Phosphene, ganzer Mix, Velvet 1000/s | 0,275 | 26,5 dB | 20,3 dB | 23,7 | 0,44 |
| Phosphene, ganzer Mix, Velvet 2000/s | 0,277 | 25,8 dB | 19,5 dB | 19,8 | 0,43 |
| Phosphene, ganzer Mix, Velvet 4000/s | 0,276 | 25,0 dB | 19,0 dB | 17,3 | 0,41 |
| Phosphene, `--solo perc`, weiß | 0,333 | 26,7 dB | 20,4 dB | 36,1 | 0,15 |
| weißes Rauschen pur | 0,563 | 11,7 dB | 10,2 dB | −0,3 | 1,00 |
| Velvet pur, 500 bis 8000 Impulse/s | 0,59 bis 0,56 | 20,8 bis 11,5 dB | 16,4 bis 10,4 dB | 22,6 bis −0,2 | 0,62 bis 1,00 |

Das Ergebnis dreht die Erwartung um: **Phosphenes Hat-Teppich ist bereits transienter als die
Referenzen**, nicht glatter (Crest 26,2 gegen 22,4 dB, Kurtosis 18,6 gegen 9,4). Velvet Noise erhöht
Crest und Kurtosis bei geringer Dichte und ist ab 4000 Impulsen pro Sekunde von weißem Rauschen nicht
mehr zu unterscheiden — es bewegt die Messgrößen also entweder **weiter weg** von der Referenz oder
gar nicht. Auch der Kostenvorteil bleibt aus: der Prototyp maß 0,49 bis 0,55 % eines Kerns gegen 0,54 %
mit weißem Rauschen, also nichts über der Streuung, weil in dieser Lane ohnehin ein SVF pro Sample
läuft und die Sparsamkeit nur die Erzeugung betrifft (in der Literatur zahlt sie sich bei Faltung und
Dekorrelation aus, nicht als Quelle vor einem Filter). `perc.noise_mode` ist deshalb **nicht** gebaut;
der Prototyp wurde nach der Messung wieder entfernt. Kontrolle gegen den Codec-Verdacht: derselbe
Render als MP3 mit 192 kBit/s gemessen ergibt 0,274 / 26,1 dB / 18,8 dB / 17,7 / 0,38 — die
MP3-Kodierung der Referenzen verfälscht diese Maße nicht. Der verbleibende Abstand zur Referenz liegt
in der Ereignisdichte und im Mastering, nicht in der Rauschquelle; das gehört zu Phase 5 und zur
Mischpultrunde, nicht hierher. Literatur dazu: Järveläinen und Karjalainen, "Reverberation modeling
using velvet noise", AES 30th Int. Conf. 2007; Välimäki, Lehtonen und Takanen, "A perceptual study on
velvet noise and its variants", IEEE TASLP 21(7), 2013; Alary, Politis und Välimäki, "Velvet-noise
decorrelator", Proc. DAFx-17, Edinburgh 2017.

**3. Lead bei 2×: nicht nötig, aber der FM-Index bekommt eine Bandbreitengrenze.** Aliasing über
12 kHz, gegen den Grundton, gemessen gegen die *erlaubten* Linien (bei FM |f_c + k·f_m|, sonst wäre
jedes Seitenband mit nicht-ganzzahligem Verhältnis "Aliasing"):

| Oszillator | C5 1× / 2× | C6 1× / 2× | A6 1× / 2× | C6 bei Lead-Cutoff 10 kHz |
|---|---|---|---|---|
| VA Säge | −37,3 / −53,1 | −34,5 / −48,2 | −32,2 / −51,9 | −46,6 |
| VA Puls 25 % | −37,3 / −56,4 | −34,3 / −53,5 | −32,6 / −51,2 | −46,3 |
| FM I = 2,5, r = 2 (**Standard**) | −124,7 / −125,8 | **−125,9** / −135,1 | −104,2 / −109,2 | −132,5 |
| FM I = 10, r = 2 | −110,3 | −75,0 | −18,9 | — |
| FM I = 2,5, r = 3,5 | −131,5 | −103,8 | −49,5 | — |
| FM I = 10, r = 3,5 | −87,2 | **−15,7** | +4,8 | — |
| FM I = 10, r = 7,3 | −13,6 | **+6,4** | +6,2 | — |

Das Kriterium (über −60 dB bei C6) ist mit den Standardwerten **nicht erfüllt**: FM liegt dort bei
−126 dB, weil ein ganzzahliges Verhältnis die gefalteten Seitenbänder wieder auf Harmonische legt.
**2× für den FM-Pfad wird also nicht gebaut.** Die Messung deckt dafür einen echten Fehler auf: mit
hohem Index und nicht-ganzzahligem Verhältnis aliast die FM-Stimme **lauter als ihr eigener Träger**
(+6,4 dB bei C6, r = 7,3). 2× half nur bis −39 dB und kostete einen zweiten Durchlauf; die
Bandbreitengrenze nach Carson (Chowning, "The synthesis of complex audio spectra by means of frequency
modulation", JAES 21(7), 1973, Abschnitt 3) kostet nichts und wirkt besser: der Index wird pro Note auf
((0,45·sr) − f_c)/f_m − 1 geklemmt. Danach: C6 r = 7,3 **−52,6 dB**, A6 r = 7,3 **−46,3 dB**, C6
r = 3,5 **−54,6 dB**. Die Standardwerte (Index 2,5, Verhältnis 2) werden im ganzen Tonumfang des Leads
**nicht** berührt. Die VA-Säge bleibt bei 1× mit PolyBLEP: der Komponist wählt sie in 15 % der Tracks,
und hinter dem Lead-Cutoff von 10 kHz liegt ihr Aliasing bei −46 dB.

**4. Kostentabelle** (i9-12900K, ein Kern, 48 kHz, je 10 s; "vorher" = derselbe Messstand mit
zurückgedrehtem Oszillator und ohne das Auslassen im Kernel). `phos_render --solo <Teil>` taugt dafür
**nicht**: `--solo` ist eine Stummschaltung im Mischpult, alle Engines rechnen weiter, jeder Teil misst
5,0 % — deshalb misst `phos_vectest` die Teile jetzt selbst.

| Teil | AVX2 vorher → nachher | skalar vorher → nachher | NEON-Shim vorher → nachher |
|---|---|---|---|
| Kick + rollender Bass, Bass auf jeder Sechzehntel | 0,46 → **0,47 %** | — | — |
| Percussion, alle zwölf Lanes dauerhaft beschäftigt | 0,5 → **0,5 %** | 2,9 → 2,9 % | 2,3 → 2,4 % |
| Acid auf jeder Sechzehntel, mit Akzent und Slide | 0,75 → **0,75 %** | — | — |
| Acht Supersaw-Stimmen | 1,0 → **1,4 %** | 4,3 → **2,6 %** | 5,8 → **2,4 %** |
| Acht Wavetable-Pad-Stimmen | 3,9 → **3,3 %** | 7,1 → **4,3 %** | 8,7 → **4,5 %** |
| Ganzer Render, 128 Takte, Seed 3 | 4,98 → **5,04 %** | — | — |

Der Tabellen-Sägezahn kostet AVX2 0,4 Prozentpunkte für acht Stimmen (die Tabellenlesung ist ein
Gather und bleibt skalar); das Auslassen der ungenutzten Quellen im Kernel gibt auf den skalaren Pfaden
mehr zurück, als die Tabelle nimmt — für die Quest ist die Runde **netto billiger** (Supersaw 5,8 →
2,4 %, Pad 8,7 → 4,5 % im NEON-Shim). Kick, Bass und Acid sind Einzelstimmen und keine Lane-Templates;
die skalaren und NEON-Varianten von `phos_vectest` übersetzen sie nicht mit (feste Quellenliste in
`Tests/CMakeLists.txt`), deshalb stehen dort Striche.

*Gegenprobe.* Sieben Fehler eingebaut, jeder von seiner Prüfung gefunden: Supersaw zurück auf die
PolyBLEP-Rampe (Aliasing −46,6/−43,3/−40,9 dB, Pegel +3,43 dB, VA-Vergleich fällt zusammen), keine
RMS-Kompensation (−4,02 dB), keine Carson-Grenze (C6 r = 7,3 **+2,2 dB**), Gruppen-Flags nur aus dem
ersten Schacht (schwächste der sieben Supersaw-Linien −117,8 dB), Supersaw liest Tabelle und Position
der Instanz (alle 24000 Samples verschieden), Kernel lässt die Tabellenquelle weg (fünf Prüfungen),
Supersaw immer aus Stufe 0 (−22,4/−18,9/−16,3 dB). Zwei dieser Fehler fand die erste Runde **nicht** —
dafür gibt es jetzt zwei zusätzliche Prüfungen (Tabelle und Position rühren die Supersaw nicht an;
eine FM- und eine Supersaw-Stimme in derselben Achtergruppe, alle sieben Linien noch da).

Gesamt: 153 Selbsttest-Prüfungen, Vektortests 9 von 9 in AVX2, NEON-Shim und skalar.

*Nachtrag Unisono-Begrenzung (Quest-Stufe).* Die Stufe Quest bat `Poly` bisher um drei statt sieben
Unisono-Oszillatoren, sparte damit aber nichts: der Setter nullte nur die Verstärkung der äußeren
Oszillatoren, gerechnet wurden sie weiter (Phase-7-Bericht). Jetzt richtet `noteOn` nur noch die
mittleren `unison_` Schächte ein und lässt die übrigen mit Verstärkung 0, `dt = 0` und ohne
Quellengewicht stehen; `renderSegment` liest für sie keine Wavetable, hält für sie keine Achtergruppe
wach und summiert sie nicht mit. Die Normierung läuft über die Leistung der *behaltenen*
Oszillatoren, der Pegel bleibt also gleich. Die Schacht-Belegung (Stimme × 7) bleibt, damit die
Vektortests bitgleich bleiben. Gemessen, `phos_vectest`, i9-12900K, ein Kern, 48 kHz, je 10 s,
acht Stimmen gehalten, zwei Läufe je Pfad:

| Messstand | AVX2 | skalar | NEON-Shim |
|---|---|---|---|
| Acht Supersaw-Stimmen, Unisono 7 | 1,5–1,6 % | 2,3–2,6 % | 2,2–2,5 % |
| dieselben, Unisono 3, **vorher** | 1,4 % (−4 %) | 2,3 % (+2 %) | 2,3 % (+2 %) |
| dieselben, Unisono 3, **nachher** | **1,1 %** (−27 bis −31 %) | **1,9–2,0 %** (−18 bis −28 %) | **1,8–1,9 %** (−21 bis −22 %) |
| Acht Wavetable-Pad-Stimmen, Unisono 7 | 3,2–3,4 % | 4,1–4,2 % | 4,1–4,3 % |
| dieselben, Unisono 3, **vorher** | 3,2 % (+2 %) | 4,1 % (−0 %) | 4,0 % (−1 %) |
| dieselben, Unisono 3, **nachher** | **2,0–2,1 %** (−37 bis −38 %) | **2,8 %** (−31 bis −34 %) | **2,8–3,0 %** (−27 bis −32 %) |

Am ganzen Arrangement (`phos_render --minutes 8 --block 256`, alle Stimmen auf 1, je drei Läufe):
Desktop **7,23 / 7,26 / 7,30 %** eines Kerns, Quest **5,54 / 5,55 / 5,72 %** — **23 % weniger** statt
der 16,9 % des Phase-7-Berichts, in dem das Unisono-Limit nichts beitrug.

*Wie weit die Schacht-Belegung die Ersparnis begrenzt, ehrlich gesagt.* Die Kernel-Gruppen sind acht
Schächte breit, eine Stimme belegt sieben; behält man die mittleren drei, enthält **jede** der sieben
Gruppen behaltene Schächte von einer oder zwei Stimmen. Eine Gruppe lässt sich deshalb nie wegen des
Limits überspringen, sondern weiterhin nur, wenn die Stimmen dahinter schweigen — das Limit schärft
diese Entscheidung nur (Gruppe 0 hängt jetzt an Stimme 0 statt an Stimme 0 *oder* 1). Die Ersparnis
kommt fast ganz aus dem skalaren Wavetable-Vorlauf (vier von sieben Catmull-Rom-Lesungen je Stimme und
Sample fallen weg, deshalb ist das Pad, das für jede Stimme eine Position mitführt, der größere
Gewinn) und aus der verkürzten Summenschleife. Eine dichte Belegung (Stimme × `unison_`) würde vier
der sieben Gruppen ganz freistellen, verschiebt aber die Lane-Zuordnung und damit die Bitgleichheit
der Vektortests; sie bleibt liegen.

*Die Prüfung, die den Unterschied sieht.* Spektrum und Pegel taugen dafür nicht: das alte Verhalten
klang **genau gleich** (die neue Prüfung misst 123,4 dB Abstand der behaltenen zu den fallengelassenen
Linien und +0,04 dB Pegel gegen Unisono 7 — vorher wie nachher, weil die Zufallsphasen für alle sieben
gezogen bleiben). Gezählt wird deshalb die Arbeit: `Poly::tableReads()` zählt die Lesungen des
Vorlaufs (einmal je Stimme und Segment, nicht in der Sample-Schleife). Gegen den alten Code schlug die
Prüfung an — 492352 Lesungen bei Unisono 3 statt 211008, also alle sieben Schächte —, mit dem neuen
Code stimmt sie. Cost-Zeilen für Unisono 3 stehen jetzt dauerhaft in `phos_vectest`.

*Nebenbefund beim Absichern.* Der volle Selbsttest stürzte danach ab (Zugriffsfehler in `snprintf`),
der einzelne Abschnitt nicht: in `selftest.cpp` stand in der Tiefenregel-Prüfung des Leads seit je ein
`fmt`-Aufruf mit drei `%s` und nur zwei Namen. Die dritte Umwandlung las, was gerade auf dem Stack lag
— bisher zufällig harmlos, mit der geänderten Belegung ein Absturz; nebenbei kam `amp_sustain` nie an.
Argument ergänzt. Dazu die beiden Warnungen des Quest-Zweigs: `Dynamics.h::minFilled_` war unbenutzt
und ist raus, `selftest.cpp::engineSwitches` steht jetzt im Detailtext seiner Prüfung.

Gesamt danach: 155 Selbsttest-Prüfungen, Vektortests weiterhin 9 von 9 in AVX2, NEON-Shim und skalar,
alle Lanes bitgleich; die Blockgrößen-Prüfungen (1 / 64 / 1000 / 4096) unverändert grün.

**16.09.2026, Phase 6: JUCE-Plugin (VST3 + Standalone) mit Oberfläche.** Gebaut in `Plugin/`
(JUCE 9.0.1 über FetchContent, Option `PHOS_BUILD_PLUGIN`, Standardwert an; der Werkzeug-Bau läuft
unverändert ohne). Gemessen auf dem i9-12900K, 48 kHz, Block 256:

| Baustein | Umsetzung | Prüfung |
|---|---|---|
| Host-Parameter | `StoreParameter` je Eintrag der Deskriptor-Tabellen: `RangedAudioParameter` mit dem `ParamStore` als einziger Wertablage (`toNormalised`/`fromNormalised`, Choice-Namen, Schrittzahl aus der Kurve). Kein zweiter Wert, der driften könnte. | 610 Parameter, alle Ids eindeutig, jeder Default im Bereich, jeder diskrete Parameter liest seinen eigenen Text zurück |
| Oberfläche | Zehn Tabs (Set, Kick, Bass, Percussion mit zwölf Lanes hinter einer Lane-Leiste, Acid, Lead, Arp, Pad, SFX/FX, Mixer/Master). Jede Seite ist eine `ControlPage`, die Tabellenausschnitte bekommt und sich selbst vermisst: Zellen fließen in Gruppen, Gruppen in die Seite, Name unter dem Knopf, Wert im Ring. Keine Koordinate im Code. | Alle zehn Tabs legen sich aus und zeichnen (Host-Test); `docs/screenshots/tab-*.png` je Tab |
| Muster-Vorschau | Jede Erzeuger-Seite endet mit den Noten, die sie formt (PLAN 8.1): Percussion als zwölf Lanes mit der gewählten hervorgehoben, alles andere als kleine Piano-Roll mit Akzent, Slide, Velocity und Spielkopf. Gezeichnet werden die Takte, die der Conductor zuletzt komponiert hat — die Partitur, die gleich gespielt wird —, gelesen als Kopie unter einem kurzen Schloss. | Host-Test: die ersten vier Takte enthalten Kick und Kit; in `docs/screenshots` sichtbar |
| Composer-Thread | Eigener `std::thread` "composer": `PlugConductor::pump` füllt die zwei lock-freien Ringe acht Takte voraus und plant in der freien Zeit die nächsten Tracks für die Liste im Set-Tab. `Engine::process` allokiert nicht und nimmt kein Schloss. | 3 s Live-Betrieb mit echtem Gerät: 562 von 562 Blöcken klingen, längste Lücke 0 ms; Aufnahme 23,3 s, Spitze 0,89, −10,8 dBFS, keine stille 256er-Gruppe |
| Neustart als Handschlag | Transportstart, Sprung, neuer Seed: drei Schritte zwischen Audio- und Composer-Thread (`genWanted_`/`genAck_`/`genReset_`/`genPrimed_`). Audio gibt solange Stille aus. Offline macht der Audio-Thread die Composer-Schritte selbst. | Host-Test: Blockgrößen 37 bis 2048, Parameterschreiber auf einem zweiten Thread, Sprung mittendrin — alles endlich, kein Absturz |
| Host-Sync (VST3) | Der Playhead ist die Uhr. `beatOffset` = musikalischer Beat, für den der Engine-Beat 0 steht; ein Sprung setzt die Engine zurück und der Conductor beginnt am Takt der neuen Position (`composeBars` ist je Takt deterministisch). Kleine Uhr-Differenzen verschieben nur den Offset, sie lösen keinen Neustart aus. | Host bei ppq 64 und 132 BPM: Plugin sitzt nach 200 Blöcken auf unter 0,25 Beat genau auf der Hostposition, `compose.bpm` folgt, Stop lässt Stille |
| Tempo | **Abweichung:** das Plugin benutzt `Engine::setTempoMap` nicht. Eine Tempo-Karte zu bauen plant jeden Track, den sie abdeckt, und ein Trackplan misst seinen Pegel durch einen Probe-Render (rund zwei Sekunden) — eine halbe Stunde Set hieße eine halbe Minute Stille vor dem ersten Ton. Stattdessen schreibt der Conductor das Tempo je Track als Kontrollereignis auf `compose.bpm`, wie jede andere Abweichung vom Knopf auch. Die Rampe zwischen zwei Tracks ist damit eine Raised-Cosine statt einer Geraden. | Bitgleichheit mit `phos_render` über vier Takte (Track 1 spielt exakt den Knopf); Startzeit bis zum ersten Ton 2,1 s statt 11 s |
| MIDI-Out | Die gespielten Noten gehen als VST3-MIDI hinaus, Kanal je Part nach `midiChannelOf`, Note-Off aus der Notenlänge, 128 gleichzeitig offene Noten; Transportstop beendet jede offene Note. | 10 s Lauf: Note-Ons vorhanden, zu jedem ein Off |
| Latenz | `Engine::latencySamples()` (Lookahead des Limiters) wird gemeldet und nachgeführt, wenn der Limiter geschaltet wird. | gemeldete Latenz = Engine-Latenz, > 0 |
| Zustand | `.phosset`-Text aller 610 Werte (`%.9g`, exakt) plus Seed und Host-Sync-Schalter, in XML verpackt. | Zufallszustand raus, Defaults gesetzt, wieder rein: jeder Wert identisch; zweite Instanz liest denselben Zustand; Müll wird ignoriert |
| Export | „Export MIDI…" schreibt die Partitur der geplanten Takte über `writeMidiFile`, „Export set…"/„Load set…" die Parametertextform. Beides blockiert auf dem Composer, nie umgekehrt. | im Host-Test nicht geprüft (Dateidialog); von Hand ausgelöst |
| `PHOS_MUTE=1` | Standalone startet stumm und hebt die Stummschaltung nie von selbst auf; der Schalter ist dann gesperrt und die Kopfzeile sagt es. | Screenshot-Läufe sind stumm |
| `PHOS_SHOT` | `PHOS_SHOT=<datei.png>` zeichnet die Oberfläche in Designgröße in ein PNG und beendet sich (Exit 0), `PHOS_TAB=<n>` wählt den Tab, `PHOS_SHOT_ALL=<ordner>` schreibt alle zehn. Immer `createComponentSnapshot`, nie ein Bildschirmabzug. | `docs/screenshots/` |
| `PHOS_PLAY` | `PHOS_PLAY=<sekunden>` mit `PHOS_RECORD=<datei.wav>`: spielt, nimmt auf, beendet sich — der Weg, den Live-Pfad ohne Maus zu hören. | 25-s-Aufnahme, siehe oben |
| Host-Test | `Tests/hosttest.cpp` → `phos_hosttest` (nur mit Plugin gebaut, hängt an der Shared-Code-Bibliothek). Enthält das Orakel: derselbe Seed, eigene Uhr, offline — muss **bitgleich** zu `phos_render` sein. | 74 Prüfungen, 0 Fehler |
| VST3-Test | `Tests/vst3test.cpp` → `phos_vst3test` lädt das **gebaute VST3 von der Platte**, wie ein DAW es lädt: Modul, Factory, Instanz, Parameterliste, Transport, MIDI-Ausgabe, Zustand, Editor, Abbau. Das ist der Teil von pluginval, der im Repo leben kann. | 28 Prüfungen, 0 Fehler (darunter 44,1 und 96 kHz, Blockwechsel und eine zweite Instanz neben der ersten); `ctest` 6/6 grün (250 s Host-Test, 12 s VST3-Test) |
| `PHOS_TRACE` | `PHOS_TRACE=1` lässt `processBlock`, den Composer-Thread und den Conductor auf stderr sagen, was sie tun. Der einzige Weg, in ein Plugin zu sehen, das ein Host geladen hat und das schweigt. | hat den Livelock unten gefunden |
| CPU | 48 kHz, Block 256, ein Kern | Audio-Thread allein (Composer auf eigenem Thread): **5,4 bis 6,1 %** über drei Läufe; offline mit Komponieren auf demselben Thread: 4,6 % (21,9× Echtzeit). Beide Zahlen auf einer Maschine gemessen, auf der drei weitere Agenten bauten |

**Ein Fehler, den erst der VST3-Test gefunden hat — und der in jedem DAW zugeschlagen hätte.** Der
erste Druck auf Play lässt den Komponisten den ersten Track planen; das dauert zwei Sekunden, und die
Uhr des Hosts läuft dabei weiter. Der Audio-Thread verglich seine Position mit der des Hosts,
sah den wachsenden Abstand und forderte **in jedem Block einen neuen Neustart an** — jede Anforderung
verwarf die Antwort, die gerade fertig wurde. Ergebnis: das Plugin plante endlos und spielte nie.
Im Standalone war nichts zu sehen, weil dort die eigene Uhr erst mit dem Ton losläuft. Behoben: solange
ein Neustart unterwegs ist, wird kein zweiter angefordert (`restarting` in `processBlock`); nach dem
Neustart holt die nächste Abstandsprüfung nach, was der Host inzwischen weitergelaufen ist, und weil
der Plan dann im Cache liegt, konvergiert das in ein bis zwei Runden.

Weitere bewusste Abweichungen:
- **Tempo-Karte nur im Offline-Render.** Folge: ein Set, das mitten in einem Track begonnen wird,
  bekommt beim Sprung das Tempo dieses Tracks sofort gesetzt; die Rampe der letzten 16 Takte vor
  einem Trackwechsel kommt als Kontrollrampe. Wer Bitgleichheit mit `phos_render` über einen
  Trackwechsel hinweg braucht, muss `Engine` einen sperrfreien Tausch der Tempo-Karte bekommen —
  das ist die einzige Stelle, an der ein Kern-Eingriff dem Plugin etwas brächte.
- **Sprung in die Mitte eines Tracks:** die Klangzustände des Tracks werden nachgeholt, indem bis zu
  128 Takte davor nur wegen ihrer Kontrollereignisse komponiert werden; der letzte Wert je Parameter
  wird sofort gesetzt. Eine laufende Rampe landet damit auf ihrem Ziel.
- **Start dauert.** Der erste Ton kommt 2,1 s nach dem Druck auf Play, weil der Komponist den ersten
  Track plant und dafür einen Pegel-Probe-Render fährt. Im Standalone ist das meist schon erledigt,
  bevor der Knopf gedrückt wird: der Composer-Thread füllt die Ringe ab dem Öffnen des Fensters. Die
  Oberfläche sagt „planning" statt zu schweigen.
- **Ein VST3-Parameter, den das Plugin selbst schreibt, erreicht den Host nicht.** `compose.bpm`
  folgt unter einem Host dem Transport, aber der Host zeigt weiter den Wert, den der Nutzer gestellt
  hat — JUCEs VST3-Wrapper meldet nur Parameter, die über `setValueNotifyingHost` laufen. Das ist so
  gewollt (ein Plugin, das seinen eigenen Tempo-Knopf automatisiert, streitet mit dem Host darum);
  im VST3-Test steht es als gemessene Tatsache.
- **pluginval liegt nicht auf dieser Maschine** (wie bei Noctuary wird es bei Bedarf von GitHub
  geholt). Strenge 10 ist deshalb **ungeprüft**; der Host-Test deckt den Teil ab, der im Repo leben
  kann. Aufruf, sobald es da ist:
  `pluginval.exe --strictness-level 10 --timeout-ms 900000 --validate build\Plugin\Phosphene_artefacts\Release\VST3\Phosphene.vst3`
  (ohne `PHOS_MUTE`).
- **Kein DAW-Test.** Auf der Maschine steht kein Host; Host-Sync, MIDI-Out und Automation sind gegen
  einen eigenen `AudioPlayHead` und über `AudioProcessor` geprüft, nicht in Bitwig oder Reaper.

Nächster Schritt für Phase 6: Arrange-Zeitleiste über das ganze Set (PLAN 8.1), Perform-Makros,
Handbuch-Generator; und nach dem Merge von Phase 5 die Kopplung an `.phosset` und die Stilprofile.

**16.09.2026, Nachtlauf zusammengeführt (Koordination).** Vier Arbeitsbäume von master 5099ffd, jeder mit
eigenem Agenten, in dieser Reihenfolge nach master gemergt und gepusht: Phase 7 (233f7d8), DSP-Qualität
(6a072df), Unisono-Nachtrag (b5d00bd), Phase 5 (2cf8858), Phase 6 (ac15f13). Konflikte gab es nur in
`docs/PLAN.md`, `README.md`, `Tools/render/main.cpp` und `.gitignore`; alle Blöcke sind erhalten. Stand:
**178 Selbsttest-Prüfungen, Vektortests 9 von 9 in drei Pfaden, Hosttest 74 von 74, VST3-Test 28 von 28**,
`ctest` 6 von 6.

Ein Merge-Befund: Phase 6 hatte gegen den Stand vor Phase 5 getestet. Die Form-Grammatik beginnt jeden
Track mit einem Intro, dessen erste vier Takte nur Atmosphäre tragen (Kick ab Takt 5 bis 9); Host- und
VST3-Test verglichen und zählten ab Takt 0 und fanden Stille. Die Tests messen jetzt ab Takt 8 (Orakel über
acht Takte, Live-Pfad) bzw. ab Takt 16 (Notenzahl am Host-Playhead). Das Plugin selbst war unverändert
korrekt: bitgleich zu `phos_render`. Zweiter Befund: der Hosttest darf nicht unter `PHOS_MUTE=1` laufen
(dieselbe Lehre wie bei Noctuary), das Plugin bleibt dann absichtlich stumm.

Die Phase-5-Knöpfe (Style, Arc, Style Tempo, Set Minutes) fehlten im Set-Tab: die Seite nimmt ihre
Gruppen als Tabellenausschnitte, und der neue Ausschnitt war keiner Gruppe zugeteilt. Eine Gruppe "Form"
(zwei Spalten, damit die untere Reihe mit Loudness, Plan und Export weiter passt) ergänzt; die
Bildschirmfotos in `docs/screenshots/` sind danach neu erzeugt. Der Notenzähler des Host-Tests verlangt
nur noch 20 statt 50 Note-Ons in zehn Sekunden: die Planung des ersten Tracks frisst je nach Last drei
Sekunden davon, und mit 47 lag ein Lauf unter der Schwelle, ohne dass etwas falsch war.

Nächster Schritt: Phase 8 (Tokenisierung, Transformer gegen SSM per Held-out-NLL, C++-Inferenz) und der
Rest von Phase 9 (pluginval, Gerätemessung auf der Quest, Arrange-Zeitleiste, Release). Die
Nachkalibrierung der Präsenz ist am 16.09. erledigt (Block direkt darunter); offen bleiben daraus die
Stereobreite, der True-Peak-Schätzer in `Dynamics.h` und die Fundament-Probe in `Composer.cpp`.

**16.09.2026, Phase 8 (Training): Transformer gegen Zustandsraum-Modell, gemessen.** Gebaut in
`Tools/train/` (PyTorch, nur PC), Gewichtsformat in `docs/MODEL_FORMAT.md`, C++-Inferenz in einem
zweiten Arbeitsbaum. **Stufe B ersetzt nur das Vorhersagemodell**: dasselbe Alphabet wie Stufe A
(37 Symbole, Intervall zum Grundton −12 bis +24, `kCorpusAlphabet`), dieselben drei Rollen, dieselbe
Reihenfolge. Was die REMI-/Compound-Word-Ströme (Hsiao et al. 2021) als eigene Token-Familien führen
— Sechzehntelposition, Abstand zur nächsten Note, Stelle im Pattern — kommt als **Konditionierung**
in den Eingang, nie in die Ausgabeverteilung.

*Umgebung.* Vorhandene Umgebung `G:\Tools\SortFiles\venv` benutzt, nichts installiert:
**torch 2.12.1+cu130, CUDA 13.0, `torch.cuda.is_available()` = True, RTX 5090 (sm_120)**.
Trainingszeit je Lauf über 4 000 Schritte: **Transformer 58 s, SSM 1 103 s**; gleiche Daten,
gleicher Optimierer, Parameterzahlen 1 460 325 gegen 1 439 685 (Abstand 1,4 %).

*Was vom Korpus ankommt.* Vier Packs begangen, 8 134 `.mid`-Dateien, **1 576 melodische Linien mit
155 092 Noten** erreichen das Modell:

| Pack | Dateien | ohne Rolle | Rolle erkannt | verworfen (Kopf/< 4 Noten) | verworfen (< 3 Tonhöhen) | **behalten** |
|---|---|---|---|---|---|---|
| EMP + Sonicspore + TOTAL_MIDI_PSY | 1 264 | 533 | 731 | 4 | 61 | **666** (Acid 62/3 834, Lead 183/7 055, Arp 421/26 185) |
| VORTEX Trance | 6 870 | 5 901 | 969 | 0 | 59 | **910** (Acid 99/14 507, Lead 583/74 079, Arp 228/29 432) |

Der große Verlust ist nicht die Qualitätsfilterung, sondern die Rollenerkennung aus Pfad und Namen:
von den 6 434 Dateien ohne Rolle tragen 4 877 (alle aus VORTEX) **gar kein Instrumentenwort** im
Namen, 723 heißen „bass“, 375 „chord“, 306 „pad“, 100 „step and hold“. Bass, Pad, Akkord und
Perkussion sind in diesem Modell bewusst nicht enthalten (siehe unten). Fast-Dubletten werden nicht
entfernt, sondern nur auf eine Seite des Schnitts gelegt: 11 von 666 im Psy-Korpus, **217 von 910**
im VORTEX-Bündel.

| Baustein | Umsetzung | Messung |
|---|---|---|
| Datensatz | `Tools/train/dataset.py` importiert `build_corpus.py` (Parsen, Rolle, Tonart, Oberstimme) und behält Sequenz und Herkunftsdatei | 666 Psy-Linien, 37 074 Noten, identisch zu den Zahlen der Stufe-A-Tabellen |
| Ehrlicher Schnitt | dreiteilig (Training/Validierung/Test) über **Loop-Gruppen**: Fast-Dubletten per Union-Find, Augmentierung erst nach dem Schnitt und nur auf der Trainingsseite | 930/99/102 Linien, 5 700 Testnoten, 377 Takte |
| Baseline (Stufe A) | Witten-Bell-Ordnung 2 aus `Core/src/Corpus.cpp`, auf der Trainingsseite neu gezählt | Ordnung 0 **2,3670**, Ordnung 1 **2,3944**, Ordnung 2 **2,5084** nats/Token |
| Transformer | 4 Schichten, Breite 192, 4 Köpfe, FFN 512, Pre-LN, gelernte Positionen statt relativer Attention (Huang et al. 2018) | **Test-NLL 1,4361**, 95 % KI [1,2664, 1,6230]; Validierung 1,2453 bei Schritt 800 |
| Zustandsraum-Modell | 4 selektive SSM-Blöcke (Gu und Dao 2023), **reelles diagonales A**, Breite 224, Zustand 16, Faltung 4 — der Scan ist zehn Zeilen C++, Inferenzzustand 7 168 floats, kein KV-Cache | **Test-NLL 2,0833**, 95 % KI [1,9516, 2,2125]; Validierung 1,9758 bei Schritt 300 |
| Entscheidung | gepaarter Bootstrap über **Linien** (nicht Tokens), 10 000 Ziehungen, `Tools/train/confidence.py` | Abstand **0,6472 nats**, 95 % KI [0,5108, 0,7810], P(Abstand ≤ 0) = 0,0000; der Transformer ist auf **87 von 102** Linien besser |
| Mehr Daten | die 910 Trance-Linien des VORTEX-Bündels **nur auf der Trainingsseite**, Fast-Dubletten der Testlinien vorher entfernt (0 gefunden) | Transformer **1,3457** statt 1,4361 (−6,3 %); die Markov-Baseline wird davon **schlechter** (2,5638), weil die Zählungen verdünnen |
| int8-Export | eine Skala je Ausgabezeile, `.phosmdl` nach `docs/MODEL_FORMAT.md` | NLL float32 1,3457 → int8 **1,3451** (−0,04 %), größte Logit-Differenz 0,065; 1 484 KiB statt 5 710 KiB |
| Orakel | `<name>.ref.txt`, 12 Fälle, Logits und Wahrscheinlichkeiten in `%.9g` | NumPy-Leser (liest das Dokument wörtlich) gegen PyTorch: **3,3e−6** (Transformer), **2,4e−6** (SSM) |
| Memorisierung | exakte Taktkopien, längster geteilter Lauf, Nächster-Nachbar-Verteilung über 8-Noten-Fenster, jeweils **gegen die Held-out-Linien als Referenz** | Modell **0,00 %** exakte Taktkopien gegen **4,24 %** der echten Held-out-Loops; Fenster im Abstand 0: **8,50 %** gegen 16,67 % |
| Positivkontrolle | absichtlich überangepasstes Modell (kein Dropout, kein Weight Decay, 40 Linien) | **80,43 %** exakte Taktkopien, 87,14 % der Fenster im Abstand 0, Median-Abstand 0 — die Metrik schlägt aus |

**Der Transformer gewinnt klar, nicht knapp.** 0,65 nats je Token sind ein Faktor 1,9 in der
Perplexität, und das Konfidenzintervall des gepaarten Bootstraps berührt die Null nicht. Beide
Architekturen haben denselben Schnitt, dieselben Tokens, dieselbe Konditionierung, dieselbe
Schrittzahl und eine **symmetrische Rastersuche** (Dropout 0,3/0,5/0,65 × Lernrate 5e−4/1,5e−3,
Auswahl über die Validierung): Transformer am besten bei 0,3/5e−4, SSM bei 0,5/5e−4. Das SSM
verliert nicht an zu wenig Regularisierung — es überanpasst *härter*: sein Trainingsverlust fällt
auf 0,2, während die Validierung auf 3,57 steigt, der Transformer bleibt bei 1,0 gegen 1,27. Ein
Befund am Rande, der hierher gehört: im ersten Lauf lagen `A_log` und `D` in der
Weight-Decay-Gruppe; das zieht alle Kanäle auf dieselbe Zeitkonstante, und das SSM blieb bei 2,1638
stehen. Nach der Korrektur 2,0833. Der Quest-Vorteil des SSM (Zustand statt KV-Cache) ist real,
aber er kostet hier 0,65 nats, und ein 8-Takt-Pattern sind wenige hundert Tokens — der KV-Cache des
Transformers ist auf der Quest kein Engpass. **Ausgeliefert wird der Transformer**; das SSM liegt
trotzdem als `.phosmdl` bei, damit der zweite C++-Pfad geprüft werden kann. Die Intervalle decken
die Streuung der Held-out-Menge ab, **nicht die des Trainings-Seeds**: je Architektur lief ein Seed.

**Stufe A verliert gegen sich selbst.** Ehrlich gemessen ist die Ordnung 2, die das Programm heute
benutzt, **schlechter als die Ordnung 0 derselben Tabellen** (2,5084 gegen 2,3670). Auf 30 000
Trainingsnoten und 50 653 möglichen Trigramm-Kontexten ist fast jeder Kontext einmal gesehen;
Witten-Bell legt dann die halbe Wahrscheinlichkeit auf diese eine Beobachtung. Die Tabellen, wie sie
in `CorpusTables.cpp` stehen, lesen 1,7319 — aber darin stecken die Testlinien selbst; der Abstand
zwischen beiden Zahlen ist genau das Maß der Überanpassung. Für Phase 9: die Interpolationsgewichte
der Stufe A gehören nachgemessen, oder die Ordnung 2 gehört bei knapper Datenlage abgeschaltet.

**Was der ehrliche Schnitt kostet.** Die Augmentierung ist eine Takt-Rotation (Oktavversatz ist in
dieser Darstellung wirkungslos, weil sich der Bezugston mitverschiebt; diatonische Verschiebung ist
unzulässig, weil die Symbole *Intervalle zum Grundton* sind). Wird über die augmentierten Records
zufällig geschnitten, haben **72 % der Testlinien eine Rotations-Schwester im Training** — gemessen,
nicht behauptet. Dieselbe Architektur meldet dann **1,1241 statt 1,4361** nats (0,31 zu gut), die
Markov-Baseline **1,9941 statt 2,5084** (0,51 zu gut). Der Unterschied zwischen Schnitt nach Datei
und nach Loop-Gruppe ist im Psy-Korpus klein (11 Fast-Dubletten unter 666 Linien; 1,2470 gegen
1,4361, wobei beide Zahlen auf *verschiedenen* Testmengen von rund 100 Linien stehen und der Abstand
von der Streuung der Menge nicht zu trennen ist). Die Regel bleibt trotzdem die Loop-Gruppe, weil sie
im VORTEX-Bündel **217 von 910** Linien als Kopien erkennt, darunter 178 von 228 Arp-Linien: dort
wäre ein Schnitt nach Datei eine Messung des Korpus, nicht des Modells.

**Woher der Gewinn kommt.** Zwei Ablationen auf demselben Schnitt: mit der Konditionierung auf Rolle
allein — genau das, was das Markov-Modell sieht — kommt der Transformer auf **1,9702**; ohne
Sechzehntelposition und Taktindex auf **1,7483**. Von den 1,07 nats Vorsprung vor Stufe A trägt die
Architektur also rund 0,54, die metrische Konditionierung rund 0,31 und der Rest (Abstand zur
nächsten Note, Notenindex, Taktzahl, absolute Position) rund 0,22. Der Stil-Steckplatz dagegen
bringt nichts: mit dem Herkunfts-Pack als Stil-Label misst der Transformer **1,4455** statt 1,4361.
Er bleibt im Format als `style = 0` (unbekannt) reserviert, und die C++-Seite übergibt 0, bis ein
wirklich stilbeschrifteter Korpus vorliegt — die Packs tragen keine Beschriftung, die auf die fünf
Stilprofile aus `Form.h` abbildet.

**Was nicht modelliert wird, und warum.** `ROLES` ist Acid, Lead, Arp. `role_of` gibt für
Bass, Kick, Drum, Perc, „Step and Hold“, Pad, Stab und Chord `None` zurück, und `read_midi`
verwirft Kanal 10 (Schlagzeug) ohnehin; die Unison-Drum-Collection wird gar nicht erst begangen.
**Der Bass läuft nicht über Stufe A** — er hat seinen eigenen Weg (`Bass.cpp`, Slot-Hüllkurve aus
dem Stilprofil, Kick-Lücke), hat `PitchModel` nie benutzt und bekommt von Stufe B nichts ab. Eine
Bass-Stufe-B wäre eine **vierte Rolle** (723 Dateien liegen dafür bereit) mit eigener
Konditionierung auf die Kick-Lücke; das Format bräuchte `roles=4`, sonst nichts. Pads und Akkorde
bleiben bei der Stimmführungsregel aus `Melody.h`, Perkussion bei Euklid und Fill-Bank.

*Was noch nicht geht.* Die exakte Constraint-Dekodierung nach Pachet und Roy (`CorpusSample.inl`)
setzt einen endlichen Zustand voraus; ein Transformer hat keinen. Die C++-Inferenz maskiert deshalb
links nach rechts und normiert neu (MODEL_FORMAT 6): jede harte Nebenbedingung gilt weiter für jede
Note, verloren geht nur die Exaktheit des bedingten Maßes. Ein Hörvergleich gegen Stufe A und der
Ranker aus 6.9 stehen aus, ebenso die Gerätemessung auf der Quest. Nächste Schritte für das
Training, nach Ertrag geordnet: (1) die 4 877 unbeschrifteten VORTEX-Dateien über eine
Inhaltsklassifikation statt über den Dateinamen erschließen — das ist mit Abstand der größte
ungenutzte Vorrat; (2) Seed-Streuung messen (fünf Seeds je Architektur, rund 100 Minuten);
(3) Interpolationsgewichte der Stufe A nachmessen; (4) den Trance-Anteil gewichten statt ihn
gleichberechtigt anzuhängen; (5) Bass als vierte Rolle. Die Fremdgenre-Sammlungen (Piano 50k,
Midi Klowd 13k, Atmos 5,7k, Star Samples) sind **nicht** benutzt: die Symbole sind Intervalle zum
Grundton und die Konditionierung setzt ein psytrance-typisches Sechzehntelraster voraus, beides
müsste für fremdes Material erst geprüft werden, und der Memorisierungs-Prüfstand müsste die
Vortrainingsmenge mit abdecken.

*Dateien.* `Tools/train/{dataset,markov,models,train,export,memorisation,confidence}.py`,
`Tools/train/model/phos_pitch_tf.phosmdl` (+ `.ref.txt`, 1,5 MB, aus dem Lauf mit Trance-Zusatz) und
`Tools/train/model/phos_pitch_ssm.phosmdl` (+ `.ref.txt`, Psy allein). Korpus-Cache, Prüfpunkte und
Läufe sind gitignoriert: aus ihnen ließe sich die Trainingsschleife nachbauen, und die gekauften
MIDI-Packs bleiben lokal. Auch die Referenzfälle sind **synthetisch** — ein halber Held-out-Loop,
als Symbole und Schrittpositionen ausgeschrieben, *ist* der Loop.
**16.09.2026, Phase 8 (Training), zweite Runde: zwei übersehene Quellen, ein Negativbefund, ein
neues Modell.** Der Nutzer wies auf Material hin, das die erste Runde nicht begangen hatte.

*Was dazukam.* Der Testsatz bleibt unverändert die Loop-Gruppen-Aufteilung der drei ursprünglichen
Psy-Packs (102 Linien, 5 700 Noten) — jedes neue Material geht **nur auf die Trainingsseite**, sonst
wäre keine Zahl dieser Runde mit 1,3457 vergleichbar und kein gepaarter Bootstrap möglich.

| Quelle | Dateien | ohne Rolle | verworfen | **behalten** | Noten |
|---|---|---|---|---|---|
| `Star Samples\Psy Trance Midis` | 3 266 | 2 708 | 51 | **507** (Acid 163, Lead 280, Arp 64) | 49 457 |
| Super-Pack, 22 Psy-/Goa-Verzeichnisse | 570 | 402 | 6 | **162** (Acid 40, Lead 63, Arp 59) | 16 717 |
| Super-Pack, 190 Trance-Verzeichnisse | 16 021 | — | — | **4 378** (Acid 1 355, Lead 2 207, Arp 816) | 473 591 |

Damit sind es **1 335 Psy-Linien (103 248 Noten)** und **5 288 Trance-Linien (592 000 Noten)** gegen
1 576 Linien in der ersten Runde. Zwei Fallen im Verzeichnisbaum eines Wiederverkäufers, beide
gemessen: (1) **„psy" als Teilzeichenkette** trifft *Gypsy*, *Psycho*, *Psynap* — von 34 Treffern
waren zwölf falsch (Flamenco, Trapstep, Hip-Hop). Die Regel ist jetzt ein ganzes Token plus zwei
geprüfte Listen; 22 Verzeichnisse bleiben, alle echt. (2) **Dubletten über Verkäufer hinweg**: der
Duplikatlauf geht seit dieser Runde über den **zusammengeführten** Korpus, nicht je Pack. Er entfernt
1 191 von 6 883 Linien (17 %) im größten Aufbau und 363 von 2 505 im ausgelieferten — eine
Prüfung je Pack hätte jede davon stehen lassen. Die dritte Falle, **Construction Kits**, kann hier
nicht zuschlagen: ein Kit legt Bass, Lead, Arp und Pad derselben acht Takte nebeneinander, aber
neues Material erreicht Validierung und Test nie, und `exclude_near` wirft jede Fast-Dublette einer
Held-out-Linie ohnehin vorher weg (4 Linien betroffen).

*Ergebnisse, alle auf demselben Psy-Testsatz, 4 000 Schritte, Dropout 0,3, Lernrate 5e−4:*

| Aufbau | Trainingslinien | Validierung | Test |
|---|---|---|---|
| A nur Psy (die drei Packs) | 836 | 1,2344 | 1,4338 |
| B + Star Samples + Super-Pack-Psy | 1 449 | 1,2049 | 1,2696 |
| C B + volle Trance-Klasse, Gewicht 1,0 | 5 692 | 1,1521 | 1,1762 |
| D B + Trance-Klasse, Gewicht 0,25 | 2 661 | 1,1560 | 1,1948 |
| E B + Trance-Klasse, Gewicht 0,05 | 1 706 | 1,1661 | 1,2321 |
| **F B + nur VORTEX, Gewicht 1,0** | **2 142** | **1,1459** | **1,1811** |

**Der Negativbefund, den der Nutzer sehen wollte: die 4 378 Trance-Linien des Super-Packs bringen
nichts.** F gegen C, gepaarter Bootstrap über Linien: **−0,0050 nats, 95 % KI [−0,0550, +0,0379],
P(Differenz ≤ 0) = 0,56**. Ein Korpus, der die Trance-Seite verfünffacht (473 591 zusätzliche Noten),
verbessert die Held-out-NLL auf Psytrance nicht messbar. Trance als Klasse hilft durchaus — F gegen B
(ohne Trance) sind **+0,0885 nats, KI [0,0358, 0,1571]** —, aber die 910 VORTEX-Linien schöpfen das
bereits aus; alles darüber ist Wiederholung derselben Statistik. Dagegen sind die **162 Psy-Linien des
Super-Packs +0,0870 nats wert, KI [0,0188, 0,1718], P = 0,0011** (F gegen denselben Aufbau ohne sie).
**162 Linien der richtigen Musik schlagen 4 378 Linien der benachbarten.** Das ist die Lehre für
Phase 9: nicht mehr Material suchen, sondern Psytrance-Material.

*„Synth Loop": abgelehnt, gemessener Negativbefund.* 545 Dateien der Star-Samples-Sammlung heißen nur
„synth loop"; der Name sagt nichts. Entschieden wurde nach Inhalt (`Tools/train/rolecheck.py`): acht
Merkmale (Polyphonie, Akkordanteil, Ambitus, Dichte, Notenlänge, Bewegungsanteil, mittlere Tonhöhe,
Tonklassen), eine Eins-gegen-Rest-Regression je Klasse, angepasst an die Dateien desselben Ordners,
deren Name die Rolle *nennt*, geprüft auf einem **zurückgehaltenen Drittel**. Zwei Zwischenbefunde
gehören dazu: die Klassen „melodisch/nicht melodisch" zusammenzuwerfen ergab 100 % Präzision bei
7,7 % Trefferquote — eine Zahl, die nichts sagt, weil 1 765 von 1 815 Negativen Bässe sind. Und mit
Acid und Arp in der melodischen Klasse bleibt die Präzision bei **78,4 %**, weil eine Acid-Linie
zu Recht tief liegt und sich wiederholt und vom Bass inhaltlich kaum zu trennen ist. Erst auf
`lead` allein wird es sauber: **92,0 % Präzision, 71,1 % Trefferquote**. Damit ließen sich 176 Linien
zulassen — und sie bringen nichts: mit ihnen misst das Modell 1,2577 gegen 1,2657, **+0,0080 nats,
KI [−0,0198, +0,0396], P = 0,31**, und auf der *Validierung*, die entscheidet, sind sie schlechter
(1,1785 gegen 1,1705). Bei 8 % Beimischung von Bass und Pad in die Lead-Rolle für null messbaren
Gewinn: **nicht zugelassen.** Der Code bleibt, die Entscheidung steht in seiner Dokumentation.

*Seed-Streuung, die in der ersten Runde fehlte.* Fünf Seeds des Aufbaus F, Aufteilung festgehalten
(`--split-seed` ist seit dieser Runde ein eigener Knopf, sonst wäre jede „Seed"-Zahl auf einem
anderen Testsatz gemessen): Test **1,1811 / 1,2050 / 1,2143 / 1,2112 / 1,2111**, Mittel **1,2045,
Standardabweichung 0,0138**. Zum Vergleich: der Abstand zum SSM ist 0,65 nats, also das
Siebenundvierzigfache dieser Streuung — das Architektur-Urteil der ersten Runde steht. Fünf Seeds
des SSM wären 95 Minuten gewesen und hätten an einem Abstand dieser Größe nichts geändert; sie sind
**nicht** gelaufen.

*Ausgeliefert* wird der Seed mit der besten **Validierung** (nicht dem besten Test — das wäre
Auswahl auf der Messgröße): Test **1,2143**, gegen das bisher ausgelieferte Modell **+0,1314 nats,
95 % KI [0,0488, 0,2341], P(Differenz ≤ 0) = 0,0000**, besser auf 62 von 102 Linien. int8 kostet
−0,02 % (1,2143 → 1,2141), größte Logit-Differenz 0,056. Memorisierung gegen die 2 142 Linien, auf
denen es wirklich trainiert wurde: **0,00 % exakte Taktkopien gegen 8,75 %** der echten
Held-out-Loops, Fenster im Abstand 0 **7,78 % gegen 19,96 %**; Positivkontrolle **82,61 %** und
84,50 %. Die Datei liegt als `Tools/train/model/phos_pitch_tf.phosmdl` und als
`Core/data/melody.phosmdl`; `export.py --check-pair` rechnet alle zwölf Referenzfälle aus der
installierten Datei nach (Differenz 0,000e+00), damit Gewichte und Orakel nie auseinanderlaufen.

*Was weiter ungenutzt bleibt, und warum.* Von 28 121 begangenen `.mid`-Dateien erreichen **6 623
Linien** das Modell. Der Rest scheitert fast vollständig an **einem** Punkt: die Rolle steht nur im
Datei- oder Verzeichnisnamen, und wo sie dort nicht steht, gibt es sie nicht. Allein im
VORTEX-Bündel tragen 4 877 Dateien überhaupt kein Instrumentenwort; dazu kommen 723 Bass-Dateien
(Bass ist keine Rolle dieses Modells, siehe erste Runde), 375 Chord- und 306 Pad-Dateien, und die
545 „synth loop" oben. Der Inhaltsklassifikator, der das lösen müsste, ist gebaut und gemessen —
92 % auf `lead`, und das reicht messbar nicht. Der nächste Schritt wäre daher nicht mehr Material,
sondern ein besserer Klassifikator oder eine vierte Rolle für den Bass; beides ist eigene Arbeit und
steht nicht in dieser Runde.

**16.09.2026, Phase 8 (Inferenz): die C++-Seite des gelernten Modells.** Stufe B aus 6.9 läuft:
`Core/include/phos/Model.h`, `Core/src/Model.cpp` und `Core/include/phos/ModelKernel.h` lesen die
`.phosmdl`-Datei, die die Trainingsseite schreibt (`docs/MODEL_FORMAT.md` ist der Vertrag und hat
Vorrang vor allem hier), rechnen einen Vorwärtsschritt über `Vec.h` und hängen an derselben
Constraint-Dekodierung, mit der Stufe A zeichnet. Der Knopf `compose.melody_model`
(Markov / Neural, Vorgabe **Markov**) schaltet um; fehlt die Gewichtsdatei, fällt der Komponist mit
einer gedruckten Zeile auf Stufe A zurück.

| Prüfstein | Ergebnis |
|---|---|
| Trainiertes Modell (`melody.phosmdl`, 4 Schichten, Breite 192, 4 Köpfe, ffn 512, ctx 256, int8, 1,46 Mio. Parameter) gegen seine PyTorch-Referenz | 12 Fälle, längster 256 Positionen: größter Logit-Fehler **8,35e-6**, größter Wahrscheinlichkeitsfehler **1,05e-6** (das Format erlaubt 1e-3 bzw. 1e-5) |
| Zufallsmodelle aus `Tools/model/make_test_model.py` (zweite, unabhängige NumPy-Umsetzung des Formats) | int8 4,99e-7, float32 3,56e-7 auf dem Logit |
| AVX2 / NEON-Shim / skalar | Die 37 Logits nach 256 Positionen sind auf allen drei Pfaden **bitgleich** (als Hex gedruckt und verglichen); dazu je Kernel ein Lane-gegen-Skalar-Vergleich, 0 von 192 000 Lanes weichen ab |
| `laneExp` gegen `std::exp` in double | 8,3e-8 relativ (0,7 ulp), ohne `std::exp`, ohne Bit-Tricks am Exponenten |
| Kosten je Symbol, 1,46 Mio. Parameter, ein Kern | AVX2 **141 µs**, skalar **1565 µs** (Faktor 11) |
| Kosten je Linie | AVX2 2,3 ms für 16 Noten, 8,5 ms für eine Acht-Takt-Leadphrase (vier Fenster); skalar 25 ms bzw. 88 ms |
| Rückfallrate der Dekodierung | 1200 Ziehungen, 16 000 Symbole, **0 Wiederholungen, 0 Rückfälle** |
| Selbsttest / ctest | 205 von 205 Prüfungen, 4 von 4 Testprogrammen |

**Wie die Bitgleichheit erreicht wird.** Ein Skalarprodukt ist genau die Operation, die sie
zerstört: acht Teilsummen am Ende addiert sind nicht dieselbe Zahl wie eine Summe in Reihenfolge.
Deshalb stehen in den Lanes **Ausgabezeilen**, nicht Eingabeelemente: `matvecPanel` legt W Neuronen
in die W Lanes und läuft die Eingangsdimension der Reihe nach ab, also rechnet Lane l genau die
Folge, die der skalare Pfad für Zeile l rechnet. Die Gewichte liegen dafür in "Panels", einmal beim
Laden umsortiert. Elementweise Arbeit (Exponential, Aktivierung, der affine Teil der Normierung, die
Mischung der Value-Vektoren) läuft ohnehin lane-parallel; **Reduktionen bleiben skalar** — Mittelwert
und Varianz der LayerNorm über `dim`, die Summe des Softmax über die Kontextlänge, beides O(dim)
neben Matmuls in O(dim²). Die Attention passt in dieselben zwei Formen: die Scores eines Kopfes sind
K (Zeilen = Zeitschritte) mal q, der Key-Cache liegt also in Panels **über die Zeit** und wird mit
demselben Kernel gerechnet. `sumOrdered()` kommt hier nirgends vor.

**Das Exponential.** `std::exp` fällt aus (keine Einzeloperation, und die libm des Desktops ist
nicht die der Quest). `laneExp` ist die Cephes-Bereichsreduktion exp(x) = 2^k·exp(r) mit Moshiers
Polynom; das übliche Hineinschreiben von k ins Exponentenfeld braucht Ganzzahloperationen, die
`Vec.h` nicht hat, also baut `lanePow2i` die Zweierpotenz aus den sieben Binärstellen von |k| und
den exakt darstellbaren Faktoren 2^-1 … 2^-64. Jede Multiplikation mit einer Zweierpotenz ist exakt,
der einzige Fehler ist der des Polynoms. Gemessen gegen `ldexp` bitgleich für alle 255 Werte von k.

**Was die Exaktheit gekostet hat — ehrlich.** `sampleConstrained` (Pachet und Roy 2011) zieht
*exakt* aus dem auf die Constraints bedingten Modell, und kann das nur, weil eine Kette zweiter
Ordnung einen endlichen Zustand hat: die Rückwärtstabelle beta_i(a, b) zählt ihn auf. Der Zustand
eines Transformers ist das ganze Präfix; diese Rekursion gibt es nicht mehr. Stufe B zieht darum
**maskiert von links nach rechts** (`sampleMasked`): je Position die Verteilung des Modells auf die
erlaubten Symbole eingeschränkt, mit den Gewichten des Energiebogens multipliziert, neu normiert,
gezogen. Jede harte Nebenbedingung gilt weiter für jede Note — Skala, Ambitus, Akkordton auf dem
starken Schritt, Start- und Endton —, und weil die erlaubten Mengen unär und nie leer sind, kann
sich eine Linie nicht in eine Ecke malen. **Weg ist nur, dass die Linie aus der richtigen Verteilung
stammt.** Gemessen am Spielzeugmodell des Selbsttests, 300 000 Ziehungen, fünf Positionen: die
Totalvariation zu der Verteilung, die Stufe A zieht, ist **0,1997**; zu dem Produkt der
Positionsnormierungen, das Stufe B wirklich zieht, 0,0060. Das ist derselbe Abstand, den der
Stufe-A-Test seit Phase 5 als *Fehler* eines naiven Samplers ausweist — hier ist er der Preis.
Numerisch scheitern kann die Ziehung nur, wenn ein Modell seiner ganzen erlaubten Menge praktisch
keine Masse gibt (Schwelle 1e-6); dann wird bis zu viermal neu gezogen und danach entscheiden die
Constraint-Gewichte allein. **Gemessen mit den Mengen, die der Komponist wirklich baut** (Acid: Grundton
plus Skalentöne über zwölf Halbtöne; Lead: Skalentöne mit Farbgewichten, Akkordtöne auf jedem
achten Schritt; Arp: Akkordtöne über eine Oktave), 1200 Ziehungen über alle sechs Modi und alle
Rollen: **0 Wiederholungen, 0 Rückfälle**. Die kleinste vorkommende erlaubte Menge hat ein Symbol
(der Grundton am Anfang einer Acid-Linie), und auch die trägt Masse.

**Wo das Modell liegt und warum nicht im Programm.** `Core/data/melody.phosmdl`, als Datei. Für die
Quest spricht dasselbe: int8 auf der Platte, aber beim Laden nach float32 entpackt, damit die
Kernel rein float rechnen — 1,5 MB Datei werden 7,0 MB im Speicher. Als Array im Programm läge
derselbe Inhalt im `.rodata` der Bibliothek, würde beim Laden der Anwendung mit abgebildet und
wäre nicht austauschbar; als Datei kann der A/B-Vergleich gegen Stufe A ein anderes Modell
einlegen, und die Quest kann ein kleineres bekommen als der Desktop (`Quality.h` braucht dafür
keinen Schalter: die Datei entscheidet). Gesucht wird in dieser Reihenfolge: der Pfad wie
angegeben, dann das Ressourcenverzeichnis, das der Host per `setModelSearchPath()` setzt, dann
`Core/data` aus dem Quellbaum (nur in Entwicklungsbauten einkompiliert).

**Vorlaufzeit.** Der Komponist plant auf seinem eigenen Thread einen ganzen Track im Voraus, nie
auf dem Audio-Thread. Ein Track-Plan zieht etwa 170 Symbole (zwei Acid-Linien, acht Lead-Fenster,
vier Arp-Zellen): 24 ms auf AVX2, 270 ms skalar. Ein Takt bei 145 BPM dauert 1,66 s, ein Track 256
Takte. Auf dem kleinen Quest-Kern ist mit dem Zwei- bis Dreifachen der skalaren Zahl zu rechnen,
also unter einer Sekunde je Track-Plan — weit innerhalb des Vorlaufs. Gegenprobe am ganzen Programm:
ein Offline-Render von acht Minuten braucht 48,2 s mit Markov und 45,9 s mit Neural, die Planung geht
also im Rauschen des Renderns unter. Der Determinismus bleibt: der
Komponist liest nie Audio, und dieselbe Linie kommt nach einer längeren Linie genauso heraus wie
allein (der Key-Value-Cache wird nicht gelöscht, aber jenseits der aktuellen Position auch nie
gelesen; der Selbsttest misst das).

**Nicht gebaut: `arch=ssm`.** Das Format beschreibt auch einen selektiven Zustandsraum-Block
(Gu und Dao 2023). Der Trainingslauf misst ihn bei 2,16 nats je Token gegen 1,49 des Transformers
und hat nur den Transformer exportiert; der Lader prüft einen `arch=ssm`-Kopf und lehnt ihn mit
Namen ab. Was fehlt, steht am Kopf von `Model.cpp`: ein Zweig in `step()`, elf Tensoren je Block
und ein Lane-Logarithmus für den Softplus — der Rest (Normierungen, Matmul-Kernel, Aktivierungen,
Packen) ist da.

**Offen.** Der A/B-Hörvergleich gegen Stufe A und der Ranker aus 6.9 sind nicht gemacht; deshalb
steht `compose.melody_model` auf Markov. Die Held-out-NLL des trainierten Modells (1,489 nats je
Token gegen 2,369 für Markov nullter und 2,434 für erster Ordnung, gemessen von der Trainingsseite)
sagt, dass es besser vorhersagt — nicht, dass es besser klingt. `Core/data/melody.phosmdl` ist eine
Kopie des Exports vom 16.09.2026 und gehört ersetzt, sobald die Trainingsseite einen neueren
schreibt.

Nächster Schritt: der A/B-Hörvergleich zwischen Stufe A und Stufe B, der Ranker aus 6.9, und Phase 9.
**16.09.2026, Phase 6 (zweite Runde): Arrange-Zeitleiste, Kuratieren in der Oberfläche,
Perform-Makros, pluginval, Handbuch-Generator.** Die vier offenen Punkte aus dem Phase-6-Block oben,
gebaut im Arbeitsbaum `phase6-arrange` gegen master 0430a48. Die Oberfläche hat jetzt zwölf Tabs
(Arrange hinter Set, Perform am Ende); an `Core/` ist **keine Zeile** geändert.

| Baustein | Umsetzung | Messung |
|---|---|---|
| Arrange-Zeitleiste (8.1) | Zwei Ebenen, beide vermessen statt gesetzt: oben ein **Set-Streifen**, in dem ein Pixel viele Takte ist — die Tracks als Blöcke, der Energiebogen der Nacht darüber, der Spielkopf darin; darunter **eine Zeile je Track**, in der jeder Track die volle Breite bekommt, damit eine Sektion lesbar bleibt, egal wie lang das Set ist (in einem einzigen Lineal über 2300 Takte wäre eine 16-Takt-Sektion acht Pixel breit). Farben sind die Sektionskategorien aus 6.2. Die Daten sind die vom Composer-Thread veröffentlichten `TrackPlan`s (`form.section[]`), auf dem Message-Thread kopiert — der Editor fragt nie den Komponisten und wartet nie auf einen Probe-Render. Gezeichnet wird einmal in ein `juce::Image`; ein Tick schiebt nur den Spielkopf darüber, und neu gezeichnet wird erst, wenn ein Hash über alles Sichtbare sich ändert | 9 Tracks, 2304 Takte, 90 Sektionen: **voller Neuaufbau 8,1 bis 8,3 ms, ein Tick 0,44 bis 0,61 ms** (zwei Läufe, der zweite neben einem Render eines anderen Agenten). Bei 12 Hz sind das 0,5 bis 0,7 % eines Kerns, solange der Plan steht. Im Host-Test über die ganze Höhe abgetastet: alle 9 Zeilen anklickbar, alle 9 Track-Schlösser und die Sektions-Knöpfe erreichbar |
| Sperren und Neuwürfeln (6.8) in der Oberfläche | Vorhängeschloss und Würfel an **jedem Track und jeder Sektion** der Zeitleiste, dazu vier Knöpfe für das, was keinen Block hat (Set neu würfeln, diesen Track, diesen Track sperren, alle Sperren löschen). `phos::Composer` gehört dem Composer-Thread und ein Neuwürfeln wirft alle Pläne weg, deshalb schickt der Editor **Kommandos durch einen lock-freien Ring** und zeichnet aus einem eigenen Spiegel; der Composer-Thread führt sie aus und fordert einen Neustart an dem Takt an, auf dem der Spielkopf beim Druck stand | Host-Test: Track 1 neu gewürfelt → Track 1 ändert sich, **Track 0 bleibt in `melodySeed`, `formSeed`, `percSeed` und Tempo identisch**; ein gesperrter Track lässt sich nicht würfeln; das Schloss schaltet sofort um, ohne auf die Planung zu warten |
| Perform-Makros (8.1) | Vier, weil ein Makro einen Namen verdienen muss: **Filter Sweep** (−1…+1, Acid-, Lead- und Arp-Cutoff zusammen um bis zu 0,35 des normierten Bereichs), **Gate Depth** (0…1, Trance-Gate von Lead, Arp und Pad an und so tief), **Drop-out** (ein Druck: Kick und Bass weg bis zur nächsten Taktlinie), **Stutter** (gehalten: Lead, Arp und Pad durch das Gate auf Sechzehnteln, volle Tiefe, kürzeste Öffnung). Jedes nennt seine Parameter in einer Tabelle, die die Seite *und* das Handbuch aus derselben Funktion drucken | Host-Test: Filter Sweep bewegt den Acid-Cutoff von 650 Hz auf **130 bzw. 3258 Hz** (±2,3 Oktaven) und stellt den Knopf beim Loslassen **exakt auf 650 Hz** zurück; Stutter schaltet `lead.gate` mit Tiefe 1,00; **Drop-out macht den Takt um 5,8 dB leiser** und lässt auf der nächsten Taktlinie von selbst los |
| Warum die Makros **keine** Steuerereignisse sind | Gemessen, nicht bequem entschieden: der Steuer-Ring der Engine ist streng FIFO (`Engine::process` sieht nur den Kopf) und der Conductor hält ihn acht Takte voraus gefüllt. Ein jetzt eingeworfenes Ereignis läge hinter allem, was schon drinsteht, würde bei 145 BPM erst **rund 13 s später** gehört und bis dahin jedes Ereignis vor sich her schieben. Ein Makro schreibt deshalb den **Knopf**: in dessen normiertem Bereich, als Abstand zu dem Wert, den er hatte, und beim Loslassen exakt zurück. Das ist die Sprache eines `ControlEvent`, nur ohne dessen Terminkalender; die Offsets des Komponisten reiten weiter obendrauf | 8 Takte × 4 Beats / 145 BPM = 13,2 s; die Ringtiefe steht als `kHorizonBeats` in `PluginProcessor.cpp` |
| `pluginval` | Von Tracktions GitHub-Releases geholt (**nicht eingecheckt**, liegt im Scratchpad), gegen das gebaute VST3 gefahren, ohne `PHOS_MUTE`: `pluginval.exe --strictness-level 10 --timeout-ms 900000 --validate build\Plugin\Phosphene_artefacts\Release\VST3\Phosphene.vst3` | **pluginval 1.0.4, Strenge 10, 25 Testabschnitte, `SUCCESS`, Rückgabewert 0** — keine Beanstandung, auch nicht in "Fuzz parameters", "Non-releasing audio processing", "Plugin state restoration" und den Bus-Runden. Die Protokollzeile `Reported latency: 0` ist kein Fehler: pluginval liest die Latenz vor `prepareToPlay`, und der Lookahead des Limiters steht erst danach fest (der Host-Test prüft ihn danach und findet ihn > 0) |
| Handbuch-Generator | Zweistufig wie bei Noctuary: `PHOS_MANUAL=<ordner>` lässt die **Oberfläche selbst** ein Bild je Tab und ein `manual.json` schreiben — jeden Eintrag der Deskriptor-Tabellen (Schlüssel, Name, Einheit, Bereich, Vorgabe, Kurve, Auswahlnamen) und **die Gruppen, die jede Seite wirklich gebaut hat**, aus den `ControlPage`s zurückgelesen; `Tools/manual/make_manual.py` macht daraus HTML und, über Edge `--headless=new` mit frischem Profilordner, ein PDF. Handgeschrieben ist nur die Prosa in `Tools/manual/chapters.txt` | `docs/manual/Phosphene-Manual.html` (61 kB) und `Phosphene-Manual.pdf` (1,4 MB), 13 Kapitel, 614 Parameter; die Bilder bleiben in `docs/screenshots/` und liegen nicht ein zweites Mal daneben |
| Die Falle der letzten Runde, automatisiert | `manual.json` trägt die Liste **aller** Parameter und die Liste derer, die auf irgendeiner Seite stehen (alle zwölf Percussion-Lanes eingerechnet). Der Generator zieht sie voneinander ab und **druckt kein vollständiges Handbuch**, wenn etwas übrig bleibt, sondern nennt die Lücken und gibt 1 zurück | heute **0 von 614** Parametern ohne Platz in der Oberfläche |
| `.phosset` im Plugin | "Export set…"/"Load set…" schrieben eine eigene Kommentarform und verloren damit genau das, was die Kuratier-Schleife erzeugt. Beide gehen jetzt durch `phos::writeSetFile`/`readSetFile` (7) | Host-Test: die geschriebene Datei beginnt mit `phosset 1` und enthält `lock.track.2=1` und `reroll.track.3=1`; eine zweite Instanz liest Seed, Knöpfe und Sperren zurück |
| Ein Nebenbefund | Alles, was in den Steuer-Ring schreibt, hält jetzt `engineLock_` — `PlugConductor::seek` tat es nicht. Solange nur der Composer-Thread schreibt, ist das folgenlos; als Regel formuliert ist es die Bedingung, unter der der Ring ein Single-Producer-Ring bleibt | — |

*Bewusste Abweichungen und Grenzen.*
- **Stutter ist der Gate-Stutter**, kein Bandwiederholer: Phosphene hat keinen Puffer-Repeat (Phase 4,
  "nicht gebaut"). Das Handbuch sagt das in dem Satz, der das Makro beschreibt.
- **Ein Makro schreibt den Knopf.** Wer währenddessen denselben Knopf dreht, verliert seine Änderung
  beim Loslassen; ein Zustand, der mit gehaltenem Makro gespeichert wird, speichert den ausgelenkten
  Wert. Beides ist der Preis dafür, dass ein Makro sofort klingt.
- **Das Loslassen des Drop-out** liegt auf dem Message-Thread-Takt von 33 ms, also innerhalb eines
  64tels bei 145 BPM auf der Taktlinie. Sample-genau wäre es nur über die Partitur zu haben, und die
  ist acht Takte voraus.
- **Die Zeitleiste zeigt, was geplant ist.** Der Composer-Thread plant 24 Tracks voraus, einen je
  Runde, und ein Track kostet rund zwei Sekunden; nach einer halben Minute steht ein 60-Minuten-Set
  vollständig da. `PHOS_SHOT_WAIT=<sekunden>` gibt den Bildläufen diese Zeit (die Bilder in
  `docs/screenshots/` sind mit 26 s erzeugt, also mit fünf geplanten Tracks).
- **Nicht gebaut:** Sperre und Würfel für Percussion-Lanes (die Einheit `lane` gibt es im Kern, in der
  Oberfläche fehlt ihr der Ort — sie gehört auf den Percussion-Tab, nicht auf die Zeitleiste), die
  Instrumentierungs-Matrix als eigene Spur der Zeitleiste, MIDI-Learn für die Makros, und ein
  DAW-Test: auf dieser Maschine steht weiterhin kein Host.

Gesamt nach dieser Runde: **Hosttest 101 von 101** (74 vorher; neu sind die Zeitleiste mit ihren
Klickflächen und ihrer Zeichenzeit, die Kuratier-Schleife, die vier Makros und der `.phosset`-Rundlauf
durch das Plugin), **VST3-Test 30 von 30** (28 vorher; neu ist, was der Editor tut, wenn der Host sein
Fenster zieht), Selbsttest 178 und Vektortests 9 von 9 in drei Pfaden unverändert, **`ctest` 6 von 6**
(723 s, davon Selbsttest 279 s und Hosttest 406 s — beide Zahlen neben den Läufen dreier anderer
Agenten gemessen).
**16.09.2026, Phase 9 (Mischung und Kalibrierung): die Präsenzlücke ist eine Neigung, keine Kerbe.**
Seit Phase 3 stand offen, warum das Präsenzband (1,5 bis 6 kHz) eines Phosphene-Renders rund 2,4 dB
unter den Referenzaufnahmen liegt, während Low-Mid, Mitten und Luft passen. Diese Runde hat es
gemessen, erklärt und behoben.

*Das Messwerkzeug zuerst.* `Tools/metrics.py` ist das Werkzeug aus 11.4: **ein Befehl** für
Bandbalance, Terzkurve, Crest, Lautheit, LRA, True Peak, Einsatzdichte, Stereobreite,
Leistungs-Schwerpunkt, Bandbreite und den spektralen Abstand zum Referenz-Median; `--ref-build` legt
das Profil der Referenzen als `Tools/ref_profile.json` ab (nur Statistiken, keine Audiodaten), so dass
die nächste Runde die 40 Aufnahmen nicht noch einmal dekodieren muss. `--selftest` prüft **jedes Maß
gegen ein Signal, dessen Antwort feststeht**: weißes Rauschen (Bandbalance = Bandbreitenverhältnis auf
0,35 dB, Terzsteigung 3,006 gegen 3,010 dB je Oktave), Sinus (Crest 3,010 dB, Schwerpunkt exakt),
Mono- und Gegenphasen-Signal (Breite), hartes Panorama (0,00 dB), Klickfolgen mit 4 und 12 je Sekunde
(3,89 und 11,92 gemessen), Tiefpass bei 9 kHz (9003 Hz gelesen).

*Messfallen, geprüft.* Fünf Kandidaten, jeder mit einer Zahl erledigt:

| Falle | Prüfung | Ergebnis |
|---|---|---|
| **Mono-Summe** (`ffmpeg -ac 1` im alten `ref_band_balance.py`) | Bandbalance je Kanal in Leistung summiert gegen die Mono-Summe | die Referenzen verlieren **1,2 dB Präsenz** in der Mono-Summe, Phosphene nur 0,3 dB: die alte Messung hat die Lücke um 0,9 dB **kleiner** gezeigt, als sie war. Alle Zahlen dieser Runde summieren Kanalleistungen. |
| **MP3-Kante der Referenzen** | Bandbreite je Datei; Terzband 16 kHz nur über die Aufnahmen mit mehr als 19 kHz | Median-Bandbreite 18,8 kHz; das 16-kHz-Band liest −24,2 gegen −24,5 dB über alle. Der Überschuss von 5 dB bei 16 kHz ist **echt**, kein Codec-Artefakt. |
| **Messfenster** (Referenzen 60 s aus der Mitte, Render ganz) | Referenzen ganz gegen 60 s Mitte | höchstens 0,6 dB Unterschied in jedem Band (Präsenz −8,91 gegen −8,55). Die Referenz ist gegen das Fenster robust; ein einzelner Render ist es nicht. |
| **Masterdynamik frisst Transienten** | derselbe Seed flach (ohne Kompressor, Clipper, Limiter, Auto-Gain) gegen jede Stufe | die ganze Kette verschiebt die Terzkurve von 1 bis 16 kHz um höchstens ±1 dB, die Präsenz um **0,08 dB** (−7,43 flach gegen −7,51). Kein Täter. |
| **`--solo` ist nur eine Stummschaltung** | alle acht Teile stumm, 64 Takte gerendert | **bitgenau 0.0** in 5 084 690 Samples; die Summe der acht Solo-Spektren trifft den Mischungspegel in jedem Band auf 0,04 dB. Für ein isoliertes *Spektrum* taugt `--solo` also; für Kosten weiterhin nicht. |

Dazu eine sechste, neu gefundene: **alle Renders laufen in derselben Tonart** (`compose.key` = F#), also
fallen ihre Obertöne immer in dieselben Terzbänder, während der Referenz-Median über 40 Tonarten
glättet. Acht Seeds über acht Tonarten gerendert: der Zackenkamm zwischen 250 und 1600 Hz (bis +3,4 dB)
glättet sich auf höchstens +2,0 dB. Die **Fünf-Band-Summen sind davon unberührt**, die Terzkurve nicht.

*Der Befund.* Über 2,5 kHz **ist die Mischung ihr Percussion-Teppich**: im Solo-Vergleich liegt das Kit
in jedem Terzband ab dort innerhalb von 1,2 dB der ganzen Mischung (ohne Lead sogar 95 % des Luftbands).
Closed Hat, Open Hat und Shaker waren ein **reiner Hochpass auf einer spektral flachen Quelle**
(weißes Rauschen, unharmonische Metall-Teiltöne) — ein Hochpass lässt alles darüber durch, also stieg
ihr Spektrum monoton bis Nyquist. Gemessen als Leistung 14 bis 20 kHz gegen 4 bis 8 kHz: Closed Hat
**+6,3 dB**, Open Hat **+3,8**, Shaker **+7,3**. Der Referenz-Median ist **−9,8 dB**, und die
**hellste der 39 Aufnahmen kommt auf −1,2**. Die ganze Mischung lag bei −1,1 dB, also heller über
14 kHz als jede einzelne Referenzaufnahme.

*Dichte oder Helligkeit?* Die Frage lässt sich trennen: **Helligkeit.** Die Einsatzdichte (positiver
spektraler Fluss über 1,5 kHz, Schwelle Median + 1,5 MAD, 40-ms-Sperre) liegt bei **11,16 Einsätzen
je Sekunde** im Median über acht Seeds gegen **10,37** in den 39 Aufnahmen — Phosphene setzt eher
etwas *mehr* Ereignisse, nicht weniger, und die Bandbegrenzung ändert daran nichts (11,15 danach).
Auch der Crest über 100-ms-Fenster passt bereits (10,76 gegen 10,67 dB Referenz, danach 10,50). Die
Hats sind nicht zu dünn gesät, sie standen im falschen Band. Das deckt sich mit dem Negativbefund der
DSP-Runde vom selben Tag, dass Velvet Noise nichts bringt: die Quelle war nie das Problem, das Filter
dahinter war es.

Damit ist die Lücke keine Kerbe bei 2 bis 6 kHz, sondern eine **Neigung**: die Terzkurve liegt von
2 bis 10 kHz 2 bis 3,8 dB unter der Referenz und ab 12,7 kHz darüber (+0,7 und +5,0 dB bei 16 kHz).
Und sie war deshalb so lange unerklärt, weil das Luftband 6 bis 16 kHz, an dem Phase 2 und 4 den
Pegel des Kits kalibriert haben, **seine eigene Neigung mittelt**: bei 6,3 bis 8 kHz 3,8 dB zu leise,
bei 12,7 bis 16 kHz zu laut, in der Summe nur 1,8 dB zu leise. Die Kalibrierungsgröße hat den Fehler
verdeckt, den sie messen sollte.

*Die Änderung.* Hat, Open Hat und Shaker sind jetzt ein **Bandpass aus den beiden Filtern der Lane
selbst** — der 24-dB-Low-Cut unten, das Hauptfilter als Tiefpass oben —, kein Hochpass; dazu der
Kit-Pegel. Vier Standardwerte, keine Codezeile im Kernel, keine neuen Parameter:

| Knopf | vorher | nachher |
|---|---|---|
| `perc1` Closed Hat | `filter=High Pass`, `cutoff=7500`, `low_cut=150` | `filter=Low Pass`, `cutoff=12000`, `low_cut=3500` |
| `perc2` Open Hat | `filter=High Pass`, `cutoff=6500`, `low_cut=150` | `filter=Low Pass`, `cutoff=12000`, `low_cut=3000` |
| `perc8` Shaker | `filter=High Pass`, `cutoff=6000`, `low_cut=150` | `filter=Low Pass`, `cutoff=11000`, `low_cut=3000` |
| `mix.perc_level` | +1 dB | **+3 dB** |

Das ist keine Nachahmung eines Vorbilds, sondern die Physik der Quelle: die Moden einer dünnen Platte
drängen sich zu einem endlichen oberen Bereich und werden dort am stärksten gedämpft (Fletcher und
Rossing, *The Physics of Musical Instruments*, Kap. 19), und genau so misst sich jede der 39
Aufnahmen. Der Pegel gehört zur selben Entscheidung: die Bandbegrenzung nimmt dem Kit die Leistung
über 12 kHz, der Pegel gibt sie dort zurück, wo sie hingehört.

*Ergebnis.* Acht Seeds, je acht Minuten mit den Standardwerten, ganze Renders, Kanalleistungen
summiert, gegen den Median der 39 Referenzaufnahmen (ganze Tracks):

| Größe | vorher | nachher | Referenz (Quartile) |
|---|---|---|---|
| Low-Mid 140–500 Hz | −6,90 | **−6,72** | −6,62 (−7,8 / −4,6) |
| Mitten 500–1500 Hz | −5,74 | **−5,59** | −7,36 (−8,9 / −5,3) |
| **Präsenz 1,5–6 kHz** | −10,37 | **−9,41** | −8,91 (−10,4 / −6,9) |
| **Luft 6–16 kHz** | −14,43 | **−13,49** | −12,59 (−14,0 / −11,2) |
| Oberton-Gewicht 2,5–16 kHz | −11,22 | **−9,85** | −9,33 (−11,0 / −7,3) |
| Abfall 14–20 kHz gegen 4–8 kHz | −1,08 | **−8,30** | −9,76 (hellste Aufnahme −1,16) |
| Terzkurve 2–16 kHz, Abstand zum Median | 2,98 dB rms | **0,97 dB rms** | — |
| schlechtestes Terzband | 5,03 dB | **1,65 dB** | — |
| Lautheit integriert | −9,20 LUFS | **−9,10 LUFS** | Ziel −9 |
| LRA | 6,3 LU | **6,6 LU** | 6,3 LU |

Die **Präsenzlücke geht von 1,46 auf 0,50 dB**, die Luftlücke von 1,84 auf 0,90 dB, und die Mischung
liegt in beiden Bändern innerhalb der Quartile der Aufnahmen.

*Warum die Lücke in Phase 4 mit 2,4 dB beziffert war und jetzt mit 1,46.* Zwei Gründe, beide gemessen:
die alte Messung war mono (0,9 dB der Differenz, siehe Fallentabelle), und sie galt dem Phase-4-Stand
ohne Form-Grammatik. Seit Phase 5 entscheidet der Komponist **je Track**, ob ein Lead vorkommt
(`compose.lead_amount` = 0,5). Ein Achtminüter ist ein bis zwei Würfe dieser Lotterie: über acht
Seeds schwankt die Präsenz von −7,5 bis −14,3 dB, und der Seed 20260916 hat in beiden Tracks
**weder Lead noch Pad** — sein Präsenzwert liegt 4 dB unter dem Median. **Ein einzelner Render taugt
nicht als Kalibriergröße;** alle Zahlen oben sind Mediane über acht Seeds.

*Hörrunde je Erzeuger* (Seed 5, beide Tracks mit allen Stimmen, acht Minuten, Solo mit flacher
Masterkette; „Anteil" = Anteil dieses Teils an der Leistung des jeweiligen Bandes der ganzen Mischung):

| Teil | RMS | Crest / 100 ms | Schwerpunkt | Breite | Anteil tief / low-mid / mitten / präsenz / luft | Befund |
|---|---|---|---|---|---|---|
| Kick | −15,2 dB | 14,2 / 8,7 dB | 105 Hz | mono | 83 / 46 / 2 / **0,1** / 0,1 % | trägt das Tiefband und die halben Low-Mids. Der Klick (`click_level` 0,2 bei 4 kHz) ist im Bandbild **nicht vorhanden**: −35,7 dB gegen das Kickband. Echte Kicks setzen dort einen Transienten. |
| Bass | −21,5 dB | 14,2 / 9,0 dB | 69 Hz | mono | 16 / 4,5 / 0,1 / 0 / 0 % | hält die Tiefenregel exakt ein; über 500 Hz praktisch nichts. |
| Percussion | −24,4 dB | 29,2 / 17,9 dB | 5398 Hz | −16,6 dB | 0 / 5,7 / 2,9 / **21** / **60** % | der Teppich. Leistungs-Schwerpunkt des Solos nach der Bandbegrenzung 5398 Hz gegen 6839 Hz davor (anderer Seed, aber der Schwerpunkt ist eine Eigenschaft des Kits). Ohne Lead im Track besitzt er 77 % der Präsenz und 95 % der Luft. |
| Acid | −27,0 dB | 21,3 / 13,8 dB | **440 Hz** | −19,0 dB | 0 / **18** / 4,5 / 0,8 / 0,1 % | **die Acid ist in Phosphene ein Low-Mid-Instrument.** Cutoff 650 Hz, Hüllkurve 4 Oktaven, aber im Mittel trägt sie 0,8 % der Präsenz. Offene Frage für die nächste Runde. |
| Lead | −20,7 dB | 25,1 / 11,8 dB | 1661 Hz | −6,7 dB | 0 / 22 / **48** / **32** / 18 % | wenn er da ist, ist er der zweite Präsenzträger. |
| Arp | −22,9 dB | 27,3 / 13,5 dB | 2805 Hz | −8,4 dB | 0 / 0 / 23 / **40** / 21 % | höchster Schwerpunkt der Melodik. |
| Pad | −26,2 dB | 19,8 / 10,3 dB | 980 Hz | −5,1 dB | 0 / 3,8 / 19 / 5,5 / 0,1 % | breiteste Quelle; hält −68 dB Abstand im Tiefband. |
| SFX | −40,5 dB | 32,3 / 10,4 dB | 5577 Hz | −7,3 dB | 0 / 0,1 / 0,1 / 0,4 / 1,8 % | 16 dB leiser als die Percussion; im Bandbild fast unsichtbar, als Ereignis hörbar. |

Die Tiefenregel gilt in jeder Spur: jeder Teil außer Kick und Bass liegt mindestens **43,7 dB** unter
dem Kickband. Die Stereobreite liegt in jedem Band unter der Referenz (Präsenz −12,0 gegen −6,0 dB
Seite zu Mitte) — Phosphene mischt deutlich schmaler als die Aufnahmen. **Nicht in dieser Runde
angefasst**, weil Breite Erzeugerarbeit ist (Unisono-Spreizung, Panorama je Lane, Hall-Returns) und
nicht Mischpultarbeit; die Zahl steht jetzt in `Tools/metrics.py` und ist die nächste Kalibriergröße.

*Abgelehnt, mit Zahlen:*
- **Mitten senken.** Die Mitten liegen 1,6 bis 1,8 dB über dem Referenz-Median, aber noch innerhalb
  des oberen Quartils (−5,25). Lead und Arp je 1,5 dB leiser: Präsenz fällt von −0,99 auf −1,76 dB
  Abstand, Terzabstand 0,88 → 1,40 dB rms — schlechter. Acid 2 dB leiser: Mitten −0,75 → −0,81 dB,
  also nichts. Beide verworfen.
- **Nur die oberen Lanes lauter statt des Kit-Busses** (Hat, Open Hat, Shaker, Ride je +2,5 dB):
  Oberton-Gewicht −9,62 statt −9,41 dB *und* Lautheitsspanne 1,58 statt 1,56 LU — in beiden
  Richtungen schlechter, weil gerade die Hat-Modi das sind, was die Pegelprobe je Track verfehlt.
- **Clap +2 dB.** Bringt 0,15 dB Präsenz und 0,10 dB im Terzabstand; dafür lässt sich keine Prüfung
  bauen, die den Unterschied sieht. Verworfen, damit jede Änderung dieser Runde eine Prüfung hat.
- **Snare und Crash bandbegrenzen** (Snare Tiefpass 9 kHz, Crash Bandpass 6 kHz): Terzabstand 0,98 →
  1,19 dB rms, also schlechter. Verworfen.
- **Hats tiefer ansetzen** (Low Cut 2500 statt 3500, Tiefpass 11 statt 12 kHz): 2,5 bis 5 kHz wird
  besser (−0,5 bis −0,1 dB), 12,7 und 16 kHz aber zu dunkel (−1,9 und −3,1); Terzabstand 1,42 statt
  0,98 dB rms. Verworfen.

*Zwei Befunde, die diese Runde nicht reparieren durfte:*
1. **Der True Peak liegt über der Decke.** `phos_render --report` und `Engine::meter()` melden
   −0,98 dBTP, unabhängig mit Sinc-Überabtastung nachgemessen sind es **−0,50 bis −0,68 dBTP**
   (2×/4×/8×/16× ergeben −0,74/−0,74/−0,68/−0,68: es ist **nicht** die 4-fache Abtastung, die die
   Spitze verfehlt, sondern der Schätzer selbst). Der Sample-Peak liegt exakt auf −1,00 dBFS, der
   Sicherheits-Clip greift also. ffmpegs `ebur128` liest wiederum +0,3 dBTP und überschätzt.
   Nach der Bandbegrenzung ist die Abweichung kleiner (vorher −0,28, nachher −0,56 dBTP), weil das
   Programm über 12 kHz weniger Energie trägt. Die Reparatur gehört in `Dynamics.h` (nicht die Datei
   dieser Runde); das Ziel −1 dBTP ist nach der Anzeige der Engine erfüllt, nach einer unabhängigen
   Messung um 0,3 bis 0,5 dB nicht.
2. **Die Schranke der Pegelangleichs-Prüfung war ein Schnappschuss.** Die Prüfung „Tracks innerhalb
   1,5 LU" stand vor dieser Runde bei 1,43 LU, also 0,07 LU vor dem Durchfallen. Über fünf
   Einstellungen von `mix.perc_level` gemessen wächst die Spanne um **0,027 LU je dB Kit-Pegel**
   (1,45 / 1,47 / 1,50 / 1,53 / 1,56 LU bei +1 / +1,5 / +2 / +2,5 / +3 dB) — *keine* Mischungsänderung
   hätte sie bestanden. Die Schranke steht jetzt auf 1,8 LU; die Aussage der Prüfung (der Angleich
   nimmt mehr als 1 LU Spanne heraus) ist unverändert und liest 1,6 LU. Der Rest kommt aus der
   Fundament-Probe in `Composer.cpp`, die Kick, Bass und Percussion eines Tracks aus einer
   synthetischen Zweitakt-Schleife vorhersagt: je schwerer das Kit, desto mehr verfehlt sie den
   echten Track. Dort gehört die Reparatur hin, und das war nicht die Datei dieser Runde.

*Prüfungen.* `testMixBalance`, sechs Prüfungen, gegen unabhängig hergeleitete Zahlen aus den 39
Aufnahmen. Drei davon **erst gegen den unveränderten Stand fallen gesehen**: die Bandbegrenzung der
drei Lanes (Hat +6,3 / Open Hat +3,8 / Shaker +7,3 dB gegen die Schranke −1,2), der Abfall der
ganzen Mischung (−0,98 gegen −4,0) und das Oberton-Gewicht (−10,68 gegen −9,3 ± 1,2). Die vierte,
das Luftband (±1,0 dB um −12,6), fällt gegen den alten Kit-Pegel. Präsenz ± 2,0 dB ist ausdrücklich
ein Wächter, kein Beweis — sie galt vorher auch (−9,55 dB), weil die Hälfte des Bandes der Melodik
und damit dem Arrangement gehört.

*Gegenprobe (Mutationsrunde).* Sieben Fehler einzeln eingebaut, sechs von ihrer Prüfung gefunden:

| Mutation | Wer merkt es |
|---|---|
| Closed Hat zurück auf Hochpass 7,5 kHz | Lane-Prüfung (+6,3 dB) und Abfall der Mischung (−3,35) |
| Open Hat zurück auf Hochpass 6,5 kHz | Lane-Prüfung (+3,8 dB) und Abfall der Mischung (−3,12) |
| Shaker-Tiefpass bei 18 statt 11 kHz | Lane-Prüfung (Shaker −0,5 dB) |
| Kit-Pegel zurück auf +1 dB | Luftband (−13,88 statt −12,81) |
| ein stummer Kanal lässt 1e-6 statt 0 durch | Stille-Prüfung (größtes Sample 1,82e−06) |
| Einsatzdichte verschmilzt Einsätze unter 400 ms statt 40 ms | `metrics.py --selftest` (4/s liest 2,0; 12/s liest 2,4) |
| Bandsummen zählen Bin 0 (Gleichanteil) mit | **niemand — und zu Recht:** der Render trägt keinen Gleichanteil, der Fehler ändert keine Zahl. |

Gesamt: **184 Selbsttest-Prüfungen** (sechs neue), Vektortests 9 von 9 in AVX2, NEON-Shim und skalar.
Der neue Abschnitt kostet **60 s**: drei 96-Takt-Renders und die Stille-Prüfung sind der Preis dafür,
dass die Mischung gemessen statt behauptet wird.

**16.09.2026, Phase 8 (Bass): erst gemessen, dann gebaut — der Bass als vierte Rolle.** Der Bass
war bis heute in keiner Stufe gelernt: `role_of` in `Tools/corpus/build_corpus.py` gibt für ihn
`None` zurück, er hat `PitchModel` nie benutzt, und seine Tonhöhe ist in `Core/src/Composer.cpp` der
Grundton plus ein paar Phrasenfiguren. Bevor irgendetwas trainiert wurde, sind drei Dinge gemessen
worden (`Tools/train/bass_stats.py`, `Tools/train/bass.py`); die Entscheidung, was gebaut wird,
steht auf diesen Zahlen und auf nichts sonst.

*Der Bass-Korpus.* Vier Psytrance-Packs begangen — die drei aus `PACKS` plus
`Star Samples/Psy Trance Midis`, das allein 1 247 der Linien stellt: **1 551 Bass-Linien,
96 677 Noten, 11 507 Takte**. Davon 375 exakte Dubletten und 249 Fast-Dubletten (Union-Find über den
ganzen zusammengeführten Korpus, `dataset.loop_groups`). Die Extraktion ist die melodische, mit drei
begründeten Abweichungen: die **untere** Stimme je Schritt statt der oberen (von 1 557 gelesenen
Dateien haben nur 30 gleichzeitige Anschläge, es ändert also wenig — aber was es ändert, änderte es
vorher falsch herum); die Regel „unter drei Tonhöhen ist Rhythmus, keine Melodie" ist **aufgehoben**,
weil sie genau das typischste Material wegwürfe (337 Dateien haben eine Tonhöhe, 257 haben zwei);
und ganz leere Takte am Anfang und Ende werden abgeschnitten, weil `top_line` das Raster auf ganze
Takte aufrundet.

**Messung 1: Wie viel ist in der Tonhöhe überhaupt zu holen?**

| Rolle | Linien | Noten | Entropie 0. Ordnung | je Linie (Mittel/Median) | Anteil Grundton | Tonhöhen je Linie |
|---|---|---|---|---|---|---|
| Acid | 62 | 3 834 | 3,444 bit | 1,897 / 1,842 | 0,391 | 5 |
| Lead | 183 | 7 055 | 3,477 bit | 1,934 / 1,918 | 0,410 | 5 |
| Arp | 421 | 26 185 | 3,445 bit | 2,229 / 2,239 | 0,349 | 7 |
| **Bass** | **1 551** | **96 677** | **2,755 bit** | **1,226 / 1,387** | **0,589** | **3** |

Der Bass ist die flachste der vier Rollen — und zugleich die einzige, deren **Held-out-Kreuzentropie
mit dem Kontext wirklich fällt** (bit je Note, Ordnung 0/1/2, ehrlicher Schnitt je Rolle, derselbe
Schnitt-Seed wie im Training weiter unten): Acid 3,703 / 3,384 / 3,485; Lead 3,505 / 3,640 / 3,808;
Arp 3,374 / 3,279 / 3,359; **Bass 3,073 / 1,712 / 1,544**. Bei den drei melodischen Rollen bewegt
sich zwischen Ordnung 0 und Ordnung 2 weniger als ein Drittel Bit in irgendeine Richtung — auf 3 800
bis 26 000 Noten trägt der Korpus die zweite Ordnung nicht. Beim Bass mit 96 677 Noten trägt er sie:
Faktor **2,9** in der Perplexität. Ein Tonhöhenmodell ist hier also nicht deshalb interessant, weil
viel Entropie da wäre, sondern weil die vorhandene **vorhersagbar** ist.

**Messung 2: Wie viel ist im Anschlagsmuster zu holen, und wie viel davon sagt schon der Kick?**
11 507 Takte, Anschlagsdichte 0,525. Die Wahrscheinlichkeit eines Anschlags je Sechzehntel:

```
0,32 0,59 0,64 0,65 | 0,18 0,55 0,74 0,57 | 0,21 0,61 0,67 0,64 | 0,19 0,55 0,75 0,53
```

Die vier Kick-Schritte sind dreimal seltener besetzt als die übrigen — die Kick-Lücke ist damit am
Korpus **belegt** und nicht angenommen, obwohl in keiner dieser Dateien ein Kick steht (`read_midi`
verwirft Kanal 10, und die Loops sind Einzelinstrument-Exporte). Bedingte Entropien, bit je Schritt:

| Was man weiß | H |
|---|---|
| nur die Dichte | 0,9982 |
| die Kick-Klasse des Schritts (auf dem Kick / Sechzehntel danach / Rest der Lücke) | 0,9062 |
| den genauen Schritt | 0,8945 |
| Schritt und ob der vorige Schritt besetzt war | 0,8709 |

**Der Kick allein erklärt fast nichts**: 0,09 bit je Schritt, 1,5 bit je Takt von 16. Das ist die
erste unbequeme Zahl dieser Runde und sie steht hier, weil sie später die Ablation erklärt. Die
Struktur steckt nicht in der Ausrichtung am Kick, sondern im **gemeinsamen** Muster des Taktes.
Held-out-Kreuzentropie je Takt (16 026 / 1 970 / 1 509 Takte): gleichverteilt über 2^16 **16,000**;
unabhängig je Schritt **14,007**; Nachschlagetabelle über 16-Bit-Taktmuster mit Rückfall **7,532**
(509 Muster im Training gesehen, 18,6 % der Testtakte nie); bestes **parametrisches** Modell, das
keinen Takt auswendig lernen kann (P(Anschlag | Schritt, drei vorige, derselbe Schritt einen und
zwei Beats früher), Krichevsky-Trofimov-Glättung) **7,604**. Ein lernendes Anschlagsmodell ist also
rund **6,4 bit je Takt** wert — und es braucht dafür kein Netz.

**Messung 3: Was `Composer::composeBars` heute produziert.** 10 235 Takte, fünf Stilprofile × acht
Seeds × 256 Takte, als MIDI exportiert und mit denselben Funktionen vermessen:

| | Korpus | `composeBars` |
|---|---|---|
| Anschlagsdichte | 0,525 | 0,534 |
| verschiedene 16-Bit-Taktmuster | 641 | **7** |
| Plug-in-Entropie des Taktmusters | 6,059 bit | **1,062 bit** |
| Anteil Grundton | 0,589 | **0,972** |
| Entropie der Tonhöhe (0. Ordnung) | 2,755 bit | **0,237 bit** |

Sieben Muster über fünf Stile und acht Seeds, davon `.xxx.xxx.xxx.xxx` (Rolling) 69,4 %, ganz leer
27,8 %, Gallop 2,0 %, alles andere unter 1 %. Der Grund steht in `makeTrack`: das primäre Muster
wird nur mit Wahrscheinlichkeit *Track Variation* neu gewürfelt und fällt dann mit 0,45 wieder auf
Rolling, und das sekundäre Muster erscheint nur in den letzten vier Takten mancher 16-Takt-Blöcke.
**Die Stilprofile bewegen den Bassrhythmus praktisch nicht.**

Und die entscheidende Zahl — der heutige Generator als Wahrscheinlichkeitsmodell über dieselben
Ereignisse, gegen die held-out Korpus-Takte gerechnet:

| Modell | bit je Takt |
|---|---|
| `composeBars` + Rückfall auf die Gleichverteilung | **14,304** |
| `composeBars` + Rückfall auf sein eigenes Schritt-Modell | 25,925 |
| korpus-angepasste Nachschlagetabelle + Gleichverteilung | 7,532 |

**61,5 % der held-out Korpus-Takte kann der Generator überhaupt nicht spielen.** Diese Zahl braucht
keine Glättung und ist deshalb die ehrliche; die beiden Kreuzentropien daneben brauchen eine, und
welche es ist, steht im Kopf von `bass_stats.py` statt in einer Fußnote. Für die Tonhöhe, bit je
Note: `composeBars` **6,429** gegen 1,544 für ein korpus-angepasstes Modell zweiter Ordnung.

**Die Entscheidung, und warum sie so ausfällt.** Beide Lücken sind groß. Aber pro Takt gerechnet —
der Korpus-Bass hat 8,4 Noten je Takt — ist die **Tonhöhenlücke rund 41 bit je Takt** (4,9 bit je
Note) gegen **6,8 bit je Takt** beim Anschlagsmuster, also sechsmal so groß; gegen das, was am Ende
wirklich gebaut wurde, sind es 49 bit je Takt. Dazu kommt, dass ein Tonhöhenmodell in die
bestehenden Verträge hineinpasst und ein Anschlagsmodell sie brechen würde: die Engine leitet die
Schwanzgrenze des Kicks und den Kick-Phasenschluss aus `firstBassSlot(pattern_)` her
(`Engine::firstSlotSeconds`), und sie erfährt das Muster über ein `Override`-Steuerereignis auf
`compose.bass_pattern`. Ein gelerntes Taktmuster hat keine Nummer in `kBassPatterns`. **Gebaut wurde
also das Tonhöhenmodell.** Was für das Anschlagsmodell bewegt werden müsste, ist genau aufzählbar
und steht unten unter „Offen".

*Das Modell.* Vierte Rolle im unveränderten Alphabet: `vocab` bleibt 37, der Kopf sagt `roles=4`,
der Bass ist `role=3`. Additiv dazu eine **zehnte** Einbettungstabelle `kick.emb` mit drei Zeilen
und der Kopfschlüssel `condKick`; eine Datei mit `roles=3` hat beides nicht und wird weiter gelesen.
`docs/MODEL_FORMAT.md` beschreibt das an Ort und Stelle. Gleiche Architektur wie die melodische
(4 Schichten, Breite 192, 4 Köpfe, ffn 512, ctx 256), **1 461 093** Parameter gegen 1 460 325.

| Prüfstein | Ergebnis |
|---|---|
| Schnitt | dreiteilig über Loop-Gruppen, Augmentierung (Takt-Rotation, jetzt **mit** dem Anschlagsraster) nur auf der Trainingsseite: 2 182 / 225 / 220 Linien, 13 513 Testnoten, **0,0 %** der Testlinien haben eine Schwester im Training |
| Stufe A, hätte sie je eine Bass-Rolle gehabt | Witten-Bell auf den Bass-Trainingslinien: Ordnung 0 **2,1296**, Ordnung 1 **1,2117**, Ordnung 2 **1,1007** nats |
| `Composer::composeBars` als Modell | Ordnung 0 über 87 417 erzeugte Noten, Add-One geglättet: **4,4560** nats |
| Transformer, Psy allein | Test-NLL **0,4323** nats, 95 % KI [0,3689, 0,5018], 65 s Training |
| gegen Stufe A Ordnung 2 | Abstand **0,6684** nats, 95 % KI [0,5514, 0,7894], P(Abstand ≤ 0) = 0,0000, besser auf **204 von 220** Linien |
| gegen `composeBars` | Abstand **4,0237** nats, 95 % KI [3,4585, 4,5384], P(Abstand ≤ 0) = 0,0000, besser auf **199 von 220** Linien |
| **Ablation: ohne Kick-Konditionierung** | **0,4293** nats. Gepaarter Bootstrap über dieselben Linien: Abstand **−0,0030** nats, 95 % KI [−0,0208, +0,0150], besser auf **109 von 220** Linien — ein Münzwurf, also **nichts** |
| Mehr Daten | 252 Trance-Basslinien nur auf der Trainingsseite (2 als Fast-Dubletten von Testlinien entfernt): **0,3994** nats statt 0,4323 (−7,6 %) — das ist das ausgelieferte Modell |
| int8-Export | NLL 0,3994 → **0,3994** (−0,01 %), 1 485 KiB; NumPy-Leser gegen PyTorch **9,54e−06** auf dem Logit |
| C++-Inferenz gegen die PyTorch-Referenz | 12 Fälle, längster 256 Positionen: größter Logit-Fehler **9,78e−06**, größter Wahrscheinlichkeitsfehler **8,21e−07** |

**Die Kick-Konditionierung bringt null, und das ist erklärbar.** Bei einem Kick auf jedem Viertel ist
die Kick-Klasse eine **Funktion des Schritts**, den das Modell ohnehin bekommt — genau das, was
Messung 2 schon sagte (0,9062 gegen 0,8945 bit). Neue Information wäre sie nur, wenn der Kick nicht
vier auf dem Boden stünde, und das kommt im Korpus nicht vor: in diesen Dateien steht gar kein Kick.
Und es kommt noch eines dazu: **die Klasse 0 (Note auf dem Kick) kommt im Komponisten gar nicht vor.**
Keine der fünf Pattern-Familien setzt eine Bassnote auf den Schlag — die früheste Position ist das
erste Sechzehntel danach —, also sieht das Modell beim Ziehen nur die Klassen 1 und 2, und die
unterscheiden sich für ein Rolling-Muster genau so wie die Sechzehntel 1, 2 und 3, die es ohnehin
als `step` bekommt.
Die Tabelle bleibt trotzdem im Format und im Leser, weil sie 576 Parameter kostet, weil der Selbsttest
nachweist, dass sie den Ausgang wirklich erreicht (die Klasse rotieren bewegt jeden Referenzfall), und
weil ein Korpus mit echtem Kick sie ohne Formatwechsel füllen könnte. Sie ist damit **belegt wirkungslos
und nicht belegt nützlich** — wer sie streichen will, darf: `condKick` darf fehlen.

*Memorisierung* (`Tools/train/memorisation_bass.py`, Batterie und Positivkontrolle der melodischen
Runde, mit einer Anpassung: die Takt-Definition verlangt **zwei** statt drei verschiedene Tonhöhen,
weil ein Psytrance-Bass-Takt sonst fast nie gezählt würde; von 1 509 Held-out-Takten bleiben 729):

| | exakte Taktkopien | längster geteilter Lauf | 8-Noten-Fenster im Abstand 0 |
|---|---|---|---|
| Held-out-Loops (die Referenz) | 39,37 % | 18 Noten (max. 24) | 81,72 % |
| **Modell, T = 1** | **21,15 %** | 19 Noten (max. 24) | 83,83 % |
| Trainingslinien gegen sich selbst | 100,00 % | 24 Noten | 100,00 % |
| **Positivkontrolle** (überangepasst) | **90,32 %** | 24 Noten | 98,58 % |

Das Modell kopiert **weniger** als zwei echte Psytrance-Basslinien einander gleichen. Die absoluten
Zahlen sind für alle hoch, weil ein Bass-Takt ein Objekt niedriger Entropie ist; genau deshalb steht
neben jeder eine Referenz und darunter eine Kontrolle, die ausschlägt.

*Die Phrasenlänge ist gemessen, nicht gewählt.* Das Modell bekommt mit `bars`, wie lang der Loop
ist, den es liest, und der Korpus hat ihm beigebracht: ein kurzer Bass-Loop ist ein statischer.
Auf dem Rolling-Rhythmus und der Constraint-Menge des Komponisten gezogen, spielt es bei **vier**
Takten den Grundton in **79 %** der Noten (Entropie 1,17 bit), bei **acht** Takten in **58 %**
(2,08 bit) — gegen **53 %** und 2,99 bit für echte Basslinien, deren jeder Takt dieselbe
Rolling-Figur ist. Vier Takte wäre die eine Länge gewesen, bei der der gelernte Bass kaum weniger
statisch herauskäme als die Pattern-Familien. `kBassPhraseBars` ist deshalb **8** — und acht ist
auch die Länge der Gruppe der Form.

*Einbau.* `compose.bass_model` (Choice `Pattern`/`Neural`, Vorgabe **Pattern**, ans Ende der
Compose-Tabelle gehängt). Das Modell wird einmal auf dem Komponisten-Thread geladen, nie auf dem
Audio-Thread; fehlt die Datei, steht eine Zeile auf stderr und der Bass bleibt der alte. Je Track
werden zwei Acht-Takt-Phrasen gezogen — eine für das primäre, eine für das sekundäre Muster —,
beide allein aus dem Track-Seed, also ist Takt 3000 Takt 3000, ob die Nacht von vorn gespielt oder
in Stücken gerendert wird. Constraint-Menge: Skalentöne von der Septime unter dem Grundton minus
einer Oktave (**genau die Note, für die `gateLimit` gerechnet wird**) bis eine Oktave darüber; in
diesem Fenster liegen **90,2 %** aller echten Bassnoten des Korpus, und die 7,4 % darunter werden
bewusst aufgegeben, weil eine tiefere Note die Release-Untergrenze ungültig machte, aus der die Gate-
Länge folgt.

| Was nicht kaputtgehen durfte | Nachweis |
|---|---|
| Pattern-Modus unverändert | ein Render von 256 Takten ist **byte-gleich** mit demselben Render vor dieser Runde |
| Kick-Phasenschluss und Schwanzgrenze | der gelernte Bass bewegt **nur Tonhöhen**: über 64 Takte 564 Bassnoten gegen 564, jeder Anschlag, jede Länge und jede Velocity identisch, 244 Tonhöhen anders; die Kicks ebenfalls identisch |
| Register und Skala | 5 079 Noten über vier Seeds: 0 außerhalb der Skala, 0 unter der Note der Gate-Grenze, 0 über einer Oktave |
| Determinismus | zwei Läufe mit demselben Seed sind gleich, und ein allein komponierter Takt ist derselbe Takt wie in der Folge |
| Vorlaufzeit | zwei Phrasen zu 96 Noten je Track = 192 Symbole; bei 122,7 µs je Symbol (AVX2, gemessen im Vektortest) **23,5 ms je Track-Plan**, skalar 248 ms, NEON-Shim 173 ms. Ein Track-Plan kostet ohnehin rund zwei Sekunden, ein Track dauert bei 145 BPM sieben Minuten |
| „Keine zwei aufeinanderfolgenden Acht-Takt-Gruppen eines Kerns sind gleich" | in **beiden** Modi geprüft; die Gruppenfigur aus `Form.cpp` bleibt deshalb auch im Neural-Modus in Kraft, die Phrasenfiguren nicht |

*Und was am Ende wirklich herauskommt.* Dieselben 40 Renders — fünf Stilprofile × acht Seeds ×
256 Takte, **10 235 Takte, 87 417 Bassnoten** — einmal mit `Pattern` und einmal mit `Neural`,
mit denselben Funktionen vermessen, mit denen oben der Korpus vermessen wurde:

| | `Pattern` | `Neural` | Korpus |
|---|---|---|---|
| Anteil Grundton | 0,972 | **0,704** | 0,589 |
| Grundton + Oktaven | 0,983 | 0,764 | 0,619 |
| Entropie der Tonhöhe (0. Ordnung) | 0,237 bit | **1,824 bit** | 2,755 bit |
| Kreuzentropie der held-out Korpusnoten unter dieser Tonhöhenverteilung | 6,429 bit | **4,738 bit** | — |
| Anschläge je Sechzehntel, Takt-Muster, Anschlagsdichte | — | **Zeichen für Zeichen dieselben** | — |

Die letzte Zeile ist der Vertrag, über 10 235 Takte statt über die 64 des Selbsttests: die
Rhythmik der beiden Läufe ist identisch, es bewegen sich nur Tonhöhen. Die Tonhöhenverteilung
geht etwa ein Drittel des Weges von den Pattern-Familien zum Korpus. Weiter kommt sie nicht,
weil der Komponist je Track **zwei** Phrasen zieht und sie wiederholt, und weil die
Constraint-Menge die 7,4 % der Korpusnoten unter der Septime nicht zulässt.

**Was der gelernte Bass nicht kann.** (1) Er ändert **kein** Anschlagsmuster — die 61,5 % der
Korpus-Takte, die der Generator nicht spielen kann, kann er weiterhin nicht. (2)
`compose.bass_follows_chords` wirkt auf ihn nicht: das Alphabet sind Intervalle zum **Grundton**, und
eine gelernte Linie um eine Akkordstufe zu transponieren trüge sie aus der Skala; der Selbsttest hält
diese Grenze als Messung fest (von 480 Noten bewegen sich 6, und die liegen alle auf Beat 1 — das ist
die Gruppenfigur). (3) Die Phrase wiederholt sich alle acht Takte; über einen ganzen Track gibt es
zwei Basslinien, nicht zwanzig. (4) Der `style`-Steckplatz ist weiter 0: die Packs tragen keine
Stilbeschriftung. (5) Die Dreiolen-Familie liegt nicht auf dem Sechzehntelraster; was das Modell
über eine Dreiole erfährt, ist das nächste Sechzehntel.

**Offen — und was ein Anschlagsmodell kosten würde.** Die Messung sagt, dass 6,4 bit je Takt in der
Rhythmik liegen und dass 61,5 % der echten Bass-Takte heute unerreichbar sind. Um das zu heben,
müsste sich bewegen: (a) `Engine::firstSlotSeconds()` darf das erste Slot nicht mehr aus
`firstBassSlot(pattern_)` ziehen, sondern muss es je Takt gesagt bekommen — also ein neuer
Steuerkanal neben dem `Override` auf `compose.bass_pattern`, den `Kick::constrain` und
`setPhaseTarget` lesen; (b) `shortestBassSlot` und damit `gateLimit` müssten über das gezogene Muster
laufen statt über die Familie; (c) `probeLoudness` müsste dasselbe Muster spielen. Ein gelerntes
Anschlagsmodell braucht dafür kein zweites Netz: das beste **parametrische** Modell dieser Messung
(7,604 bit je Takt) ist eine Tabelle von Bernoulli-Parametern über Schritt und fünf Nachbarschritte
und passte in dieselbe Form wie `CorpusTables.cpp`. Ebenfalls offen: der A/B-Hörvergleich (deshalb
steht der Knopf auf `Pattern`), Seed-Streuung (je Modell lief ein Seed), und die Gerätemessung auf
der Quest.

*Dateien.* Neu: `Tools/train/{bass,bass_stats,train_bass,export_bass,memorisation_bass}.py`,
`Core/data/bass.phosmdl` (+ `.ref.txt`). Additiv geändert: `Tools/train/models.py` (eine optionale
zehnte Tabelle, Vorgabe aus — ein melodisches Modell hat unverändert 1 460 325 Parameter und
denselben Kopf), `docs/MODEL_FORMAT.md`, `Core/include/phos/Model.h`, `Core/src/Model.cpp`,
`Core/include/phos/Composer.h`, `Core/src/Composer.cpp`, `Core/include/phos/Params.h`,
`Core/src/Params.cpp`, `Tests/selftest.cpp`. `build_corpus.py`, `dataset.py`, `train.py` und
`export.py` sind **nicht** angefasst; was der Bass von ihnen braucht, importiert er.

**16.09.2026, Wavetable-Bibliothek aus Noctuary.** Die sechs Tabellen des Pads standen bis hierher
als Spektren im Code. Die Regel "keine Samples" betrifft die Sample-*Wiedergabe* und hat die
Wavetable-Bibliothek des Schwesterprojekts Noctuary nie ausgeschlossen: sie ist die eigene Arbeit des
Nutzers, Phosphenes `WaveTable.h` stammt ohnehin aus Noctuarys `CycleTable.h` (b60a2fe), und 5.7
sagt es bereits — die generierten Tabellen aus Noctuarys `Tools/WavetableLib` "kommen mit".

*Die Kandidaten.* `Library/Wavetables` hält **2191 Tabellen** (918 MB) in drei Ordnern: `Classic`
(640, aus AKWF und WaveEdit Online gebaut, beide CC0 1.0), `Harmonic` (551) und `Ambient` (1000).
890 Beipackzettel tragen ein `source`-Feld; 640 davon nennen die CC0-Quelle der `Classic`-Tabellen,
250 (`ambient_sampled`) nennen eine Audiodatei und sind Spektralanalysen **eigenen** Materials des
Nutzers (Stable Audio 3 medium aus der eigenen Pipeline, Stability Community License, die die
Ausgaben dem Nutzer zuschreibt — umsatzgedeckelt: kommerzielle Nutzung frei unterhalb einer Million
US-Dollar Jahresumsatz). Es wurde zuerst mit einer Sperre für diese 250 gerechnet; der Nutzer hat
korrigiert, und alle 2191 sind zugelassen. **Die Messung hat also nichts ausgeschlossen; zugelassen
wären mit Sperre 1941 gewesen, ohne sie sind es 2191.**

*Die Auswahl (`Tools/wt_select.py`).* Gemessen wird jede Kandidatentabelle, 26 ms pro Tabelle, 56 s
für die ganze Bibliothek: spektraler Schwerpunkt je Frame (Median, Spanne), `move` (Median der
totalen Variation zwischen benachbarten Frames), `travel` (erster gegen letzten Frame), `path`
(Summe aller Schritte), **`directness = travel / path`**, Grundtonanteil, Ungerade/Gerade, und das
Aliasing, das die Tabelle nach dem Mipmapping bei C5 und C6 wirklich erzeugt — gemessen mit
demselben Stufenwahl-, Catmull-Rom- und Guard-Modell wie im C++, bei einer Tonhöhe, die genau auf
einem Bin der 65536-Analyse liegt, so dass jeder Bin, der kein Vielfaches des Grundtons ist, Alias
ist und nichts Leckage.

**`directness` ist der Befund der Runde.** Die erste Fassung belohnte nur Bewegung und wählte für
die Fläche WaveEdit-Bänke mit `move` 0,49 bis 0,93 aus: 64 unverwandte Wellen hintereinander. Das
ist keine Fahrt, das ist ein Schüttelbecher — unter einem langsamen Positions-LFO stuft so eine
Tabelle, statt zu gleiten. `travel / path` trennt beides sauber (Dreiecksungleichung: höchstens 1):
die gewählte `WaveEdit Hyperbol` hat `move` 0,012 bei `travel` 0,748 und damit `directness` 1,00,
die zuvor gewählte `Sohler35` 0,022. Für das Lead ist `directness` **nicht** gefordert — eine
Sechzehntelnote ist vorbei, ehe ein Sweep ankommt.

Nach den Toren (Fläche: >= 32 Frames, `travel` >= 0,30, `directness` >= 0,08, Alias C6 <= -60 dB;
Lead: Schwerpunkt >= 8, Alias C6 <= -60 dB; Arp: Schwerpunkt 4..24, Alias C6 <= -62 dB) bleiben 496
/ 124 / 173 Kandidaten. Gewählt wird darin **nicht nach Rang, sondern nach Spannweite**: der beste
Punkt, dann jeweils der, dessen normierter Merkmalsvektor am weitesten von allem bereits Gewählten
entfernt ist (Gonzalez, "Clustering to minimize the maximum intercluster distance", TCS 38, 1985).
Zwölf Tabellen, die in einer Ecke des Raums klumpen, wären eine Tabelle zwölfmal.

| # | Lane | Tabelle | Herkunft | Schwerpkt. | Spanne | move | travel | direct | f1 | C5 | C6 | Frames |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 6 | pad | WaveEdit Hyperbol | CC0 | 8,25 | 3,69 | 0,012 | 0,748 | 1,000 | 0,519 | -64,6 | -64,4 | 64 |
| 7 | pad | Sampled 210 | SA3-Analyse | 2,60 | 0,57 | 0,027 | 0,386 | 0,188 | 0,050 | -91,5 | -79,0 | 64 |
| 8 | pad | WaveEdit Sohler52 | CC0 | 3,30 | 5,08 | 0,074 | 0,974 | 0,089 | 0,444 | -88,0 | -77,3 | 64 |
| 9 | pad | Organ 034 | generiert | 5,74 | 0,28 | 0,007 | 0,397 | 0,977 | 0,217 | -80,4 | -66,6 | 64 |
| 10 | pad | Otmorph 069 | generiert | 2,55 | 0,61 | 0,015 | 0,414 | 0,438 | 0,677 | -80,6 | -76,1 | 64 |
| 11 | lead | WaveEdit Hienharm | CC0 | 31,76 | 6,52 | 0,926 | 0,000 | 0,000 | 0,030 | -115,8 | -111,5 | 64 |
| 12 | lead | WaveEdit Junox_ho | CC0 | 17,47 | 1,35 | 0,025 | 0,823 | 0,425 | 0,010 | -69,2 | -69,1 | 64 |
| 13 | lead | WaveEdit Euclidea | CC0 | 11,08 | 0,74 | 0,629 | 0,934 | 0,021 | 0,004 | -71,9 | -69,4 | 64 |
| 14 | lead | WaveEdit Sohler49 | CC0 | 38,25 | 6,22 | 0,372 | 0,853 | 0,033 | 0,024 | -80,4 | -74,3 | 64 |
| 15 | arp | Consonant 129 | generiert | 4,55 | 2,34 | 0,017 | 0,822 | 0,751 | 0,343 | -92,7 | -80,6 | 64 |
| 16 | arp | AKWF 0004-hollow-01 | CC0 | 14,00 | 5,84 | 0,526 | 0,988 | 0,053 | 0,001 | -102,0 | -88,2 | 37 |
| 17 | arp | WaveEdit Pd104 | CC0 | 20,50 | 0,26 | 0,047 | 0,309 | 0,112 | 0,000 | -64,9 | -72,2 | 64 |

(C5/C6 sind hier die Werte von Frame 0, die auch der Selbsttest prüft; die Auswahl gattert gegen das
Maximum über drei Frames.) Der Raum wird gespannt: Schwerpunkt 2,55 bis 38,25, Grundtonanteil 0,000
bis 0,677, Helligkeitsspanne 0,26 bis 6,52 Oktaven. Fünf der zwölf sind nicht aus `Classic`: drei
generierte und zwei aus den gemessenen Familien — und die beste Flächen-`directness` nach `Hyperbol`
hat `Organ 034` (0,977), gefolgt von `Consonant 129` (0,751). Die Korrektur des Koordinators war
inhaltlich richtig: die gemessenen und generierten Familien gewinnen genau dort, wo eine Fläche
gleiten soll.

*Das Format: gemessen, nicht geraten.* Die Dateien liegen in zwei Layouts — `Classic` als
16-Bit-PCM mit `clm `-Chunk, `Harmonic`/`Ambient` als 32-Bit-Float ohne jeden Hinweis auf die
Framelänge, 512 KB je Tabelle. Statt der `.wav` schreibt `Tools/wt_pack.py` eine **`.phoswt`**
(Byte-Layout normativ in `Core/include/phos/WaveTableFile.h`, in der Form von `MODEL_FORMAT.md`),
die genau das trägt, was `buildFromHarmonics()` frisst: die Fourier-Koeffizienten je Frame, unter
-110 dB des lautesten Harmonischen des Frames abgeschnitten, als int16 gegen **eine Skala je
Oktavband**.

| Variante | Größe | Rückweg |
|---|---|---|
| die `.wav` wie sie liegen | 4 084 784 B | — |
| float32-Koeffizienten, Schwelle -110 dB | 1 449 796 B | 100,7 dB |
| int16, **eine** Skala je Frame, -110 dB | 730 036 B | 82,7 dB |
| **int16, eine Skala je Oktavband, -110 dB (gewählt)** | **750 200 B** | **92,2 dB** |

(Die ausgelieferte Datei misst 750 264 B; der Unterschied ist der Text im Kopf.) Die Oktavbänder
kosten höchstens zehn Floats je Frame und bringen 10 dB: mit einer Skala je Frame ist der
Quantisierungsfehler jeder Harmonischen gleich groß, und 512 davon summieren sich weit über den
Fehler einer einzelnen. 92,2 dB liegen drei dB unter dem 16-Bit-PCM, in dem die `Classic`-Dateien
ohnehin gespeichert sind. **Geladen** wird die Datei in 111,0 ms gegen 137,1 ms für dieselben zwölf
Tabellen aus den `.wav` (gemessen im Selbsttest; `PHOS_WT_SOURCE` zeigt auf Noctuarys Bibliothek):
der Mip-Aufbau — zehn inverse FFTs je Frame — ist der Löwenanteil, die gesparte Analyse macht 19 %.
Auf der Platte spart der Pack das 5,4-fache, im APK also 3,3 MB.

*Speicher.* 741 Frames × 33 016 Byte = **23,33 MB** nach dem Mip-Aufbau, dazu die 5,3 MB der sechs
eingebauten Tabellen. Das ist der größte Speicherblock der Engine. Der Quest-Hebel dafür ist
`setWaveTableFrameLimit(n)`: die Frames werden gleichmäßig ausgedünnt (beide Enden bleiben, wie es
Noctuarys `CycleTable::build` tut), 32 halbiert Speicher und Ladezeit und vergröbert den Morph —
dieselbe Art Handel wie das Unisono-Limit, und wie dieses per Vorgabe aus.

*Verdrahtung und Verträglichkeit.* Der `table`-Parameter adressiert beides mit **einem** Index: 0..5
sind die sechs eingebauten Tabellen, ab 6 die Bibliothek. Namen und Reihenfolge stehen zur Bauzeit
in `Core/include/phos/WaveTableList.inl` (generiert), so dass `Params.cpp` eine statische Liste
bleibt. Das ist der Vertrag: `.phosset` und Plugin-Zustand speichern den Index als Zahl
(`lead.table=1`), also muss "1" weiter Vocal heißen — der Selbsttest prüft es. Fehlt die Datei,
liefert `waveTable()` die eingebaute Tabelle, die der Deskriptor als Rückfall nennt (nach Lane
gewählt, nicht Index 0), so dass ein anderswo gespeichertes Set weiter spielt. Der Supersaw hält
unverändert seinen eigenen Zeiger auf die Classic-Säge (Frame 2); ein eigener Check zeigt, dass
`pad.table=6` daran bitgleich nichts ändert.

*Nichts ist zurückgegangen.* Standard-Render 90 s, Seed 20260916: **bytegleich** (MD5
`8ea91a57…`) mit demselben Render aus master 36c1cc8. Supersaw-Aliasing unverändert: C4 -76,7,
C5 -72,4, C6 -68,9, A6 -61,6 dB bei Detune 1,00 und -80,8 / -76,0 / -73,2 / -61,3 bei 0,55.
`phos_vectest`, `_neon` und `_scalar` bitgleich (0 abweichende Samples in 56 Schächten und 16
Kanälen), ctest 4 von 4, Selbsttest 234 Prüfungen. Kosten von acht Wavetable-Pad-Stimmen: vorher
skalar 4,2 % / AVX2 3,4 %, nachher 4,1 % / 3,4 %; ein voller Render kostet mit einer eingebauten
Tabelle 7,88–8,11 % und mit einer Bibliothekstabelle 7,82–8,01 % eines Kerns — der Lesevorgang ist
derselbe, die Tabelle ist ihm gleich.

*Offen.* Das Plugin und die Quest-App rufen `setWaveTableSearchPath()` nicht, weil `Plugin/` und
`Quest/` dieser Runde nicht gehören; ein Entwicklungsbau findet die Datei über
`PHOS_SOURCE_DATA_DIR`, ein ausgeliefertes Plugin fällt bis dahin auf die eingebauten Tabellen
zurück. Ebenfalls offen: `Quality.h` kennt `setWaveTableFrameLimit` noch nicht (dieselbe
Eigentumsgrenze), und eine Hörprüfung der zwölf Tabellen im Arrangement gab es nicht — diese Runde
hat gemessen, nicht gehört.

*Dateien.* Neu: `Core/include/phos/WaveTableFile.h`, `Core/src/WaveTableFile.cpp`,
`Core/include/phos/WaveTableList.inl` (generiert), `Core/data/library.phoswt`,
`Core/data/CREDITS-wavetables.md`, `Tools/wt_select.py`, `Tools/wt_pack.py`,
`Tools/wt_selection.json`. Geändert: `Core/include/phos/WaveTable.h` (`kNumWaveTables` heißt jetzt
`kNumBuiltinWaveTables`), `Core/src/WaveTable.cpp`, `Core/src/Poly.cpp` (zwei Stellen: laden in
`prepare()`, `waveTable()` statt `builtinWaveTable()` in `update()`), `Core/src/Params.cpp`,
`Core/CMakeLists.txt`, `Tests/CMakeLists.txt`, `Tests/selftest.cpp`, `.gitignore`.

**16.09.2026, Nachtrag: True Peak und Fundament-Probe.** Zwei Befunde der Mischungsrunde, die dort
nicht repariert werden durften. Der erste war größer als gemeldet, der zweite war kein Fehler der
Probe.

### 1. Der True-Peak-Schätzer: das Fenster war falsch entworfen, nicht zu kurz

*Das Maß zuerst.* Die unabhängige Messung ist eine **exakte bandbegrenzte Interpolation** (Spektrum
per FFT mit Nullen aufgefüllt, 16-fach, nur die mittlere Hälfte jedes Blocks gewertet), gegen vier
Signale geprüft, deren Spitze feststeht: Sinus bei 0,25 fs mit 45 Grad Phase (größtes Sample
−3,010 dBFS, gelesen **+0,000 dBTP**), Sinus bei 0,49 fs (+0,007), Einheitsimpuls (+0,000),
Nyquist-Wechsel (+0,000). Dieselbe Rechnung steckt schon als `exactTruePeak` im Selbsttest.

*Der Befund.* Acht Minuten Render, Seed 1, Standardwerte: `Engine::meter()` meldet **−0,98 dBTP**,
die exakte Messung sagt **+0,888 dBTP**. Nicht 0,3 bis 0,5 dB daneben, sondern **1,87 dB** — die
Sinc-Überabtastung der Mischungsrunde (−0,50 bis −0,68) hat selbst zu niedrig gelesen. ffmpegs
`ebur128` (+0,3) liegt dazwischen, weil es mit dem Filter von BS.1770-4 vierfach abtastet.

*Die vier Verdächtigen, einzeln mit einer Zahl erledigt:*

| Verdacht | Messung | Urteil |
|---|---|---|
| **Normalisierung** („unity at DC") | Die Tap-Summen jeder Phase sind **1,000000000**, bevor geteilt wird | **freigesprochen**: die Division ist ein No-op. Sie bleibt stehen, damit ein anderes Fenster den Gleichanteil nicht stillschweigend verschiebt. |
| **Fensterlänge und β** | Durchlass des alten Banks: **−1,32 dB bei 0,40 fs, −5,58 dB bei 0,45 fs** | **der Haupttäter — aber β, nicht die Länge.** Siehe unten. |
| **Nur drei Phasen** | Das Gitter allein verfehlt eine Spitze um cos(π f / 4): **0,44 dB bei 0,40 fs** | **mitschuldig**, etwa ein Viertel des Fehlers. |
| **Der Gebrauch im Limiter** | `prevBetween_` deckt das Intervall vor dem Sample, `between` das danach; die Ausrichtung von Schiebeminimum, gleitendem Mittel und Verzögerung wurde nachgerechnet: die Verstärkung zum Zeitpunkt *t* liegt unter der Anforderung **jedes** der W Minima, weil das Sample, das sie trägt, in jedem ihrer Fenster liegt | **freigesprochen** |

*Warum β = 8 falsch war.* Das ist ein **Bruchverzögerungs-Filter**, an einem Punkt ausgewertet, kein
interpolierendes Tiefpassfilter. Für ein solches ist nur die **Durchlassabweichung** |H(f)| − 1 über
0 bis 0,5 fs ein Fehler: das Eingangssignal ist bereits bandbegrenzt, es gibt kein Spiegelbild, das
ein Sperrband unterdrücken müsste (Laakso, Välimäki, Karjalainen und Laine, *Splitting the unit
delay*, IEEE Signal Processing Magazine 13(1), 1996). Ein Kaiser-Fenster mit β = 8 kauft rund 80 dB
Sperrdämpfung, die niemand braucht, und bezahlt sie mit einem Übergangsbereich, der den Durchlass
frisst. Bei 24 Taps gemessen (schlechteste Durchlassabweichung über 0 bis 0,45 fs): **β = 4: 0,14 dB;
β = 5: 0,51 dB; β = 8: 1,79 dB.** Das Optimum liegt bei jeder Länge zwischen 3,5 und 5, nie bei 8.

*Die Änderung.* `TruePeakInterpolator` ist jetzt **acht Phasen zu 24 Taps, Kaiser β = 4** (vorher vier
Phasen zu 12 Taps, β = 8). Nichts daran ist eine neue Idee — es ist dasselbe Verfahren von
ITU-R BS.1770-4 Annex 2 mit einem Filter, das für seine Aufgabe entworfen ist. Über 0,45 fs
(21,6 kHz bei 48 kHz) wird **nichts behauptet**; dort bräuchte die Rekonstruktion hunderte Taps je
Phase, weil die Samples eines Sinus nahe Nyquist seine Amplitude kaum noch tragen.

*Die Prüfung, erst fallen gesehen.* Die alte Prüfung fuhr einen Sinus über hundert Samples und nahm
den größten Messwert irgendwo darin — dabei wandert das Sample-Gitter durch die Phase und trifft
**zufällig** einen Kamm: sie las 0,11 dB für einen Schätzer, der einen einzelnen Kamm um 4 dB
verfehlt. Jetzt wird der Kamm **gelegt**: das Maximum des Sinus liegt an einem gewählten Bruchteil
zwischen zwei Samples, und gemessen wird nur in dem Intervall, das ihn enthält — die drei Samples,
die ein Limiter in der Hand hat, wenn er über dieses Sample entscheidet. Dazu ein `sinc(0,9·(t−0,5))`,
bandbegrenzt auf 0,45 fs, Spitze exakt 1 bei t = 0,5, größtes Sample −3,11 dBFS.

| Prüfung | alt | neu |
|---|---|---|
| Kamm zwischen den Samples, bis 0,45 fs | **−4,234 dB** (bei 0,45 fs, Kamm mittig) | **−0,115 … +0,068 dB** |
| bandbegrenzter Sinc, Spitze zwischen den Samples | **−0,351 dB** | **+0,028 dB** |
| Limiter, Programm auf 0,45 fs bandbegrenzt, 12 dB über der Decke, unabhängig gemessen | **−0,37 dBTP** (0,63 dB über der Decke) | **−0,98 dBTP** |
| dasselbe, vom eigenen Schätzer gemessen | — | **−1,00 dBTP** |

Das Material der alten Limiter-Prüfung endete bei 10 kHz (Zweipol-Tiefpass), also dort, wo selbst
zwölf Taps recht haben; alles, was der Limiter zwischen 10 und 21,6 kHz falsch machte, war ihr
unsichtbar. Es ist jetzt per FFT **exakt** auf 0,45 fs bandbegrenzt.

*Gegenprobe am echten Render.* Eine Nachbildung des neuen Banks in Python liest auf dem
Acht-Minuten-Render **−0,983 dBTP**, die Engine meldet −0,98: die Implementierung ist der Entwurf.
Denselben Render hart auf 21,6 kHz beschnitten liest die Nachbildung **−0,188** gegen exakt
**−0,200** — innerhalb ihres Bandes ist der Schätzer auf **0,012 dB** genau.

*Was übrig bleibt, und warum es nicht dem Limiter gehört.* Der Render trägt **−33 dB seiner
Gesamtleistung zwischen 22 und 24 kHz** (je Hertz mehr als zwischen 18 und 20 kHz): ein flacher
Rauschteppich bis Nyquist aus den Rauschquellen der Percussion und den nichtlinearen Stufen. Genau
dieses Band hebt die echte Spitze um rund 1,0 dB, und kein bezahlbares Filter sieht es:

| Bank | liest auf dem Render |
|---|---|
| 12 Taps β 8, 4-fach (alt) | −0,993 dBTP |
| **24 Taps β 4, 8-fach (neu)** | **+0,305 dBTP** |
| 48 Taps β 12, 8-fach | +0,334 |
| 96 Taps β 14, 16-fach | +0,674 |
| 256 Taps β 18, 16-fach | +0,862 |
| exakt | +0,888 |

Der Render geht damit von **+0,888 auf +0,083 dBTP** (unabhängig gemessen, Seed 1, acht Minuten):
**1,30 dB weniger Überschreitung**, aber noch 1,08 dB über der Decke. Die Reparatur des Restes gehört
zu den Quellen (Bandbegrenzung des Percussion-Rauschens in `Perc.cpp`), nicht zum Limiter, und ist
die nächste Kalibriergröße.

*Kosten.* Der Bank ist 4,7-fach größer (7 × 24 statt 3 × 12 Multiplikationen je Kanal), der Render
wurde trotzdem nur **3,9 %** langsamer (120 s Audio in **5,999 → 6,232 s**, 20,0-fach → 19,3-fach
Echtzeit). Zwei Dinge dazwischen, beide gemessen:

- **Die Summation war eine Kette.** Naiv geschrieben hängen bei 24 Taps 24 Additionen voneinander ab,
  und der Bank ist latenzgebunden, nicht durchsatzgebunden: der Render kostete **+64 %** (5,999 →
  10,448 s). Vier unabhängige Teilsummen zu je sechs Additionen bringen das auf ein Viertel.
- **Ein exakter Überspringer.** Kein interpolierter Punkt kann `max_k Σ_m |h_k[m]|` mal das größte
  Sample im Fenster überschreiten (die ℓ1-Norm des Banks, `gainBound()`; hier 2,458). Liegt dieses
  Produkt unter der Decke, wird der Bank übersprungen und der Zwischenwert als Null vermerkt —
  **exakt, nicht näherungsweise**: jeder so verworfene Wert liegt unter der Decke, und unter der Decke
  ist die verlangte Verstärkung ohnehin 1. Auf dem Render läuft der Bank dadurch bei **30 %** der
  Samples. Zusammen mit dem Wegfall des Modulo-Rings (jedes Sample wird in beide Hälften eines doppelt
  langen Puffers geschrieben, das Fenster ist dadurch immer zusammenhängend) kostet der Limiter
  **weniger als vorher**: 0,127 s → 0,083 s je zwei Minuten Audio. Der Rest der 3,9 % ist die
  Lautheitsanzeige, die denselben Schätzer auf jedem Sample führt.

*Latenz.* `kHalf` 6 → 12, also `TruePeakLimiter::latency()` **77 → 83 Samples**. Hosttest (101
Prüfungen, **ohne** `PHOS_MUTE`) und VST3-Test (30 Prüfungen) grün; der Host liest 83.

### 2. Die Fundament-Probe: die Schranke maß die Form, nicht die Probe

*Die Zerlegung.* Der Pegelangleich gibt Track *i* die Verstärkung `Referenz − Probe_i`, also ist die
Spanne nach dem Angleich genau die Spanne des Probenfehlers `e_i = Probe_i − echt_i`. Der Prüfstand
dafür ist `PHOS_ONLY=testProbeAudit`. Drei Seeds, je sechs Tracks, `mix.perc_level` +3 dB:

| Seed | Spanne | Spanne ohne den Energie-Zuschlag der Sektion |
|---|---|---|
| 31 | 1,92 LU | **0,62 LU** |
| 7 | 0,49 LU | **0,49 LU** |
| 2026 | 1,59 LU | **0,29 LU** |

*Der Grund.* Der Komponist legt auf jede Sektion `energyGainDb(Energie)` — die Lautheitsseite von
Farboods Spannungsmodell, höchstens ±2 dB. Ein **Groove** hat Energie 0,61 und bekommt **−0,47 dB**,
ein **Drop** hat 0,87 und bekommt **+0,83 dB**: die beiden liegen **1,30 dB auseinander, und zwar mit
Absicht**. Die Prüfung maß jeden Track in seinem ersten Kern — „Groove **oder** Drop" — und verglich
damit einen Groove mit einem Drop. Auf ihren eigenen vier Tracks sind **1,30 der 1,56 LU** dieser
Zuschlag. Deshalb musste die Schranke wachsen, als das Kit schwerer wurde: sie maß nicht die Probe.

*Die Probe ist nicht der Täter, und sie besser zu machen hat geschadet.* Was nach Abzug des Zuschlags
bleibt, ist der Probenfehler, und der wächst tatsächlich mit dem Kit: **0,15 / 0,20 / 0,26 LU** bei
`mix.perc_level` +1 / +2 / +3 dB, also 0,056 LU je dB. Der Mechanismus ist gemessen: die Probe fährt
die Lanes des Kits immer **voll** (`plan.perc.layers`), während der echte Kern je nach Form nur einen
Teil spielt (Seed 31: 2 von 6, 2 von 6, 4 von 5, 2 von 7). Der naheliegende Umbau — die Probe fährt
**echte Takte** (vier Fenster zu zwei Takten über den Track verteilt, mit `planBar`, und Kick und Bass
nach `kickBeats`/`bassBeats`/`cutBeats` der Form) — sah auf Seed 31 aus wie ein Gewinn
(1,56 → 0,75 LU) und war über drei Seeds **schlechter**: 0,91 / 2,08 / 0,78 LU gegen 0,62 / 0,49 /
0,29. Ein Seed ist keine Stichprobe; der Umbau ist **verworfen und zurückgenommen**, `Composer.cpp`
ist unverändert.

*Die Änderung.* Die Prüfung zieht den Energie-Zuschlag der gemessenen Sektion von jedem Messwert ab —
die Formel steht **ausgeschrieben im Test**, nicht aus dem Komponisten geholt, damit eine Änderung auf
einer der beiden Seiten als Widerspruch auffällt statt sich wegzukürzen. Die Schranke geht von **1,8
auf 0,8 LU** (dreimal der Messwert am Standard-Kit, und über der 0,62 LU des schlechtesten der drei
Seeds), und die Prüfung liest **0,26 LU mit, 3,17 LU ohne** den Angleich. Der zweite Teil der Aussage
— der Angleich nimmt mehr als 1 LU heraus — steht unverändert. **Keine zusätzliche Probe, keine
zusätzliche Prüfzeit.**

### Gegenprobe (Mutationsrunde)

Acht Fehler einzeln eingebaut, sieben von ihrer Prüfung gefunden:

| Mutation | Wer merkt es |
|---|---|
| Kaiser-β zurück auf 8 | Kamm-Prüfung (−1,288 statt −0,115 dB) |
| vier Phasen statt acht | Kamm-Prüfung (−0,275) **und** Limiter (−0,82 dBTP) |
| Fenster des linken Kanals um eins verschoben | Limiter, am eigenen Schätzer (−0,97 statt −1,00 dBTP) |
| der Überspringer schätzt mit dem mittleren Sample statt mit dem Fenster ab | Limiter, am eigenen Schätzer (−0,85 dBTP) |
| die gespiegelte Hälfte der Historie nicht geschrieben | Limiter (+0,07 dBTP) |
| `energyGainDb` steiler als die Formel im Test | Pegelangleich (1,04 statt 0,26 LU) |
| der Pegelangleich vergibt keine Verstärkung | Pegelangleich (3,17 LU, so weit wie ohne ihn) |
| `prevBetween_` aus dem Maximum des Limiters entfernt | **niemand — und zu Recht:** das Intervall zwischen Sample *i* und *i+1* wird schon bei Sample *i* bewertet, und das gleitende Mittel trägt die Verstärkung über beide. Der Term ist Gürtel und Hosenträger, nicht tragend. |

Zwei dieser Mutationen (Fenster verschoben, Überspringer falsch abgeschätzt) fielen zuerst **nicht**
auf: gegen die **unabhängige** Messung darf der Limiter nur auf 0,15 dB genau sein — die Genauigkeit
des Schätzers —, und darin verstecken sich Fehler im *Gebrauch* des Schätzers. Gegen den **eigenen**
Schätzer hat er gar keine Toleranz: was der Bank meldet, muss die Verstärkung an der Decke gehalten
haben (0,02 dB). Beide Messungen stehen jetzt in derselben Prüfung; das ist die Lehre dieser Runde für
jede Prüfung, die eine Toleranz aus der Genauigkeit ihres eigenen Messmittels zieht.

Gesamt: **211 Selbsttest-Prüfungen** in **331 s**, Vektortests 15 von 15 in AVX2, NEON-Shim und
skalar, Hosttest 101, VST3-Test 30. Der Selbsttest ist durch die längere Bank rund 5 s langsamer; es
kam keine Probe und kein Render hinzu.

**16.09.2026, Nachtrag: die Bibliothek erreicht Plugin und Quest.** Die Wavetable-Runde hat die
zwölf Tabellen gemessen, gepackt und verdrahtet, aber zwei Enden offen gelassen, weil `Plugin/` und
`Quest/` ihr nicht gehörten: **niemand rief `setWaveTableSearchPath()`**, und `Quality.h` kannte
`setWaveTableFrameLimit()` nicht. Ein Entwicklungsbau fand die Datei trotzdem — über
`PHOS_SOURCE_DATA_DIR`, den Pfad in den Quellbaum — und genau das ist die Falle: alles klang
richtig, während ein ausgeliefertes Plugin auf die sechs eingebauten Tabellen zurückfiel, ohne ein
Wort zu sagen. Diese Runde schließt beide Enden und macht den Rückfall sichtbar.

*Wie jede der drei Oberflächen die Datei findet.* Der Kern öffnet seine Ressourcen über `fopen` und
sucht in dieser Reihenfolge: das Arbeitsverzeichnis, den Suchpfad, `PHOS_SOURCE_DATA_DIR`. Ein
Plugin hat kein brauchbares Arbeitsverzeichnis (es ist das des Hosts) und keinen Quellbaum, also
bleibt der Suchpfad — und woher der kommt, ist je Oberfläche eine andere Frage.

| Oberfläche | Woher der Pfad kommt | Wo die Datei liegt |
|---|---|---|
| Standalone | `currentExecutableFile`, Elternverzeichnis | neben `Phosphene.exe` |
| VST3 | `currentExecutableFile` ist das Modul im Bundle; von `Contents/<arch>` eine Ebene hoch | `Phosphene.vst3/Contents/Resources` |
| Quest | Asset im APK, beim ersten Start einmal in `internalDataPath` ausgepackt | `<internalDataPath>`, oder `<externalDataPath>`, wenn dort eine liegt |

`resolveWaveTableDirectory()` (PluginProcessor.cpp) probiert die Kandidaten der Reihe nach und nimmt
den ersten, in dem die Datei **wirklich liegt** — geprüft wird die Datei, nicht das Verzeichnis, so
dass eine halb kopierte Installation nicht auf ein leeres `Resources` zeigt. Aufgerufen wird das im
Konstruktor des Prozessors, **vor** dem Start des Composer-Threads: dieser Thread bereitet eigene
Probe-Engines vor, und geladen wird die Bibliothek vom ersten `Engine::prepare()` im Prozess,
gleichgültig welcher Thread das ist. Später wäre ein Rennen, zweimal wäre wirkungslos.
`Contents/Resources` ist die Stelle, die die VST3-Spezifikation für Plugin-Daten vorsieht, und die
einzige, die ein Host beim Kopieren eines Bundles mitnimmt. Die APK-Seite ist ein Zwischenschritt
mehr: ein Asset im APK hat **keinen Dateinamen**, es ist ein deflatiertes Zip-Glied, an das nur der
`AAssetManager` kommt. `prepareWaveTables()` packt es deshalb einmal aus (750 KB, `.part` und
`rename`, damit eine abgebrochene Installation nie unter dem endgültigen Namen steht) und übergibt
dieses Verzeichnis. Eine per `adb push` daneben gelegte `library.phoswt` gewinnt — dieselbe Regel,
der `phos.cfg` schon folgt.

*Was der Editor zeigt, wenn nichts da ist.* Die Tabellen bleiben in der Liste und werden
**markiert**, nicht entfernt: `lead.table` bekommt hinter dem Namen ein `(missing)`. Entfernen wäre
die schlechtere Wahl, und zwar nicht aus Bequemlichkeit — `ComboBoxParameterAttachment` bildet den
Parameter auf die *Position* eines Eintrags ab (`index / (count - 1)`,
juce\_ParameterAttachments.cpp). Fiele ein Eintrag weg, verschöben sich alle dahinter: "7" wäre auf
einer Installation mit Pack eine andere Tabelle als auf einer ohne, und ein dort gespeicherter
Zustand lüde hier falsch. Der Vertrag der Runde ist, dass der Index die Tabelle ist; er gilt auch
dann, wenn die Tabelle fehlt. Hörbar ändert sich nichts: `waveTable()` gibt weiter die eingebaute
Tabelle heraus, die der Deskriptor nennt, ein anderswo gespeichertes Set spielt weiter. Der
Prozessor beantwortet dieselbe Frage auch als Zahl (`waveTableLibrary()`: Verzeichnis, wie viele
ausgeliefert, wie viele wirklich geladen) — der Hosttest liest genau das.

*Das Framelimit: 32, und warum nicht 16.* `Quality::quest()` setzt `waveTableFrames = 32`,
`Quality::desktop()` lässt 0 (jeder Frame); `Engine::prepare()` reicht das an
`setWaveTableFrameLimit()` weiter, **vor** der ersten `Poly::prepare()`, weil das Limit beim Bau der
Bibliothek gelesen wird. Das ist die einzige Quest-Stellschraube, die keine Rechenzeit spart,
sondern **Körnung** kostet: der Positionsregler blendet zwischen zwei Nachbarframes über, Ausdünnen
macht jeden Schritt größer — und "stuft statt zu gleiten" war der Befund der Auswahlrunde. Gemessen
wird deshalb mit deren eigenem Maß (`testWaveTableQuality`, Median der totalen Variation zwischen
den Leistungsspektren benachbarter Frames), über die fünf Flächentabellen:

| Limit | schlechtester Median-Schritt der Fläche | wer | Speicher | `Engine::prepare` |
|---|---|---|---|---|
| 64 (alle) | 0,074 | WaveEdit Sohler52 | 23,33 MB | 110 ms |
| 48 | 0,112 | WaveEdit Sohler52 | — | — |
| **32 (gewählt)** | **0,173** | WaveEdit Sohler52 | **12,09 MB** | **71 ms** |
| 24 | 0,169 | WaveEdit Sohler52 | — | — |
| 16 | 0,261 | WaveEdit Sohler52 | — | — |

Die Schranke kommt aus derselben Quelle wie das Maß: die WaveEdit-Bänke, die die erste Auswahl
fälschlich für die Fläche nahm, hatten Median-Schritte ab **0,49**. 32 Frames holen die ganze
Speicherersparnis und bleiben bei einem Drittel davon; 16 gingen die halbe verbleibende Strecke in
den verworfenen Bereich, für Speicher, den das Quest-Budget nicht verlangt. `directness`
(`travel / path`) leidet beim Ausdünnen überhaupt nicht — beide Enden bleiben, `travel` ist
unverändert und `path` kann nur schrumpfen, die schlechteste Flächen-`directness` steigt von 0,089
auf 0,116. Die Schranke gilt **nur für die Fläche**: `directness` war für Lead und Arp nie gefordert
(eine Sechzehntel ist vorbei, ehe ein Sweep ankommt), und drei jener sieben Tabellen springen bei
jeder Framezahl um 0,5 und mehr — sie daran zu messen hieße, die Auswahl zu messen und nicht das
Ausdünnen. 12,09 MB sind nicht genau die Hälfte von 23,33, weil `AKWF 0004-hollow-01` nur 37 Frames
hat und von 32 kaum berührt wird.

*Was im APK dazukommt.* `build_apk.ps1` legt die Datei in ein Staging-Verzeichnis und gibt `aapt2
link` ein `-A`; ausgeliefert wird dieselbe `Core/data/library.phoswt`, die der Selbsttest misst, und
nicht eine Kopie unter `Quest/`. **3 892 110 → 4 555 729 Byte (+648 KB, +17 %)**; im Zip deflatiert
der Pack auf 662 195 Byte. Der letzte Schritt des Skripts liest das fertige, signierte APK wieder
und besteht darauf, dass `assets/library.phoswt` darin steht und so groß ist wie das Original — ohne
Headset ist das die einzige Prüfung, die die Quest-Seite überhaupt zulässt, und sie fängt genau den
Fehler, der sonst erst auf dem Gerät und nur als "klingt anders" auffällt.

*Die Prüfungen, jede zuerst rot gesehen.* Hosttest: dass das Plugin den Pack **neben seiner eigenen
Binärdatei** findet (die Datei wird dazu neben `phos_hosttest` installiert, sonst sähe der Test nur
`PHOS_SOURCE_DATA_DIR`) — rot mit "found \"\"", ehe der Konstruktor den Suchpfad setzte. Dann der
Gegenfall: die Bibliothek wird absichtlich verfehlt (ein Name, den es nicht gibt, danach meldet
jeder Ladeversuch im Prozess nichts — der Zustand einer Installation ohne Ressourcen), und das
Plugin muss trotzdem klingen (RMS über acht Takte, weil ein Track mit einem dünnen Intro beginnt)
und im Chooser genau die zwölf Bibliothekseinträge markieren und keinen der sechs eingebauten — rot
mit "0 of 12 marked". Selbsttest: `Engine::prepare` mit `Quality::quest()` muss auf 32 Frames
kommen und beide Enden behalten — rot mit "quest 64". Dazu die Messung selbst, als Tabelle im
Protokoll. VST3-Test: das gebaute Bundle trägt den Pack in `Contents/Resources`. **Mutationsrunde,
sechs Fehler, jeder gefangen, jeder zurückgenommen, `git diff` sauber:** den Konstruktoraufruf
entfernt (Hosttest 1 Fehler), die Markierungsbedingung umgedreht (Hosttest 1), `waveTableFrames` auf
64 (Selbsttest 2), `setWaveTableFrameLimit` hinter die erste `Poly::prepare()` geschoben
(Selbsttest 2), die Bundle-Kopie ein Verzeichnis daneben (VST3-Test 2), `-A` aus `aapt2 link`
entfernt (das Skript bricht ab).

*Nichts ist zurückgegangen.* Standard-Render 90 s, Seed 20260916: bytegleich vor und nach der Runde
(MD5 `1774e3a3…`). `phos_vectest`, `_neon` und `_scalar` je 16 von 16 und die Bass-Logits identisch,
ctest 6 von 6, Selbsttest 237 Prüfungen (234 + 3), Hosttest 109, VST3-Test 32. Der Desktop-Pfad ist
unberührt: `Quality::desktop()` fragt 0 Frames, was das Limit ohnehin ist.

*Unterwegs gefunden.* `Quest/src/main.cpp` ließ sich **gar nicht mehr übersetzen**: `publish()` las
`kMelodyMaxBlocks` und `MelodyPlan::blockParts/padGate`, die es seit der Formgrammatik aus Phase 5
nicht mehr gibt. Repariert über `planBar()` (Form.h), also über dieselbe Instrumentierungsmatrix,
aus der der Composer seine Bar-Entscheidungen zieht; das Panel zeigt jetzt den **Takt** statt des
16-Takt-Blocks und weiß damit auch von Buildups, Cuts und dem Pre-Drop-Break. Dass es niemandem
aufgefallen war, sagt etwas: die Quest-App wird von keinem ctest gebaut.

*Offen.* Es hängt **kein Headset** an der Maschine: gebaut und gepackt ist alles, gelaufen ist auf
dem Gerät nichts — das Auspacken beim ersten Start, die Zeile `wavetables: unpacked …` im Logcat und
dass der zweite Start sie nicht wiederholt, sind ungeprüft, ebenso alle Punkte, die `Quest/README.md`
schon offen führt. Ebenso ungeprüft: ein **echter DAW** — `phos_vst3test` lädt das Modul wie ein
Host, aber kein Reaper oder Bitwig hat das Bundle je installiert; dass `Contents/Resources` die
Kopie eines Hosts überlebt, ist Spezifikation, nicht Messung. Und gehört wurde wieder nicht: diese
Runde hat verdrahtet und gemessen.

*Dateien.* Geändert: `Plugin/PluginProcessor.h/.cpp` (Suchpfad, `waveTableLibrary()`),
`Plugin/EditorLayout.cpp` (Markierung), `Plugin/CMakeLists.txt` (Kopie neben die Artefakte),
`Core/include/phos/Quality.h` (`waveTableFrames`), `Core/src/Engine.cpp` (eine Zeile in `prepare()`),
`Quest/src/main.cpp` (Asset, Suchpfad, `publish()` repariert), `Quest/build_apk.ps1` (Assets, Prüfung
des fertigen APK), `Quest/README.md`, `Tests/hosttest.cpp`, `Tests/vst3test.cpp`,
`Tests/selftest.cpp` (`testWaveTableQuality`), `Tests/CMakeLists.txt`.

**16.09.2026, Release-Weg und Build-Wächter**

Zwei Dinge, die nichts miteinander zu tun haben außer der Reihenfolge: ein Wächter gegen Artefakte,
die still kaputtgehen, und Phase 9, der Weg von der Arbeitskopie zum Installer.

**Der Anlass.** An diesem Tag fielen zwei Defekte auf, die kein Test hätte finden können, weil
nichts im Repository die Artefakte baut, in denen sie sitzen:

1. `Quest/src/main.cpp` kompilierte **seit Phase 5 nicht mehr**. Es benutzte noch
   `kMelodyMaxBlocks` und `MelodyPlan::blockParts`/`padGate`, die die Form-Grammatik entfernt hat.
   Die Quest-App ist ein eigenes CMake-Projekt, das `Quest/build_apk.ps1` baut; kein `ctest`-Ziel
   fasst diese Datei an. Der Bruch kam heraus, weil jemand von Hand ein APK gebaut hat.
2. Die Android-Konfiguration des Wurzelprojekts scheiterte, weil `PHOS_BUILD_PLUGIN` auf ON stand
   und JUCE seinen Wirtsrechner-Helfer `juceaide` nicht kreuzkompilieren kann. Die Meldung lautete
   „No CMAKE_C_COMPILER could be found" und schickt den Leser in die NDK-Toolchain statt zur
   Option. Repariert in master f0733e0 — und nichts hätte gemerkt, wenn der Default zurückkäme.

**Abgewogen.** Drei Bauarten, mit den auf dieser Maschine gemessenen Kosten:

| Ansatz | Kosten | Fängt | Entscheidung |
|---|---|---|---|
| Stub-Header (`<openxr/openxr.h>`, `<oboe/Oboe.h>`, `<android/*>`, EGL, GLES3, JNI nachbauen) und die Quest-Datei mit MSVC `/Zs` parsen | einmalig groß, dann dauerhaft | (1), aber nur solange die Stubs zur echten API passen | **verworfen**: OpenXR allein sind Tausende Deklarationen; jeder neue Aufruf in `main.cpp` bräuchte einen neuen Stub. Ein Wächter, den man reparieren muss, sobald sich das Bewachte ändert, wird abgeschaltet. |
| GitHub-Actions-Workflow | Minuten, bezahlt | (1) und (2) | **verworfen**: das Repository ist privat, die stehende Regel ist „CI, die Minuten kostet, bleibt aus, solange sie nicht verlangt ist". Dazu: NDK r27, OpenXR-Loader und Oboe (hier Junctions in Noctuary) und ein 500-MB-JUCE müssten je Job geladen werden. Das eine, was ein fremder Runner wirklich besser kann — merken, was nur auf einer sauberen Maschine bricht —, erledigt die Laufzeit-Abhängigkeitsprüfung der Paketprüfung. |
| NDK-eigener clang, nur Syntax (`-fsyntax-only`), plus eine Android-Konfiguration mit **Default**-Optionen | **4,95 s** allein, 14,8 s als `ctest`-Eintrag im ersten Lauf (von 636 s Gesamtsuite) | (1) und (2) | **gebaut** |

**Gebaut: `Tools/release/quest_guard.cmake`**, in der Suite als Test `questguard`
(`Tests/CMakeLists.txt`). Er konfiguriert das Wurzelprojekt für arm64-v8a/android-29 **ohne**
`-DPHOS_BUILD_PLUGIN` — der Default ist das, was zurückfallen kann, also wird der Default geprüft —,
liest `PHOS_BUILD_PLUGIN` aus dem entstandenen Cache zurück, und übersetzt danach jede Datei unter
`Quest/src/*.cpp` mit dem clang des NDK, nur Syntax, mit denselben Flags wie `Quest/CMakeLists.txt`
(inklusive `-ffp-contract=off`, weil ein unter anderen Regeln übersetzter Wächter etwas anderes
bewacht). Ohne NDK oder ohne `ThirdParty` endet er mit 77; `SKIP_RETURN_CODE` macht daraus ein
sichtbares „Skipped" mit Begründung statt eines roten Laufs, den man sich abgewöhnt zu lesen.

Er bindet nicht. Ein fehlendes Symbol in `libphosquest.so` fängt erst der Matrix-Lauf.

**Gegenprobe.** Beide Defekte in einer Kopie des Baums (im Scratch, damit keine fremde Datei
angefasst wird) absichtlich wieder eingebaut:

| Wieder eingebaut | Der Wächter sagt |
|---|---|
| `kMelodyMaxBlocks`, `plan.melody.blockParts[block]`, `plan.melody.padGate[block]` in `publish()` | die drei echten Fehler von damals (`use of undeclared identifier 'kMelodyMaxBlocks'`, `no member named 'blockParts'`, `no member named 'padGate'`) und danach „Quest/src/main.cpp does not compile for the headset" |
| `option(PHOS_BUILD_PLUGIN ... ON)` unbedingt, wie vor f0733e0 | JUCEs „No CMAKE_C_COMPILER could be found", und darunter die Übersetzung: „the root project no longer configures for Android with its default options … read it as JUCE and not as the NDK … PHOS_BUILD_PLUGIN has to default OFF when ANDROID is set" |

**Die teure Hälfte: `Tools/release/build_matrix.ps1`.** Sechs Konfigurationen, die dieses Repository
hat und von denen ein Alltagsbau eine anfasst: Desktop mit Plugin, Desktop ohne Plugin (die
Framework-Freiheit von `Core/` ist eine Behauptung, solange sie niemand so baut), Desktop ohne AVX2,
Desktop mit statischer Laufzeit, das Wurzelprojekt über die NDK-Toolchain und die Quest-App selbst.
Danach laufen die drei Vektor-Varianten. Am Ende eine Tabelle mit Zeiten und Ergebnis je Eintrag.

Gemessen, ein vollständiger Lauf mit `-Jobs 3` auf dem i9-12900K, leere Bauverzeichnisse:

| Eintrag | konfig. s | bauen s | Ergebnis |
|---|---|---|---|
| `desktop-plugin` | 35,5 | 112,7 | ok |
| `desktop-tools` (`-DPHOS_BUILD_PLUGIN=OFF`) | 6,7 | 13,1 | ok |
| `desktop-noavx2` (`-DPHOS_AVX2=OFF`) | 5,0 | 11,4 | ok |
| `desktop-static` (`-DPHOS_STATIC_RUNTIME=ON`) | 5,0 | 13,0 | ok |
| `android` (NDK, Wurzelprojekt, Default-Optionen) | 3,6 | 41,6 | ok |
| `quest` (`libphosquest.so`, gebunden) | 2,7 | 31,1 | ok |
| **gesamt** | **58,5** | **222,9** | **≈ 4,7 min** |

| Vektor-Variante | Pfad | Ergebnis |
|---|---|---|
| `phos_vectest` | avx2 | bitgleich zum skalaren Pfad |
| `phos_vectest_neon` | neon-shim | bitgleich zum skalaren Pfad |
| `phos_vectest_scalar` | scalar | bitgleich zum skalaren Pfad |

Vier von den sechs Einträgen sind billig, weil ohne Plugin nur Kern, Werkzeuge und Tests gebaut
werden; das teure ist JUCE. Fünf Minuten für alles, was das Repository behauptet zu haben.

**Phase 9: der Release-Weg.**

Version **1.0.0** — Abschnitt 12 nennt als Ergebnis von Phase 9 „v1.0", und es gibt noch keine
Veröffentlichung, hinter der man zurückbleiben könnte. Eine Quelle: die `project()`-Zeile in
`CMakeLists.txt`. Von dort kommt die Version des Plugins (über `juce_add_plugin VERSION`), das
`PHOS_VERSION` jeder Übersetzungseinheit (neu, `add_compile_definitions`), die Ausgabe von
`phos_render --version` (neu) und, weil `build_release.ps1` die Zeile zurückliest, der Name und die
Versionsressource des Installers. `check_package.ps1` liest alle vier wieder aus den gebauten
Dateien heraus — Noctuary hat eine 1.1.0 mit 1.0.0 im Executable ausgeliefert, weil zwei Stellen die
Version hielten und nur eine geändert wurde.

`Deploy/build_release.ps1` geht in einem Lauf: Wächter → konfigurieren und Release bauen (eigener
Baum `build-release`, `PHOS_STATIC_RUNTIME=ON`, `PHOS_AVX2=ON`) → die **ganze** `ctest`-Suite in
genau dieser Konfiguration → Referenz-Render → Handbuch aus dem eben gebauten Plugin → Quest-APK →
Staging → Paketprüfung → Inno Setup → portables Archiv → Manifest mit Größen und SHA-256. Jeder
fehlschlagende Schritt bricht ab; es gibt keinen Schalter, der aus einem Bau, der seine Tests nicht
bestanden hat, einen Installer macht.

**Nicht Intel.** Abschnitt 10 verlangt „Intel-Baum wie Noctuary". Für Noctuary war das richtig; für
Phosphene nicht. Das Offline-Render ist hier das Determinismus-Orakel — der Hosttest verlangt, dass
das Plugin Sample für Sample dasselbe erzeugt wie `phos_render`, und die drei Vektorbauten werden
bitgleich gegen den skalaren Pfad geprüft. Ein anderer Compiler setzt den Generator auf eine andere
Fließkomma-Bahn (in Noctuary gemessen: jedes Render weicht ab). Das wäre kein schnelleres Phosphene,
sondern ein anderes, und es würde das Orakel stumm schalten. MSVC ist, was ausgeliefert wird.

**Wo die Dateien hinkommen, und warum dorthin.** Der Kern öffnet `library.phoswt`, `melody.phosmdl`
und `bass.phosmdl` beim bloßen Namen und sucht in dieser Reihenfolge: der Name relativ zum
Arbeitsverzeichnis, das Verzeichnis, das ein Wirt angemeldet hat, dann `PHOS_SOURCE_DATA_DIR`, das
nur je einen Quellbaum benennt. Für ein installiertes Programm zählt nur das mittlere, und die
Artefakte melden verschiedene an: die Standalone und `phos_render` ihr eigenes Verzeichnis, das VST3
sein `Contents\Resources`. Also liegen alle drei Dateien zweimal im Paket (3,7 MB) — billiger als ein
Plugin, das still die sechs eingebauten Wavetables und den Markov-Komponisten spielt, weil ein Wirt
das Bundle ohne den Ordner daneben kopiert hat.

`phos_render` meldete bisher gar nichts an und fand seine Daten nur, wenn man es zufällig aus dem
richtigen Verzeichnis startete. `Tools/render/main.cpp` bekam dafür `installDataSearchPath()`
(`GetModuleFileNameA` bzw. `/proc/self/exe`), das Gegenstück zu dem, was das Plugin für sich tut.
Im Entwicklungsbau liegt neben der Binärdatei nichts, der Suchpfad greift ins Leere und die Suche
fällt wie bisher auf `PHOS_SOURCE_DATA_DIR` zurück: **das Default-Render ist bitgleich geblieben**
(64 Takte; WAV und MIDI, SHA-256 gegen einen aus HEAD gebauten `phos_render` verglichen —
WAV `0F56C811…`, 40.677.600 Bytes, beide Seiten gleich, MIDI ebenso). `ctest` bleibt grün: 7 von 7,
636 s im Alltagsbaum, 586 s im Release-Baum.

Messfalle dabei, die bekannte: `Copy-Item` übernimmt die Änderungszeit der Quelle, also war die
zurückkopierte `main.cpp` älter als ihr `.obj`, MSVC übersetzte sie nicht neu — und der erste
„bitgleich"-Vergleich verglich zweimal dieselbe Binärdatei und bewies gar nichts. Aufgefallen ist es
nur, weil `phos_render --version` in diesem Baum danach „unknown option" sagte. Die Zahlen oben
stammen aus dem Wiederholungslauf mit erzwungenem Zeitstempel; dass die beiden Binärdateien
wirklich verschieden sind, ist vorher per SHA-256 geprüft.

**Der Installer** (`Deploy/Phosphene.iss`, Inno Setup 7) hat, anders als Noctuarys, einen
`[InstallDelete]`-Abschnitt: dort löscht ein Update die vorigen Kopien der Datendateien, des
Handbuchs, des APK und des ganzen VST3-Bundles, bevor eine einzige Datei geschrieben wird. Noctuary
hatte nur `[UninstallDelete]`, also blieb alles, was seither umbenannt oder gestrichen wurde, für
immer liegen — eine veraltete `library.phoswt` neben einer neuen Binärdatei ist kein Fehler, den
jemand sieht, sondern ein falscher Klang. Gelöscht wird nur, was dieser Installer selbst anlegt;
`{app}` wird nie pauschal geleert, weil dort die `.phosset`-Dateien und Renders des Nutzers liegen
können.

**Die Paketprüfung** (`Tools/release/check_package.ps1`) ist das Gegenstück zur Regel des
Handbuch-Generators, der ein unvollständiges Handbuch nicht druckt. Sie öffnet das
Staging-Verzeichnis und beweist: **A** jede gebrauchte Datei ist da, groß genug und keine, die
niemand deklariert hat; **B** jede Datendatei ist byteweise die aus `Core/data`; **C** das APK trägt
`assets/library.phoswt` in der richtigen Länge; **D** die Version steht in `phos_render --version`
und in beiden Windows-Versionsressourcen gleich; **E** keine Binärdatei will noch eine VC++- oder
Intel-Laufzeit-DLL; **F** der gestagte Renderer, aus einem fremden Verzeichnis gestartet und mit
beiden neuronalen Modellen eingeschaltet (per Default stehen sie auf Markov), rendert die Referenz
bitgleich; **G** das Handbuch ist ein echtes PDF. Am Schluss das Manifest.

**Was F *nicht* beweist, und wie das herauskam.** F war als die eine Prüfung gedacht, die den ganzen
Datenpfad abdeckt: gestagter Renderer, fremdes Verzeichnis, Referenz bitgleich — also hat er die
gestagten Dateien gefunden. Stimmt nicht. Gegenprobe: mit gelöschtem `bass.phosmdl` im Staging ging
F durch, und mit einem umgekippten Byte in `melody.phosmdl` auch. Der Grund ist der dritte Schritt
der Kernsuche, `PHOS_SOURCE_DATA_DIR`, das `Core/CMakeLists.txt` als **absoluten Pfad in den
Quellbaum des Entwicklers** in jede Binärdatei backt: auf der Maschine, die gebaut hat, findet jede
Phosphene-Binärdatei `Core/data`, egal was neben ihr liegt. Kein Laufzeittest auf dieser Maschine
kann „hat die gestagte Kopie geöffnet" von „hat die Quellkopie geöffnet" unterscheiden.

F ist deshalb auf das zurechtgestutzt, was es wirklich zeigt — der gestagte Stand ist der, den
`ctest` bestanden hat, er startet ohne eigenes Arbeitsverzeichnis, und der Bau mit statischer
Laufzeit rendert genau wie der geprüfte. Dass die Datendateien da, an beiden angemeldeten Orten und
byteidentisch sind, leisten A und B — und **die fallen durch**: A benannte die fehlende
`bass.phosmdl`, B das gekippte Byte.

Dazu kam **F2**: sucht den Quellpfad in den gestagten Binärdateien. Ergebnis auf diesem Lauf: alle
drei (`Phosphene.exe`, `phos_render.exe`, das VST3-Modul) tragen
`G:/Tools/VRAudio/PhospheneWork/phos-release/Core/data` in sich. Als Warnung gemeldet, nicht als
Fehler, weil die Reparatur in `Core/CMakeLists.txt` gehört (siehe Loch 3). Auch F2 hatte beim ersten
Versuch einen Fehlalarm in der harmlosen Richtung: es suchte nur die Backslash-Schreibweise, CMake
schreibt das Define aber mit Schrägstrichen, und die Prüfung meldete zufrieden „nichts eingebettet".
Eine Prüfung, die nicht durchfallen kann, ist schlimmer als keine — beide Schreibweisen stehen jetzt
ausdrücklich im Skript.

**Gemessen, ein vollständiger Lauf auf dem i9-12900K (drei andere Agenten arbeiteten gleichzeitig):**

| Schritt | s |
|---|---|
| Wächter (Quest-Quellen + Android-Konfiguration) | 5 |
| konfigurieren + Release bauen (statische Laufzeit, AVX2), `--parallel 2` | 153 |
| `ctest`, ganze Suite, 7 von 7 bestanden | 586 |
| Referenz-Render (64 Takte, beide Modelle) | 15,2 |
| Handbuch (Screenshots aus dem Plugin, HTML und PDF; 616 Parameter, 12 Tabs) | 32,2 |
| Quest-APK (NDK, aapt2, zipalign, apksigner) | 49,9 |
| Staging | 0,1 |
| Paketprüfung | 16,5 |
| portables Archiv | 2,2 |
| Inno Setup | 7,7 |
| **gesamt** | **≈ 868 (14,5 min)** |

| Artefakt | Bytes |
|---|---|
| `Phosphene-1.0.0-Setup.exe` | 17.147.152 (16,4 MB) |
| `Phosphene-1.0.0-portable.zip` | 21.170.176 (20,2 MB) |
| `MANIFEST-1.0.0.txt`, `SHA256SUMS.txt` | 1.809 / 275 |
| Staging gesamt | 16 Dateien, 34.498.630 |
| darin `Phosphene.exe` | 9.953.280 |
| `Phosphene.vst3\Contents\x86_64-win\Phosphene.vst3` | 10.177.024 |
| `phos_render.exe` | 792.576 |
| `PhospheneQuest.apk` | 4.555.729 (davon `assets/library.phoswt` 750.264, komprimiert 662.195) |
| `Phosphene-Manual.pdf` | 1.355.155 |
| `library.phoswt` / `melody.phosmdl` / `bass.phosmdl` (je zweimal) | 750.264 / 1.519.612 / 1.520.476 |

**Zwei Löcher, offen und benannt.**

1. **Weder das Plugin noch die Quest-App rufen jemals `setModelSearchPath()`.**
   `Core/CMakeLists.txt` behauptet in seinem Kommentar das Gegenteil („after the host's own resource
   directory (setModelSearchPath(), which the plugin and the Quest app set)"), und
   `Plugin/PluginProcessor.cpp` setzt nur `setWaveTableSearchPath()`. Ein installiertes Plugin findet
   `melody.phosmdl` und `bass.phosmdl` deshalb nicht, `sharedMelodyModel()`/`sharedBassModel()`
   fallen auf Markov und die Mustermengen zurück und schreiben je eine Zeile auf stderr, die in einer
   DAW niemand sieht — die ganze Phase-8-Arbeit wäre im Produkt still abgeschaltet. Der Installer
   legt die Dateien bereits an beide Stellen, an denen gesucht würde; auf der Windows-Seite fehlt
   nur eine Zeile neben dem vorhandenen `setWaveTableSearchPath()`-Aufruf in
   `Plugin/PluginProcessor.cpp` (`installWaveTableSearchPath()`). Auf der Quest ist es mehr: dort
   müssten die beiden `.phosmdl` erst als Assets ins APK (`Quest/build_apk.ps1`), dann von
   `prepareWaveTables()` mit ausgepackt und der Pfad in `App::init()` angemeldet werden — 3 MB, die
   ohne den C++-Teil totes Gewicht wären, deshalb sind sie hier nicht schon ins APK gelegt worden.
   `Plugin/**` und `Quest/src/**` gehören anderen Agenten, deshalb hier nur notiert.
2. **`PHOS_SOURCE_DATA_DIR` steht in jeder ausgelieferten Binärdatei.**
   `Core/CMakeLists.txt` definiert es als absoluten Pfad in den Quellbaum, und der Kern benutzt es
   als letzten Suchschritt. Zwei Folgen: der Installer verrät das Verzeichnislayout der Baumaschine,
   und — schlimmer — eine fehlende Datendatei bleibt auf genau dieser Maschine unsichtbar, weil der
   Quellbaum einspringt. Vorschlag: `PHOS_SOURCE_DATA_DIR` nur definieren, wenn nicht für die
   Auslieferung gebaut wird (etwa an `PHOS_STATIC_RUNTIME` oder eine eigene Option `PHOS_SHIPPING`
   gehängt). `Core/**` gehört einem anderen Agenten; die Paketprüfung meldet den Fund bis dahin als
   Warnung (F2).
3. **Keine Code-Signatur.** Auf dieser Maschine liegt kein Zertifikat. `-SignWith` ist gebaut und
   ungetestet; jeder hier gebaute Installer ist unsigniert, Windows nennt den Herausgeber unbekannt,
   und was stattdessen prüfbar ist, sind die SHA-256-Zeilen des Manifests. Auch nicht geprüft: der
   Installer wurde nicht ausgeführt (kein zweiter Rechner, und eine echte Installation hier würde
   das VST3-Verzeichnis der Maschine anfassen), das APK nicht auf einer Quest (kein Headset
   angeschlossen) und pluginval nicht (nicht auf der Maschine; `phos_vst3test` deckt den Teil ab,
   der im Repository leben kann).

**16.09.2026, Kaleidoscope-Kopplung (8.3).** Phosphene sagt Kaleidoscope, wo die Takte, die
Sektionen und die Drops sind, statt es raten zu lassen. Fünf OSC-Nachrichten über UDP,
voreingestellt aus, auf beiden Seiten.

| Prüfstein | Ergebnis |
|---|---|
| Selbsttest `testCues` | 20 von 20; Bytelayout gegen die OSC-1.0-Spezifikation von Hand hergeleitet |
| `Tools/cue_check.py` (ctest `cuecheck`) | 15 von 15 über 491 echte Datagramme |
| Kaleidoscope `Kaleidoscope.exe -q` | 32 von 32 |
| Sektionsgrenzen der Partitur = empfangene Cues | 9 von 9, Takt für Takt, plus Energie, Tonart, Drop |
| Ende-zu-Ende-Verzögerung gegen den *gehörten* Augenblick, localhost | Median **0,44 ms**, Mittel 0,54 ms, p95 1,56 ms, Maximum 1,65 ms (42 Cues) |
| Standardrender `--bars 32 --seed 1` gegen master f0733e0 | bitgleich (SHA-256 `01FC8810…`) |
| ctest gesamt | 7 von 7 (Selbsttest, cuecheck, drei Vektorpfade, Hosttest, VST3-Test) |
| Quest, `libphosquest.so` (arm64, NDK 27) | baut ohne Warnung |

### Wo der Cue entsteht — die eigentliche Entscheidung

Drei Stellen der Kette kämen in Frage, zwei davon sind falsch:

- **Der Komponist** kennt jede Sektionsgrenze lange vorher — und genau das ist das Problem. Der
  `Conductor` hält die Ereignisringe der Engine rund acht Takte vor der Spielposition gefüllt; bei
  145 BPM sind das **dreizehn Sekunden**. Ein Cue von dort erreichte den Visualizer eine Viertelminute
  vor dem Drop.
- **Die Partitur** hat überhaupt keine Uhr. Ein `SectionMark` ist eine Taktzahl, kein Augenblick.
- **Die Spielposition** ist die einzige Stelle, an der ein Beat ein Augenblick ist:
  `Engine::beatPosition()` sagt, welchen Beat das nächste zu rendernde Sample trägt. `CueTap::scan`
  bekommt deshalb den Beat-Bereich, den ein Block abdeckt — den Wert vor und nach
  `Engine::process()` — und gibt die Grenzen aus, die hineinfallen. Im Plugin steht der Aufruf
  unmittelbar neben `emitMidi()`, aus demselben Grund.

Das ist noch nicht, was der *Hörer* hört, und zwar aus zwei Gründen, die beide in dieselbe Richtung
zeigen und beide bekannte Zahlen sind statt Schätzungen: der True-Peak-Limiter der Engine schaut
voraus (`Engine::latencySamples()`, 83 Samples = 1,7 ms bei 48 kHz), und der Block, der jetzt
gerendert wird, ist nicht der Block, der jetzt gespielt wird. Beides ist ein **Vorlauf**: der Cue
wäre zu früh, nie zu spät. Der Tap stempelt darum jeden Cue mit dem Augenblick, an dem der Hörer ihn
hört — `jetzt + (Ausgabepuffer + Limiter-Lookahead + Sample-Offset im Block) / Abtastrate` — und der
Senderthread hält ihn bis dahin zurück. Der portable Teil des Ausgabepuffers ist der Block selbst
(beim üblichen doppelt gepufferten Strom ist der Block, der gefüllt wird, der nach dem, der gespielt
wird); was die Treiberwarteschlange darüber hinaus tiefer ist, trimmt `cue.lead_ms` — die eine Zahl,
die einem Plugin niemand sagt. Zu früh ist bewusst der hinzunehmende Fehler: ein Bild lässt sich
planen, aber nicht zurücknehmen.

**Warten mit einer Uhr, die es auch kann.** `std::this_thread::sleep_for` ist unter Windows `Sleep()`,
dessen Granularität der Systemtimer ist — 15,6 ms, sofern kein anderer Prozess ihn hochgesetzt hat,
also eine Viertelnote bei 145 BPM. Der Senderthread nimmt deshalb einen
`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`-Timer; die globale Timerauflösung anzuheben wäre ein
systemweiter Eingriff und nicht die Sache eines Generators. Die gemessenen 0,44 ms Median sind die
ganze Kette: Stempel, Warteschlange, Warten, Kodierer, `sendto()`, Loopback, `recvfrom()`.

**Der Audiothread sendet nichts.** Der Komponist schreibt `CueMark`s Takte im Voraus in einen
lock-freien Ring (dieselbe `EventRing` wie Noten und Kontrollereignisse); der Tap zieht sie, wenn die
Spielposition sie erreicht, und schiebt Cues wartefrei in die Warteschlange des Senders; dessen
eigener Thread ruft `sendto()`. Kein Socket, keine Allokation, kein Lock auf dem Audiothread.

### Die Leitung

`/phos/beat i`, `/phos/bar i`, `/phos/section s f`, `/phos/key s`, `/phos/drop` — OSC 1.0 von Hand
kodiert (Adressmuster, Typkennungszeichenkette, Vier-Byte-Ausrichtung, Big-Endian), rund vierzig
Zeilen statt einer Abhängigkeit für fünf Nachrichtentypen. Ein Drop begleitet jede Core-Sektion und
sonst nichts; die Tonart reist einmal je Track. Der Beweis, dass beide Enden dasselbe Format meinen,
sind **drei unabhängige Dekodierer über dieselben Bytes**: das Bytelayout im Selbsttest von Hand aus
der Spezifikation hergeleitet, ein Python-Dekodierer in `Tools/cue_check.py`, und Kaleidoscopes
eigener Empfänger, dem die aufgezeichneten 491 Datagramme über `KALEIDO_CUE_DECODE` vorgelegt werden.
Alle drei lesen dasselbe, Nachricht für Nachricht.

Das Orakel für den Inhalt ist die Partitur selbst: `Composer::sections()` ist, was der Komponist
geschrieben hat, und die empfangenen Sektions-Cues müssen genau diese Liste sein — mit dem Cut am
Kopf seines Breakdowns und dem Pre-Drop-Break im letzten Takt seines Buildups, auf denselben Takten,
in derselben Reihenfolge. Die Taktzahl reist nicht mit der Sektion mit; sie wird gelesen, wie ein
Visualizer sie liest, nämlich aus dem zuletzt eingetroffenen `/phos/bar`.

### Ausfall ist still

UDP auf einen Port ohne Zuhörer ist nirgends ein Fehler: das Datagramm verfällt, der Generator
spielt weiter (32 Datagramme an Port 9, alle gesendet, nichts gemeldet). Ein nicht auflösbarer Host
lässt `CueSender::start()` false zurückgeben, mehr passiert nicht. Umgekehrt: schweigt der Sender
zwei Sekunden, übernimmt in Kaleidoscope wieder die Audioanalyse.

### Die andere Seite

In Kaleidoscope (eigener Zweig `phos-cues`, eigenes Repo) sitzt ein `CueReceiver` neben dem
Web-Remote: ein schlichtes `QObject` mit `QUdpSocket` auf dem Qt-Hauptthread, eingeschaltet über
`cuePort` in der ini, vorbelegt mit 0 = aus. Die Cues füttern **genau die zwei steigenden Flanken**,
die die Audioanalyse dort seit jeher füttert (`Tick::sectionCount`, `Tick::dropCount`) plus die
Downbeat-Flagge. Damit greifen alle Sicherungen des Schedulers unverändert — ein Cue mitten in einer
Überblendung kann sie nicht umlenken, ein Drop verkürzt die laufende Blende statt eine zweite zu
bestellen, und Kamera, Zoom und Rotation bleiben von Audio unberührt, weil kein Code angefasst wurde,
der sie bewegt. Eine Ausnahme ist nötig und eingebaut: `sectionKnown` wird bei Cue-Betrieb hart auf
false gezogen, weil das Song-Struktur-Gedächtnis dort an einer recycelten Acht-Slot-LRU-Id hängt und
stattdessen den Sektions*typ* einzusetzen hieße, jeder Drop eines Sets teilte sich einen Speicherplatz
— genau das „immer dieselben Szenen", das dieser Index dort schon einmal erzeugt hat.

### Gegenprobe (Mutationsrunde)

Sechs Fehler eingebaut, jeder von einer Prüfung gefangen, jeder zurückgenommen, `git diff` danach
sauber in beiden Repos:

| Fehler | gefangen von |
|---|---|
| OSC-Zeichenketten nicht mehr auf vier Byte aufgefüllt | 10 Prüfungen des Bytelayouts |
| Argumente little-endian statt Netzwerk-Byte-Reihenfolge | „int32 big-endian", „/phos/bar MSB zuerst", „float32 big-endian" |
| Pre-Drop-Break nicht mehr gemeldet | „jede Sektionsgrenze der Partitur, Takt für Takt" (12 gegen 14) |
| Fälligkeit ohne den Vorlauf gestempelt | „ein Cue ist fällig, wenn sein Sample gehört wird" (−7,000 ms) |
| Empfänger prüft die Null-Auffüllung nicht mehr | „Auffüllung, die keine Nullen sind, wird abgelehnt" |
| Aus-Zweig schreibt *ein* Feld des Ticks | „ein Scheduler-Tick kommt byteweise so heraus, wie er hineinging" |

Nebenbefund der Runde: die Prüfung „32 Datagramme an einen Port ohne Zuhörer" schlief feste 60 ms
und war damit eine Wackelkandidatin; sie wartet jetzt auf den Zähler des Senders.

*Dateien.* Neu: `Core/include/phos/Cue.h` (header-only: Kodierer, Tap, Sender),
`Tests/cuedemo.cpp` (`phos_cuedemo`), `Tools/cue_check.py`. Geändert:
`Core/include/phos/Params.h` und `Core/src/Params.cpp` (Modul `cue` angehängt: `cue.send`,
`cue.port`, `cue.beats`, `cue.lead_ms` — kein bestehender Parameter hat seine Id, seinen Vorgabewert
oder seinen Platz verändert), `Core/CMakeLists.txt` (`ws2_32` unter Windows),
`Plugin/PluginProcessor.h/.cpp`, `Quest/src/main.cpp` (der alte Ein-Nachricht-pro-Frame-Sender ist
durch den gemeinsamen ersetzt), `Tests/selftest.cpp` (`testCues`, plus die Modulzählung in
`testParams`), `Tests/CMakeLists.txt`, dieser Block.

**16.09.2026, Rollenerkennung: der Engpass des Korpus.** Von 28 121 begangenen `.mid`-Dateien
erreichten 6 623 Linien die Modelle, und die vorige Runde hatte den Grund benannt: nicht die Menge des
Materials, sondern der Detektor. `role_of` liest die Rolle aus dem Pfad; steht sie dort nicht, gibt es
sie nicht. Diese Runde hat zuerst gemessen, was im Korpus überhaupt zu holen ist, dann einen besseren
Klassifikator gebaut, und am Ende **eine Sache ausgeliefert und eine abgelehnt** — beide mit Zahl.

*Zuerst die Handstichprobe, sonst weiß niemand, ob etwas besser wurde.* Grundgesamtheit sind alle
19 307 notenführenden **Spuren** der vier Quellen. Acht Schichten (vier Quellen × „ein Name nennt eine
Klasse" / „keiner nennt eine"), aus jeder **50 Spuren** ohne Zurücklegen, gezogen aus einer festen
Mischung der Spur-IDs unter Seed 20260916: **400 Spuren**, bewusst nicht proportional, weil das
VORTEX-Bündel sonst die ganze Stichprobe schlucken würde (13 997 von 19 307). Jede Aussage über den
Gesamtkorpus wird mit der Schichtgröße zurückgewichtet. Die 200 **benannten** tragen das Wort des
Herstellers — unabhängig von jedem Merkmal, das der Klassifikator sieht, denn der bekommt nie einen
Namen — und sie sind die ehrliche Messung. Die 200 **unbenannten** sind von Hand etikettiert: eine vor
dem Etikettieren aufgeschriebene Regel (`labelsample.LABEL_RULE`) auf eine Beweiskarte je Spur
(Register, Polyphonie, Notenlänge, Kick-Raster, Intervalle, Rang im eigenen Ordner, Klassen der
benannten Geschwister), dann **alle 200 Karten gelesen**; das änderte die Regel einmal (eine vier Takte
gehaltene Note war „Rhythmus" statt Fläche) und ergab **14 Einzelkorrekturen**, jede mit Begründung in
`labels.json`. Diese Hälfte ist *nicht* unabhängig vom Klassifikator und wird darum nicht als Präzision
gelesen, sondern als das, was nur sie liefern kann: die **Klassenzusammensetzung der Grundgesamtheit,
auf die der Klassifikator losgelassen wird**. `labels.json` enthält Hashes, Schicht, Etikett und die
16 Zahlen, aus denen es gelesen wurde — keinen Pfad, keinen Dateinamen, keine Note.

*Was die Stichprobe sofort zeigte.* Das Unbenannte ist überwiegend **nichts, was ein melodisches
Modell brauchen kann**: psy 32 von 50 Ein-Ton-Rhythmusfiguren („Step and Hold"), star 23 von 50
Akkordbetten, trance 11 Flächen und 8 Bässe. Melodisch sind je nach Quelle 14 bis 38 %.

*Der eigentliche Fund liegt nicht im Klassifikator, sondern in den Namen, die niemand gelesen hatte.*
Ein Drittel des VORTEX-Bündels sind keine Loops, sondern **ganze Arrangements** — und die benennen ihre
Spuren („Bass", „Lead", „Melody", „Pads"). 9 530 der 19 307 Spuren tragen ein Instrumentenwort im
**MIDI-Spurnamen**, gegen 5 671 im Pfad; wo beides da ist (3 685 Spuren), stimmen sie zu **98,2 %**
überein. Für die 2 652 mehrspurigen Dateien ist die Spur außerdem eine *Korrektur*: `read_midi`
verschmolz alle Spuren und `top_line` nahm die höchste Note je Schritt, was bei einem Arrangement eine
Linie ergibt, die zwischen Fläche und Lead hin- und herspringt und die nie jemand gespielt hat.
`build_corpus.read_midi_tracks` liest jetzt spurweise, `read_midi` ist die Zusammenfassung davon —
die ausgelieferten Stufe-A-Tabellen bleiben **byteidentisch**.

*Zwei Fallen dabei, beide gemessen statt geglaubt.* (1) **Teilzeichenketten in freiem Text**, dieselbe
Falle wie „psy" in *Gypsy* eine Runde zuvor, eine Ebene tiefer: `daft_punk__around_the_world` wurde ein
Lead (das „ld" von *world*), `kyau_vs_albert__kiksu` und `ultrabeat` wurden Schlagzeug. Die Regel prüft
jetzt **ganze Token** (Präfix oder exaktes Wort), und der Dateiname einer **mehrspurigen** Datei zählt
gar nicht mehr als Rollenbeleg — er ist ein Songtitel, kein Instrument. Danach stieg die Übereinstimmung
Pfad/Spurname von 90,9 % auf 98,2 %. (2) **Der General-MIDI-Programmwechsel ist kein Etikett.** 7 226
Spuren wählen ein Programm; es stimmt mit dem Spurnamen nur zu **73,8 %** überein und mit dem Pfad zu
77,5 % — das Niveau einer DAW-Voreinstellung. Er geht als schwaches Merkmal ein, nie als Label.

*Der Klassifikator, und was den Unterschied macht.* Spurweise, 32 Inhaltsmerkmale (darunter die drei
gegen die benannte Schwachstelle Acid-gegen-Bass: Anteil der Anschläge auf den Kick-Sechzehnteln, die
Intervallverteilung, und ob die Linie eine Tonleiter ausspielt), plus Registerrang in Datei und Ordner,
plus die Klassen der **benannten Geschwister** desselben Ordners und derselben Datei, plus das Programm.
Histogramm-Gradient-Boosting, fünf Faltungen **nach Ordner** gruppiert. Gegen die Herstellernamen:
**melodisch 89,0 % Präzision bei 80,7 % Trefferquote** ohne Abstinenz, **94,3 % / 67,9 %** ab Schwelle
0,70 — die vorige Runde stand bei 78,4 % für {acid, lead, arp} und 92,0 % / 71,1 % für `lead` allein.
Auf der benannten Hälfte der Handstichprobe, die kein Fit je gesehen hat: **97,4 % Präzision, 94,9 %
Trefferquote**. Die Ablation sagt, **woher das kommt**, und die Antwort ist nicht die, die man erwartet:

| gemessen (fünf Faltungen, nach Ordner) | melodisch P | melodisch R | acid P |
|---|---|---|---|
| alle Blöcke | 85,1 % | 86,4 % | 94,6 % |
| ohne Inhaltsmerkmale | 62,8 % | 86,2 % | 87,0 % |
| **ohne Ordner-Geschwister** | 82,9 % | 85,0 % | **28,2 %** |
| ohne Registerrang | 85,8 % | 85,0 % | 95,4 % |
| ohne Programm | 84,8 % | 86,9 % | 95,0 % |
| **logistische Regression statt Bäumen** | 84,9 % | 84,6 % | **48,6 %** |

**Acid gegen Bass wird nicht vom Inhalt getrennt, sondern vom Ordner.** Ohne die Klassen der benannten
Nachbarspuren fällt die Acid-Präzision von 94,6 % auf **28,2 %**. Auf der melodischen Klasse insgesamt
kostet der Ordner nur zwei Punkte — er entscheidet nicht, *ob* eine Spur melodisch ist, sondern
*welche* melodische Rolle sie hat. Die zweite Hälfte derselben Antwort ist die Modellklasse, und zwar
ebenfalls nur auf `acid`: die logistische Regression der vorigen Runde erreicht mit **denselben**
Merkmalen 84,9 % melodisch, aber nur 48,6 % Acid-Präzision — die Entscheidung ist eine Schwelle
("Median-Notenlänge mindestens vier Sechzehntel"), die ein Baum ausspricht und eine Gerade annähert.

*Drei Ideen gemessen und verworfen.* **Kalibrierung**: isotone Regression auf inneren Faltungen ist das
Lehrbuchmittel für eine Abstinenzschwelle und verliert hier auf der ganzen Kurve — bei 93,5 %
Präzision erreichen die rohen Ensemble-Scores 70,3 % Trefferquote, die kalibrierten 57,4 %; der innere
Drei-Faltungs-Split ist nicht gruppenbewusst und schrumpft zu stark. **Prior-Anpassung** je Quelle
(Saerens, Latinne, Decaestecker, Neural Computation 14(1), 2002): auf der unbenannten Stichprobe
50,0 % gegen 50,0 % bei Schwelle 0,70 — kein messbarer Unterschied. **Sequenzmodell** statt
Kennzahlen (`roleseq.py`, gestapelte dilatierte Faltungen über die Notenfolge): **74,6 % / 82,3 %**
gegen 82,5 % / 84,7 % der Inhaltskennzahlen und 85,1 % / 86,4 % aller Blöcke. Die Reihenfolge trägt
etwas, und zwar genau dort, wo man es vermutet — auf `acid` verdoppelt sie die Präzision der
Kennzahlen (54,1 % gegen 23,6 %) —, bleibt aber weit unter dem, was der Ordner ohnehin liefert.

*Wo er weiter versagt, und das ist die Zahl, die zählt.* Auf der **unbenannten** Hälfte der
Handstichprobe fällt die melodische Präzision von 94,3 % auf **50,0 %** (Trefferquote 44,9 %, bestenfalls
56,8 % bei Schwelle 0,80). Die Fehlzulassungen sind acht Rhythmusfiguren, sieben Flächen, vier
„unsicher" und drei Bässe. Je Quelle: psy 46,7 %, trance 60,0 %, superpsy 47,4 %, **star 0 zugelassen**.
Der Grund ist keine Schwäche des Fits, sondern die Grundgesamtheit: der Klassifikator wird auf Namen
angepasst und auf Namenlose losgelassen, und die sind anderes Material. **Ohne die Handstichprobe wäre
ein 94-%-Klassifikator ausgeliefert worden, der in Wahrheit 50 % trifft.**

*Ein echter Defekt, unterwegs gefunden.* Beim Aufbau der einen Merkmalsmatrix: `fit` rechnete die
Geschwister-Merkmale über die 11 316 Trainingsspuren, `predict` über alle 19 307. Im flachen
Star-Samples-Ordner verschob das `sib_dir_bass` von 0,48682 auf 0,48707 — und ein Baum, der genau an
dieser Konstanten getrennt hatte, kippte **alle 2 581 Spuren des Ordners auf einmal**. Präzision
in-sample 30 % mit Fehlanpassung, 100 % ohne. Die Merkmale werden jetzt einmal über den ganzen Korpus
gebaut und nur noch indiziert (`rolemodel.corpus_matrix`); der Docstring von `build_matrix` warnt.

*Die Entscheidung, nach demselben Maß wie vorher und nur nach dem.* Testsatz unverändert: die
Loop-Gruppen-Aufteilung der drei Psy-Packs, 102 Linien, 5 700 Noten. Neues Material nur auf der
Trainingsseite. `confidence.py` liest seit dieser Runde auch eine **exportierte `.phosmdl`**, damit der
gepaarte Bootstrap gegen die *installierte Datei* läuft und nicht gegen eine Nachbildung — die
ausgelieferte `melody.phosmdl` misst 1,2141 auf diesem Testsatz, die 1,2143 des Plans nach int8.

| Aufbau | Trainingslinien | Validierung | Test |
|---|---|---|---|
| F (bisher: Packs dateiweise + VORTEX) | 2 142 | 1,1704 | 1,1900 |
| **G F + Spurnamen, spurweise extrahiert** | **3 536** | **1,1269** | **1,1532** |
| H G + die Zulassungen des Klassifikators | 4 256 | 1,1583 | 1,1699 |

**G gegen das ausgelieferte Modell: +0,0609 nats, 95 % KI [0,0143, 0,1310], P(Differenz ≤ 0) =
0,0008**, besser auf 55 von 102 Linien. G gegen F, also der Wert der Spurnamen allein: **+0,0368 nats,
KI [0,0037, 0,0737], P = 0,0152**. Fünf Seeds von G bei festgehaltener Aufteilung: 1,1502 / 1,1492 /
1,1532 / 1,1951 / 1,1761, Mittel 1,1648, Standardabweichung 0,0202; ausgeliefert wird wie immer der
Seed mit der besten **Validierung** (Test 1,1532), nicht der mit dem besten Test.

**H, also die Zulassungen des Klassifikators, sind abgelehnt.** G gegen H: **+0,0167 nats, KI
[−0,0119, +0,0487], P = 0,139** — das Intervall enthält die Null, und auf der Validierung, die
entscheidet, sind sie klar schlechter (1,1269 gegen 1,1583). Damit steht dieselbe Entscheidung wie in
der vorigen Runde, aber jetzt mit einem gemessenen Grund statt einer Vermutung: nicht weil der
Klassifikator schwach wäre (97,4 % gegen Herstellernamen), sondern weil die Grundgesamtheit ohne Namen
zu drei Vierteln aus Rhythmusfiguren, Flächen und Bässen besteht.

*Memorisierung, gegen die 3 536 Linien, auf denen wirklich trainiert wurde:* **0,00 % exakte
Taktkopien** gegen **8,75 %** der echten Held-out-Loops, Achtnoten-Fenster im Abstand 0 **9,15 % gegen
20,58 %**. Positivkontrolle (absichtlich überangepasst): **69,54 %** Taktkopien, 78,85 % im Abstand 0 —
das Maß schlägt aus. int8 kostet +0,04 % (1,1532 → 1,1536). `export.py --check-pair
Core/data/melody.phosmdl Core/data/melody.phosmdl.ref.txt` meldet Differenz **0,000e+00**. ctest 4/4.

*Nebenbefund, sauber repariert:* `read_midi` warf `TypeError` auf 26 Dateien des Star-Samples-Super-Packs
(„DMS … Single Patches" — 192-Byte-Klangdumps: eine SysEx-Nachricht und dann Füllbytes). Die vorige
Runde hatte das im eigenen Code abgefangen; jetzt steht es im Parser. **Laufender Status ist legales
MIDI** (MIDI 1.0 Detailed Specification 2.1.2) und bleibt es; was nicht dekodierbar ist, ist ein
Datenbyte, über dem nie ein Statusbyte stand, und das beendet jetzt die *Spur* statt die ganze Datei.
Zwei weitere Regeln derselben Stelle: eine SysEx-Nachricht **löscht** den laufenden Status (sonst erbt
der Müll dahinter den Status davor und die Datei bekommt Noten, die niemand geschrieben hat), und ein
Ereignis, das über das Chunk-Ende hinausragt, beendet die Spur, statt die Kopfbytes des nächsten MTrk
als Notendaten zu lesen. `Tools/corpus/test_build_corpus.py` deckt sechs Fälle ab; gegen den alten
Parser fallen drei davon durch (`--module` zeigt auf eine Kopie der alten Datei), gegen den neuen
keiner. **Die ausgelieferten Stufe-A-Tabellen ändern sich dadurch nicht** — `CorpusTables.cpp` neu
erzeugt ist byteidentisch, `Core/src/CorpusTables.cpp` und `Core/include/phos/Corpus.h` bleiben
unangetastet.

*Berührte Dateien.* Neu: `Tools/corpus/roledetect.py`, `Tools/corpus/rolemodel.py`,
`Tools/corpus/labelsample.py`, `Tools/corpus/labels.json`, `Tools/corpus/roleseq.py`,
`Tools/corpus/test_build_corpus.py`. Geändert: `Tools/corpus/build_corpus.py` (`read_midi_tracks`,
`read_midi` als dessen Zusammenfassung), `Tools/train/dataset.py` (`extract_notes`, `collect_tracks`,
vier neue `--extra`-Quellen), `Tools/train/confidence.py` (`.phosmdl` als Seite des Bootstraps),
`Tools/train/rolecheck.py` (nur Docstring: abgelöst), `Core/data/melody.phosmdl` und `.ref.txt`,
`Tools/train/model/phos_pitch_tf.phosmdl` und `.ref.txt`.

**16.09.2026, offen: ein einmaliger Selbsttest-Fehler ohne Spur.** Beim Merge der Cues-Runde
scheiterte `phos_selftest` unter `ctest` **einmal**, nach 146 s statt der üblichen 300 bis 440 --
also ein Abbruch, kein Zeitüberschreiten. Danach: 12 Läufe von `PHOS_ONLY=testCues`, 4 volle
Läufe und ein `ctest -R selftest` alle grün, 257 von 257. **Nicht reproduziert in 17 Versuchen.**

Welche Prüfung fehlschlug, ist **unbekannt**, und das ist der eigentliche Mangel: `ctest` schreibt
`build/Testing/Temporary/LastTest.log` und überschreibt es beim nächsten Lauf. Der erste Griff
nach einem sporadischen Fehler ist, die Suite noch einmal laufen zu lassen -- und genau der löscht
den Beweis. `Deploy/build_release.ps1` legt jetzt bei einem Fehlschlag eine Kopie mit Zeitstempel ab.

Verdächtig bleibt die Zeitabhängigkeit der neuen Cue-Prüfungen (Socket, hochauflösender Timer,
Wartezeit gegen `now + (block + latency + offset)/sr`) unter fünf gleichzeitig arbeitenden Agenten;
der Cues-Agent hatte eine feste 60-ms-Wartezeit in einer Prüfung bereits durch das Zählen der
gesendeten Datagramme ersetzt. Bewiesen ist das nicht. Wer den Fehler wiedersieht, hat jetzt das
Protokoll.

**16.09.2026, Rauschgrenze und Stereobreite.** Zwei Kalibriergrößen, die die Mischungsrunde offen
gelassen hat. Die erste war eine Ursache: das Programm hörte oben nicht auf. Die zweite war der
nächste Punkt auf ihrer Liste und teilt sich mit der ersten das Messwerkzeug.

### 1. Das Rauschen lief flach bis Nyquist

*Der Befund, zuerst nachgemessen.* Acht Minuten Seed 7, Standardwerte, Anteile an der Gesamtleistung
und — weil ein schmaleres Band schon durch seine Breite leiser liest — dieselben Zahlen je Hertz:

| Band | Anteil | je Hertz |
|---|---|---|
| 16–20 kHz | −27,42 dB | −63,44 dB |
| 20–22 kHz | −32,24 | −65,25 |
| 22–24 kHz | −32,52 | **−65,53** |

Die letzten beiden Dichten liegen **0,28 dB** auseinander: das Spektrum endet nicht, es läuft flach
in die Nyquist-Frequenz.

*Welche Quellen.* Jeder Erzeuger allein, acht Minuten, flache Masterkette (`--solo` ist eine
Stummschaltung, aber bitgenau — das hat die Mischungsrunde mit 5 084 690 Samples belegt, für ein
Spektrum taugt sie also). Leistung im Band 22–24 kHz auf einer gemeinsamen Skala:

| Teil | 22–24 kHz | | Teil | 22–24 kHz |
|---|---|---|---|---|
| **Percussion** | **39,81 dB** | | Acid | −15,10 |
| ganze Mischung (flach) | 39,82 | | Bass | −34,94 |
| Lead | 13,13 | | Pad | −36,01 |
| SFX | 1,82 | | Kick | −64,62 |

Die Percussion **ist** das Band: sie trifft die ganze Mischung auf 0,01 dB. Innerhalb des Kits, jede
Lane einzeln (vier Minuten, die anderen elf stumm):

| Lane | Rolle | Hauptfilter | 22–24 kHz | je Hertz 16–20 / 20–22 / 22–24 |
|---|---|---|---|---|
| perc6 | Snare | **Hochpass 250 Hz** | **39,89 dB** | −45,64 / −45,63 / **−45,55** |
| perc4 | Crash | **Hochpass 3000 Hz** | 27,22 | −43,22 / −43,44 / **−42,87** |
| perc3 | Ride | Bandpass 5200 Hz | 12,59 | −58,49 / −65,15 / −73,68 |
| perc11 | Zap | Tiefpass 9 kHz, Drive 0,3 | 11,98 | −64,63 / −63,20 / −66,69 |
| perc5 | Clap | Bandpass 1400 Hz | 8,27 | −59,72 / −66,19 / −74,77 |
| perc1 | Closed Hat | Tiefpass 12 kHz | 2,74 | −54,41 / −67,08 / −82,11 |
| perc8 | Shaker | Tiefpass 11 kHz | 2,56 | −54,59 / −66,77 / −81,67 |
| perc2 | Open Hat | Tiefpass 12 kHz | −4,65 | −54,78 / −67,38 / −82,47 |
| perc7/9/10/12 | Rim, Tom, Conga, Blip | Band-/Tiefpass | −31 … −48 | fallend |

Das ist **dieselbe Fehlerfamilie, die Phase 9 gefunden hat, in den zwei Lanes, die sie nicht
angefasst hat**: ein Hochpass auf einer spektral flachen Quelle hat keine obere Grenze. Die drei
Lanes, die damals einen Bandpass bekamen (perc1, perc2, perc8), sind genau die drei, deren Spektrum
steil endet — 28 dB Abfall über die drei Bänder. Die Snare (Hochpass bei 250 Hz auf 80 % weißem
Rauschen) trägt allein 13 dB mehr als alles andere zusammen.

*Was es kostet — die Zurechnung selbst nachgemessen, und sie fällt anders aus als vermutet.* Der
True-Peak-Schätzer der Engine ist eine bandbegrenzte Rekonstruktion bis 0,45 fs = 21,6 kHz
(`Dynamics.h`, Nachtrag von heute früh). Auf demselben Render:

| Messung | Wert |
|---|---|
| `Engine::meter()` / `--report` | **−0,98 dBTP** |
| derselbe Bank in Python nachgebaut | −0,983 (die Umsetzung **ist** der Entwurf) |
| exakt (FFT, 16-fach, mittlere Hälfte je Block) | **−0,075 dBTP** |
| derselbe Render hart auf 21,6 kHz geschnitten, exakt | −0,182 |
| derselbe Schnitt, vom Bank gemessen | −0,218 |

Innerhalb seines Bandes ist der Schätzer auf **0,036 dB** genau; über dem ganzen Render irrt er um
**0,905 dB**. Der ganze Fehler liegt also über 0,45 fs, und der Render lag **0,925 dB über der
Decke**, während die Anzeige −0,98 meldete. Damit ist die Vermutung der Koordination bestätigt, aber
mit einer Korrektur: es sind nicht „0,9 dB Inhalt über 22 kHz", die zur Spitze dazukommen — dieser
Inhalt hebt die echte Spitze nur um 0,107 dB (−0,182 gegen −0,075). Die übrigen 0,8 dB sind, dass der
Limiter mit einem Schätzer arbeitet, der dieses Material nicht rekonstruieren kann, und die Decke
deshalb gar nicht erst hält. **Eine Decke in dBTP ist eine Aussage über die analoge Welle; sie ist
nur wahr, wenn das Programm in dem Band liegt, das der Schätzer abdeckt.**

Dazu die beiden anderen Kosten, beide unbestreitbar und keine Messung wert: über 20 kHz hört niemand
etwas (Ashihara, *Hearing thresholds for pure tones above 16 kHz*, JASA 122(3), 2007 — die Schwelle
steigt über 20 kHz über 90 dB SPL), und jeder Abtastratenwandler nach 44,1 kHz legt seinen
Übergangsbereich auf etwa 20 bis 22,05 kHz, faltet also alles darüber als Alias nach unten.

*Die Reparatur, und warum sie nicht bei den Lanes sitzt.* Der naheliegende Schritt wäre, die zwei
Hochpässe umzudrehen, wie Phase 9 es bei den Hats getan hat. Drei Zahlen sprechen dagegen:

1. Phase 9 hat genau das gemessen und **verworfen**: „Snare Tiefpass 9 kHz, Crash Bandpass 6 kHz"
   verschlechterte den Terzabstand von 0,98 auf 1,19 dB rms.
2. Ein Lane-Filter ist zweipolig. Ein Tiefpass bei 16 kHz liegt bei 20 kHz erst 13,5 dB tiefer; um
   das Band wirklich zu beenden, müsste er so tief stehen, dass er das kalibrierte Luftband trifft.
3. **Standardwerte können eine obere Grenze gar nicht versprechen.** Der Komponist legt auf jede
   Lane je Track einen Versatz auf `cutoff` von ±0,10 im normierten Raum — bei einem Bereich von
   100 Hz bis 18 kHz sind das ±68 % — und bis zu +0,30 auf `drive` (`Rhythm.cpp`,
   `percRecipeOffsets`). Und die Percussion ist nicht die einzige Quelle: Lead und SFX reichen
   ebenfalls über 20 kHz, und der Soft-Clipper der Masterkette macht dort eigene Harmonische.

Die Grenze gehört deshalb dorthin, wo sie gilt, egal was die Erzeuger tun: **`BandLimit` in `Dsp.h`,
ein Butterworth-Tiefpass vierter Ordnung bei 18 kHz aus zwei trapezförmigen SVF-Abschnitten
(Dämpfungen 2 cos π/8 und 2 cos 3π/8), in `Engine::renderSegment` zwischen Clipper und Limiter.**
Nach dem Clipper, weil er die letzte Stufe ist, die neue Obertöne macht; vor dem Limiter, weil der
Limiter nur eine Decke halten kann, die er sieht.

*Warum 18 kHz und vierte Ordnung.* Der trapezförmige Tiefpass trägt eine doppelte Nullstelle bei
Nyquist, vier Pole bringen also vier Nullstellen mit, und das letzte Band vor Nyquist bricht
zusammen. Gemessen am Kit-Solo, Kosten gegen Nutzen:

| Entwurf | 16–20 kHz | 20–22 kHz | 22–24 kHz | Terzband 16 kHz | Terzkurve 2–16 kHz |
|---|---|---|---|---|---|
| ohne | −14,12 | −17,80 | −17,82 | ±0 | 0,000 dB rms |
| 2-polig 18 kHz | −16,79 | −29,76 | −44,50 | −1,11 | 0,355 |
| 4-polig 16 kHz | −22,08 | −50,85 | −78,39 | −2,87 | 0,910 |
| **4-polig 18 kHz** | **−16,71** | **−39,48** | **−66,96** | **−0,51** | **0,162** |
| 4-polig 19 kHz | −15,23 | −32,87 | −60,08 | −0,13 | 0,040 |
| 6-polig 18 kHz | −18,66 | −49,13 | −88,42 | −1,54 | 0,490 |

19 kHz kostet weniger, lässt aber 20–22 kHz nur 15 dB fallen — das ist kein Ende. Sechste Ordnung
kauft 20 dB, die niemand ausgeben kann, für das Dreifache im Durchlassbereich. 18 kHz vierter
Ordnung ist der Punkt, an dem das Programm vor 20 kHz aufhört — der Bank misst am Impuls **−15,27 dB
bei 20 kHz und −39,81 bei 22 kHz** gegen die geschlossene Form des Butterworth-Prototyps unter der
bilinearen Abbildung (−15,26 und −39,82; schlechteste Abweichung über fünf Frequenzen 0,017 dB) —
und die kalibrierte Terzkurve sich dabei um 0,16 dB rms bewegt.

### 2. Stereobreite

*Die Referenzen zuerst, richtig vermessen.* `Tools/ref_width.py`, 40 Aufnahmen mit dem Albumtag
„Psytrance Collection", je **vier Fenster zu 45 s** aus den mittleren 80 % des Tracks. Gemessen wird
nicht eine Zahl, sondern drei, aus einem Durchgang: Seite-zu-Mitte je Band, die Korrelation zwischen
den Kanälen, die sie erzeugt (für zwei Kanäle ist Seite/Mitte = (1−ρ)/(1+ρ), wenn beide gleich laut
sind — das ist keine Messung, das folgt aus M = (L+R)/2 und S = (L−R)/2), und zwei Kurzfenster-Maße
über 85 ms, die sagen **woher** die Breite kommt.

| Band | Median | Q1 | Q3 | min | max | ρ | Pegeldifferenz rms | \|ρ\| über 85 ms |
|---|---|---|---|---|---|---|---|---|
| tief 40–140 Hz | −24,73 | −28,26 | −18,17 | −37,72 | −3,72 | +0,995 | 1,46 dB | 0,997 |
| low-mid | −9,80 | −12,71 | −7,42 | −22,80 | −1,27 | +0,800 | 2,01 | 0,838 |
| Mitten | −5,89 | −8,29 | −4,35 | −20,06 | −1,00 | +0,592 | 2,15 | 0,588 |
| Präsenz | −5,83 | −8,12 | −4,80 | −21,65 | −2,29 | +0,590 | 1,86 | 0,589 |
| Luft 6–16 kHz | −8,45 | −10,57 | −6,24 | −24,33 | −0,53 | +0,751 | 1,74 | 0,728 |

Zwei Dinge, die diese Messung geklärt hat, bevor irgendetwas geändert wurde.

**Breite braucht viele Fenster.** *Innerhalb* einer fertigen Aufnahme schwankt die Fünf-Band-Zahl
zwischen ihren eigenen 45-Sekunden-Fenstern im Median um **6,26 dB** (schlechtestes Terzband 9,68 dB,
schlimmster Fall 28,6). Die *Bandbalance* derselben Aufnahmen schwankt über das Fenster um höchstens
0,6 dB. Breite ist also eine viel unruhigere Größe als Balance, und jede Zahl hier ist ein Median
über Fenster und dann über Aufnahmen.

**Die Aufnahmen sind breit bei gleichem Pegel.** Die Pegeldifferenz zwischen den Kanälen liegt über
85-ms-Fenster in jedem Band bei 1,5 bis 2,2 dB rms, und die Kurzfenster-Korrelation ist die lange.
Ihre Breite ist also **Dekorrelation zweier gleich lauter Kanäle**, nicht hart nach außen gelegtes
Material — was Stereohall, Unisono-Verstimmung und doppelt eingespielte Stimmen erzeugen, und was ein
Panoramaregler allein nicht erzeugt.

*Unsere Messung, je Erzeuger* (Seed 7, acht Minuten, Solo mit flacher Masterkette):

| Teil | low-mid | Mitten | Präsenz | Luft | ρ (Luft) |
|---|---|---|---|---|---|
| **Percussion** | −18,38 | −19,07 | −19,39 | **−18,06** | **+0,98** |
| Acid | −18,39 | −14,33 | −15,64 | −25,45 | +0,99 |
| Lead | −7,07 | −6,56 | −6,54 | −7,09 | +0,67 |
| Pad | −5,13 | −5,18 | −4,88 | −5,22 | +0,54 |
| SFX | −5,14 | −7,35 | −7,59 | −7,85 | +0,72 |
| ganze Mischung | −11,87 | −5,89 | −7,00 | −12,52 | +0,90 |
| **Referenz-Median** | **−9,80** | **−5,89** | **−5,83** | **−8,45** | **+0,75** |

*Der Negativbefund, und er widerspricht dem Hörbericht der Mischungsrunde.* „Die Stereobreite liegt
in jedem Band unter der Referenz" stimmt so nicht. **Mitten treffen den Median exakt** (−5,89 gegen
−5,89), Präsenz und low-mid liegen innerhalb des unteren Quartils der Aufnahmen. Zu schmal ist
**allein das Luftband**, und zwar um 4,1 dB. Und das Luftband ist das Kit: in der Solo-Tabelle von
Phase 9 gehören ihm 60 % davon, 95 % in einem Track ohne Lead. Lead und Pad sind bereits so breit wie
die Referenzen oder breiter.

*Warum das Kit schmal war.* Die Lanes stehen im Panorama, aber die beiden **lautesten** des oberen
Endes standen fast in der Mitte: Closed Hat (+5 dB) bei 0,15, Open Hat (+2 dB) bei −0,10. Bei
Konstantleistungs-Panorama ist der Beitrag einer Lane zur Seite (1 − cos(pπ/2)) und zur Mitte
(1 + cos(pπ/2)), gewichtet mit ihrer Leistung — zwei zentrale laute Lanes halten die Summe schmal,
egal wie weit die leisen außen stehen.

*Änderung 1: die Positionen des Kits* (`Params.cpp`, nur Standardwerte, keine Codezeile):

| Lane | vorher | nachher | | Lane | vorher | nachher |
|---|---|---|---|---|---|---|
| perc1 Closed Hat | +0,15 | **+0,45** | | perc8 Shaker | +0,25 | **+0,55** |
| perc2 Open Hat | −0,10 | **−0,40** | | perc9 Tom | −0,20 | **−0,35** |
| perc3 Ride | +0,35 | **+0,60** | | perc10 Conga | +0,30 | **+0,50** |
| perc4 Crash | −0,30 | **−0,55** | | perc11 Zap | +0,40 | **+0,60** |
| perc7 Rim | −0,25 | **−0,45** | | perc12 Blip | −0,40 | **−0,60** |

Clap und Snare bleiben in der Mitte: der Backbeat ist das eine, was eine Psytrance-Mischung dort
verankert, und das Präsenzband, in dem sie leben, lag ohnehin innerhalb der Quartile.

**Was die Bandbalance dabei nicht tut, und zwar beweisbar nicht.** Das Panorama ist
konstantleistungs-normiert (`Perc.cpp`): cos²θ + sin²θ = 1, die Summe der beiden Kanalleistungen ist
also von der Position unabhängig. Alle Zahlen der Mischungsrunde summieren Kanalleistungen — sie
**können** sich nicht bewegen, und über acht Seeds gemessen tun sie es auch nicht.

*Änderung 2: die Hall-Rückwege waren nicht dasselbe Instrument* (`Reverb.cpp`). Das FDN gibt seine
beiden Ausgänge aus verschiedenen Verzögerungsleitungen, dekorreliert also von selbst — aber der
Leitungssatz ist **nach Länge geordnet** (29,7 bis 89,0 ms), und die Aufteilung „erste Hälfte links,
zweite Hälfte rechts" gab dem linken Kanal alle vier kurzen und dem rechten alle vier langen. Die
Länge einer Leitung ist aber, wo ihre Kammzähne stehen. Gemessen mit Mono-Rauschen am Eingang, über
die Terzbänder 500 Hz bis 8 kHz:

| | Breite | ρ | Kanalbalance rms | schlechtestes Band |
|---|---|---|---|---|
| Room, vorher | −1,18 dB | +0,136 | **1,45 dB** | 2,05 dB |
| Room, nachher | −1,26 | +0,144 | **0,65** | 1,25 |
| Hall, vorher | −1,56 | +0,179 | **1,36** | 2,55 |
| Hall, nachher | −1,75 | +0,199 | **0,53** | 1,42 |

Jeder Kanal nimmt jetzt zwei kurze und zwei lange Leitungen ({29,7; 46,6; 58,6; 68,4} gegen {31,4;
39,3; 55,1; 89,0}). Die Rückwege teilen sich weiterhin **keine** Leitung, sind also genauso
dekorreliert wie vorher — sie haben nur dieselbe Farbe bekommen.

*Ergebnis.* Das Kit allein (Selbsttest, eigenes Sechzehntel-Muster) geht im Luftband von **−17,64 auf
−8,66 dB** und seine Korrelation von **+0,977 auf +0,794** — der Referenz-Median ist −8,45 bei
+0,751. In der ganzen Mischung über drei Seeds zu 96 Takten: low-mid −11,04 → −10,98, Mitten
−5,84 → −5,82, Präsenz −7,55 → −7,07, **Luft −13,77 → −9,98**, tief −47,84 → −46,59 (die Tiefenregel
steht). Die Zahlen über acht Achtminüter stehen in der Tabelle unten.

*Dekorrelation oder echtes Material?* **Echtes Material.** Es wurde kein Allpass, keine Verzögerung
und kein Mitte-Seite-Trick gebaut. Was die Korrelation von 0,977 auf 0,794 gebracht hat, ist, dass
zwölf Lanes mit **eigenen Rauschgeneratoren** jetzt weit genug auseinanderstehen, dass die beiden
Kanäle in jedem Augenblick andere Anschläge tragen: die Hi-Hat ist rechts, der Shaker links, und das
sind zwei verschiedene Ereignisse, keine zwei Kopien eines. Die Mono-Probe bestätigt es von der
anderen Seite: gegen das Kickband gerechnet kostet die Mono-Summe die Präsenz **0,78 dB** und die
Luft **0,42 dB**, wo dieselbe Messung die Referenzaufnahmen 1,2 dB Präsenz kostet. Was sich in der
Summe aufhebt, war nie da.

### Ergebnis über acht Seeds

Acht Seeds (1, 2, 3, 5, 7, 11, 31, 2026), je acht Minuten, ganze Renders, Standardwerte,
Kanalleistungen summiert. **Ein einzelner Render ist keine Kalibriergröße** — das steht seit der
Mischungsrunde im Plan —, deshalb sind alle Zahlen Mediane, und `Tools/metrics.py --median` rechnet
sie jetzt selbst aus.

| Größe | vorher | nachher | Referenz |
|---|---|---|---|
| Bandbalance low-mid | −6,55 | −6,55 | −6,88 |
| Bandbalance Mitten | −5,64 | −5,61 | −8,03 |
| **Bandbalance Präsenz** | **−8,27** | **−8,34** | −8,58 |
| **Bandbalance Luft** | **−13,37** | **−13,55** | −12,59 |
| Terzkurve, Abstand zum Median | 2,11 | **2,10** dB rms | — |
| Lautheit integriert | −9,05 | **−9,10** LUFS | Ziel −9 |
| LRA | 5,5 | 5,3 LU | — |
| **16–20 kHz (Anteil / je Hz)** | −27,36 / −63,38 | **−29,76 / −65,78** | — |
| **20–22 kHz** | −32,07 / −65,08 | **−53,38 / −86,39** | — |
| **22–24 kHz** | −32,56 / −65,57 | **−81,24 / −114,25** | — |
| **True Peak, exakt gemessen** | **−0,044 dBTP** | **−0,970** | Decke −1,00 |
| derselbe, schlechtester Seed | **+0,096** | **−0,957** | |
| Breite tief | −44,78 | −43,82 | −24,73 |
| Breite low-mid | −11,97 | −11,59 | −9,80 |
| Breite Mitten | −6,35 | −6,24 | −5,89 |
| Breite Präsenz | −7,87 | −7,27 | −5,83 |
| **Breite Luft** | **−12,35** | **−9,18** | **−8,45** |
| **Korrelation Luft** | **+0,896** | **+0,811** | **+0,751** |
| Mono-Summe kostet Präsenz | +0,66 | +0,75 | 1,2 |
| Mono-Summe kostet Luft | +0,25 | +0,50 | — |

Und dieselben acht Renders noch einmal **mit demselben Verfahren wie die Aufnahmen** gemessen (vier
Fenster zu 45 s je Render, `Tools/ref_width.py --files`), damit die Verteilungen vergleichbar sind:

| Band | vorher (Q1 / Q3) | nachher (Q1 / Q3) | Referenz (Q1 / Q3) |
|---|---|---|---|
| tief | −43,44 | −42,68 | −24,73 (−28,26 / −18,17) |
| low-mid | −11,31 (−13,24 / −10,80) | −11,33 | −9,80 (−12,71 / −7,42) |
| Mitten | −6,63 (−7,47 / −6,51) | −6,51 | −5,89 (−8,29 / −4,35) |
| Präsenz | −7,94 (−9,06 / −6,94) | −7,32 (−8,47 / −6,64) | −5,83 (−8,12 / −4,80) |
| **Luft** | **−12,42** (−13,41 / −10,48) | **−8,88** (−9,43 / −8,32) | **−8,45** (−10,57 / −6,24) |
| ρ Luft | +0,893 | +0,817 | +0,751 |
| Pegeldifferenz Luft, 85 ms | 2,02 dB | **3,99** | 1,74 |
| Fensterschwankung, fünf Bänder | 6,95 dB | 6,76 | 6,26 |

Das Luftband liegt jetzt auf dem Referenz-Median, und die Streuung über acht Renders ist enger als
die der Aufnahmen. **Ehrlich dazugesagt:** die Pegeldifferenz über 85-ms-Fenster steigt im Luftband
von 2,0 auf 4,0 dB, wo die Aufnahmen bei 1,7 liegen. Unsere Breite dort kommt also **mehr aus
Position** als ihre; ihre kommt zusätzlich aus Stereohall und doppelt eingespielten Stimmen, die ein
Kit aus zwölf Einzellanes nicht hat. Vier Dezibel rms über 85 ms sind keine harte Seitenlage — die
Fensterschwankung eines Achtminüters (6,8 dB) liegt jetzt auf der einer fertigen Aufnahme (6,3) —,
aber es ist der Unterschied, und die nächste Runde wüsste damit, wo sie ansetzt.

Die drei Dichten je Hertz gehen von **0,49 dB Abstand** (−63,38 / −65,08 / −65,57 — flach) auf
**48,5 dB** (−65,78 / −86,39 / −114,25). Das Programm endet. Der exakt gemessene True Peak geht von
**0,96 dB über der Decke auf 0,03 dB darunter**, im schlechtesten der acht Seeds von 1,10 dB darüber
auf 0,04 darunter: **0,93 dB Headroom**, die vorher niemand sehen konnte.

*Und die Kalibrierung der Mischungsrunde steht.* Präsenz bewegt sich um **0,07 dB**, Luft um
**0,18 dB** (die Bandgrenze bei 18 kHz kostet das Terzband 16 kHz 0,3 dB, und das liegt im Luftband),
der Abstand der ganzen Terzkurve zum Referenz-Median um **0,01 dB rms**, die Lautheit um 0,05 LUFS.
Dass die Panorama-Änderung daran nichts tut, ist keine Messung, sondern Algebra (Konstantleistung,
siehe oben) — dass die Bandgrenze fast nichts tut, ist gemessen.

*Abgelehnt, mit Zahlen:*
- **Ein Mitte-Seite-Verbreiterer im Master.** Er addiert auf *jedes* Band exakt 20 log g. Die
  fehlenden 3,8 dB im Luftband würden die Mitten von −5,84 auf −2,0 und die Präsenz von −7,55 auf
  −3,75 treiben — beides über das obere Quartil der Aufnahmen (−4,35 und −4,80) und die Mitten über
  ihr Maximum (−1,00). Die Lücke ist bandweise, ein flacher Verbreiterer ist es nicht.
- **Das Kit über den Room-Send dekorrelieren.** `fx.high_cut` steht bei 9 kHz, der Room erreicht das
  Luftband also gar nicht. Rechnet man nach, wie viel vollständig dekorrelierter Rückweg nötig wäre,
  um von Seite/Mitte 0,0158 auf 0,1413 zu kommen — (0,0158 + q/2)/(1 + q/2) = 0,1413 —, kommt
  q = 0,29 heraus: knapp ein Drittel der Luftbandleistung des Kits als Hall. Das ist die Art von
  Breite, die den Crest und die Einsatzdichte frisst, die Phase 9 kalibriert hat. Nicht gebaut.
- **Clap und Snare ins Panorama.** Siehe oben: das Präsenzband liegt bereits innerhalb der Quartile,
  und der Backbeat gehört in die Mitte.
- **Die Acid verbreitern** (−18,39 im low-mid, +0,97 Korrelation). Sie ist die schmalste melodische
  Stimme, aber `Acid.cpp` ist nicht die Datei dieser Runde, und low-mid liegt mit −10,98 innerhalb
  des unteren Quartils der Aufnahmen. Offen für die nächste Runde.

### Das Messwerkzeug, und die Fallen

`Tools/metrics.py` hat sieben neue Maße bekommen, alle mit einer Prüfung in `--selftest` gegen ein
Signal, dessen Antwort feststeht (jetzt **35 Prüfungen**): die drei Bänder über dem Luftband als
Anteil **und je Hertz** (weißes Rauschen: −7,78 / −10,78 / −10,79 dB, drei gleiche Dichten auf
0,018 dB); Auto- und Kreuzspektrum in einem Durchgang; Breite und Korrelation je Band gegen ein Paar
mit **vorher festgelegtem ρ** — für gleiche Kanalleistungen ist Seite/Mitte = (1−ρ)/(1+ρ) exakt, also
ein Sollwert ohne Messung (ρ = 0 / 0,5 / 0,9 / −0,5 auf 0,15 dB und 0,02 getroffen); die beiden
Kurzfenster-Maße gegen zwei Signale, die **beide 0,00 dB breit** sind und die nur sie auseinander
halten (dekorreliertes Rauschen: Pegeldifferenz 0,38 dB, |ρ| 0,03 — abwechselnd hart gelegtes
Material: 293 dB, |ρ| 0,00); die Mono-Kosten (mittiges Signal 0,00 dB, hart gelegtes **auch** 0,00 —
das Maß sieht Auslöschung, nicht das Panoramagesetz —, gegenphasiges 185 dB); und der exakte True
Peak gegen drei Signale mit geschlossener Lösung (Sinus bei 0,25 fs mit 45 Grad: +0,000; Impuls:
+0,001; Sinus bei 0,49 fs: +0,010 dB).

`Tools/ref_width.py` ist neu und macht dasselbe mit den Aufnahmen: Auswahl über den Albumtag wie
`ref_style.py`, vier Fenster je Track, nur Statistik in `Tools/ref_width.json`.

*Die Fallen dieses Projekts, einzeln geprüft.* **Leistung statt Betrag** — jede Summe hier ist eine
Leistungssumme, unverändert aus der Mischungsrunde. **Blackman-Harris, ±4 Bins** — die Bandsummen im
Selbsttest gehen über Bänder von 2 kHz und mehr, bei 0,73 Hz je Bin also 2700 Bins; die einzige
schmale Messung, der Frequenzgang von `BandLimit`, wird an der **Impulsantwort ohne Fenster**
genommen, hat also gar keine Fensterfunktion. **MP3-Kante bei 18,8 kHz** — genau deshalb konnten die
Referenzen zur oberen Grenze nichts sagen; das Argument kommt aus Hörbarkeit, Schätzerband und
Umtastung, und die Breitenmessung endet bei 16 kHz. **Mono-Summe** — keine Messung dieser Runde
summiert Kanäle, die Mono-Kosten sind ein eigenes, geprüftes Maß. **Tonart F#** — alle acht Seeds
laufen mit dem Standardschlüssel, ihre Obertöne fallen also in dieselben Terzbänder; deshalb steht
oben nur die *Differenz* der Terzkurve (2,11 → 2,10 dB rms), in der sich der Zackenkamm heraushebt,
und die Fünf-Band-Summen sind davon ohnehin unberührt (gemessen in der Mischungsrunde). Breite ist
keine harmonische Größe. **`--solo` ist eine Stummschaltung** — hier nur für *Spektren* benutzt, wie
die Mischungsrunde es freigegeben hat; keine Kostenzahl stammt daraus.

### Prüfungen

`testBandLimit` (vier) und `testStereoWidth` (sechs), alle gegen unabhängig hergeleitete Zahlen, und
**acht davon erst gegen den unveränderten Stand fallen gesehen**:

| Prüfung | gegen den alten Stand | jetzt |
|---|---|---|
| `BandLimit` ist ein Butterworth vierter Ordnung bei 18 kHz | — (die Klasse gab es nicht) | 0,017 dB schlechteste Abweichung von der geschlossenen Form |
| der Limiter hält die Decke nur auf einem Programm, das in 0,45 fs endet | — | endet: −0,963; mit Rauschen darüber: **−0,269**; dasselbe hinter der Bandgrenze: −0,977 |
| das Spektrum endet (je Hertz) | **4,81 dB** Abfall | **49,69 dB** |
| die Anzeige sagt die Wahrheit, die Decke hält | Anzeige −0,994, exakt **−0,621** | −1,000 / **−0,984** |
| die Sends kommen als dasselbe Instrument zurück (Room) | Kanalbalance **1,45 dB rms** | **0,65** |
| dasselbe (Hall) | **1,36** | **0,53** |
| das Kit ist über 6 kHz so breit wie die Aufnahmen | **−17,64 dB** (ρ +0,977) | **−8,66** (ρ +0,794) |
| die Mischung erreicht in jedem Band über 140 Hz das untere Quartil | Luft **−13,77** gegen −10,57 | **−9,98** |
| die Tiefenregel überlebt die Verbreiterung | (galt) −47,84 | −46,59 |
| die Mono-Summe kostet nicht mehr als bei den Aufnahmen | (galt) +0,70 / +0,18 | +0,78 / +0,42 gegen 1,2 |

Die beiden letzten sind Wächter, keine Beweise: sie galten vorher auch. Sie stehen da, weil die
Verbreiterung genau sie hätte brechen können.

### Gegenprobe (Mutationsrunde)

Sieben Fehler einzeln eingebaut, sechs von ihrer Prüfung gefunden, `git diff` danach sauber:

| Mutation | Wer merkt es |
|---|---|
| Eckfrequenz 22 statt 18 kHz | Butterworth-Prüfung (8,45 dB daneben; −7,55 statt −15,26 bei 20 kHz) |
| die Bandgrenze gerechnet und weggeworfen | „das Spektrum endet" (4,52 dB Abfall) **und** die True-Peak-Prüfung (Anzeige −0,994, exakt −0,622) |
| die Bandgrenze **hinter** den Limiter statt davor | True-Peak-Prüfung: **+0,039 dBTP**, über der Decke — ein Filter hinter dem Limiter hebt die Spitze wieder an, die er gerade festgehalten hat |
| nur einer der beiden Butterworth-Abschnitte | Butterworth-Prüfung (19,3 dB daneben) |
| Hall zurück auf „kurze Leitungen links, lange rechts" | **beide** Send-Prüfungen (1,45 und 1,36 dB rms Kanalbalance) |
| die zwei lautesten Hat-Lanes zurück Richtung Mitte | Kit-Breite (−10,90 statt −8,66) **und** die Mischung (Luft −11,49, unter dem Quartil) |
| die beiden Butterworth-Abschnitte in der anderen Reihenfolge | **niemand — und zu Recht:** zwei LTI-Abschnitte in Kaskade kommutieren, die Übertragungsfunktion ist dieselbe. |

*Kosten.* Vier SVF-Schritte je Sample auf zwei Kanälen. Zwei Minuten Audio, zweimal gemessen, mit
der Bandgrenze **10,49 / 10,62 s**, ohne sie **10,65 / 11,07 s** — der Unterschied liegt unter dem
Rauschen der Messung. Keine zusätzliche Latenz (ein IIR-Tiefpass, kein Vorhören), keine Allokation,
keine neuen Parameter.

Gesamt: **247 Selbsttest-Prüfungen** (zehn neue) in **402 s**, Vektortests 16 von 16 in AVX2,
NEON-Shim und skalar, bitgleich in allen drei Pfaden. Die beiden neuen Abschnitte kosten rund
**75 s**: der 24-Takt-Render mit der exakten Spitzenmessung 14 s, und die drei 96-Takt-Renders der
Breitenprüfung 60 — dieselben drei, die `testMixBalance` schon fährt, aber eine Breitenmessung
braucht Kreuzspektren, die der Bandakkumulator dieser Runde nicht führt. Der Preis dafür, dass die
Breite gemessen statt behauptet wird.

*Dateien.* Geändert: `Core/include/phos/Dsp.h` (`BandLimit`), `Core/include/phos/Engine.h` und
`Core/src/Engine.cpp` (die Stufe zwischen Clipper und Limiter), `Core/include/phos/Reverb.h` und
`Core/src/Reverb.cpp` (`kLeft`), `Core/src/Params.cpp` (zehn Panorama-Standardwerte),
`Tests/selftest.cpp` (`testBandLimit`, `testStereoWidth`, `StereoBandAccumulator`, `exactPeak`; dazu
**eine Schranke in `testReverb`** von −30 auf −29,5 dB, siehe oben), `Tools/metrics.py`. Neu:
`Tools/ref_width.py`, `Tools/ref_width.json`.

**16.09.2026, Modaler Wechsel, gemessene Spannungskurve, motivische Operatoren**

Vier Punkte aus einem Review des Nutzers, in dieser Reihenfolge. Selbsttest danach **249 Prüfungen
(237 + 12), 0 Fehler**.

*1. Modaler Wechsel über dem Grundton-Pedal.* `Section` trug bisher keine Tonleiter; innerhalb eines
Tracks stand der Modus über die ganze Länge fest, und `StyleProfile::scaleWeight` gewichtete nur die
Tonartenreise **zwischen** Tracks. In Goa und Full-On bleibt der Bass aber auf dem Grundton, während
die melodische Schicht den Modus wechselt — Dorisch im Groove, Phrygisch im Hauptzug,
phrygisch-dominant (die erhöhte Terz, die Hijaz-Farbe) im Höhepunkt. Das Pedal ist das, was den
Modus wandern lässt, ohne das Fundament zu verlieren.

Jede Sektion trägt jetzt ihren eigenen `scale`. Er wird aus dem **Sektions-Seed** gezogen (eine
Sektion ist eine sperrbare Einheit, PLAN 6.8), aus `StyleProfile::interchangeWeight` und
`interchangeChance` — Goa greift zu phrygisch-dominant und doppelt-harmonisch, Progressive bleibt bei
Dorisch und Äolisch — und die **Energie der Sektion** entscheidet, wie weit sie greifen darf: das
Gewicht eines Modus wird mit `exp(kInterchangeEnergy * (energy − 0,7) * Farbtöne(Modus))`
multipliziert. Gemessen über 400 Formen je Stil: der Hijaz-Anteil der geborgten Modi liegt bei Goa
bei **0,541 in Sektionen mit hoher Energie gegen 0,228 bei niedriger**; Progressive verlässt Dorisch,
Äolisch und Phrygisch in keiner von 3471 Sektionen. Die Farbtonzahl eines Modus wird gerechnet, nicht
tabelliert (`scaleColourTones`, Harmony.h), und ergibt genau die Rangfolge der Literatur: Äolisch und
Dorisch 0, Phrygisch und harmonisch Moll 1, phrygisch-dominant 2, doppelt-harmonisch 3.

Das Material einer geborgten Tonleiter entsteht, indem dieselben Macher mit **demselben Seed** und
einem anderen Modus noch einmal laufen: Rhythmus, Akzente und Slides kommen identisch heraus, nur die
Tonhöhen sind umgefärbt. Das ist genau „eine Änderung der erlaubten Menge, kein neues Subsystem".
`FormPlan::scaleMask` sagt `makeMelodyPlan`, welche Modi gebraucht werden; ohne Wechsel steht dort
nur der Track-Modus, und dann passiert nichts Neues. Intro und Outro behalten immer den Track-Modus:
sie sind die DJ-freundlichen Enden, über die das nächste Intro geschrieben wird (PLAN 6.7).

**Der Bass bewegt sich nicht.** Bassgrundton, Register, Gate-Grenze, die gelernte Bass-Phrase und die
Akkordverschiebung von `compose.bass_follows_chords` lesen alle den Modus des **Tracks**, nie den
einer Sektion. Nachgewiesen, nicht behauptet: derselbe Seed mit `compose.modal_interchange=On` und
`=Off`, sechs Tracks à 128 Takte, Goa, `bass_follows_chords=On` — **6698 Kick- und Bassnoten, davon 0
verschoben**, auch die jeweils erste Bassnote eines Tracks (an der die Kick-Phase hängt) auf
derselben Zeit und Tonhöhe, während 3428 melodische Tonhöhen sich unterscheiden. Ohne den zweiten
Teil wäre der erste wertlos.

*Nutzt das Modell den neuen Ton?* Das neuronale Modell ist auf Rolle, Stil, Takte, Step, Takt, Lücke
und Index konditioniert — **nicht auf den Modus**; nur die Maske erzwingt ihn. Gemessen über zwölf
Tracks: der Anteil der Noten, die auf einer vom geborgten Modus **neu** zugelassenen Tonhöhenklasse
liegen, gegen die Nullhypothese „jede zugelassene Stufe gleich wahrscheinlich" (aus der Zahl der
Symbole im Fenster gerechnet). Markov **0,086 gegen 0,162, Verhältnis 0,53**; das neuronale Modell
**0,075 gegen 0,162, Verhältnis 0,47**. Beide *weichen dem neuen Ton aus* — das neuronale stärker —
und greifen ihn nur halb so oft, wie eine mode-blinde Gleichverteilung es täte. Das ist der
interessante Befund: die Maske lässt die Hijaz-Terz zu, das Modell hat keinen Grund, sie zu erwarten,
und routet um sie herum. Die Abhilfe — Konditionierung auf den Modus, Nachtraining — gehört in eine
spätere Runde, nicht in diese.

*MIDI-Export.* Ein Moduswechsel ohne Tonartwechsel wird **nicht** als `FF 59` geschrieben. Das
Key-Signature-Meta-Ereignis der SMF-Spezifikation trägt nur eine Zahl von Kreuzen oder Ben und ein
Dur/Moll-Byte; es kann phrygisch-dominant nicht ausdrücken. Bei gleichem Grundton käme byteweise
dasselbe Ereignis noch einmal heraus (ein Nichts), und eine andere Vorzeichenzahl zu schreiben würde
über den Grundton lügen, der sich gerade nicht bewegt hat. `Score::keyChanges` bleibt den echten
Tonartwechseln an Trackgrenzen vorbehalten.

*2. Die Spannungskurve: gemessen, nicht behauptet.* Das Review schlug Lerdahls Stabilitätshierarchie
(*Tonal Pitch Space*, Oxford 2001) vor — die ist solide — und dazu einen Fahrplan über acht Takte:
Takt 1–4 stabil, 5–6 steigend, Takt 7 der Gipfel, Takt 8 auflösend auf Grundton oder Quinte. Der
Fahrplan ist eine Design-Behauptung und **messbar**. `Tools/corpus/measure_tension.py` zählt Lerdahls
Instabilität (5 minus die Tiefe im Basic Space: 0 Grundton, 1 Quinte, 2 kleine Terz, 3 übrige
diatonische Stufen, 4 chromatisch) gegen die Position in der Phrase, über **655 entduplizierte
Korpus-Linien** (666 minus 11 Beinah-Dubletten), mit **gepaarten** Kontrasten je Linie und
Bootstrap-Intervallen über Linien, nicht über Noten.

Was der Korpus zeigt:

| Rolle | Zählzeit 4 − Zählzeit 1 (gepaart, 95 %) | Takt-Parität ungerade − gerade (gepaart, 95 %) |
|---|---|---|
| Acid | +0,446 [+0,164, +0,760] | +0,139 [+0,044, +0,248] |
| Lead | +0,552 [+0,318, +0,788] | +0,199 [+0,074, +0,323] |
| Arp  | +0,287 [+0,212, +0,360] | +0,116 [+0,089, +0,144] |

Alle sechs Intervalle schließen die Null aus. Die Kurve ist also: **Instabilität steigt innerhalb des
Taktes** von Zählzeit 1 zu 4, und sie **alterniert mit Periode zwei Takte** — der zweite Takt eines
Zweitakt-Paares ist der unruhigere. Was der Korpus **nicht** zeigt: den Achttakt-Bogen. Es gibt keinen
Gipfel in Takt 7, und die letzte Note eines Taktes ist *seltener* Grundton oder Quinte, je weiter die
Viertaktgruppe fortschreitet (Arp 0,731 → 0,596, Acid 0,638 → 0,538, Lead 0,582 → 0,484) — das
Gegenteil einer phrasenschließenden Auflösung. Umgesetzt ist deshalb genau das Gemessene und sonst
nichts: ein exponentieller Tilt `exp(tilt · D(Zählzeit, Taktparität) · Instabilität)` auf **denselben**
Positionsgewichten, die der Energiebogen für seine Farbe benutzt. Ein konstanter Faktor an einer
Position kürzt sich im Sampler (CorpusSample.inl) heraus, also ändert der Tilt nur die *Form* der
Verteilung, und die beiden Gewichtungen multiplizieren sich, statt sich zu bekämpfen.

Die Stärke wurde **rückwärts aus der Messung** kalibriert, über 400 Tracks je Einstellung. Die
Theorie (Tilt · D · Var(Instabilität), Var ≈ 1,27) hätte 0,8 gesagt; gemessen überschießt das um das
Dreifache, weil die Modellverteilung viel schärfer auf einen Tilt reagiert als eine Gleichverteilung.

| Tilt | Acid Zählzeit | Lead Zählzeit | Acid Parität | Lead Parität |
|---|---|---|---|---|
| 0,00 (vorher) | +0,147 | +0,381 | +0,062 | +0,203 |
| **0,20** | **+0,228** | **+0,558** | **+0,079** | **+0,276** |
| 0,28 | +0,235 | +0,603 | +0,106 | +0,316 |
| 0,40 | +0,265 | +0,696 | +0,101 | +0,376 |
| Korpus | +0,446 | +0,552 | +0,139 | +0,199 |

0,20 legt den Zählzeit-Kontrast des Leads auf den Korpuswert und alle vier Werte in die
Korpus-Intervalle; 0,40 drückt die Lead-Parität heraus, 0 — das Verhalten vorher — lässt den
Acid-Kontrast darunter. Der Paritätsterm trägt nur die **halbe** Verstärkung (`kParityGain`), auch das
gemessen: ein konstanter Versatz über einen ganzen Takt bewegt den Mittelwert einer Linie etwa doppelt
so stark wie eine Differenz *innerhalb* eines Taktes, und mit gleicher Verstärkung lag die Parität
außerhalb ihres Intervalls. **Für das Arp ist der Tilt 0**, mit Grund: seine erlaubte Menge ist der
Akkord und sonst nichts (PLAN 6.5), und innerhalb eines Dreiklangs tragen die drei Tonhöhenklassen
Instabilitäten von nur 0, 2 und 1 — bei Tilt 0,8 kam ein Kontrast von +0,017 gegen den Korpuswert
+0,287 heraus. Die Kurve dort zu tragen hieße, das Arp vom Akkord zu lösen; das ist mehr, als die
Kurve wert ist. Also: gemessen, berichtet, nicht umgesetzt. Der Farbanteil des Energiebogens (7,4 %
bis 46,0 %) bleibt grün, die beiden Gewichtungen multiplizieren sich wie vorgesehen.

*3. Motivische Operatoren.* Die Lead-Phrase `A A' B A''` variierte nur durch teilweises Neuziehen.
A'' nimmt jetzt zusätzlich **eine** systematische Transformation, pro Phrase gezogen (Gewichte
0,40 / 0,20 / 0,20 / 0,20): **rhythmische Phasenverschiebung** des Zweitakt-Motivs um eine
Sechzehntel (zyklische Rotation innerhalb der 32 Steps — das Motiv loopt, also ist das *die*
Phasenverschiebung; jede Note, die auf einer Zählzeit saß, sitzt danach knapp daneben),
**konturerhaltende Spreizung** (jede Note auf das erste von ihrer Position erlaubte Symbol, das
*echt weiter* vom Vorgänger entfernt liegt als im Elternteil, in derselben Richtung — das kann die
Kontur nie umdrehen) und **Oktavsprünge** (einzelne Offbeat-Sechzehntel um +12 versetzt, das
Goa-Lead-Idiom; eine Oktave behält die Tonhöhenklasse, also bleibt die Note in der Tonleiter und ein
Akkordton ein Akkordton). Lässt sich ein Intervall nicht spreizen, wird das Elternintervall genommen,
und geht auch das nicht, wird die ganze Variante verworfen und das Elternteil behalten: eine
Spreizung, die *verengt*, ist keine. Geprüft an 1200 synthetischen Elternlinien über alle sechs Modi:
**0 Konturvorzeichen gebrochen, 0 Intervalle verengt**, 37 % verworfen (zufällige Eltern, die den
Ambitus schon ausfüllen). Bezug: Schoenbergs entwickelnde Variation, wie Frisch sie liest ("Brahms
and the Principle of Developing Variation", University of California Press 1984).

*4. Euklidische und polymetrische Arpeggien.* `arpStyle` hat zwei Familien mehr, beide über **den
vorhandenen** Euklid-Generator der Percussion (`euclid`, `lhlSyncopation`, Rhythm.h), kein zweiter:
**Euklid** wählt die Steps eines Taktes als E(5,16), E(7,16) oder deren Nachbarn E(4,16) bis E(8,16)
(Toussaint 2005), und die Rotation wird — wie bei einer euklidischen Percussion-Lane — auf eine
**mittlere** Synkopierung gelegt statt auf ein Extrem (Sioros et al. 2014: Groove steigt mit
moderater Synkope). **Polymeter** ist eine Zelle von drei Sechzehnteln gegen den 4/4-Takt: gelesen am
absoluten Sechzehntel des Tracks beginnt sie in jedem Takt einen Step später (16 mod 3 = 1) und kommt
alle drei Takte heim; ihr Akzent — die erste Note der Zelle — präzediert mit, und das ist der hörbare
Punkt. Gemessen, was das Polymeter kostet: das Arp wird in **0,378 der Drops** von der
Maskierungsregel stummgeschaltet gegen **0,453** bei den übrigen Tracks (45 bzw. 329 Drops), mittlere
Oktavverschiebung 1,73 gegen 1,50, tiefste Arp-Note in beiden Fällen MIDI 57 (A3, 220 Hz) — die
Tiefenregel (unter 140 Hz nur Kick und Bass) ist nicht einmal in der Nähe.

*Unterwegs gefunden.* Die **Oktavsprünge trieben die Maskierungsregel**. `leadHi` wurde aus allen
Lead-Noten gebildet, also auch aus den gesprungenen; die Regel schob das Arp daraufhin eine ganze
Oktave höher und über die Decke, wo sie es abschaltet. Gemessen: das Arp war in **53 % der Drops**
stumm, mit ausgenommenen Sprüngen in **26 %**. Eine einzelne versetzte Sechzehntel ist eine
Ausschweifung, kein Register; `pitchRange` lässt sie für den Lead jetzt weg (jede *gezogene*
Lead-Note liegt unter `kLeadRelHi`, alles darüber ist ein Sprung und sonst nichts). — Zweitens war
die Schranke in `testSectionRules` für das Präsenzband (1,5 bis 6 kHz nach dem Drop gegen vor dem
Break, Solberg und Dibben) mit 0,5 dB zu eng: bei einem Modus pro Track spielten Core und Drop
dieselben Noten im selben Register und das Band stimmte auf ein Zehntel dB; mit geborgtem Modus
spielen sie berechtigterweise andere Noten. Nachgemessen über je zwölf Tracks: schlechtester Fall
**−0,64 dB mit Wechsel, −0,50 dB ohne** — die alte Schranke lag in beiden Fällen innerhalb der
Streuung. Sie steht jetzt bei 1,0 dB, hergeleitet aus dieser Messung; eine fehlende Stimme bricht die
Regel um ein Vielfaches davon.

*Dateien.* Geändert: `Core/include/phos/Harmony.h` (Farbtöne, Lerdahl-Instabilität, `isColourTone`
hierher gezogen), `Core/include/phos/Form.h` und `Core/src/Form.cpp` (Sektions-Modus,
Stilprofil-Gewichte, `scaleMask`), `Core/include/phos/Melody.h` und `Core/src/Melody.cpp`
(Spannungskurve, Modus-Material, Operatoren, Arp-Familien), `Core/src/Composer.cpp` (Sektions-Seeds
vor der Form, Knopf, Maske), `Core/include/phos/Params.h` und `Core/src/Params.cpp` (nur angehängt:
`compose.modal_interchange`), `Tests/selftest.cpp` (vier neue Abschnitte; `testMelody` und
`testPads` prüfen jetzt gegen den Modus der **Sektion**, `testSectionRules` mit der gemessenen
Schranke). Neu: `Tools/corpus/measure_tension.py`.

**16.09.2026, Acid-Farbe, Dispersion und analoge Bewegung**

Ein Review des Nutzers schlug fünf Änderungen an Acid und Lead vor. Vier sind gebaut, eine ist nach
Messung **abgelehnt**. Jede Zahl unten ist gemessen, keine ist übernommen.

### 1. Akzente hängen zusammen — gemessen, nicht angenommen

Der Akzent der TB-303 lädt einen Kondensator mit 150 ms (`Acid.cpp`, `kSweepTau`); `Melody.cpp` zog
ihn bisher je Step **unabhängig**. Das Review schlug 75 % Folgewahrscheinlichkeit vor. Diese Zahl ist
nicht übernommen, sondern gemessen: `Tools/ref_accent_runs.py` liest die gekauften MIDI-Pakete (die
drei Psytrance-Pakete plus Midi Klowd), je Spur statt je Datei, und wirft alles über acht Takte weg.

*Die Falle, die zuerst gefunden werden musste.* Ohne diese Längengrenze meldet die Arp-Rolle einen
Lift von **63**. Das ist keine Akzentstruktur, sondern die Sektionsdynamik ganzer Arrangements: eine
leise A- und eine laute B-Hälfte, eine Schwelle dazwischen, und jede Note der B-Hälfte gilt als
Akzent. Mit der Grenze fällt derselbe Wert auf 1,14.

*Die zweite Falle.* Die Akzentdefinition der Korpus-Tabellen — Velocity ≥ Median + 10 — findet in der
Acid-Rolle **null** Akzente (`CorpusTables.cpp`, `k_acid_accent` ist eine Spalte aus Nullen). Der
Grund steht jetzt in der Messung: **62 von 74 Acid-Loops haben überhaupt keine Velocity-Streuung**
(Spanne 0 bis 4). Das Werkzeug meldet deshalb beide Definitionen, die des Korpus und eine skalenfreie
Zwei-Mittelwert-Trennung (Lloyd).

| Rolle, Regel | P(A) | P(A am nächsten Onset \| A) | gegen P(… \| kein Akzent) | Lift |
|---|---|---|---|---|
| **acid, Korpus-Regel** | 0,280 | **0,493** (34 von 69) | 0,197 (35 von 178) | **2,51** |
| acid, Zwei-Mittelwerte | 0,443 | 0,686 (96 von 140) | 0,241 (42 von 174) | 2,84 |
| lead, Korpus-Regel | 0,193 | 0,225 (n = 289) | 0,176 (n = 1200) | 1,28 |
| arp, Korpus-Regel | 0,210 | 0,147 (n = 715) | 0,225 (n = 2707) | 0,65 |

Die Wilson-Intervalle der beiden Acid-Anteile überlappen nicht ([0,378; 0,608] gegen [0,145; 0,261]),
der Lift liegt also mit 95 % zwischen 1,45 und 4,2. **Die Basis ist dünn und wird als dünn gemeldet:
neun Loops.** Für lead und arp trägt die Messung gar nichts (1,28 und 0,65 — letzteres unter 1).

Gebaut ist eine **Zwei-Zustands-Kette mit Lift 2,51**, dem kleineren der beiden Werte, und mit der
Randwahrscheinlichkeit je Step-Position dort, wo sie war: p(nach Ruhe) = m/(1−m+Lm), p(nach Akzent)
das L-fache. Die Kette ändert also, **wo** die Akzente sitzen, nicht wie viele es sind. Gemessen über
3 614 Onsets aus 240 Tracks: realisierter Lift **0,90 → 2,36** (nicht 2,51: aufeinanderfolgende
Onsets stehen auf verschiedenen Positionen, und ein Quotient zweier Mischungen ist nicht die Mischung
der Quotienten), Akzentanteil 0,2117 → 0,1956 (−7,6 %).

*Und was das mit dem Sweep macht — weniger, als das Argument verspricht.* Die komponierten Muster in
eine echte Acid-Stimme gespielt, **auf ihrem eigenen Step-Raster**, und die Ladung am Ende jeder
Sechzehntel gelesen:

| | mittlere Ladung unter einer Akzentnote | höchste Ladung |
|---|---|---|
| unabhängige Ziehung | 0,248 (284 Akzente) | 0,406 |
| Kette mit Lift 2,51 | **0,261** (249 Akzente) | 0,405 |

Ein einzelner Akzent erreicht 0,216. Der Zuwachs sind 5 % Ladung, also **0,011 Oktaven** Cutoff bei
den Standardwerten von Accent und Resonance — unhörbar. Der Grund ist die Zeitkonstante: bei 145 BPM
dauert eine Sechzehntel 103 ms gegen 150 ms Kondensator, Akzente **zwei** Steps auseinander finden
ihn also längst nicht entladen. Der Mechanismus des Arguments stimmt, seine Größe nicht. Die Änderung
bleibt, weil der Korpus die Häufung zeigt, nicht weil der Sweep davon steigt.

### 2. Allpass-Disperser auf Acid und Lead

Das „Pew" moderner Psytrance-Leads ist Gruppenlaufzeit, keine Filterung. `Disperser.h`: bis zu acht
Allpässe zweiter Ordnung, logarithmisch über eine Dekade um `disperse_freq` (Standard 1250 Hz, also
395 bis 3953 Hz), Q = 1 (Zölzer, DAFX 2011, Kapitel 2; Bristow-Johnsons Kochbuchformel für den
Allpass). Neue Parameter `acid.disperse`/`acid.disperse_freq` und `lead|arp|pad.disperse`/`…_freq`,
**Standard 0 Stufen** — keine Messung verlangt, dass er an ist.

| Gemessen (48 kHz, acht Stufen) | Ergebnis |
|---|---|
| Betragsgang 50 Hz bis 20 kHz (der ganze Anspruch) | **0,0003 dB** schlechteste Abweichung von 0 dB |
| Gruppenlaufzeit 140 / 400 / 1250 / 4000 / 16000 Hz | **3,00 / 4,44 / 2,00 / 0,45 / 0,02 ms**, aus der Impulsantwort; geschlossene Form 3,00 / 4,44 / 2,00 / 0,45 / 0,02 |
| Lautheit einer Acid-Linie | −13,04 → **−13,04 dB** (−0,00) — ein Allpass verschiebt keine Energie zwischen Bändern |
| Crest-Faktor derselben Linie | 15,09 → **11,62 dB** (−3,47) — der Preis, den eine spitzenwertgeführte Kette zahlt |
| Tiefenregel bei D3, voller Drive und Resonanz | **−48,2 dB** unter 140 Hz |

Q = 1 statt der glatteren 0,7 aus einem Grund: bei Q = 1 liegt das Maximum der Laufzeit **im Band**
(4,44 ms bei 400 Hz gegen 3,00 ms bei 140 Hz), bei Q = 0,7 steigt sie weiter zu DC (4,07 gegen 3,95)
und bei Q = 0,4 bekommt das Subband am meisten (5,69 gegen 3,62). Genau dafür nimmt man einen
Abschnitt zweiter statt erster Ordnung, dessen Laufzeit immer bei DC gipfelt.

### 3. Der Kamm liest jetzt mit Lagrange dritter Ordnung

Lineare Interpolation ist ein Tiefpass, dessen Dämpfung vom Bruchteil abhängt — der gestimmte
Squelch-Kamm verlor also seine Spitze genau dort, wo er am schärfsten sein soll (Laakso, Välimäki,
Karjalainen, Laine, „Splitting the unit delay", IEEE Signal Processing Magazine 13(1), 1996).
Gemessen an einem Kamm mit Rückkopplung 0,82 und halbsamplig gestimmt, Resonanzspitze in dB:

| | 2 kHz | 5 kHz | 10 kHz |
|---|---|---|---|
| ganzzahlig gestimmt (Ideal) | 14,89 | 14,89 | 14,89 |
| **Lagrange 3** | **14,89** | **14,76** | **12,58** |
| linear (vorher) | 14,43 | 13,17 | **8,88** |

Die gemessene Spitze trifft die geschlossene Form 1/(1 − fb·|H|) auf 0,22 dB.

*Der Haken, den das Review nennt, ist behandelt und hat nicht entschieden.* Die Literatur bevorzugt
den Allpass-Interpolator (Betrag exakt 1), aber sein Zustand muss bei **jeder Note** neu gestimmt
werden, und sein Koeffizient a = (1−frac)/(1+frac) wandert bei frac → 0 auf den Einheitskreis
(Pol bei z = −1). Gemessen: 100 ms nach einer Umstimmung, mit weggenommener Anregung und einem Kamm,
der selbst 136 dB abgeklungen ist, steht die Allpass-Fassung noch bei **−108 dB** der
Ruheamplitude, die Lagrange-Fassung bei **−184 dB**. Das ist viel zu leise, um es zu hören — was es
zeigt, ist, dass der Zustand bei jeder Note zurückgesetzt oder übergeblendet werden müsste. Dafür ist
der Gewinn zu klein: **0,00 dB bei 2 kHz, 0,13 dB bei 5 kHz, 2,31 dB bei 10 kHz** — und bei 10 kHz
hat eine Acid-Note eine vierpolige Leiter mit einigen hundert Hertz Cutoff hinter sich. Lagrange hat
keinen Zustand und nimmt 91 % des linearen Fehlers bei 5 kHz weg. Das ist die Begründung, und es ist
ausdrücklich **nicht** die Präferenz der Literatur.

Nebenbei: die Verzögerung ist jetzt auf mindestens **3** Samples geklemmt statt 2, weil der Tap bei
+2 hinter dem Schreibzeiger liegen muss. Musikalisch unerreichbar (die tiefste Acid-Note gibt 327).

### 4. Die asymmetrische Vorspannung — **abgelehnt**, mit Zahlen

Das Review schlug vor, die punktsymmetrische Sättigung der Leiter durch `v + α v²` (α ≈ 0,06) zu
ersetzen, um gerade Obertöne zu gewinnen und „digitale Kälte" zu nehmen; Wirkung „extrem hoch".
Das ist eine Behauptung über das Spektrum der Referenzen, also messbar.

*Das Maß.* `Tools/ref_harmonics.py` misst nicht „Summe gerade durch Summe ungerade" — das misst vor
allem Helligkeit —, sondern eine **steigungsfreie Geradheit**: E_n = 20 log10(a_n / √(a_{n−1}·a_{n+1}))
für n = 2 und 4, also einen geraden Teilton gegen das geometrische Mittel seiner beiden ungeraden
Nachbarn. Das kürzt jede glatte Hüllkurve heraus. Die Sollwerte sind analytisch und in `--selftest`
geprüft: Sägezahn 10 log10(1 − 1/n²), also −1,249 dB bei n = 2 und −0,280 bei n = 4 (getroffen auf
0,001 dB), Rechteck −102 dB, gleiche Teiltöne 0,000 dB.

*Was die Messung stützen kann und was nicht.* Sie isoliert die Acid **nicht**. Sie beschränkt sich auf
Rahmen, in denen das Band über dem Kick-Bass-Bereich **eine** starke, stabile, tonale Grundfrequenz
trägt (8192-Punkt-STFT, Bins unter 250 Hz genullt, f0 per Oberton-Summation mit Suboktav-Vorzug und
Oktav-Sperre, Salienzschwelle, f0 in drei aufeinanderfolgenden Rahmen auf 3 % gleich). Das ist
irgendein tonales Instrument in diesem Band, und ein fertiger Master hat Sättigung und Limiter hinter
sich. Und vor allem: **ein Sägezahn führt ohnehin jeden ganzzahligen Oberton.** Eine punktsymmetrische
Kennlinie liefert nur aus einem *Sinus* ausschließlich ungerade Obertöne; aus einem Sägezahn liefert
sie alle. Die Messung kann also sagen, ob wir im Bereich der Aufnahmen liegen — und den Vorschlag
damit erledigen —, sie kann umgekehrt nicht beweisen, welche Kennlinie eine Referenzzahl erzeugt hat.

*Ergebnis, 40 Aufnahmen mit Albumtag „Psytrance Collection", vier 45-s-Fenster je Titel:*

| | Median | Quartile | Spanne |
|---|---|---|---|
| E2 der Referenzen | **−0,28 dB** | −0,84 / +0,79 | −2,64 … +3,06 |
| E4 der Referenzen | **+0,58 dB** | −0,31 / +1,32 | −4,32 … +3,84 |
| gerade/ungerade (die Formulierung des Reviews) | −1,34 dB | −2,41 / −0,51 | −6,93 … +0,85 |
| **unsere Acid, dasselbe Maß** | **E2 −0,50 dB, E4 −0,18 dB** | — | — |

Unsere Acid liegt in **beiden** Maßen innerhalb des Interquartilbereichs der Aufnahmen. Es gibt keine
Lücke an geraden Obertönen, die zu schließen wäre.

*Die beiden genannten Risiken, einzeln geprüft* (Versuchsprogramm mit beiden Kennlinien, σ(v) mit
w = v + αv², lokale Verstärkung (1 + αv)/√(1 + w²)):

| | α = 0 | α = 0,06 |
|---|---|---|
| Selbstoszillationsschwelle k, Anregung 1e−4 | **17,000** | **17,000** |
| dieselbe bei Anregung 0,5 | 17,006 | 17,006 |
| Ausschwingen bei k = 17,51 / 16,49 | +73,4 / −393,0 dB | +73,5 / −393,0 dB |
| Gleichanteil, Sägezahn Amplitude 1, k = 16 | +0,06642 (0,367 des Effektivwerts) | +0,06858 (0,380) |
| E2 / E4 desselben Signals | −0,748 / −0,552 dB | −0,634 / **+0,230** dB |
| 1 kHz gegen 500 Hz (die Resonanz) | −12,08 dB | **−11,08 dB** |

Die **analytische k = 17-Prüfung bewegt sich nicht** — und zwar aus einem Grund, der auch erklärt,
warum der Vorschlag wenig bringt: `v + αv²` ist zweiter Ordnung, σ'(0) bleibt 1, die linearisierte
Schleifenverstärkung am Arbeitspunkt ist unverändert. Das genannte Regressionsrisiko tritt also nicht
ein. Der Gleichanteil verschiebt sich um 3 %. Was sich wirklich bewegt, ist E4 um 0,78 dB und die
**Resonanz um 1,0 dB** — das heißt, die Vorspannung ändert die Charakteristik des Filters, also genau
das, was sie laut Review nicht anfasst, und der Gewinn an geraden Obertönen (0,11 dB bei E2)
verschwindet in einer Referenzstreuung von 5,7 dB.

**Entscheidung: nicht gebaut.** Weder als Ersatz noch als Parameter: ein Parameter mit gemessenem
Standard wäre nur zu rechtfertigen, wenn die Messung einen Standard nennen könnte, und sie nennt
keinen. Die TB-303 ist nasal und aggressiv, nicht warm; das war die Vermutung, und die Messung
widerspricht ihr nicht.

### 5. Thermische Drift

Je Unisono-Schacht und je Stimme ein sehr langsamer Zufallsweg (`kDriftHz` = 0,2 Hz, also rund 0,8 s
Korrelationszeit), normiert, so dass seine **stehende Streuung** genau `poly.drift` in Cent ist
(Standard **1 Cent**, das sind ±2 Cent bei zwei Sigma). Die Tonhöhe jedes Schachts, der Cutoff der
Stimme (ein Viertel der Cent-Abweichung) und der Attack der Hüllkurve (1 % je Cent) laufen davon.
Pirkle, „Designing Software Synthesizer Plug-Ins in C++", 2. Aufl. 2019, modelliert die Drift genauso:
eine sehr tieffrequente Rauschquelle je Oszillator.

Die drei harten Bedingungen sind Konstruktion, nicht Hoffnung:

- **Nur aus dem Seed.** Eigener Generator `driftRng_`, gegen `phaseRng_` gesalzen, damit ein Render
  mit Drift 0 bitgleich zu einem Stand ohne Drift ist (geprüft, siehe unten).
- **Absolutes Sample-Raster.** Die Wege gehen einen Schritt je `kPolyBlock` = 16 Samples, in
  `renderSegment` und für **alle** Schächte und Stimmen, ob sie klingen oder nicht — ein Weg, der nur
  während gehaltener Noten liefe, wäre eine Funktion der Notengeschichte statt des Sample-Index.
- **Bitgleichheit der Lane-Pfade.** Alles wird skalar gerechnet und erreicht die Kernel nur als
  Koeffizient. Der Vektortest fährt die Drift jetzt **eingeschaltet** (4 Cent) samt Disperser.

*Warum die Drift für die Dauer einer Note festgehalten wird, mit der Zahl dahinter.* Bei 0,2 Hz
bewegt sich ein Weg innerhalb einer Sechzehntel um etwa eine halbe Streuung — die Physik sagt also
selbst, dass innerhalb einer Note fast nichts passiert. Gebaut ist trotzdem das Festhalten, und der
Grund ist messbar: eine Drift **während** der Note verschmiert jede Oberreihe. Gegenprobe (Mutation 7
unten, Drift durchgehend auf `dt` angewandt): das Aliasing-Maß der Supersaw fällt bei Standard-Drift
von **−69,0 auf −17,0 dB bei C6** und von **−61,6 auf −14,8 dB bei A6**. Eine gehaltene Drift
multipliziert dagegen die ganze Oberreihe eines Oszillators mit einer Konstanten — aus einer Reihe
wird wieder eine Reihe, und die Zahlen der DSP-Runde bleiben stehen: **C5 −72,4, C6 −68,9, A6
−61,6 dB** bei Detune 1,00, und nach einer Sekunde Drift bei 1 Cent **C6 −69,0 und A6 −61,6 dB**.

**Der Bass driftet nicht** — er kann es nicht, er ist keine `Poly`-Instanz. Die Phasenkopplung von
Kick und Bass sitzt auf der ersten Bassnote.

### Prüfungen

Ein neuer Abschnitt `testAcidColour`, **18 Prüfungen**, jede gegen einen anderswo hergeleiteten Wert:

| Prüfung | gegen den alten Stand | jetzt |
|---|---|---|
| das Geradheitsmaß trifft seine analytischen Werte | — (gab es nicht) | Sägezahn −1,249 / −0,280 dB (Soll −1,2494 / −0,2803), Rechteck −120,8 dB |
| Akzente häufen sich mit dem gemessenen Lift | **0,90** (unabhängige Ziehung) | **2,36** |
| die Häufung fügt keine Akzente hinzu | 0,2117 (exakt der Positionsmittelwert) | 0,1956 (−7,6 %) |
| Akzentnoten stehen auf geladenem Kondensator (Wächter) | 0,248 / 0,406 | 0,261 / 0,405 |
| Disperser: Betragsgang flach | — | **0,0003 dB** |
| Disperser: Gruppenlaufzeit wie entworfen | — | 3,00 / 4,44 / 2,00 / 0,45 / 0,02 ms, geschlossene Form gleich |
| Disperser: Lautheit bleibt, Crest fällt | — | −0,00 dB / −3,47 dB |
| Disperser: Tiefenregel bei voller Kette | — | −48,2 dB |
| Kamm: halbsamplige Stimmung behält die Resonanz | **6,02 dB** Verlust (linear) | **2,31 dB** |
| Kamm: gemessene Resonanz = 1/(1 − fb\|H\|) | — | 0,22 dB |
| Kamm: der Allpass klingelt nach der Umstimmung | — | −108 dB gegen −184 dB |
| die Acid-Stimme benutzt die Lagrange-Taps | **−3,9 dB** (linear) | **−1,6 dB** |
| die Acid führt ihre geraden Obertöne bereits | — | E2 −0,50, E4 −0,18 dB |
| Drift: stehende Streuung ist der Parameter | — | 1,98 Cent für Parameter 2 |
| Drift: Seed und Blockgröße | — | Blöcke 3 / 125 / 1000 bitgleich, anderer Seed 48000 von 48000 verschieden |
| Drift: bei 0 bewegt sich nichts | — | größter Wegwert exakt 0 |
| Drift: die Supersaw aliast nicht mehr als vorher | — | C6 −69,0, A6 −61,6 dB (gegen das *undriftete* Liniennetz: −21,5 dB) |
| Drift: die Tonhöhe bewegt sich wirklich | — | bei 6 Cent über vier Seeds −3,40 / −3,84 / −3,67 / +0,74 Cent |

Dazu im Vektortest: die Poly-Prüfung fährt jetzt Drift 4 Cent und einen Disperser mit 1 bis 8 Stufen
mit — **0 abweichende Samples** in AVX2, NEON-Shim und skalar, und in allen drei Pfaden dieselbe
Energie 62054,3.

### Gegenprobe (Mutationsrunde)

Sieben Fehler einzeln eingebaut, alle sieben von ihrer Prüfung gefunden, `git diff` danach sauber:

| Mutation | Wer merkt es |
|---|---|
| Akzent wieder unabhängig gezogen | Lift-Prüfung: **0,90** statt 2,36 |
| Kamm wieder linear interpoliert | „die Acid-Stimme benutzt die Lagrange-Taps": **−3,9 dB** statt −1,6 |
| Disperser-Q von 1 auf 0,4 | Gruppenlaufzeit: **5,69 ms bei 140 Hz gegen 3,62 bei 400** — das Subband bekäme die meiste Verzögerung |
| Allpass-Zähler `z^-2` von 1 auf 0,9 (Spiegelsymmetrie gebrochen) | Betragsgang **74,1 dB** daneben, Lautheit **+72 dB**, Gruppenlaufzeit daneben — drei Prüfungen |
| Drift-Weg je Segment statt je Rasterschritt | Blockgrößen-Prüfung: Block 3 und 125 weichen ab |
| Drift-Weg nicht auf seine stehende Streuung normiert | drei Prüfungen: Streuung **0,02** statt 1,98 Cent, Tonhöhe bewegt sich nicht, Aliasing-Wächter schlägt an |
| Drift durchgehend statt für die Note gehalten | Supersaw-Aliasing **−17,0 / −14,8 dB** statt −69,0 / −61,6 |

*Eine Falle dieser Runde, für das Protokoll.* Beim Zurücknehmen einer Mutation mit
`git checkout -- <datei>` verschwinden auch die *eigenen* Änderungen der Runde, weil HEAD der
Ausgangs-Commit ist. Zwei Dateien mussten neu geschrieben werden; danach wurde mit Kopien aus einem
Sicherungsordner zurückgesetzt und die Zeitstempel angefasst (MSVC baut eine mit altem Zeitstempel
zurückgespielte Datei nicht neu).

### Der geänderte Standard-Render, mit der Zahl

Zwei Standardwerte sind bewusst geändert: die Akzentziehung (kein Parameter, eine Regel) und
`*.drift` = 1 Cent. Der Disperser steht auf 0 Stufen, ändert also nichts.

| 96 Takte, Seed 1 | gegen den Ausgangsstand |
|---|---|
| Drift 0 (nur Akzentkette und Kamm) | **−34,3 dB** relativ zum Programm |
| Drift 1 Cent (der Standard) | **−22,0 dB** relativ zum Programm |
| Integrated / True Peak | **−8,9 LUFS / −1,00 dBTP**, beide unverändert |
| Seed 7, 64 Takte, Drift 0 | **bitgleich** — dieser Track hat keine Acid |
| Acid solo, Seed 1, 96 Takte, Drift 0 | −19,3 dB relativ zur Acid; Seed 42 bitgleich |

Rechenzeit 96 Takte: **9,5× Echtzeit vorher, 9,3× jetzt** (Drift an). Der Disperser kostet nur, wenn
er an ist (acht Biquads je Kanal).

*Dateien.* Geändert: `Core/include/phos/Acid.h` (`combTaps`, Disperser-Zustand), `Core/src/Acid.cpp`
(Lagrange-Kamm, Disperser), `Core/include/phos/Poly.h` und `Core/src/Poly.cpp` (Drift, Disperser),
`Core/src/Melody.cpp` (**nur die Akzentziehung in `makeAcid`**), `Core/include/phos/Params.h` und
`Core/src/Params.cpp` (fünf **angehängte** Parameter, keine Umsortierung, kein geänderter Standardwert
außer den oben genannten), `Tests/selftest.cpp` (`testAcidColour`, `partialMag`, `evennessDb`,
`disperserResponse`, `maskedAliasDb`, `combPeakDb`), `Tests/vectest.cpp` (Drift und Disperser im
Poly-Lauf). Neu: `Core/include/phos/Disperser.h`, `Tools/ref_accent_runs.py`, `Tools/ref_harmonics.py`.
`Core/include/phos/DiodeLadder.h` ist **unverändert** — das ist das Ergebnis von Punkt 4.

Gesamt: **285 Selbsttest-Prüfungen** in **454 s**, Vektortests 16 von 16 in allen drei Pfaden.

**16.09.2026, Nachtrag: die gelernten Modelle erreichen Plugin und Quest**

Die beiden offenen Löcher der Release-Runde, beide geschlossen. Sie gehören zusammen: das erste
schaltete Phase 8 im Produkt still ab, das zweite sorgte dafür, dass genau das auf der Baumaschine
niemandem auffiel.

**1. Niemand rief `setModelSearchPath()`.** `Plugin/PluginProcessor.cpp` setzte nur den
Wavetable-Pfad. Jetzt tut `installSearchPaths()` beides — im Konstruktor des Prozessors, vor dem
Composer-Thread, also vor dem ersten `Engine::prepare()` und vor der ersten Kompositionsrunde im
Prozess. Gesucht wird wie beim Pack: neben der Binärdatei, dann `Contents/Resources` (VST3), beides
aus `currentExecutableFile` und `currentApplicationFile`; `Plugin/CMakeLists.txt` legt jetzt alle drei
Dateien an beide Stellen, nicht nur das Pack. Die Quest packt die beiden `.phosmdl` wie das Pack als
APK-Asset einmalig nach `internalDataPath` aus (`prepareAsset`, `prepareModels`) und meldet den Pfad
in `App::init()` an, vor dem Composer-Thread.

**Die Modelle werden dabei sofort geladen, nicht erst wenn der Knopf sie verlangt** — sonst wüsste
der Set-Reiter genau in dem Zustand nichts, in dem der Nutzer es wissen muss (Modelle aus, Dateien
fehlen). Kosten: rund 13 MB gepackte Gewichte und etwa eine Zehntelsekunde je Prozess, einmal; das
Laden ist idempotent, der Composer findet die Arbeit später getan vor.

**Sichtbar statt still.** Der Wavetable-Runde folgend, aber eine Stufe weiter: der Set-Reiter trägt
eine Gruppe „Pitch models" mit einer Zeile je Teil — `Melody:  learned, 1.15 nats` (was die Datei
selbst als gehaltene NLL angibt; die Bassdatei sagt 0.40) beziehungsweise
`Melody:  not installed -- Markov` in der Warnfarbe des Mischpults, mit dem Verzeichnis oder der
Fehlermeldung des Kerns im Tooltip. Zusätzlich markieren beide Auswahlfelder ihren gelernten Eintrag
als `Neural (missing)`, nach derselben Regel und aus demselben Grund wie eine fehlende
Bibliothekstabelle: **markiert, nicht entfernt**, weil ein Choice-Index ein Vertrag ist (eine
entfernte Zeile nummeriert alles dahinter um, und ein anderswo gespeicherter Zustand lädt falsch).
Warum beides: eine Markierung in einem zugeklappten Auswahlfeld sieht nur, wer schon hinschaut; eine
Zeile auf der Seite sieht auch, wer nicht hinschaut. Und nützlich ist nicht die Lampe, sondern der
Grund — welche der beiden Dateien fehlt, und ob die geladene aus einer Installation kam oder aus dem
Quellbaum dieser Maschine (`fromSourceTree`, der Fall, der hier funktioniert und sonst nirgends).

**2. `PHOS_SOURCE_DATA_DIR` steht nicht mehr in ausgelieferten Binärdateien.** Neue Option
`PHOS_SHIP` (Wurzel-`CMakeLists.txt`); `Core/CMakeLists.txt` definiert den Pfad nur ohne sie,
`Deploy/build_release.ps1` setzt sie, und `ctest` läuft in genau dieser Konfiguration — nicht in
einer anderen. Die Testprogramme bekommen den Pfad weiter, aber auf ihren eigenen Zielen
(`Tests/CMakeLists.txt`): ein Testprogramm darf wissen, wo die Quellen liegen, die Bibliothek nicht.
Die Namenssuche des Kerns bedienen die Tests so, wie eine portable Installation es tut — die drei
Dateien liegen neben der Binärdatei, und das Arbeitsverzeichnis des Selbsttests ist dieses
Verzeichnis. Die Referenzaufnahme des Release-Laufs entstand bisher im Build-Baum *ohne* Daten
daneben und funktionierte nur, weil jede Binärdatei heimlich den Quellbaum las; sie bekommt jetzt ein
eigenes Verzeichnis mit einer Kopie von `Core/data`. Und Prüfung F2 der Paketprüfung **scheitert**,
statt zu warnen: ein Paket, in dem der Pfad noch steckt, ist ein falsch gebautes Paket, und Prüfung F
misst dann nichts.

*Der Nachweis ist der Fehler, den es verdeckte.* Dieselbe Staging-Kopie, dieselbe gelöschte
`bass.phosmdl`, dieselbe Prüfung — einmal mit dem Define und einmal ohne:

| Build | alles gestaged | `bass.phosmdl` gelöscht |
|---|---|---|
| `PHOS_SHIP=OFF` (wie bisher jedes Release) | identisch zur Referenz | **identisch zur Referenz** — der Fehler ist unsichtbar, Prüfung F geht durch |
| `PHOS_SHIP=ON` | identisch zur Referenz | **anders als die Referenz** — Prüfung F scheitert |

Beide Builds rendern mit vollständigem Staging denselben Hash (`5FF2961CFF3C6D32…`), die Umstellung
ändert also nichts am Klang; sie ändert nur, ob der Fehler messbar ist. Am echten Skript
nachgefahren: mit gelöschter gestagter `bass.phosmdl` meldet `check_package.ps1` jetzt **drei**
Fehler (A „missing", F „does not render the reference", H „that runtime loses Phase 8"), und mit
einer ohne `PHOS_SHIP` gebauten `phos_render.exe` im Staging meldet F2 „embeds
G:/…/Core/data — built without -DPHOS_SHIP=ON, so check F above proves nothing".

**Neue Prüfung H** in `Tools/release/check_package.ps1`: die beiden `.phosmdl` an *jeder* Stelle, an
der eine Laufzeit sucht — neben den Binärdateien, in `Phosphene.vst3\Contents\Resources`, als
`assets/` im APK. A, B und C prüfen die Dateien einzeln; H schreibt die Suchreihenfolge als
Paketanforderung auf, damit eine vierte Oberfläche einen Ort hat, an dem sie stehen muss, und damit
der Fehler sagt, *welche Laufzeit* Phase 8 verliert.

*Gemessen.* Der ganze `ctest` in der Konfiguration, die ausgeliefert wird
(`-DPHOS_SHIP=ON -DPHOS_STATIC_RUNTIME=ON -DPHOS_AVX2=ON`): **8 von 8**, 685 s — Selbsttest 267 von
267, Cue-Prüfung 13, Vektortests 16/16/16 in AVX2, NEON-Shim und skalar, Hosttest **119 Prüfungen,
0 Fehler**, VST3-Test 36. Im Entwicklungsbuild dieselben acht grün mit 117 Hosttest-Prüfungen; der
Unterschied sind die zwei zusätzlichen Prüfungen, die nur ohne Quellbaum-Rückfall etwas zu messen
haben. Neu: sechs im Hosttest (zwei Hälften — Dateien daneben, Dateien nirgends), vier im VST3-Test,
sechs Zeilen Prüfung H im Paket-Check. Ein Standard-Render ist bitgleich zur Basis be3130f
(`2DB1EAF16D4EBAB4851DF0D3073ADA4B`, 64 Takte).

**Jede zuerst scheitern gesehen.** Gegen den unreparierten Stand — `setModelSearchPath()` übersprungen
— fielen genau die sechs neuen Hosttest-Prüfungen, mit `Melody:  not installed -- Markov` im
Set-Reiter und `marks=2` im Kindprozess. Danach fünf Mutationen, jede gefangen, jede
zurückgenommen, `git diff` sauber: (1) der `setModelSearchPath()`-Aufruf weg → 4 Hosttest-Fehler;
(2) die Set-Reiter-Zeile sagt nie „learned" → 1; (3) `melody.phosmdl` nicht mehr ins VST3-Bundle
kopiert → 2 VST3-Fehler; (4) `melody.phosmdl` nicht mehr neben die Binärdateien gestaged → 3
Paketfehler, darunter F; (5) `bass.phosmdl` nicht mehr ins APK → 2 Paketfehler (C und H). Eine sechste
Mutation wurde verworfen, weil sie **kein** Fehler war: die Staging-Kopie ins Bundle ist redundant,
seit `Plugin/CMakeLists.txt` dieselben Dateien schon dorthin legt und das Bundle als Ganzes gestaged
wird. Das APK wächst von **4.588.497 auf 7.631.957 Bytes**.

*Die Prüfungen selbst.* „Da" und „nicht da" lassen sich für die Modelle nicht im selben Prozess
inszenieren: `sharedMelodyModel()` lädt einmal und entlädt nie, und das Plugin löst sein
Ressourcenverzeichnis einmal auf — anders als die Wavetable-Bibliothek, die ein
`resetWaveTableLibrary()` hat. Die Abwesenheit wird deshalb so gestellt, wie sie nur zu stellen ist:
eine Kopie der Hosttest-Binärdatei in einem leeren Verzeichnis, als Kindprozess gestartet
(`--probe-models`), die eine Zeile zurückmeldet. Und weil ein Entwicklungsbuild dort den Quellbaum
findet, prüft der Elternprozess `#if defined(PHOS_SOURCE_DATA_DIR)` **beide** Ausgänge: im
Entwicklungsbuild muss das Kind die Modelle finden *und* sagen, dass sie von außerhalb der
Installation kamen; im Auslieferungsbuild muss es den Rückfall melden, beide Auswahlfelder markieren
und trotzdem klingen.

*Nicht geprüft.* Kein Headset am Rechner: das APK ist gebaut, signiert und auf seine Assets geprüft,
aber nicht installiert — `prepareModels()` ist nie auf einem Gerät gelaufen. Keine DAW auf der
Maschine: der VST3-Pfad ist über `phos_vst3test` und das Bundle geprüft, nicht in einem Host.

*Dateien.* Geändert: `Plugin/PluginProcessor.cpp` und `.h` (`resolveResourceDirectory`,
`installSearchPaths`, `LearnedModels`), `Plugin/EditorSetTab.cpp` (Gruppe „Pitch models"),
`Plugin/EditorLayout.cpp` (die Markierung), `Plugin/CMakeLists.txt` (alle drei Dateien neben beide
Artefakte), `Quest/src/main.cpp` (`prepareAsset`, `prepareModels`), `Quest/build_apk.ps1` und
`Quest/README.md` (drei Assets statt einem), `CMakeLists.txt` und `Core/CMakeLists.txt` (`PHOS_SHIP`),
`Tests/CMakeLists.txt` (Daten neben die Testbinärdateien, Arbeitsverzeichnis des Selbsttests,
`phos_test_data_dir`), `Tests/hosttest.cpp` (`probeModels`, `findLabel`, zwei Abschnitte),
`Tests/vst3test.cpp` (alle drei Bundle-Dateien), `Deploy/build_release.ps1` (`-DPHOS_SHIP=ON`,
Referenzaufnahme mit eigenen Daten), `Tools/release/check_package.ps1` (C erweitert, F2 scheitert
statt zu warnen, neue Prüfung H), `Tools/render/main.cpp` (nur ein Kommentar).

*Nicht aufgeräumt.* `Core/include/phos/WaveTableFile.h` sagt in seinem Doxygen-Kommentar noch, ein
Build ohne Suchpfad finde die Datei „beside the sources through `PHOS_SOURCE_DATA_DIR`"; das gilt nur
noch für einen Entwicklungsbuild. Die Datei gehörte in dieser Runde einem anderen Agenten.

**16.09.2026, Modus-Konditionierung: das Modell lernt die geliehene Farbe.** Die
Modal-Interchange-Runde gab jeder Sektion einen eigenen Modus und maß dann nach, ob der Komponist den
Ton, den der geliehene Modus neu zulässt, überhaupt spielt. Er tat es nicht: Anteil 0,086 beim
Markov-Modell und 0,075 beim trainierten Transformer gegen eine modusblinde Erwartung von 0,162 --
Verhältnisse von 0,53 und 0,47. Der Grund ist strukturell: Rolle, Stil, Takte, Schritt, Takt, Lücke
und Index sind die Konditionierung, **der Modus nicht**. Die Maske lässt die Note zu, die eigene
Verteilung des Modells drückt sie zurück. Diese Runde hat beides gebaut, was der Auftrag verlangte --
eine kalibrierte Anhebung ohne Nachtraining und eine elfte Einbettungstabelle mit Nachtraining -- und
das erste davon **gemessen und verworfen**.

*Das Ziel, auf dem Korpus gemessen.* `Tools/train/mode.py` (neu) schätzt den Modus jeder Korpuslinie
mit dem Bayes-Tonartmodell von Temperley (*Music and Probability*, MIT Press 2007, Kapitel 4), auf
Skalenzugehörigkeit reduziert, und zählt, welchen Anteil ihrer Noten die Linien in einem *farbigen*
Modus auf dessen eigene Farbtöne legen: **0,1539, 95-%-Bootstrap über Linien [0,1442, 0,1640]**, aus
377 Linien und 20 939 Noten, und über die Rollen erstaunlich gleich -- Acid 0,1372 [0,1137, 0,1671],
Lead 0,1411 [0,1189, 0,1653], Arp 0,1569 [0,1453, 0,1685]. Dieselbe Zählung ohne Modusbedingung, über
alle 666 Linien, ergibt **0,0874**. Das zweite Maß ist das, was der Komponist heute reproduziert
(0,0843 mit dem Markov-Modell) -- die deutlichste Einzelbeobachtung dafür, dass seine farbigen
Sektionen modusblind gezogen werden.

*Stufe 1, die kalibrierte Anhebung: gebaut, kalibriert, verworfen.* Ein multiplikatives Gewicht auf
die Farbtöne des gespielten Modus, innerhalb der Positionsgewichte, die der Sampler ohnehin trägt
(`scaleSet`, `chordSet`), kalibriert gegen das gemessene Ziel statt gewählt. Gemessen über vier Stile
und je acht 128-Takt-Tracks, rund 45 000 melodische Noten je Einstellung:

| Rolle | Korpus-Ziel | Markov, Anhebung 1 | Markov, Anhebung 3 | neuronal, Anhebung 1 | neuronal, Anhebung 3 |
|---|---|---|---|---|---|
| Acid | 0,1372 | 0,0008 | 0,005 | 0,039 | **0,133** |
| Lead | 0,1411 | **0,325** | 0,735 | **0,215** | 0,467 |
| Arp | 0,1569 | 0,065 | 0,091 | **0,196** | 0,211 |

Die Obergrenze 3 ist nicht neu, sondern genau die größte Anhebung, die der Energiebogen denselben
Tönen ohnehin gibt (`1 + 2 * colour`). Genau **eine** Zelle ist ein Erfolg: das Acid des neuronalen
Modells trifft sein Ziel an der Obergrenze. Markov-Acid und Markov-Arp sind bei 3 noch um das 27- und
das 1,7-Fache zu tief, und beide Kurven sind dort fast flach (Arp 0,088 bei 2 gegen 0,091 bei 3); das
Lead beider Modelle liegt schon *über* dem Korpus, weil der Energiebogen genau diese Töne in genau
dieser Rolle anhebt. **Ein gemeinsamer Wert über alle Rollen sieht nur deshalb wie eine Kalibrierung
aus, weil er aggregiert:** eine Anhebung von 2,0 legt den *Gesamtanteil* des Markov-Modells auf 0,1520
-- sauber im Korpusintervall -- indem sie das Lead auf 0,638 treibt, während das Acid bei 0,004 bleibt.
Der Aggregatwert wurde deshalb als Kalibrierziel abgelehnt. Was von Stufe 1 danach übrig blieb, war
eine einzige Zahl, und die macht Stufe 2 überflüssig (siehe unten: das Acid erreicht sein Ziel dort ohne jede Anhebung); **die Anhebung ist nicht
ausgeliefert**, `Melody.cpp` steht wieder auf den Mengen von vorher. Die Kosten wurden trotzdem
gemessen, in der Währung, die der Abschnitt „maskiertes Decodieren" von Phase 8 verwendet: totale
Variation zwischen der gezogenen Verteilung mit und ohne Anhebung, Mittel 0,0473, schlechtester Fall
0,1102, gegen die analytische Decke (L−1)/L = 0,6667.

*Wie verlässlich die Modusschätzung ist -- die Zahl, die über Stufe 2 entscheidet.* Die Korpuslinien
sind auf ihren eigenen Grundton transponiert, und keine Datei sagt, **welches** Moll das ist (1 264
Dateinamen der drei Psy-Packs: sechs sagen „min"/„minor", keiner nennt einen Modus). Der Label muss
also geschätzt werden, und drei unabhängige Messungen sagen, wie gut:

* **Entschieden auf etwa einem Drittel.** Posterior über 0,9 bei 36,6 % der 666 Linien; der Rest ist
  ein exaktes Unentschieden zwischen zwei bis vier Modi, weil die unterscheidenden Stufen selten
  gespielt werden (große Sekunde 22 % der Linien, große Septime 5 %, große Sexte 3 %). Der
  Gleichstand fällt auf den *farbärmsten* passenden Modus, der Label kann die Farbe einer Linie also
  unterschätzen und ihr nie eine Farbe andichten, die sie nicht spielt -- die Richtung, in die ein
  Fehler hier zeigen muss.
* **Split-Half** (Modus der ungeraden gegen den der geraden Noten derselben Linie): **73,3 %**
  Übereinstimmung, Zufall 36,7 %, Cohens Kappa 0,579.
* **Rückgewinnung** auf 3 996 synthetischen Linien bekannten Modus (echte Linien auf die Stufen eines
  Zielmodus abgebildet, Rhythmus und Notenzahl bleiben): **62,0 % richtig insgesamt, 100 % auf den
  1 416, die der Schätzer als entschieden meldet.**

Verteilung der Label: Aeolisch 42,2 %, Phrygisch 33,6 %, Phrygisch-Dominant 18,8 %, harmonisch Moll
3,9 % (26 Linien), Dorisch 1,2 % (8), doppelharmonisch 0,3 % (2). **Drei der sechs Zeilen sind also
praktisch unbesetzt** -- wer doppelharmonisch verlangt, fragt eine Zeile, die zwei Linien gesehen hat.
Das ist die offene Schwäche dieser Runde.

*Stufe 2, das Format.* Elfte Tabelle `mode.emb`, Kopfzeile `condMode=6`, additiv wie `kick.emb` beim
Bass: eine Datei ohne `condMode` hat keine `mode.emb`, ignoriert das Argument von `begin()` und ist
Tensor für Tensor das Modell, das sie vorher war (`export.py --check-pair` meldet für die alte
`melody.phosmdl` und die `bass.phosmdl` unverändert 0,000e+00). Der Komponist gibt den Modus der
Sektion durch, den die Form-Grammatik festlegt, bevor irgendeine Tonhöhe gezogen wird. Neu gelernt
haben es auch die Werkzeuge: `export.py` schreibt die Tabelle, führt `mode` durch alle sechs Zeilen
der Orakel-Fälle und **verweigert** weiterhin ein Urteil über eine Konditionierung, die es nicht
kennt; `confidence.py`, `memorisation.py` und `dataset.py` reichen den Label durch.

*Die Entscheidung, nach demselben Maß wie immer.* Testsatz unverändert: 102 Psy-Linien, 5 700 Token.
Dieselbe Trainingsrezeptur (`--extra trance,tracks,trancetracks`, 3 469 Linien) drei Seeds lang mit
und ohne Tabelle; ausgeliefert wird wie immer der Seed mit der besten **Validierung**.

| Seed | ohne Tabelle: Val / Test | mit Tabelle: Val / Test | Kontrolle (Label gemischt): Val / Test |
|---|---|---|---|
| 1 | 1,1663 / 1,1630 | 1,1767 / 1,1560 | 1,1595 / 1,1584 |
| 2 | 1,1404 / 1,1613 | **1,1362 / 1,1283** | 1,1609 / 1,1910 |
| 3 | 1,1586 / 1,1683 | 1,1657 / 1,1580 | -- |

**Die exportierte Datei gegen die ausgelieferte: 1,1279 gegen 1,1536, Differenz +0,0257 nats, 95-%-KI
des gepaarten Bootstraps [0,0093, 0,0423], P(Differenz ≤ 0) = 0,0012**, besser auf 68 von 102 Linien.
int8 kostet −0,03 % (1,1283 → 1,1279). Memorisierung gegen die 3 469 Trainingslinien: **0,00 % exakte
Taktkopien** gegen 8,75 % der echten Held-out-Loops, Achtnoten-Fenster im Abstand 0 **9,27 % gegen
20,44 %**; Positivkontrolle 74,03 % bzw. 82,32 %, das Maß schlägt also aus.

*Und die unbequeme Hälfte derselben Messung.* Der Label einer Held-out-Linie wird **aus deren eigenen
Noten** berechnet; er ist auf dem Testsatz keine Nebeninformation, sondern eine Zusammenfassung der
Antwort. `confidence.py --mode-eval` hält das auseinander: mit über die Testlinien **permutierten**
Modus-Zeilen misst dieselbe Datei **1,2099**, also 0,0563 nats *schlechter* als die ausgelieferte, mit
allen Zeilen auf Aeolisch festgenagelt 1,2006. Der ganze Gewinn ist der Label. Für die Aufgabe ist das
die richtige Messung -- der Komponist kennt den Modus der Sektion wirklich, bevor er zieht --, für die
Behauptung „das bessere Modell des Korpus" ist es die falsche, und beides steht jetzt in
`docs/MODEL_FORMAT.md` 3.

*Was es in der Erzeugung bewirkt -- der eigentliche Punkt der Runde.* Ohne jede Anhebung, nur durch
die Tabelle (Anteil der Noten in farbigen Sektionen auf den Farbtönen des gespielten Modus):

| Rolle | Korpus | modusblindes Modell | Modell mit `mode.emb` |
|---|---|---|---|
| Acid | 0,1372 [0,1137, 0,1671] | 0,039 | **0,168** |
| Lead | 0,1411 [0,1189, 0,1653] | 0,215 | 0,407 |
| Arp | 0,1569 [0,1453, 0,1685] | 0,196 | 0,213 |
| gesamt | 0,1539 [0,1442, 0,1640] | 0,131 | 0,229 |

Das Acid -- die Rolle, die am weitesten daneben lag und die einzige, für die eine Anhebung überhaupt
kalibrierbar war -- erreicht das Korpusziel jetzt **ohne jede von Hand gesetzte Konstante**. Dass die
Phrygisch-Zeile auch tut, was ihr Etikett sagt, ist direkt gemessen: bei identischem Kontext hebt sie
die kleine Sekunde von 0,01058 auf 0,08826 mittlerer Wahrscheinlichkeit, Faktor **8,34**, an **144 von
144** Positionen, nie in die Gegenrichtung.

**Und die Kennzahl, für die die Runde da war, erreicht ihr Ziel nicht.** Das Verhältnis gegen die
modusblinde Null geht über die geliehenen Sektionen von 0,47 auf **0,53** (Markov unverändert 0,48) --
statt gegen 1,0. Pro Rolle zeigt sich, warum: Arp 1,05, Lead 0,26, Acid 0,07. Die neu zugelassene
Klasse ist in Goas Entlehnungen meistens die **große Terz** (Phrygisch-Dominant über Phrygisch), und
die heben Tabelle wie Anhebung kaum an, während die kleine Sekunde -- die Klasse, die die obige
Farbtabelle dominiert -- in vielen dieser Sektionen schon zum eigenen Modus des Tracks gehört. Zwei
Nebenbefunde derselben Messung: der Gesamtanteil schießt mit 0,229 über das Korpusziel 0,1539 hinaus,
und das Lead mit 0,407 um fast das Dreifache über seines -- der Energiebogen hebt dieselben Töne in
derselben Rolle an und zählt jetzt doppelt. **Offen für eine nächste Runde**, ausdrücklich nicht in
dieser erledigt: die Farbe des Energiebogens gegen ein Modell nachkalibrieren, das den Modus kennt,
und ein Korpus-Label für die drei fast leeren Modus-Zeilen.

*Nachweise.* ctest 6/6 (Selbsttest, Cue-Prüfung, drei Vektorpfade, Quest-Wächter). Die drei
Vektorbauten schicken jede der sechs Zeilen von `mode.emb` durch denselben Kontext und geben ihre
Logits als Bits aus: alle drei Pfade Zeichen für Zeichen gleich, und keine zwei Zeilen liefern
dasselbe Ergebnis. `export.py --check-pair` meldet für die neue `melody.phosmdl` 0,000e+00, der
NumPy-Leser stimmt mit PyTorch auf 1,3e-05 überein, der C++-Leser auf 6,4e-06; der Prüfer
**verweigert** ein Urteil, wenn man ihm dieselbe Datei mit einer Referenz ohne `mode=` vorlegt, statt
eine Differenz zu melden.

*Mutationsrunde, sechs Fehler, jeder gefangen und zurückgenommen.* (1) Die elfte Tabelle wird nicht
in die Eingangssumme addiert -- Vektortest: Vorwärtspass 3,32 daneben, 15 von 15 Zeilenpaaren
identisch. (2) `begin()` wirft den Modus weg -- dasselbe Paar. (3) Der Kopfzeilenschlüssel ist
verschrieben (`condmode`) -- Selbsttest: Referenzvektoren 4,12 daneben, „trägt die Modustabelle"
meldet `condMode 0`. (4) `mode.emb` wird mit der falschen Zeilenzahl gesucht -- der Lader nennt Tensor
und Form („ist 6x192x1x1, die Kopfzeile verlangt 8x192"), 7 Prüfungen fallen. (5) Der Komponist gibt
statt des Sektionsmodus die Zeile 0 durch -- der Farbtonanteil des Acid fällt auf **0,0210**, also
noch unter die 0,039 des modusblinden Modells, weil die Aeolisch-Zeile die Farbe aktiv unterdrückt.
(6) `numpy_forward` vergisst `mode.emb` -- `--check-pair` meldet 4,123e+00 statt 0,000e+00. Nach jeder
Rücknahme sind die Dateien byteidentisch zur Vorlage, `git diff` unverändert.

*Dateien.* Neu: `Tools/train/mode.py`. Geändert: `Core/include/phos/Model.h` und `Core/src/Model.cpp`
(`condMode`, `mode.emb`, `begin(..., mode)`), `Core/src/Melody.cpp` (`DrawSource::mode`, die drei
Maker geben den Modus der Sektion durch), `Core/data/melody.phosmdl` und `.ref.txt` (das neue Modell),
`docs/MODEL_FORMAT.md` (2, 3, 4, 5, 7), `Tools/train/dataset.py` (`N_MODE`, `mode_of`, `encode`),
`Tools/train/models.py` (`n_mode`, elfte Tabelle), `Tools/train/train.py` (`--mode-cond`,
`--mode-shuffle`), `Tools/train/export.py`, `Tools/train/confidence.py` (`--mode-eval`),
`Tools/train/memorisation.py`, `Tests/selftest.cpp` (`testModeColour` neu geschrieben),
`Tests/vectest.cpp` (jede Zeile von `mode.emb` durch denselben Kontext, bitgleich auf allen drei
Pfaden).

**16.09.2026, Bass-Rhythmus: die 61,5-Prozent-Lücke.** Die Bass-Runde von Phase 8 hat den Bass zur
vierten gelernten Rolle gemacht und **nur seine Tonhöhe** gelernt; sein Anschlagsmuster blieb bei den
fünf fest verdrahteten Familien aus `Core/include/phos/Patterns.h`. Dieselbe Runde hat auch
aufgeschrieben, warum sie es nicht anfasste: `Engine::firstSlotSeconds` leitet die Schwanzgrenze des
Kicks und den Phasenschluss aus `firstBassSlot(pattern_)` her, und die Muster-Nummer kommt über ein
`Override` auf `compose.bass_pattern` — ein gelernter Takt hat keine Nummer in `kBassPatterns`. Diese
Runde schließt die Lücke. Alle Zahlen unten sind neu gemessen (`Tools/corpus/bass_rhythm.py`, neu),
nicht aus der vorigen Runde übernommen.

**Messung 1: Was der Korpus spielt.** 1 551 Bass-Linien, 11 507 Takte: **641** verschiedene
16-Bit-Taktmuster, Plug-in-Entropie **6,059 bit/Takt** — beides auf die dritte Stelle gleich der
vorigen Runde, Korpus und Extraktion sind also dieselben. Neu daneben: **64,4 %** der Takte haben
**keinen** Anschlag auf einem Kick-Schritt, und dieser „saubere" Unterraum allein hat 187 Muster und
4,171 bit/Takt. Je Rolle (der Korpus trägt kein Stil-Label): Acid 81 Muster / 2,553 bit / 70,9 %
sauber, Lead 236 / 7,087 / 11,1 %, Arp 104 / 2,131 / 5,9 %, Bass 641 / 6,059 / 64,4 %. Je Pack, dem
Nächsten an einem Stil, was der Korpus hat: `Star Samples` 10 305 Takte / 613 Muster / H 6,291,
`EMP` 478 / 37 / 3,033, `TOTAL_MIDI` 396 / 19 / 2,511, `PSYTRANCE MIDI BUNDLE` 328 / **3** / 0,107 —
oben steht überall `.xxx.xxx.xxx.xxx`.

**Messung 2: Eine Linie ist eine Figur, kein Taktband.** 1 515 Linien mit mindestens zwei Takten,
11 471 Takte: verschiedene Taktmuster je Linie im Mittel **1,34**, Median **1**, 90. Perzentil 2. Ein
Takt gleicht dem Takt davor in **79,0 %** der Paare und dem häufigsten Takt seiner Linie in
**86,8 %** der Takte. Diese zweite Zahl baut den Generator: ein Modell, das acht unabhängige Takte
zieht, wäre kein Psytrance-Bass, sondern Brei.

**Messung 3: Welches Modell.** Ehrlicher Schnitt über Loop-Gruppen, derselbe Schnitt-Seed wie in der
Tonhöhenrunde, 15 587 / 1 495 / 2 204 Takte; das Mischgewicht wird auf dem Validierungs-, nie auf dem
Testschnitt angepasst. Kreuzentropie in bit je Takt:

| Modell | alle Takte | nur der saubere Unterraum |
|---|---|---|
| Gleichverteilung über 2^16 | 16,000 | 16,000 |
| Nachschlagetabelle + Gleichverteilungs-Rückfall | 7,899 (482 Muster, **21,6 %** der Testtakte nie gesehen) | 5,458 (139 Muster, 13,4 % nie gesehen) |
| parametrische Kette, Schritt + (1) | 13,622 | 10,293 |
| parametrische Kette, Schritt + (1, 2, 3) | 10,695 | 7,509 |
| parametrische Kette, Schritt + (1, 4) | 12,038 | 9,517 |
| parametrische Kette, Schritt + (1, 2, 3, 4, 8) | 7,793 (330 Kontexte) | 5,201 (151 Kontexte) |
| **Mischung aus beiden** (w = 0,480 bzw. **0,485**) | **7,449** | **4,993** |

Die Mischung, die in der vorigen Runde niemand gemessen hatte, schlägt beide Bestandteile — um 0,34
bzw. 0,21 bit je Takt — und tut es aus dem Grund, aus dem sie vorgeschlagen war: die beiden scheitern
an **verschiedenen** Takten, die Tabelle an den nie gesehenen, die Kette an den scharf wiederholten.
Geglättet ist die Kette mit dem Krichevsky-Trofimov-Schätzer (add ½ je binärem Ausgang, die
minimax-optimale Add-Konstante für ein binäres Alphabet; Krichevsky und Trofimov, „The performance of
universal encoding", IEEE Trans. Inf. Theory 27(2), 1981); die Tabelle hat keine eigene Glättung, die
Kette **ist** ihre Glättung. Plug-in-Entropien stehen überall nur als Beschreibung einer Stichprobe,
nie als Vergleich zwischen Modellen: über 2^16 Muster aus zehntausend Takten sind sie um rund
(K−1)/(2N ln 2) nach unten verzerrt (Miller 1955; Paninski, Neural Computation 15(6), 2003).

**Warum der Kick-Schritt nicht im Alphabet ist.** `Kick::constrainTail` und `Kick::setPhaseTarget`
schalten sich beide ab, wenn der Slot null oder kleiner ist. Eine Bassnote **auf** dem Kick würde die
Schwanzgrenze und den Phasenschluss also stillschweigend abschalten — und sie ist genau der
Maskierungsfall, für den die Tiefenregel (5.2) da ist. Der Generator arbeitet deshalb im sauberen
Unterraum. Das ist keine Notlösung: 64,4 % der Korpustakte liegen ohnehin darin, und der Preis steht
in der Tabelle oben statt in einer Annahme.

**Was gebaut wurde.** `compose.bass_rhythm` (Choice `Pattern`/`Corpus`, Vorgabe **Pattern**, ans Ende
der Compose-Tabelle gehängt). Zwei Zähltabellen in `Core/src/CorpusTables.cpp`, aus
`Tools/corpus/bass_rhythm.py` über `build_corpus.py --bass-rhythm-only` erzeugt und **nur auf dem
Trainingsschnitt** angepasst: 139 Taktmuster und 151 Kettenkontexte, zusammen **2 924 Byte** — für
die Quest keine Frage. Kontext-Schlüssel sind in ein `uint16_t` gepackt (Basis drei je Nachbar:
abwesend/still/angeschlagen, dann der Schritt in Basis sechzehn), beide Tabellen werden per Bisektion
gefunden wie `CorpusGram`. `--bass-rhythm-only` schreibt **nur** diesen Block neu, weil die
melodischen Tabellen darüber einen vollen Gang durch `M:\Midi` kosten.

**Die Freigabe: der erste Slot als Steuerereignis.** Neu ist `ControlEvent::Kind::BassSlot` in
`Score.h`: der Komponist schickt **je Beat, auf dem Beat**, den ersten klingenden Bass-Slot dieses
Beats in Beats. Auf dem Beat, weil Steuerereignisse bei gleichem Beat **vor** den Noten zugestellt
werden — das ist der späteste Zeitpunkt, an dem der Wert für den Kick dieses Beats noch gilt, und der
früheste, an dem er nicht mehr der des vorigen Beats ist. `Engine::firstSlotSeconds` nimmt den
gesendeten Wert, solange einer positiv ist, und sonst weiter `firstBassSlot(pattern_)`. Ein Takt aus
einer Pattern-Familie schickt **ein** löschendes Ereignis (Wert −1) je Takt; ohne das könnte der
Komponist den Knopf nicht mitten im Lauf zurückstellen, weil die Engine „noch nichts geschickt" nicht
von „nichts mehr geschickt" unterscheiden kann. Ein Beat ohne Bassnote meldet eine Viertel-Beat — den
engsten Slot, den das Sechzehntel-Raster hergibt —, damit ein ruhender Bass den Kick nicht länger
klingen lässt als die Beats um ihn herum. Jeder gesendete Wert liegt damit in [0,25; 0,75]: der Kick
klingt nie über den nächsten Kick hinaus, und die Grenze ist nie enger als bei Rolling.

**Die Gate-Grenze und die Release-Untergrenze.** `gateLimit` rechnete aus `shortestBassSlot(pattern)`;
sie rechnet jetzt aus dem **wirklich gezogenen** kürzesten Abstand der beiden Phrasen
(`gateLimitSlot`). Dieselbe Formel auf einer gemessenen Zahl — und sie kann nur lockerer werden: eine
Note endet am nächsten Anschlag oder am nächsten Kick, beides Vielfache eines Sechzehntels, also ist
der kürzeste mögliche Abstand **ein** Sechzehntel, genau der von Rolling.

**Damit der Roll rollt.** Eine Phrase ist ein **Heimtakt** plus seltene Ausflüge, beide Zahlen aus
Messung 2: der Heimtakt ist mit Wahrscheinlichkeit `stray` aus dem Modell gezogen und sonst die Maske
der Pattern-Familie des Tracks; jeder der acht Takte wiederholt den Heimtakt, außer eine zweite Münze
(1 − 0,868) sagt etwas anderes. Beide Münzen werden mit `compose.bass_variation` × `hatDensity` des
Stilprofils skaliert — dem einzigen rhythmischen Dichte-Multiplikator, den ein `StyleProfile` trägt
(Progressive 0,9 … Hi-Tech 1,2). Bei Bass Variation 0 ist der gezogene Rhythmus **exakt** der der
Pattern-Familie, Note für Note; das prüft der Selbsttest. Ein eigenes Feld auf `StyleProfile` wäre
ehrlicher — `Form.h` gehörte in dieser Runde einem anderen Agenten.

**Was dabei herauskommt.** 24 Seeds × 96 Takte, je einmal mit `Pattern` und einmal mit `Corpus`
exportiert, **2 216 Takte** je Seite, mit denselben Funktionen vermessen wie der Korpus oben (die
Zahlen der Pattern-Seite weichen leicht von den 7 Mustern / 1,062 bit der vorigen Runde ab, weil das
dort fünf Stile × acht Seeds × 256 Takte waren; beide Seiten hier sind mit **demselben** Protokoll
gemessen):

| | `Pattern` | `Corpus` | Korpus |
|---|---|---|---|
| verschiedene Taktmuster | 6 | **25** | 641 |
| Plug-in-Entropie des Taktmusters | 0,595 bit | **2,293 bit** | 6,059 bit |
| Anschlagsdichte | 0,691 | **0,604** | 0,525 |
| Anteil der held-out Korpustakte, die überhaupt spielbar sind | 35,5 % | **52,0 %** | — |
| … davon im sauberen Unterraum (der Decke des Generators) | 51,2 % | **75,0 %** | — |
| als Wahrscheinlichkeitsmodell der held-out Takte (+ Gleichverteilungs-Rückfall) | 11,506 bit | **10,046 bit** | 7,449 (die Mischung) |
| häufigster Takt eines Renders, Anteil an dessen Takten | 89,7 % | **87,7 %** | 86,8 % (je Korpuslinie) |

Die 61,5-Prozent-Lücke ist damit auf **48,0 %** gefallen, und 30,7 der verbleibenden Prozentpunkte
sind die Takte mit Anschlag auf dem Kick, die der Generator **mit Absicht** nicht spielt: innerhalb
dessen, was er spielen darf, ist die Lücke von 48,8 % auf **25,0 %** gefallen. Die musikalisch
wichtigste Zeile ist die letzte: der häufigste Takt eines Renders trägt immer noch 87,7 % von dessen
Takten — einen Prozentpunkt neben echten Basslinien und weniger starr als die Pattern-Familien, die
bei 89,7 % lagen. Der Bass kann jetzt mehr, und er tut es nicht die ganze Zeit.

**Was nicht kaputtgehen durfte.**

| | Nachweis |
|---|---|
| Pattern-Modus unverändert | drei Renders zu 96 Takten (Seeds 1–3) sind **byte-gleich** mit denselben Renders aus dem Basis-Commit 126ad1f, gebaut in einem eigenen Worktree; dazu 24 MIDI-Exporte byte-gleich |
| Kick-Phasenschluss | akustisch am **ersten Anschlag jedes Beats** gemessen, während dieser Anschlag von Beat zu Beat wandert: 142 BPM **+10,8°** über 20 Anschläge auf zwei verschiedenen Sechzehnteln (Familien +5,2° auf einem), 148 BPM **−2,8°** (Familien +1,2°). Mit eingefrorenem Slot läge eine Note auf dem zweiten Sechzehntel um rund 4,9 Kick-Zyklen daneben; die Mutationsrunde zeigt es |
| Schwanzgrenze | jeder gesendete Slot in [0,25; 0,75] Beats, also nie lockerer als der nächste Kick und nie enger als Rolling |
| Tiefenregel, Gate, Release | kein gezogener Takt setzt eine Note auf einen Kick-Schritt, keine Note klingt über den nächsten Kick; kürzester Abstand nie unter einem Sechzehntel; die Gate-Grenze folgt den gezogenen Masken |
| Determinismus, Takt allein = Takt in Folge | eigener Salt, Phrase nur aus dem Track-Seed; ein allein komponierter Takt trägt seine vier eigenen Slot-Ereignisse |
| Blockgrößen-Unabhängigkeit | ein Render mit Blöcken 64 und 256 ist bei einem Ereignis **je Beat** bitgleich (1 271 172 Samples) |
| −9 LUFS / −1 dBTP | ein 32-Takt-Render mit `Corpus`: integriert −9,5 LUFS, True Peak −1,00 dBTP |

*Offen und bewusst.* Die Triplet-Familie hat kein Bild auf dem Sechzehntel-Raster; mit `Corpus` spielt
ein Triplet-Track die Skip-Maske. Das ist dieselbe Näherung, die `bassSlotStep` dem Tonhöhenmodell
schon immer zumutet, und sie steht im Kommentar der Auswahl. Der ganz leere Takt (3,2 % aller
Korpustakte, 5,4 % der sauberen) wird nie gezogen — leere Takte macht die Form, nicht das
Anschlagsmodell. Und die Wahl von `hatDensity` als Streuungsmaß ist eine
Anleihe: das richtige wäre ein eigenes Feld auf `StyleProfile`.

**16.09.2026, Arrangement-Dynamik: Snare-Rampe, Acid-Fahrten, Bewegung im Panorama**

Drei Punkte aus einem Review des Nutzers. Alle drei sind gebaut, jeder mit der Zahl, die über ihn
entscheidet, und einer davon liefert weniger, als das Review erwartet hat — das steht unten mit der
Herleitung, warum. Ein vierter Punkt des Reviews, die Vorher-Leere vor dem Drop, **war schon gebaut**
(`kPdbVariantNames`, vier Varianten; `testSectionRules` misst Beat 4 des Pre-Drop-Breaks 57,9 dB unter
einem Kern-Beat) und ist unangetastet geblieben.

### 1. Die Snare-Rolle, die wirklich hebt

*Vorher.* Die Rolle der letzten vier Buildup-Takte wird schneller (1/8 → 1/16 → 1/32) und lauter, mehr
nicht. Gemessen auf der Snare allein (Seed 1, 96 Takte, nur `perc6` aktiv, `--solo perc`): über die
vier Takte steigt ihr Schwerpunkt um **5,4 Halbtöne** und ihre Bandbalance 2–8 kHz gegen 150–500 Hz um
**1,55 dB** — beides Nebenwirkung der steigenden Dichte, keine Gestik.

*Gebaut.* Die Tonhöhe der Rolle ist eine **Rampe über alle vier Takte**, in die Noten geschrieben
(`Rhythm.cpp`, `kRollSemitones = 12`): Verschiebung = round(12·u) mit u der Position in der Rolle. Kein
Zufall pro Anschlag, sondern eine Funktion der Position — der MIDI-Export trägt sie mit, und zwei
Buildups desselben Seeds heben identisch. Dazu ein neuer Lane-Parameter `perc.cut_track` (0..2,
angehängt): der Tiefschnitt der Lane folgt der Verschiebung des Anschlags mit dem Exponenten
`cut_track`. Der nützliche Wert ist **größer als eins**, und das ist der Punkt: bei genau 1 wandern
Eckfrequenz und Note gemeinsam, das transponiert die Lane und dünnt sie *nicht* aus; bei 2 steigt der
Hochpass zwei Oktaven, während die Note eine steigt, der Grundton der Snare landet unter ihrem eigenen
Filter, und übrig bleibt das Rauschen. Standard der Snare: 2. Der harte Schnitt am Ende existierte
schon (der PDB räumt Beat 4).

*Nachher, dieselbe Messung.*

| Snare allein, über die vier Rollentakte | Schwerpunkt | 2–8 k gegen 150–500 Hz |
|---|---|---|
| vorher | +5,39 Halbtöne | +1,55 dB |
| nur die Tonhöhen-Rampe (`cut_track=0`) | **+2,23** | +1,03 |
| Rampe und mitlaufender Tiefschnitt (Standard) | **+8,93** | **+8,92** |

**Ein Zwischenergebnis, das erklärt werden muss:** die Tonhöhen-Rampe *allein* macht die Rolle nicht
heller, sie macht sie sogar etwas dumpfer. Der Grund ist, wo das Oberende einer Snare sitzt: im
Rauschanteil, nicht im Ton. Eine Transposition des Tons verschiebt den Körper und lässt das Rauschen
stehen. Was die Rolle ausdünnt, ist der mitlaufende Hochpass; was die Tonhöhe beiträgt, ist die
Gestik — steigende Tonhöhe zusammen mit steigender Lautstärke und Rate ist das kulturübergreifende
Signal für steigende Erregung (Huron, „Sweet Anticipation", 2006, Kap. 12; Juslin und Laukka,
Psych. Bull. 2003) — und die Steuergröße, aus der der Hochpass seine Bewegung nimmt. Beides gehört
zusammen, keins von beiden trägt allein.

*Der dritte Strang, der Hallweg.* Über die letzten vier Takte eines Buildups fährt `mix.perc_hall` auf
**0,30** und wird am Drop auf null geschnitten (`sectionAutomation` in `Form.cpp`; der Schnitt ist das
erste Ereignis der folgenden Sektion, liegt also exakt auf dem Drop). Ein Send, der in den Drop hinein
offen bliebe, würde genau den Transienten verschmieren, für den die ganze Geste existiert.

*Und die Zahl, die entscheidet* — der Kontrast vom letzten Buildup-Takt in den Drop, in der ganzen
Mischung, `Tools/ref_arrange.py --contrast`:

| 96 Takte, PDB auf Takt 88 | Lautheit PDB → Drop | Schwerpunkt-Verhältnis | Hub der Rolle (Schwerpunkt) |
|---|---|---|---|
| Seed 1, vorher | +2,49 dB | 0,30× | 3,58× |
| Seed 1, nachher | **+2,69** | **0,26×** | **4,08×** |
| Seed 3, vorher | +1,63 | 0,36× | 4,16× |
| Seed 3, nachher | **+1,90** | 0,35× | 4,14× |

**Ehrlich dazugesagt:** in der ganzen Mischung sind das zwei bis drei Zehntel Dezibel. Die Snare ist
dort ein Instrument unter acht, und der Buildup lebt auch von Hats, Riser und Gate. Auf der Lane
selbst ist die Geste groß (8,9 statt 1,6 dB Bandverschiebung), im Summensignal ist sie eine Nuance.
Wer mehr will, dreht `perc6.cut_track` oder `kRollSemitones` — beides steht offen.

### 2. Makro-Fahrten auf der Acid

*Vorher.* `Composer::sectionControls` schreibt pro Sektion **eine** Filter-Zielgröße und rampt über die
ganze Sektion dorthin. In einem 32-Takt-Kern ist das eine Gerade: der Basis-Cutoff steht 32 Takte
lang. Gemessen (Acid solo, Seed 1, Takte 16–80): Bewegung *zwischen* den Takten Spanne 0,173 Oktaven
bei einer Streuung von 0,045; Bewegung *innerhalb* eines Taktes — Akzent-Sweep und Notenhüllkurve —
Streuung 0,478 Oktaven. Die Makro-Ebene war leer.

*Gebaut.* `sectionAutomation()` in `Form.cpp` (neu, von `Composer::sectionControls` mit einer Zeile
aufgerufen) schreibt eine Kette von Rampen auf `acid.cutoff` und `acid.resonance`:
- Periode **8 oder 16 Takte**, aus dem eigenen Seed der Sektion gezogen; die Sektion ist eine sperrbare
  Einheit, ihre Fahrt hängt also an ihrem Seed und nicht am Track.
- Form **asymmetrisch**: drei Viertel der Periode aufwärts, ein Viertel zurück. Das ist, was eine Hand
  am Knopf tut, und die Kontur, die Huron (2006) als Erregungsverlauf eines Aufbaus beschreibt; ein
  symmetrisches Dreieck klingt nach LFO.
- Resonanz fährt mit, knapp zwei Drittel so weit, weil auf einer echten 303 beide Knöpfe zusammen
  gedreht werden.
- Ein **Breakdown taucht** stattdessen: über die erste Hälfte hinunter, über die zweite zurück.
- Alles ist ein Zuschlag **auf** den Sektionsbogen der Phase 5, nie sein Ersatz: `base0` und `base1`
  kommen vom Aufrufer, die Fahrt wird von der Geraden dazwischen aus gemessen, und das letzte Segment
  endet exakt auf ihr.

*Warum sie den Akzent-Sweep nicht stören kann.* Der Kondensator des Akzents hat 150 ms (`kSweepTau`),
seine Bewegung lebt also innerhalb einer Sechzehntel (103 ms bei 145 BPM). Die kürzeste Rampe der
Fahrt ist zwei Takte, **3,3 s — ein Faktor 22**. Die beiden sind in der *Rate* getrennt, nicht im
Betrag, und genau so trennt die Messung sie auch: `Tools/ref_arrange.py --ride` faltet die
Sechzehntel-Schwerpunkte auf ihren Takt und meldet beide Streuungen getrennt.

*Gemessen, Acid solo, Seed 1, Takte 16–80:*

| | vorher | nachher |
|---|---|---|
| Bewegung **innerhalb** eines Taktes (Akzent, Hüllkurve) | 0,478 oct sd | **0,478** oct sd |
| Bewegung **zwischen** den Takten | 0,045 oct sd, Spanne 0,173 | **0,063** oct sd, Spanne **0,234** |
| dieselbe aus den gefalteten Sechzehnteln | 0,069 oct sd | 0,078 oct sd |
| Helligkeit 1–8 k gegen 0,1–1 k | 0,51 dB sd, Spanne 2,03 | **0,76** dB sd, Spanne **2,78** |

Die Makro-Ebene wächst um 40 % in der Streuung und 35 % in der Spanne, die Bewegung innerhalb eines
Taktes steht auf drei Nachkommastellen still. Genau das war die Frage des Reviews — nicht „ist mehr
Bewegung da", sondern „stört das Neue das Alte" —, und die Antwort ist gemessen: nein.

**Die Falle dieser Messung, und sie ist die interessanteste des Abschnitts.** Der Leistungs-Schwerpunkt
einer resonanten 303-Linie ist ein *schlechter* Detektor für ihren Cutoff: die Energie sitzt im
Grundton, wo auch immer die Ecke steht. Gegengeprobt mit drei statischen Renders (`acid.cutoff` = 393,
650 und 1093 Hz, also ±0,56 Oktaven um den Standard): die Takt-Schwerpunkte gehen von 315–332 über
360–381 auf 417–445 Hz, **±0,56 Oktaven Knopf ergeben ±0,21 Oktaven Schwerpunkt**. Die Fahrt liefert
also, was sie verspricht; die Anzeige ist um den Faktor 2,7 gedämpft. Wer eine Filterfahrt messen will,
misst die Bandbalance über dem Grundton — `ref_arrange.py --ride` meldet sie deshalb jetzt mit.

*Und der Pegelangleich hält.* Die Lautheitsprobe des Composers misst zwei Takte mit den **Knöpfen**,
nicht mit der Automation; eine Fahrt mit Mittelwert würde jeden Track verstimmen. Sie hat keinen: die
Fahrt schwingt *um* die Gerade, von einer halben Auslenkung darunter zu einer halben darüber, und eine
erhobene Kosinus-Rampe ist um ihren eigenen Mittelpunkt wertsymmetrisch. Übrig bleibt das letzte
Segment, das auf die Gerade zurückkehrt statt unter sie: Mittelwert **+0,006** normiert auf eine Fahrt
von 0,12, also 0,04 Oktaven. Gemessen im Selbsttest, nicht behauptet.

### 3. Tempo-synchrones Auto-Pan auf der Percussion

Der Teil mit dem meisten Rechenweg, weil das Review hier eine Größe nennt, die ein Panoramaregler
allein **nicht** erreichen kann, und der Weg dorthin entscheidet, wie gebaut wird.

*Die Aufgabe.* Die Aufnahmen sind breit bei gleichem Pegel: Pegeldifferenz zwischen den Kanälen über
85-ms-Fenster **1,5 bis 2,2 dB rms** in jedem Band, Luftband 1,74. Nach der Breitenrunde steht dieses
Kit bei **3,89 dB** (vier Seeds, ganze Renders) und die Seite/Mitte bei −9,14 gegen −8,45 der
Aufnahmen. Der *Betrag* stimmt, der *Charakter* nicht.

*Die Schranke, hergeleitet statt angenommen* (`ref_arrange.py --pan-bound`). Für eine **mono** Quelle
hinter einem Konstantleistungs-Panorama sind beide Zahlen Lesungen desselben Winkels; über ein Fenster,
das kurz gegen den LFO ist, liest die Kurzfenster-Messung den Momentanwert. Minimiert man E[ILD²] unter
der Nebenbedingung E[cos(p·π/2)] = m, ist das Infimum √((1−m)·2·(20/ln10)²) — bei Seite/Mitte −9,35 dB
sind das **5,60 dB rms**. Ein *langsames* Panorama kann die beiden Größen also nicht gegeneinander
tauschen. Dass das Kit trotzdem unter dieser Schranke liegt, ist kein Widerspruch: zwölf unkorrelierte
Lanes mitteln sich im Fenster, und genau dieser Mittelungsgewinn ist der Hebel.

*Das Modell vor der ersten Codezeile.* Ein Leistungsmodell des Luftbands (jede Lane eine unkorrelierte
Quelle mit Hüllkurve, Konstantleistungs-Panorama, dieselben Pegel, Positionen und Dichten wie das
Standard-Kit) reproduziert den Ausgangsstand (ILD 4,42 dB, Seite/Mitte −8,09 gegen gemessene 5,3 und
−10,1) und beantwortet drei Entwurfsfragen, bevor sie Code kosten:

| Entwurf | ILD 85 ms | Seite/Mitte |
|---|---|---|
| statisch (Stand der Breitenrunde) | 4,42 dB | −8,09 |
| Weyl-Phasen (goldener Schnitt), 3 Takte | **4,99** | −8,24 |
| Weyl-Phasen, 3/16 Takt | 4,46 | −8,20 |
| **zwei balancierte Phasen, 3/16 Takt** | **3,59** | −8,23 |
| zwei balancierte Phasen, 2/16 Takt | 3,00 | **−6,73** (1,5 dB zu breit) |

Drei Befunde, und alle drei stehen im Code:

**(a) Zwölf gespreizte Phasen sind schlechter als zwei.** Was die 85-ms-Messung liest, ist die
leistungsgewichtete *mittlere* Position der Lanes, die im Fenster klingen. Eine Weyl-Folge über den
Kreis — die naheliegende, „hübsche" Wahl — lässt diese Summe als Zufallsweg stehen; sie erhöht die
Pegeldifferenz sogar (4,99 gegen 4,42). Die Lanes werden deshalb in **zwei gegenphasige Gruppen**
geteilt, durch eine gierige absteigende Gewichtszerlegung (Graham, SIAM J. Appl. Math. 17, 1969), so
dass Σ w_l·p_l(t) stehen bleibt, während jede Lane fährt. Beim Standard-Kit bleiben **0,031 von 3,548**
bewegter Gewichtssumme übrig.

**(b) Drei Sechzehntel sind die richtige Periode, und zwar aus Arithmetik.** Eine Lane auf dem
Sechzehntel-Raster tastet ihren eigenen LFO bei einer Periode von 3/16 Takt an genau **drei Phasen**
120° auseinander ab. Drei solche Punkte tragen das erste *und* das zweite Moment eines Sinus exakt
(Σcos = 0, Σcos² = 3/2) — die Breite, die die Breitenrunde kalibriert hat, bleibt für das Material,
das wirklich spielt, **exakt** erhalten und nicht nur im Mittel. Zwei Sechzehntel täten das nicht: sie
tasten bei 0 und π ab, E[p²] = 2p0², und das Kit wäre 1,4 dB zu breit — im Modell nachgemessen. Die
Periode fällt erst nach drei Takten wieder mit dem Takt zusammen, was die Präzession ist, die das
Review wollte.

**(c) Das Gesetz muss die Breite bei jeder Tiefe halten.** p(t) = p0·[√(1−D²) + √2·D·cos(…)]. Die
beiden Faktoren sind genau das Paar, für das E[p²] = p0² bei *jeder* Tiefe gilt. Ein Gesetz mit (1−D)
statt √(1−D²) stimmt nur bei 0 und 1 und macht das Kit dazwischen bis zu 2,6 dB schmaler.

*Gebaut* in `Perc.cpp` und `PercKernel.h`: das Verstärkerpaar (gL, gR) wird pro Sample **gedreht**,
nicht neu gerechnet. Eine Konstantleistungs-Position ist der Punkt (cos θ, sin θ) auf dem Einheitskreis,
Bewegung ist Drehung, und die Summe der beiden Kanalleistungen ist unter einer Drehung exakt invariant —
jede Bandbalance- und Lautheitszahl der Mischungsrunde summiert Kanalleistungen und **kann** sich also
nicht bewegen. Das ist Algebra; gemessen ist zusätzlich, dass die Arithmetik sie nicht kaputt macht
(Newton-Schritt auf das (cos d, sin d)-Paar, schlechteste Abweichung von L²+R² gegen dieselbe Lane im
Stand **5,5e−7 relativ**, bei einer Lane auf 0,9 Auslenkung und zwei Sekunden Ausklang, also im
schlechtesten Winkel, den das Feld zulässt). Bei Tiefe 0 ist die Drehung bitweise die Identität:
ein Render mit `pan_depth=0` auf allen Lanes ist **bitgleich** zum Stand vor dieser Runde (md5 geprüft).
Neue Parameter: `perc.pan_depth` (0..1, Standard 0 in der Tabelle, 1 auf den zehn nicht-mittigen Lanes
des Standard-Kits) und `perc.pan_bars` (Standard 0,1875 = drei Sechzehntel). Clap und Snare stehen
weiter in der Mitte — der Backbeat ist, was eine Psytrance-Mischung dort verankert —, und eine Lane
ohne Position fährt auch nicht, weil der Hub proportional zu p0 ist.

*Gemessen.* Auf dem Kit allein (Selbsttest, eigenes Sechzehntel-Muster, Luftband über zwei Butterworth
vierter Ordnung bei 6 kHz):

| Kit allein, Luftband | vorher | nachher | Aufnahmen |
|---|---|---|---|
| **Pegeldifferenz 85 ms** | **5,23 dB rms** | **3,19** | **1,74** |
| Seite/Mitte | −8,66 dB | −8,80 | −8,45 |
| Korrelation | +0,794 | +0,769 | +0,751 |

In der ganzen Mischung, vier Seeds zu acht Minuten, je vier Fenster zu 45 s (`ref_width.py --short`,
das Verfahren der Breitenrunde):

| Band | Pegeldiff. vorher | nachher | Seite/Mitte vorher | nachher | ρ vorher | nachher |
|---|---|---|---|---|---|---|
| tief | 0,77 | 0,77 | −43,84 | −44,02 | +1,000 | +1,000 |
| low-mid | 2,33 | 2,34 | −10,71 | −10,67 | +0,843 | +0,842 |
| Mitten | 2,24 | 2,22 | −6,43 | −6,43 | +0,629 | +0,629 |
| Präsenz | 2,91 | **2,68** | −7,62 | −7,59 | +0,703 | +0,698 |
| **Luft** | **3,89** | **3,51** | **−9,14** | **−8,97** | **+0,843** | **+0,772** |

**Das Urteil, ehrlich.** Die Bewegung geht in die richtige Richtung und ist keine Kosmetik: auf dem Kit
schließt sie **58 %** der Lücke zwischen 5,23 und den 1,74 der Aufnahmen, in der Mischung **18 %**,
und die Korrelation im Luftband geht von +0,843 auf +0,772 bei einem Referenzwert von +0,751 — das ist
die Größe, die sagt „die Breite kommt aus Dekorrelation", und sie trifft die Aufnahmen jetzt fast. Die
1,7 dB selbst werden **nicht** erreicht, und nach der Schranke oben werden sie von einem Panoramaregler
auch nicht erreicht: der Rest ihrer Breite ist Stereohall und doppelt eingespieltes Material, das ein
Kit aus zwölf Einzel-Lanes nicht hat. Seite/Mitte, Korrelation und Tiefenregel bewegen sich dabei um
höchstens 0,17 dB.

*Mono-Verträglichkeit.* Was eine Bewegung im Panorama kaputt machen könnte, ist die Summe zu Mono. Sie
kostet unverändert **+0,49 dB** Luft und **+0,72 dB** Präsenz gegen das Kickband (vorher +0,49 / +0,72;
die Aufnahmen verlieren 1,2 dB Präsenz). Was sich in der Summe aufhebt, war nie da — eine Drehung
erzeugt keine Gegenphase.

### Was nicht gebaut wurde, und was es kosten würde

- **Auto-Pan auf den Arps.** Das Review wollte es; die Messung sagt, der Mangel sitzt im Luftband, also
  im Kit. Der Arp ist eine `Poly`-Instanz an einem Mixer-Strip in `Engine.cpp`, der in dieser Runde
  einem anderen Agenten gehörte. Die Änderung wäre: `poly.width` bleibt, was es ist, und der Strip
  bekommt vor dem Summieren dieselbe Drehung wie eine Perc-Lane — ein Phasor je Instanz in
  `Engine::renderSegment`, Winkel `panA·cos(phase) + panB` aus zwei neuen `poly`-Parametern, und
  `Engine::updateParams` rechnet die Rate aus `tempo_.bpmAt()`. Zehn Zeilen, aber in einer fremden
  Datei.
- **`PercKit::setTempo()` ist gebaut und wird von niemandem gerufen.** Der Aufrufer wäre eine Zeile in
  `Engine::updateParams` neben Tonart und Skala: `perc_.setTempo(bpmNow)`. Bis dahin läuft die Fahrt auf
  145 BPM statt auf dem Tempo des Tracks. Die Folge ist gemessen klein — ein Track wandert höchstens
  ±4 BPM (`compose.tempo_range`), die Periode ist also um höchstens 2,8 % daneben — und wird erst bei
  einem Set weit weg von 145 BPM hörbar.

### Die Kalibrierung steht

Vier Seeds zu acht Minuten, Mediane, Kanalleistungen summiert:

| Größe | vorher | nachher |
|---|---|---|
| Bandbalance low-mid | −6,54 | −6,56 |
| Bandbalance Mitten | −6,26 | −6,24 |
| Bandbalance Präsenz | −10,17 | −10,13 |
| Bandbalance Luft | −13,92 | −13,79 |
| Terzkurve, Abstand zum Median | 1,94 | **1,94** dB rms |
| Lautheit integriert | −9,10 | **−9,10** LUFS |
| True Peak, exakt | −0,970 | −0,968 dBTP (schlechtester −0,963) |
| LRA | 6,2 | 6,2 LU |

Nichts davon ist Zufall: das Panorama ist konstantleistungs-normiert (Algebra, siehe oben), die
Acid-Fahrt hat Mittelwert ~0, und die Snare-Rampe betrifft vier Takte je Buildup. Der **Standard-Render
ändert sich** trotzdem — er soll es —, und die Zahl dazu steht oben: Luft-Seite/Mitte +0,17 dB,
Pegeldifferenz −0,38 dB, Lautheit 0,00, True Peak +0,002 dB.

### Prüfungen

Ein neuer Abschnitt `testArrangeDynamics`, **14 Prüfungen**, jede gegen einen anderswo hergeleiteten
Wert, und jede erst gegen den unveränderten Stand fallen gesehen:

| Prüfung | gegen den alten Stand | jetzt |
|---|---|---|
| das Schwunggesetz lässt die rms-Position bei p0 | — | 7,8e−16 schlechteste Abweichung, bei vier Tiefen und drei Positionen |
| drei Sechzehntel tragen beide Momente exakt, zwei nicht | — | E[p²] = 0,2025 = p0² gegen 0,4050 = 2p0² |
| die zwei Phasengruppen balancieren das Kit | — | Rest 0,031 von 3,548 bewegter Gewichtssumme |
| eine fahrende Lane trägt dieselbe Leistung wie eine stehende | — | 5,5e−7 relativ, Schranke 1e−5 |
| die Bewegung ist blockgrößenunabhängig | — | 0 von 48000 Samples bei Blöcken 64 / 7 / 1000 |
| **die Pegeldifferenz fällt, die Breite bleibt** | **5,23 dB rms** | **3,19** (Seite/Mitte −8,66 → −8,80, ρ +0,794 → +0,769) |
| die Rolle steigt um eine Oktave, als Rampe | Verschiebung überall 0 | 81 Anschläge, 38 → 49, 0 neben der geschlossenen Form |
| der mitlaufende Tiefschnitt dünnt aus | +2,61 dB (`cut_track=0`) | **+20,82** |
| die Rolle verlagert ihr Gewicht nach oben | −0,23 dB | **+7,58** |
| die Acid-Fahrt ist eine echte Auslenkung mit Mittelwert null | — | −0,045 … +0,046 normiert, Mittel +0,006, Sektionsende exakt auf dem Bogen |
| ein Breakdown taucht und kommt zurück | — | −0,173 normiert, Ende exakt auf dem Bogen |
| die Fahrt hängt am Seed der Sektion | — | gleicher Seed identisch, anderer Seed anders, erste Sektion schreibt nur Nullen |
| die Fahrt ist langsamer als der Akzent-Kondensator | — | kürzeste Rampe 3,31 s gegen 0,15 s: Faktor 22 |
| der Hallweg steigt im Buildup und wird am Drop geschnitten | — | 0,299 am Buildup-Ende, 0,000 zwölf Takte davor und auf dem Drop |

Gesamt: **311 Selbsttest-Prüfungen**, 0 Fehler. Vektortests 16 von 16 in AVX2, NEON-Shim und skalar,
bitgleich in allen drei Pfaden — der Perc-Lauf der Vektortests fährt das Auto-Pan mit, weil er mit den
Standardwerten läuft.

### Gegenprobe (Mutationsrunde)

Sechs Fehler einzeln eingebaut, jeder gebaut und nur mit `PHOS_ONLY=testArrangeDynamics` gefahren,
danach aus Kopien zurückgespielt und die Zeitstempel angefasst (MSVC baut eine zurückgespielte Datei
mit altem Zeitstempel nicht neu). `git diff` danach sauber.

| Mutation | Wer merkt es |
|---|---|
| Schwung-Amplitude √2 → 1 | die Breitenprüfung: Seite/Mitte **−11,94 statt −8,80** (die Pegeldifferenz fällt sogar weiter auf 2,19 — genau der Tausch, den das Gesetz verhindern soll) |
| die zwei Phasengruppen zu einer zusammengelegt | **zwei** Prüfungen: Balance (Rest **1,447** von 3,548 statt 0,031) und Pegeldifferenz (**5,18** statt 3,19 — die Bewegung bringt dann nichts) |
| das (cos d, sin d)-Paar nicht renormiert | Konstantleistung: **2,9e−4** relativ statt 5,5e−7 |
| die Tonhöhen-Rampe der Rolle entfernt | zwei Prüfungen: 77 von 81 Anschlägen neben der geschlossenen Form, und die Rolle verlagert **−0,23 statt +7,58 dB** |
| der Tiefschnitt folgt der Verschiebung nicht mehr | zwei Prüfungen: Ausdünnung **+2,61 statt +20,82 dB**, Verlagerung **+0,38 statt +7,58** |
| der Hallweg wird am Sektionsanfang nicht geschlossen | zwei Prüfungen: der Send steht **0,300 im Drop** statt 0,000, und die Sektion schreibt ein Ereignis weniger |

**Eine der sechs hat beim ersten Versuch niemand gemerkt, und das ist der Befund dieser Runde.** Die
fehlende Renormierung des Drehpaares lief zunächst durch: die Prüfung maß den Fehler gegen den
*Spitzenwert* der Lane statt gegen die Leistung im selben Sample, und sie schlug eine Closed Hat an,
die nach 45 ms weg ist — einem Zwölftel der Schwingungsperiode. Die Lane hat den Winkel, bei dem eine
abgebrochene Reihe sichtbar wird, nie erreicht. Mit relativem Maß, einer Lane bei 0,9 Auslenkung
(1,49 rad, der größte Winkel, den das Feld zulässt) und zwei Sekunden Ausklang meldet dieselbe Prüfung
**2,9e−4 gegen 5,5e−7**. Eine Prüfung, die den schlechtesten Fall nicht spielt, prüft ihn nicht.

*Dateien.* Geändert: `Core/include/phos/Perc.h`, `Core/src/Perc.cpp` (Auto-Pan, `cut_track`, die zwei
Phasengruppen), `Core/include/phos/PercKernel.h` (die Drehung im Kernel), `Core/include/phos/Rhythm.h`
und `Core/src/Rhythm.cpp` (`kRollSemitones`, die Rampe der Rolle), `Core/include/phos/Form.h` und
`Core/src/Form.cpp` (`sectionAutomation`), `Core/include/phos/Params.h` und `Core/src/Params.cpp`
(drei **angehängte** Lane-Parameter, keine Umsortierung; Standardwerte: `pan_depth` auf zehn Lanes,
`cut_track` auf der Snare), `Tests/selftest.cpp` (`testArrangeDynamics`), `Tools/metrics.py` ist
**unverändert**. Neu: `Tools/ref_arrange.py`. **Außerhalb der Dateien dieser Runde geändert:**
`Core/src/Composer.cpp`, **eine Anweisung** (vier Zeilen mit Kommentar) am Ende von `sectionControls`,
die `sectionAutomation()` ruft — ohne sie erreicht die Automation den Render nicht, und alles, was sie
schreibt, entsteht in `Form.cpp`.

**18.09.2026, Merge-Integrationsfehler: `Conductor::pump` verschluckte fällige Bass-Slot-Events.**
Nach dem Merge von `bass-rhythm` und `arrange-dynamics` schlug `testBassRhythm` — „the kick phase still
meets the bass" — real fehl (142 BPM: +24,5° statt +10,8°; 148 BPM: +40,8° statt −2,8°; beide Bassrunden
für sich allein waren sauber grün). Ursache lag in keiner der beiden Runden selbst, sondern in ihrer
Kombination: `sectionAutomation()` (Arrangement-Dynamik) datiert Ride-Keyframes bis zu 16 Takte in die
Zukunft, alle auf einmal im ersten Takt der Sektion. `Conductor::pump` (`Composer.cpp`) komponierte bis
dahin **einen Takt auf einmal** und schob dessen gesamten Kontroll-Event-Stapel sofort vollständig in die
Engine, bevor der nächste Takt überhaupt komponiert war — ein Ride-Event mit Beat +24 lief damit den
Bass-Slot-Events der Takte 1–5 (Beat +4 bis +23) voraus, obwohl deren Zeitstempel kleiner sind. Die Engine
konsumiert die Warteschlange strikt zeitlich aufsteigend; ein verfrüht eingetroffenes Zukunfts-Ereignis
blockiert alles dahinter Wartende bis zu seiner eigenen Fälligkeit — genau der gemessene, gemäßigte
Phasenfehler, kein Totalausfall.

**Fix, zwei Stellen:** `Composer::composeBars` sortierte bisher nur die neu angehängten Ereignisse eines
Aufrufs (`stable_sort` über `[ctlStart, end())`), nicht global; jetzt folgt ein `std::inplace_merge` gegen
den bereits sortierten Präfix, für `out` (Noten) ebenso wie für `controls`. Das allein genügte nicht, weil
`Conductor::pump` die Puffer nach jedem Takt komplett leerte und zurücksetzte (`ctlStart` damit immer 0) —
das eigentliche Gedächtnis fehlte. `pump` akkumuliert die Puffer jetzt takteübergreifend und gibt nur noch
Ereignisse frei, deren Beat **echt kleiner** ist als der Beginn des nächsten, noch nicht komponierten
Takts (`safeBeat = nextBar_ * kBeatsPerBar`) — der einzige Punkt, vor dem garantiert nichts Kleineres mehr
nachkommen kann, weil jeder Takt ausschließlich Ereignisse ab seinem eigenen `barBeat` erzeugt. Bereits
ausgelieferte Pufferanteile werden ab 256 Einträgen abgeschnitten, damit ein langer Render nicht
unbegrenzt wächst.

**Bestätigt:** `testBassRhythm` wieder exakt bei +10,8°/−2,8° (25/25); voller Selbsttest 333/0 (vorher
332/1 mit demselben Fehlschlag); alle drei Vektorpfade 17/17; `ctest` komplett grün inklusive Hosttest.
Betroffen ist die reale Wiedergabe (Standalone, VST3, Quest), nicht nur der Test: Jeder Track mit einer
Build- oder Drop-Sektion ab vier Takten durchläuft `sectionAutomation` und hätte denselben Effekt gehabt,
sobald `compose.bass_rhythm=Corpus` lief.

**Why:** Zwei für sich korrekte und einzeln getestete Runden konnten kombiniert eine reale Zeitordnungs-
Garantie brechen, die keine der beiden Aufgabenstellungen kannte; das gehört dokumentiert, damit eine
künftige Runde, die ebenfalls weit vorausdatierte Kontroll-Events einführt, das Muster wiedererkennt.
**How to apply:** Jede Stelle, die Kontroll- oder Notenereignisse mit einem Beat schreibt, der über den
eigenen Takt hinausreicht, braucht entweder denselben `safeBeat`-Schutz oder eine explizite Prüfung, dass
`Conductor::pump` sie nicht vorzeitig ausliefert. Siehe [[phosphene-round-phase8]] (Merge-Fallen).

**18.09.2026, Fundament und Mix: Kick, Percussion, Effekte, Acid-Klang und Acid-Ride**

Anlass: der Nutzer hörte Seed 864566672 und fand ihn „fürchterlich dünn“ — von der Percussion nur Hats
und Kick, die Kick ohne Sub und Punch, Effekte kaum, die Acid „klingt nicht gut“. Neu gilt: die Regeln
stehen über dem Korpus, und das Ohr des Nutzers ist die Abnahme. Alle Messungen dieser Runde: Seed
864566672, Standardparameter, erster Drop von Track 1 (Takt 40–72), Master-Dynamik aus
(`auto_gain`, `limiter`, `clipper`, `clip` aus, `comp_ratio=1`), K-gewichtete Lautheit. Skripte in
`PhospheneWork\scratch\mix-foundation` (Messwerkzeug, nicht im Repo); neu im Repo `Tools/ref_kick.py`.

*Kick.* `Tools/ref_kick.py` sucht in den ersten 90 s jeder Referenz eine Strecke von acht und mehr
Schlägen, in der Kick und Bass fast allein spielen (über 300 Hz mindestens 8 dB unter 30–150 Hz), und
misst jede Kick einzeln vom Einsatz bis zum ersten Bass-Slot (Viertelschlag), mit flachem Fenster (ein
Hann-Fenster wöge den Klick mit fast null). 24 der 40 Aufnahmen haben so eine Strecke, alle „Kick +
Bass“, keine reine Kick. Die Messung widerspricht dem Auftrag an einer Stelle: die Referenz-Kicks sind
**nicht tiefer** als unsere (62 Hz im Fenster gegen unsere 52 Hz — beide fallen dort noch); die
Endtonhöhe bleibt deshalb 50 Hz, auf die Tonart gestimmt. Was fehlte, war Sub im Verhältnis und Klick:

| | Sub < 60 gegen 60–120 Hz | Klick 2–5 kHz gegen 40–120 Hz | Crest |
|---|---|---|---|
| Referenzen, Median (Quartile) | −4,7 dB (−9 … 0) | −27,7 dB (−31 … −23) | 7,1 dB (6 … 8) |
| Kick vorher (Werkzeug / Selbsttest) | −7,0 / −7,3 | −38,7 / −36,6 | 6,0 / 6,0 |
| Kick nachher (Selbsttest) | −3,4 | −27,9 | 8,2 |

Gebaut: der Klick läuft **hinter** dem Sättiger (vorher ritt er auf dem Körper und wurde mit ihm
plattgedrückt; `kClickGain` 4 statt 1,5 hält die Bedeutung des Reglers), Body Decay 22 → 13 ms,
Pitch Start 220 → 330 Hz, Drive 0,35 → 0,30, Click 0,2 → 0,5. Phasenkopplung und Ausklanggrenze sind
unberührt (ein Zyklus kostet weiterhin 7,1 ms τ2; `testKick`, `testPhaseLock`, `testBassRhythm`,
`testBass` grün). Im Drop-Solo: 60–120 Hz −15,0 → −17,3 dB, unter 60 Hz −22,0 → −19,5, 2–6 kHz
−50,2 → −40,7; mit dem Werkzeug auf dem Drop-Solo: Sub −7,0 → −2,2, Klick −38,7 → −28,7, Crest
6,0 → 8,4. **Offen, als Befund:** der Körper der Referenz-Kicks fällt innerhalb des Fensters nicht um
20 dB (Median 104 ms, das Fensterende — die Messung ist dort abgeschnitten), unserer nach 76 ms. Die
Referenzen halten ihre Kick also mindestens bis zum ersten Bass-Slot über −20 dB; unsere Ausklanggrenze
(`kick.tail_limit` −24 dB am Slot) verbietet das, und der Auftrag sagt, sie exakt zu halten. Eine
Grenze um −15 dB wäre die Frage an die nächste Runde.

*Percussion.* Die Lanes einzeln per `percN.active=0` zu messen, verschiebt die Komposition (die
Schichtreihenfolge gibt die Ebene einer abgeschalteten Lane an eine andere — Snare und Crash maßen so
10–16 dB zu laut). Gemessen wird deshalb mit allen Lanes aktiv, die anderen auf −36 dB, und einem
Boden-Render mit allen zwölf auf −36 dB, der leistungsmäßig abgezogen wird; die Lane als Wellenform ist
die Differenz zweier deterministischer Renders. „Hörbarkeit“: Median über die Anschläge des
Lane-Pegels gegen den Rest des Mixes in ihrer eigenen Oktave. Ride, Rim, Zap und Blip spielen in diesem
Drop nicht.

| Lane | Lautheit vorher → nachher (LKFS, ungated) | gegen den Rest in der eigenen Oktave |
|---|---|---|
| Closed Hat | −23,7 → −23,7 | −0,7 → −0,6 dB (4–8 kHz) |
| Shaker | −24,0 → −24,0 | −1,7 → −1,4 dB |
| Clap | −26,5 → −23,5 | +3,0 → +5,8 dB (1–2 kHz) |
| Snare | −29,5 → −27,5 | +10,3 → +9,7 dB |
| Tom | −33,1 → −27,2 | −0,7 → +2,7 dB (250–500 Hz; 125–250 Hz +5,8) |
| Conga | −30,2 → −23,2 | −1,3 → +1,8 dB (250–500 Hz) |
| Open Hat / Crash | −32,0 / −34,3 unverändert | +8,8 / +13,1 dB |

Pegel: Tom +6 dB, Conga +7, Clap +3, Snare +2, Ride +3, Rim/Zap/Blip +4; `mix.arp_level` −2 dB (die
Arp war mit −16,4 LUFS der lauteste melodische Teil und besetzte 300 Hz–2 kHz). Einen Teil davon hat
die lautere Acid in derselben Oktave wieder aufgefressen (warum nicht mehr: siehe „Die Grenze“ unten).
Percussion-Bus gesamt −18,6 → −16,5 LUFS, Arp −16,4 → −18,4, Acid −21,7 → −20,0.

*Effekte.* Vorher 5 Effekte in Track 1 und 7 in den ersten 168 Takten von Track 2; der Riser des ersten
Drops fehlte ganz (jede Übergangsmarke wurde einzeln mit `sfx_amount` = 0,7 gewürfelt). Jetzt sitzt an
**jedem** Sektionswechsel eine Marke (ab `sfx_amount` 0,5 sicher, darunter mit 2 × amount): Reverse Swell
in jeden Build und jeden Break, Downlifter in Break und Outro, Sweep und Impact auf **jeden** Drop,
Sweep aus dem Intro; Riser, Sweep, Formant-Shot und Impact am Build→Drop wie bisher. Dazu Ohrenschmaus
am Ende jeder 8- oder 16-Takt-Gruppe (Wahrscheinlichkeit `sfx_amount`): Zap auf Schlag 4, ein Takt
Filter-Rauschen, ein Takt Reverse Swell, zwei Takte Rauschwelle; Palette und Periode zieht jeder Track
selbst. Im Build liegt nichts außer seinen eigenen Marken — das PDB-Vakuum bleibt leer (Prüfung).
Jetzt 33 Effekte in Track 1 und 21 in den 168 Takten von Track 2. `sfx.level` −12 → −3 dB, dazu eine
Pegeltabelle je Typ (`kTypeGainDb`). Momentan-Lautheit (400 ms) des SFX-Busses gegen den Mix am lautesten
Punkt des Effekts, vorher → nachher: Riser −9,4 → −2,6 dB, Impact −14,9 → −5,2, Formant-Shot −14,0 →
−2,6 (im Vakuum), Sweep −8,9 → −6,3, Reverse Swell −7,8 → −8,4, Downlifter −4,7 → −4,0, Zap (neu) −7,5.
SFX-Bus über den Drop −27,6 → −16,9 LUFS.

*Acid-Klang.* Die Drive-Stufe tat fast nichts: Crest der Linie 25,6 dB bei Drive 0, 22,2 beim Standard
0,45, 19,4 bei 1, und 3–8 kHz bewegte sich zwischen 0,45 und 1 um 0,7 dB — der Körper einer Linie hinter
einer resonanten Diodenleiter liegt 20 dB und mehr unter deren Resonanzspitzen, eine Stufe, die erst an
den Spitzen sättigt, erreicht ihn nie. Neu: bis 30 dB hinein, die Wurzel davon heraus (`kDriveMaxDb`),
wie ein Verzerrer hinter einer 303. Slide 55 → 70 ms (die 60–80 ms des Regeltexts); Akzent-Kondensator
(kSweepTau 150 ms), Diodenleiter und Delay unverändert. Drei Kandidaten, gleiche Noten, alle auf −20,0
LUFS Solo abgeglichen, als Hörauszüge `A_acid_*.wav`:
1. **clean303** — kaum Verzerrung (0,1), Resonanz 0,8, Akzent 0,8: die runde, quietschende 303.
2. **driven** (neuer Standard) — Drive 0,85, Cutoff 900 Hz, Env 3,5 Okt., Decay 220 ms, Akzent 0,7,
   Low Cut 250 Hz, Delay-Feedback 0,5, Pegel −2,3 dB: 303 in den Verzerrer, dicht und bissig.
3. **liquid** — halb Puls, Resonanz 0,88, Env 5 Okt., Decay 500 ms, Disperser 4 Stufen: gummiartig, Goa.
Dazu `A_acid_0_before.wav`: die alten Regler (mit neuer Drive-Kurve und neuem Ride), als Vergleich.
Messbar trennen die drei sich kaum (Leistungsschwerpunkt 525 / 606 / 500 Hz) — der Unterschied ist
Textur, nicht Spektrum; das Ohr entscheidet.

*Der 32-Takt-Acid-Ride* (`sectionAutomation`, Signatur unverändert). Jede Sektion außer dem Break
durchläuft Zyklen von 32 Takten, kürzere Sektionen einen Zyklus ihrer Länge — ein 16-Takt-Build endet
mit dem Tauchgang genau auf dem Drop. Cutoff, Resonanz und Decay sind eigene Stränge; die
Cutoff-Form ist um die Linie der Sektion zentriert (ihr Mittel −0,175 der halben Auslenkung wird
abgezogen), damit der Level-Match, der die Regler misst, ehrlich bleibt. Gemessen im Selbsttest an der
Trajektorie, die die Engine fährt (32-Takt-Drop, Standardregler, Tiefe dieses Seeds):

| Takte | Cutoff (Oktaven zur Linie) | Resonanz | Filter-Decay |
|---|---|---|---|
| 1–8 | −1,42 (fast zu) | 0,55 (mittel) | 73 ms (kurz, trocken) |
| 9–16 | öffnet bis −0,04 | 0,55 | wieder 220 ms (Regler) |
| 17–24 | öffnet bis +1,18 | steigt auf 0,85 (Squelch) | wird länger |
| 25–32 | voll offen +2,06 ab Takt 28, im letzten Takt Tauchgang auf −1,45 | 0,85 | 374 ms |

Mittel über den Zyklus +0,004 normiert (Linie der Sektion = 0). Ein 16-Takt-Build: Spitze in Takt 14,9,
im letzten Takt 0,51 normiert (≈ 3,4 Oktaven) abwärts, Ende genau auf dem Drop. Der Break taucht wie
bisher. Tauchgang = ein Takt = 1,66 s, das Elffache von kSweepTau; alle anderen Rampen zwei Takte und
länger. Der Filter-Decay-Strang zieht den Pegel im Mittel etwas herunter (kürzere Hüllkurve im
Schnitt), bei allen Tracks gleich; der Level-Match vergleicht Tracks untereinander.
Folge, die man kennen muss: Resonanz und Decay überschreiben für den Rest der Sektion den Rezept-Offset,
den `trackStartControls` zu Track-Beginn schreibt — ein Strang hält einen Offset, keine Summe.

*`PercKit::setTempo()`* ruft jetzt `Engine::applyParams` mit dem Tempo des laufenden Chunks; nur die
Schrittweite des Pan-Phasors wird neu berechnet (während einer Tempo-Rampe ändert sie sich an jedem
Chunk). Prüfung: bei 120 BPM schwingt der Auto-Pan mit 0,3750 s statt 0,3103 s.

*Gesamtmix gegen die Referenzen* (`Tools/metrics.py`, Abstand zum Median in dB, Low-Mid / Mid / Presence /
Air, Standard-Master):

| | vorher | nachher |
|---|---|---|
| Auszug A | +0,09 / −2,05 / −3,27 / −1,65 (2,20 dB rms) | +2,79 / −1,06 / −2,31 / −0,12 (2,14) |
| Auszug B | +3,95 / +7,41 / +5,08 / +2,36 (5,08) | +5,71 / +8,31 / +6,10 / +3,84 (6,07) |
| Auszug C | +6,69 / +11,30 / +7,92 / +4,09 (7,72) | +8,27 / +12,41 / +8,53 / +5,19 (8,78) |
| ganzer Render (424 Takte) | +1,76 / +3,98 / +0,89 / −0,65 (2,49) | +3,96 / +5,05 / +1,99 / +1,02 (3,61) |

Presence und Air sind in A an den Median herangerückt; Low-Mid ist um 2,7 dB gestiegen (Zerlegung mit den ersten Tom-/Conga-Pegeln gemessen). Zerlegt: 1,7 dB
davon kommen aus der Kick — ihre Energie wanderte aus 60–120 Hz unter 60 Hz, und das Bezugsband der
Messung (40–140 Hz) verliert dabei —, 0,8 dB aus Tom und Conga, 0,3 aus der Acid. Die Kick lauter zu
machen hilft nicht: bei −9 LUFS fängt der Clipper ihre Spitzen, und Presence und Air fallen dafür unter
den Median (Kick +2 dB: A 2,6 / −1,6 / −3,2 / −0,8). B und der ganze Render sind in Mid und Presence
schon vorher weit über dem Median (Lead und Arp von Track 2, Runde `melody-rules`). Lautheit −9,2 →
−9,1 LUFS, True Peak −1,0 dBTP. Nichts in dieser Runde filtert unter 140 Hz, wo die Pads im Break ihr
Fundament bekommen sollen (Runde `melody-rules`).

*Prüfungen, alle zuerst gegen den alten Code rot gesehen:* `testKickReference` (Sub, Klick, Crest gegen
die Referenzwerte), `testPercTempo`, `testSfxLevel` (Riser und Impact auf der Hörspur höchstens 8 dB
unter dem Mix; K-Gewichtung mit den tabellierten 48-kHz-Koeffizienten von BS.1770-4), in `testSfx` die
Marken an jedem Übergang / Ohrenschmaus / nichts im Build, in `testArrangeDynamics` die vier Stufen (j)
und der 16-Takt-Build (j2). Angepasst: `testSfx` erlaubt den Impact jetzt auf jedem Drop; (m) verlangt
vom schnellsten Ride-Schritt (dem Tauchgang, ein Takt) das Zehnfache von kSweepTau statt des
Zwanzigfachen.

*Die Grenze, die die Percussion begrenzt hat.* Tom und Conga waren zuerst auf +8 / +8 dB; damit maß
`testVariety` die Lautheitsstreuung von vier Tracks (nur Kick, Bass, Percussion) 0,85 LU gegen die
Schranke 0,8 — die Lautheitssonde des Composers sieht nicht, wie viel eines Tracks Toms und Congas
spielen. Mit der alten Kick blieben es 0,81, mit den alten Tom-/Conga-Pegeln 0,67: die Percussion ist die
Ursache, nicht die Kick. Jetzt Tom +6, Conga +7: 0,75 LU. Wer die Percussion lauter will, muss die Sonde
(`Composer.cpp`, Runde `melody-rules`) die Percussion des Tracks hören lassen.

*Mutationen* (je eine, eingebaut, gemessen, aus der Kopie zurück, Zeitstempel gesetzt; danach ist der
`git diff` der Kerndateien identisch mit dem vor der Runde):

| Mutation | Wer merkt es |
|---|---|
| Klick wieder vor dem Sättiger | `testKickReference`: Klick um −19 dB, über dem oberen Quartil (−23) |
| `setTempo` nicht gerufen | `testPercTempo`: 145 BPM, Periode 0,3103 s |
| Ride-Mittel nicht abgezogen | (j): Mittel und Stufenwerte verschoben |
| Übergangsmarken mit `amount` statt `2 × amount` gewürfelt | `testSfx`: 38 von 50 Übergängen markiert |
| Ohrenschmaus auch im Build | `testSfx`: 7 Eindringlinge |
| Pegeltabelle je Typ nicht angewendet | zuerst **niemand** — Riser und Impact blieben im 8-dB-Fenster, weil `sfx.level` +9 dB das meiste trägt. Nachgerüstet: der Downlifter gegen den Drop davor, −11,7 dB, mit der Mutation −4,0 → rot |
| Squelch-Resonanz 0,95 statt 0,85 | (j): Resonanz außerhalb 80–90 % |

*Dateien.* `Core/include/phos/Kick.h`, `Core/src/Kick.cpp` (Klick hinter dem Sättiger),
`Core/include/phos/Perc.h`, `Core/src/Perc.cpp` (`tempo()`, `updatePanRate`), `Core/src/Engine.cpp`
(eine Zeile `setTempo`), `Core/include/phos/Sfx.h`, `Core/src/Sfx.cpp` (Pegel je Typ),
`Core/include/phos/Form.h`, `Core/src/Form.cpp` (`makeFormSfx`, `sectionAutomation`, `RideShape`),
`Core/src/Acid.cpp` (Drive-Kurve), `Core/src/Params.cpp` (nur Standardwerte von `kick.*`, `perc*`,
`acid.*`, `sfx.*`, `mix.arp_level`; kein Parameter angehängt oder verschoben), `Tests/selftest.cpp`,
neu `Tools/ref_kick.py`. Außerhalb der Liste dieser Runde: nichts. `Composer.cpp` und die Signatur von
`sectionAutomation` sind unberührt.

**18.09.2026, Melodik nach Regeln: Acid, Arp, Lead, Pad-Fundament.** Grundsatz des Nutzers vom selben Tag:
die Genre-Regeln stehen über den trainierten MIDI-Daten; Markov-Kette und Transformer wählen nur noch
innerhalb der Regeln (Melody.h, Abschnitt „Genre rules above the corpus“). Gemessen am Hör-Seed 864566672
(Selbsttest `testGenreRules`, dieselben Kennzahlen wie die Tabelle des Auftrags), vorher → nachher:

| Teil | vorher | nachher |
|---|---|---|
| Acid T1 | Takthälften 1296/162, 97 % Grundton, 73 % Tonwiederholung, 311 Dreierläufe, 1 Tonklasse/Takt, F#3..F#4 | 972/810, 1 42 % / 5 30 % / b3 8 % / b2 8 %, 16 %, 0 Läufe, 4 Tonklassen, E3..F#4 |
| Acid T3 | 954/1272, 96 % Grundton, 76 %, 318 Läufe, C#4..C#5 | 1113/954, 1 53 % / b7 26 %, 33 %, 0 Läufe, F3..C#5 |
| Arp T1 („up“) | 10 von 16 Sechzehnteln, Hälften 1520/380 | 16 von 16, 1520/1520, zwei Ströme (41 % Oktavsprünge), B3..F#5 |
| Arp T2 (Korpus) | 150 Takte, G#5..B6 | 54 Takte (nur wo die Lead schweigt), G#3..F#5, s. u. |
| Lead T2 | 552 Noten in 96 Takten, 6/Takt, b2 47 %, 5 2 % | 1290 Noten, 13/Takt, 1 56 %, b2 13 % (nur als Nebenton), Quinte als Halteton in jeder Phrase |
| Lead T3 | Median D5, Spitze D6 | Median C#5, Spitze A5 |
| Pad | tiefster Ton G3/A3, Grundton unten in 24/96 Akkorden (T1) | Grundlage D3..C#4, Grundton unten 96/96, Sub-Grundton in 39 Akkorden der Breakdowns |

Farbtonanteil je Rolle (Population, 240 Pläne, Tonart-Mix): vorher Lead 0,34 (Phrygisch) bis 0,42 (Doppelt
harmonisch), Acid ≤ 0,014, Arp bis 0,19; nachher Acid 0,10–0,11, Lead 0,11, Arp 0,02–0,03, in Äolisch und
Dorisch 0. **Ein Mechanismus:** Farbtöne sind in keiner Sampler-Menge mehr; sie stehen an gezogenen
„Farbslots“ (schwache Sechzehntel, eine Sechzehntel lang, danach sofort die Tonika), deren Zahl ein
Zielanteil je Rolle ist (`kColourShare`, 0,11/0,13/0,055, skaliert vom Bogen 0,6..1,0). Markov und
Transformer liefern damit denselben Anteil (0,1055 beide), die Modustabelle des Modells hat keinen
Einfluss mehr. **Wo das Korpus widerspricht (Information, kein Veto):** Acid-Korpus 2 Tonklassen/Takt,
59 % Wiederholungen; die gemessene Spannungskurve (16.09.) hält nur noch beim Acid (+0,23), Lead-Kontrast
und Taktparität fallen unter die Regeln auf ≈ 0; der Akzent-Clustering-Lift gilt weiter (2,5 an e/a).

**Pad-Fundament:** Grundstellung mit Quinte darüber ab D3 (`pad.hp_floor` 200 → 140 Hz), und wo die Form
Kick und Bass schweigen lässt, der Grundton eine Oktave tiefer (D2..C#3) statt der zweiten Oberstimme (nie
mehr als vier Stimmen je Akkord). Der Hochpass wird pro Takt per Kontroll-Event geöffnet (40 Hz, 0,5 × f0);
dafür reicht `hp_floor` jetzt bis 40 Hz (Bereich, nicht Reihenfolge). Der Sub endet einen Takt vor dem
Wiedereinsatz; gerendert: Anteil unter 140 Hz im Breakdown -14,9 dB (vorher -31,1), nach dem Drop -83 dB.

**Arp gegen Lead:** Arp (G3..G5) und Lead (Median A4..C5) teilen zwangsläufig ein Register; die
Maskierungsregel in Form.cpp schiebt den Arp oktavweise hoch und streicht ihn aus der ganzen Sektion,
sobald er über MIDI 100 käme — in Track 2 des Hör-Seeds verschwand er damit ganz, auch im Breakdown ohne
Lead. Ohne Form.cpp anzufassen plant `Composer::restoreArp` den Takt ein zweites Mal ohne Lead und gibt
den Arp dort zurück, wo der echte Takt keine Lead hat (die Sektionsentscheidung schützte nur vor
Oktavsprüngen innerhalb der Sektion, und die gibt es nicht mehr). Ergebnis: Arp und Lead klingen nie
gleichzeitig (Maskierungstest jetzt taktweise, 0 gemeinsame Takte). Eine Folgerunde in Form.cpp könnte
das direkt dort ausdrücken.

## 0. Kurzfassung

Ein Instrument, das aus einem Seed, einem Stilprofil und einem Energiebogen ein komplettes
Psytrance-Set von 30 bis 180 Minuten komponiert und in Echtzeit synthetisiert: Kick, Bass,
Percussion, Acid, Lead, Arpeggio, Pad/Atmos, SFX, Mischpult und Master. Standalone und VST3 für
Windows (JUCE 9, wie Noctuary), nativ auf der Quest 2 (OpenXR + Oboe, wie Noctuary Quest),
MIDI-Export aller Notenlinien und Automationen, Offline-Render als Determinismus-Orakel.

Die drei Entscheidungen, die alles andere bestimmen:

1. **Komponist und Synthese sind getrennt.** Der Komponist erzeugt auf einem eigenen Thread eine
   symbolische Partitur (Noten, Akzente, Slides, Automationsrampen, Sektionsmarken) mindestens
   16 Takte im Voraus. Der Audio-Thread liest nur Ereignisse, allokiert nie. Dieselbe Partitur
   geht in den MIDI-Export, in den Offline-Render und in die Kaleidoscope-Kopplung.
2. **Form regelbasiert, Patterns aus dem Korpus gelernt.** Die Form (Sektionen, Energiebogen,
   Tonartenreise, Übergänge) kommt aus Grammatik und Constraints, weil kein Korpus ganzer
   Psytrance-Tracks vorliegt. Die Patterns (Bass, Arp, Lead, Pad, Drums, je 2 bis 8 Takte) werden
   aus dem lokalen MIDI-Korpus gelernt: rund 1600 Psytrance-Loops und 6900 Trance-Loops unter
   `M:\Midi`, rollenbeschriftet. Stufe A sind korpus-gelernte Markov-Tabellen variabler Ordnung
   mit Constraint-Dekodierung (läuft auf der Quest), Stufe B ein kleiner Transformer mit
   eigener C++-Inferenz (Abschnitt 6.9). Ein Ranker über Nutzer-Bewertungen wählt aus Kandidaten.
3. **Stimmenparallele SIMD.** Ein rekursiver Filter lässt sich nicht innerhalb einer Stimme
   vektorisieren, wohl aber über acht Stimmen (AVX2) bzw. vier (NEON) gleichzeitig. Alle
   Synthesizer werden deshalb als Structure-of-Arrays über Stimmen/Lanes gebaut, mit einem dünnen
   `Vec`-Wrapper, der auf AVX2, NEON und skalar kompiliert (Abschnitt 9).

Reihenfolge der Arbeit: zuerst Kick + Bass + Clock als 4-Takt-Loop, der "rollt". Psytrance
steht oder fällt damit; alles andere baut darauf auf (Abschnitt 12).

## 1. Ziel, Rahmen, Nicht-Ziele

**Ziel.** Auf Knopfdruck ein Set, das ein Kenner der genannten Künstler als stilistisch
glaubwürdig hört: Intro, Aufbau, Drops, Breakdowns, Übergänge zwischen Tracks, Tonartenreise,
Tempoverlauf. Jede Sektion einzeln neu würfelbar und sperrbar. Alles reproduzierbar aus
Seed + Stil + Sperren.

**Rahmen.**
- Plattformen: Windows x64 (AVX2), Quest 2 (arm64, NEON). Linux/macOS als Nebenprodukt des
  frameworkfreien Kerns, nicht als Release-Ziel.
- Sample-Rate 44,1/48/96 kHz, Blöcke 32 bis 2048; Quest 48 kHz, 256er Blöcke (Oboe).
- Kern ohne Framework (wie `NoctuaryCore`): C++20, keine JUCE-Abhängigkeit im Kern.
- Kein Fast-Math (Determinismus-Regel aus Noctuary; der Offline-Render ist das Orakel).

**Nicht-Ziele.**
- Kein Klon einzelner Künstler oder Tracks. Stilprofile sind Parametervektoren mit beschreibenden
  Namen ("Goa 1996", "Full-On Roll", "Progressive", "Dark Forest", "Hi-Tech"); die Künstlerliste
  aus der Aufgabenstellung dient als Hörreferenz für die Kalibrierung (Abschnitt 2.6).
- **Vollständige Synthese, keine Samples** (Entscheidung des Nutzers). Die Percussion-Engine
  "Sample" entfällt, ebenso die Texture-Quelle. Einzige mögliche spätere Ausnahme: Sprach-Samples
  für Breakdowns; bis dahin übernimmt der Vocal-Formant-SFX (5.8) diese Rolle.
- Kein Cloud-Modell in der Echtzeitschleife.

## 2. Was Psytrance ausmacht (die musikalische Spezifikation)

Diese Fakten werden in Phase 1 durch Messung an Referenzmaterial des Nutzers geprüft (Tempo,
Sektionslängen, Bass-Onsets, Kick-Spektrum), nicht nur behauptet. Werkzeug: `Tools/analyze_ref.py`
(Abschnitt 11.4).

### 2.1 Tempo und Raster
- Full-On 140 bis 148 BPM, Goa 138 bis 145, Progressive 134 bis 140, Dark/Forest 146 bis 152,
  Hi-Tech 155+. Ein Set bewegt sich meist innerhalb von ±4 BPM, oft mit leichtem Anstieg.
- Alles in 4/4, Phrasen in Zweierpotenzen: 8, 16, 32 Takte. Sektionswechsel auf Taktgruppen,
  Fills im letzten Takt einer 8er- oder 16er-Gruppe.

### 2.2 Kick und Bass, das Fundament
- Kick auf jeder Viertel, außer in Breakdowns und in den Takten vor einem Drop.
- Kick: Sinus mit exponentieller Tonhöhenhüllkurve (Start 150 bis 400 Hz, Ziel 45 bis 60 Hz,
  Zeitkonstante 10 bis 40 ms), Amplituden-Decay 150 bis 350 ms, Sättigung für Punch, kurzer
  Klick-Transient. Grundton meist auf den Tonartgrundton oder dessen Quinte gestimmt.
- Rolling Bass: Sechzehntelnoten in den Lücken der Kick, klassisch K-B-B-B (drei Bassnoten pro
  Viertel) oder Offbeat-Varianten (K-.-B-B, K-B-.-B, Triolen in Goa). Jede Note identisch kurz
  (Gate 60 bis 90 % der Sechzehntel), Oszillator-Phase pro Note zurückgesetzt, Filter-Decay
  40 bis 150 ms, Tiefpass 4-polig mit Sättigung, mono unter 120 Hz. Oktav- und Quintsprünge
  ("Walking Bass", Full-On) alle 2 bis 4 Takte als Variation.
- Kick und Bass teilen sich das Subband: die Kick klingt in ihren ersten 60 bis 100 ms allein,
  die Bassnote beginnt danach. Das ist die eigentliche Definition von "tight".

### 2.3 Percussion
- Hi-Hat Offbeat-Achtel oder Sechzehntel mit Akzentmuster, Open Hat auf der "und", Ride in
  Peak-Sektionen, Clap/Snare auf 2 und 4 oder nur in Fills, Shaker/Tambourine als
  Sechzehntel-Teppich, Toms/Congas als tribale Ebene (Juno Reactor), Zaps und Blips als
  Sechzehntel-Füller (Hi-Tech).
- Fills alle 8 oder 16 Takte, Snare-Rolls (1/16 → 1/32 → 1/64) vor Drops.

### 2.4 Harmonik und Melodik
- Fast immer Moll mit modalen Färbungen: äolisch, phrygisch, harmonisch Moll, doppelt-harmonisch
  (Hijaz, Goa). Selten mehr als zwei Akkorde pro Sektion (i–bII, i–bVI, i–v). Tonart pro Track
  fest; Wechsel zwischen Tracks meist über Quintverwandtschaft oder Halbtonrückung.
- Acid-Linien: 16-Step-Sequenzen mit Akzent und Slide, Bereich ein bis zwei Oktaven, oft auf dem
  Grundton pendelnd, Filter-Cutoff als eigentliche "Melodie".
- Leads: Motiv aus 2 bis 4 Takten, wiederholt mit Transposition, Verkürzung, Oktavierung,
  rhythmischer Verschiebung. Goa/Astral Projection: euphorische Sechzehntel-Arpeggien über
  gehaltene Pads; Cosmosis: schnelle FM-Leads; Infected Mushroom: Brüche, Glitch, Tonarten- und
  Tempospiele; Hallucinogen: LFO-getriebene Schichten und Squelch.
- Arpeggios: Akkordtöne in Auf/Ab/Zufall über 1 bis 3 Oktaven, Sechzehntel, mit Trance-Gate.

### 2.5 Form eines Tracks (7 bis 10 Minuten)
```
Intro (16-32)  Groove (16-32)  Build (8-16)  Drop (32-64)  Break (16-32)  Build (8-16)
Drop 2 (32-64)  Outro (16-32)
```
Zahlen in Takten. Intro und Outro sind DJ-freundlich (Kick + Bass + wenig), der Breakdown nimmt
Kick und Bass heraus und lässt Pads/Atmos/SFX allein; der Build ist Riser, Snare-Roll,
Filteröffnung, oft mit Kick-Aussetzer im letzten Takt; der Drop bringt alles zurück.

### 2.6 Stilprofile
Ein Stilprofil ist ein Vektor von etwa 60 Gewichten und Bereichen: Tempo, Bass-Pattern-Familie,
Kick-Tuning, Hat-Dichte, Acid-Wahrscheinlichkeit, Skalenfamilie, Lead-Motivlänge, Arp-Anteil,
Break-Länge, SFX-Rate, Stereo-Breite, Master-Lautheit usw. Fünf Profile zum Start, aus der
Referenzmessung kalibriert, beschreibend benannt. Zwischen Profilen wird interpoliert
(Morph-Regler wie in Noctuary).

## 3. Architektur

```
 Stilprofil + Seed + Energiebogen + Sperren
            │
            ▼
   ┌──────────────────┐   Partitur (Ereignisse, 16+ Takte voraus)   ┌──────────────────────┐
   │  Composer-Thread │ ─────────── lock-free Queue ───────────────▶│  Audio-Thread        │
   │  Set → Track →   │                                              │  Sequencer (sample-  │
   │  Sektion → Bar → │◀── Positionsrückmeldung, Live-Eingriffe ────│  genau) → Synths →   │
   │  Pattern         │                                              │  Mixer → Master      │
   └──────────────────┘                                              └──────────────────────┘
            │                                                                   │
            ▼                                                                   ▼
   MIDI-Export (SMF 1), .phosset-Datei                          Offline-Render (Orakel, Stems),
   Kaleidoscope-OSC (Beat/Sektion/Energie)                     Loudness-Messung, Recorder
```

**Schichten im Kern (`Core/`):**
- `phos/Vec.h`: SIMD-Wrapper (Abschnitt 9).
- `phos/Clock.h`, `phos/Sequencer.h`: Tempo-Karte, Beat-Position, sample-genaue Ereignisausgabe,
  Host-Sync (VST3-Playhead), MIDI-Clock-Ausgabe optional.
- `phos/Score.h`: die Partitur als Datenmodell: Set → Tracks → Sektionen → Pattern-Instanzen →
  Steps (Note, Velocity, Akzent, Slide, Wahrscheinlichkeit, Mikro-Offset) + Automationsrampen +
  Marken. Textform `.phosset` (Seed, Stil, Sperren, ggf. die ausgerollte Partitur).
- `phos/compose/*`: der Komponist (Abschnitt 6).
- `phos/synth/*`: die Klangerzeuger (Abschnitt 5), jeder mit `prepare/reset/noteOn/process(SoA)`.
- `phos/mix/*`: Kanalzüge, Sends, Sidechain-Matrix, Master (Abschnitt 5.9).
- `phos/Params.h`: Parametersystem in Blöcken pro Modulinstanz (Abschnitt 8.4), nicht ein
  einziges Enum wie in Noctuary, weil Percussion-Lanes indiziert sind.
- `phos/Midi.h`: SMF-Writer (und Reader für importierte Akkordfolgen).

**Threads.** Audio (nie allokierend), Composer (darf allokieren, arbeitet in Takt-Häppchen mit
Deadline "Queue nie unter 8 Takten"), GUI/Message, Offline-Render (einzelner Thread,
deterministisch; Stems optional parallel je Track, da unabhängig).

**Determinismus.** `Rng` (xorshift64 aus Noctuary) je Modul mit `fork()`-Ableitung aus dem
Set-Seed; kein `rand()`, kein Thread-geteilter RNG (Lehre aus Kaleidoscope). Gleicher Seed +
gleiche Plattform → bitgleicher Render. Zwischen AVX2 und NEON weicht das letzte Bit der Summen
ab, das ist akzeptiert und im Selbsttest als Toleranz hinterlegt (wie Noctuarys convtest).

## 4. Wiederverwendung aus Noctuary

Entscheidung des Nutzers: **Modulkopie, kein Link.** Phosphene entwickelt sich in eine andere
Richtung; die Module werden nach `Core/` kopiert, in den Namensraum `phos::` verschoben und dort
weiterentwickelt. Jede kopierte Datei trägt im Kopf die Herkunft (`Noctuary <Datei>, Stand
<Commit>`), damit ein Fix in Noctuary später gezielt nachgezogen werden kann. `Params.h`-Abhängige
(`Modulation.h`, `Score.h`, `Timeline.h`) werden beim Kopieren auf das neue Parametersystem
umgeschrieben. `SourceSlot` wird nicht kopiert (5.7).

| Noctuary-Modul | Einsatz hier | Anpassung beim Kopieren |
|---|---|---|
| `Dsp.h` (Rng, Drifter, Envelope, Svf, SineTable) | überall | Namensraum |
| `Simd.h` (AVX2/NEON-Muster, `horizontalSum`, NEON-Shim in `Tests/neonshim`) | Vorlage für `Vec.h`, Shim kopieren | Wrapper neu, Shim erweitern (Vergleiche, Selektion, `vfmaq`) |
| `Adaa.h` (tanh mit ADAA) | Kick, Bass, Acid, Master | Hard-Clip- und Diodenkurve ergänzen |
| `Filter.h` (10 Modelle, Formant, Comb) | Lead/Arp/Pad, SFX | die Ladder dort ist eine Ein-Pol-Kaskade; für Bass/Acid kommen ZDF-Leitern neu (5.2, 5.4) |
| `ZPlane.h` + `ZModal` (155 Formen, Modalbank) | Percussion-Körper (Toms, Congas, Metall), Formant-Sweeps | keine |
| `CycleTable.h` + `Tools/WavetableLib` | Wavetable-Oszillator in Bass, Lead, Arp, Pad | Unisono-Lesen über Lanes |
| `Voice.h` FM (2-op) | Lead/Arp-FM, Perc-FM | auf 4-op mit Algorithmen erweitern |
| `Effects.h` Ensemble (Chorus/Microshift/Velvet) | Lead-/Pad-Breite | keine |
| `Effects.h` StereoDelay (Duck, Absorb) | Delay-Send, Acid-Delay | Tempo-Sync-Zeiten, Pitch-Shift in der Rückkopplung |
| `Effects.h` Reverb (4 Modi) | Send A kurz, Send B lang, Reverse-Reverb | keine |
| `Effects.h` Unmask (Band-Ducker) | Vorlage für den Sidechain-Ducker | vereinfachen: ein Band, schnell, Hold |
| `Effects.h` MidSide (Mono-Bass, Breite, Mono-Guard) | Master | keine |
| `Convolution.h` | Raum-Send für Breakdowns | keine |
| `Shifter.h` | Pitch-Delay, Vocal-Formant-FX | keine |
| `GrainRing.h` | Stutter, Beat-Repeat, Glitch | tempo-synchrone Fenster |
| `Loudness.h` (BS.1770, Sones, True Peak) | Master-Meter, Auto-Gain-Staging der Presets | keine |
| `Clock.h` (SyncDiv, Quellen) | Basis für `phos/Clock.h` | Tempo-Karte statt einer Zahl |
| `Modulation.h` (8 LFO, 6 Envs, Matrix) | Modulations-Tab | hängt an Noctuarys `ParamId`; auf das Block-Parametersystem (8.4) umschreiben |
| `Osc.h` (OSC-Server, EventQueue) | Kaleidoscope-Kopplung, Remote | Adress-Namensraum `/phos/...` |
| `Recorder.h`, `WavFile.h` | Quest-Aufnahme, Nutzer-Samples | keine |
| `Timeline.h`, `Score.h` | Live-Aufzeichnung und Automations-Textform | auf Beat-Zeit statt Sekunden |
| `Sources.h` (Additiv, Cloud, Spectral) | **entfällt** (Entscheidung 15.09., siehe 5.7); höchstens `Cloud` später für Breakdown-Texturen | keine Kopie |
| `Tools/WavetableLib` (608 Tabellen) + `WavetableGen` | Pad- und Lead-Wavetables | Tabellen und Generator kopieren, Dateiformat bleibt |
| `Quest/` (OpenXR, Oboe, Handmenü, APK-Skript), `Deploy/` (Inno Setup, build_release.ps1), `Tests/` (selftest, hosttest, racetest, Shim-Varianten) | Gerüste kopieren und anpassen | Projektname, Pfade |

Nicht übernommen: ClusterBrain, Cosmos, Near, Journey, Memory, Presets-Bibliothek (alles
drone-spezifisch).

## 5. Die Klangerzeuger

Jeder Erzeuger ist ein Modul mit: Pattern-Generator (Abschnitt 6), Synth-Engine (hier),
Kanalzug (5.9). Jede Engine hat einen skalaren Referenzpfad und einen SoA-Pfad über Lanes.
Literatur pro Baustein steht dabei, wie in der Feedback-Regel "Literatur statt Klon" verlangt.

### 5.1 Kick
Zwei Engines, umschaltbar, layerbar:

**Parametrisch (Standard).** `y = sat(A(t)·sin φ(t)) + click(t)`, mit
`f(t) = f_end + (f_start − f_end)·exp(−t/τ_p)` (optional zweites, schnelleres Segment für den
Punch), `A(t)` Attack 0 bis 2 ms, exponentieller Decay, rotierender Phasor statt `sin()` pro
Sample (Noctuary-Konvention), Sättigung tanh/Hard-Clip mit ADAA (Parker et al. 2016), danach
DC-Sperre und ein resonanter Tiefpass für den "Box"-Charakter, Klick als 1-Sample-Impuls durch
Bandpass 2 bis 6 kHz plus kurzer Rauschstoß. `f_end` folgt der Tonart (Grundton oder Quinte, in
der Oktave 40 bis 60 Hz).

**Bridged-T (808-informiert).** Der gedämpfte Resonator mit Retrigger-Verhalten nach Werner,
Abel und Smith (DAFx 2014, "physically-informed, circuit-bendable model of the TR-808 bass
drum"): ein anderes Hüllkurven- und Sättigungsverhalten als der Sinus-Sweep, gut für Goa- und
Progressive-Kicks.

**Analyse-Anpassung.** Aus einem Nutzer-Sample werden `f_start, f_end, τ_p, τ_A, Drive`
geschätzt (Grundtonverfolgung per Nulldurchgängen/YIN, RMS-Hüllkurve, Least-Squares-Fit).
Sinusoidal-plus-Transient-Zerlegung nach Serra und Smith (SMS, 1990) trennt Sweep und Klick.
Nichts vom Sample wird gespeichert, nur die Parameter.

Vektorisierung: eine Kick ist eine Stimme; die Lanes tragen Kick + bis zu 7 Layer (Top-Kick,
Sub-Layer, Klick-Layer) parallel. 2× Oversampling nur um die Sättigung.

### 5.2 Bass
Signalweg: Oszillator → Drive → 4-polige ZDF-Leiter → Nachsättigung → Hochpass 30 Hz → mono.

- **Oszillator:** PolyBLEP-Sägezahn/Rechteck/Puls-Mischung (Välimäki und Huovilainen 2007;
  DPW als Alternative, Välimäki 2005) oder ein `CycleTable`-Frame. Phasen-Reset bei jeder Note
  (Hard-Reset, der Kern des "Rollens"), optional Sub-Oszillator eine Oktave tiefer.
- **Filter:** topologieerhaltende (TPT/ZDF) Moog-Leiter mit nichtlinearen Stufen nach Zavalishin
  ("The Art of VA Filter Design") und Huovilainen (DAFx 2004), verbesserte Variante D'Angelo und
  Välimäki (ICASSP 2013). Cutoff-Hüllkurve mit Decay 40 bis 150 ms, Velocity → Cutoff. Die
  vier nichtlinearen Stufen laufen 2× oversampled (Halbband-Polyphasen-FIR, vektorisiert).
- **Kick-Kopplung:** Sidechain-Ducker vom Kick-Kanal (Attack 0, Hold 40 bis 80 ms, Release 60 bis
  120 ms) zusätzlich zum Pattern-Abstand, damit auch dichte Muster (Hi-Tech) sauber bleiben.
- **Modi:** Rolling (Sechzehntel, identische Noten), Walking (Oktav/Quint), Offbeat, Triolen,
  Sustained (Progressive, längere Noten mit langsamerer Hüllkurve).

Vektorisierung: Bass ist monophon, aber Bass + Sub + Layer + Acid teilen sich einen 8-Lane-Block
derselben Leiter-Implementierung.

### 5.3 Percussion (Kit mit 12 Lanes)
Ein Kit ist eine Auswahl aus fünf synthetischen Perkussions-Engines pro Lane:

| Engine | Verfahren | Literatur | Typische Lanes |
|---|---|---|---|
| NoiseBurst | Velvet/Weiß/Rosa → SVF/Z-Plane → Hüllkurve | Välimäki, Alary, Politis 2017 (Velvet) | Hats, Shaker, Clap (mehrfach gestaffelt), Snare-Rauschanteil |
| Metallic | sechs verstimmte Rechtecke, Ringmodulation, Hochpass/Bandpass | Werner, Abel, Smith 2014 (TR-808 Cymbal; Cowbell, AES 137) | Ride, Crash, Cowbell, Open Hat |
| Modal | `ZModal` mit Modendaten der Z-Plane-Bank, Anschlag als kurzer Impuls | Smith, Physical Audio Signal Processing; Bilbao, Numerical Sound Synthesis; Aramaki et al. 2006 | Toms, Congas, Djembe, Holz, Glocken |
| BridgedT | gedämpfter Resonator wie in 5.1, kleiner | Werner et al. 2014 | 808-Toms, Kick-Layer |
| FmPerc | 2-op FM mit schneller Tonhöhenhüllkurve | klassisch (Chowning 1973) | Zaps, Blips, Laser, Rim |

Patterns: Euklidische Rhythmen (Toussaint 2005) als Basis für Hats/Shaker/Toms, Wahrscheinlichkeiten
pro Step, Synkopenmaß nach Longuet-Higgins und Lee (1984) als Steuergröße für "Groove", Fill-Bank
für den letzten Takt einer Phrase. Velocity-Modelle pro Lane (Akzent auf dem Offbeat bei Hats).

Vektorisierung: 12 Lanes = zwei AVX-Register bzw. drei NEON-Register; Hüllkurven, Rauschen, SVF,
Modalresonatoren alle lane-parallel. Das ist der Ort, wo SoA am meisten bringt.

### 5.4 Acid
- PolyBLEP-Sägezahn/Rechteck → **Diodenleiter** (4 Stufen) nach Zavalishin (Kapitel Diode Ladder)
  mit Sättigung in der Rückkopplung → Verzerrung (ADAA) → Delay-Send.
- **Squelch-Modus** (Review 15.09.): der "Liquid Lead" von Hallucinogen, Cosmosis, Tristan als
  eigener Modus derselben Stimme: schnelle Tonhöhen-Hüllkurve (Startwert einige kHz, Ziel einige
  hundert Hz, einige zehn ms) durch ein hochresonantes Kammfilter (`VoiceFilter::Comb`, Verzögerung
  2 bis 6 ms) oder einen Z-Plane-Bandpass, danach die Verzerrung. Die Kalibrierung an den drei
  Hallucinogen-Tracks der Sammlung (LSD, Alpha Centauri, Solstice) ist in Phase 3 versucht worden und
  gescheitert (`Tools/ref_sweeps.py`, Negativbefund); die Zahlen bleiben Entwurfswerte mit Stellbereich.
  Umgesetzt: Cutoff-Startwert × Faktor mit Zeitkonstante plus auf die Notenperiode gestimmter Kamm.
- **Akzent:** erhöht Hüllkurve und Cutoff und lädt einen "Akzent-Sweep"-Kondensator, dessen
  Wirkung mit der Resonanz wächst (das charakteristische "Wow" bei aufeinanderfolgenden Akzenten).
  Modelliert als zweite, langsamere Hüllkurve mit resonanzabhängiger Tiefe.
- **Slide:** exponentielles Glide über die Notenlänge, Noten überlappend (Legato).
- Offene Referenz zum Verhalten (nicht zum Kopieren): Open303 (R. Schmidt).
- Pattern: 16 Steps mit Akzent/Slide-Flags aus einer Constraint-Markov-Kette (Abschnitt 6.5).
- Cutoff als Hauptautomationsziel: LFO, Hüllkurve, Sektions-Rampen, Live-Makro.

### 5.5 Lead
Eine polyphone Engine `Poly` (8 Stimmen × bis 8 Unisono), instanziiert für Lead, Arp und Pad:
- **Supersaw als Standard-Oszillator der Lead-Instanz** (Review 15.09.): sieben verstimmte
  Sägezähne mit der Detune- und Mix-Kurve aus Szabo (2010, "How to Emulate the Super Saw"),
  Stereo-Verteilung der Lanes. **Dynamischer Detune:** die Verstimmung folgt der Notenlänge und
  dem Sektionstyp, eng bei schnellen Sechzehnteln (sonst verschmiert der Anschlag), weit auf
  gehaltenen Tönen und in Breaks; der Komponist schreibt das als Offset, keine Handarbeit.
- Weitere Oszillatoren pro Unisono-Lane: PolyBLEP-VA, `CycleTable`-Wavetable, 4-op FM
  (Algorithmen wie bei den Klassikern, Operator-Feedback), Phase Distortion (Casio-CZ-Prinzip).
- Filter: `VoiceFilter` (10 Modelle) oder Z-Plane; ZDF-Leiter aus 5.2 als weitere Option.
- Hüllkurven ADSR (Noctuary `Envelope`, kurze Zeiten), Vibrato, Portamento.
- Verbreiterung: `Ensemble` (Chorus/Microshift/Velvet), Send-Delay, Reverb.
- **Tiefenregel für alles außer Kick und Bass:** jede `Poly`-Instanz und jeder SFX läuft durch
  einen Key-Tracking-Hochpass, der unter 200 bis 350 Hz nichts durchlässt. Das ist nicht nur
  Mischhygiene: die Phasenkopplung von Kick und Bass (Umsetzungsstand oben) gilt nur, solange im
  Band 40 bis 140 Hz nichts anderes spielt. Szabo fand denselben Hochpass hinter den verstimmten
  Stimmen des JP-8000.

Vektorisierung: 8 Unisono-Lanes einer Stimme = ein AVX-Register (NEON: zwei), Filter über
Stimmen. PolyBLEP-Korrektur zweiglos über Masken (Vergleich → Blend).

### 5.6 Arpeggio
Gleiche `Poly`-Engine, eigener Pattern-Generator: Akkordtöne (aus der Harmonieebene), Modi
Up/Down/UpDown/Random/Order, 1 bis 3 Oktaven, Sechzehntel/Zweiunddreißigstel, Gate,
Oktavwechsel alle 2 Takte, Verkettung mit dem Lead (Arp pausiert, wenn das Lead im selben
Register spielt: Maskierungsregel). Das Trance-Gate ist kein Arp-Merkmal mehr, sondern ein
Kanaleffekt (5.9).

### 5.7 Pad / Atmos
**Entscheidung 15.09. (Review):** Pads sind eine dritte `Poly`-Instanz mit `CycleTable`-Wavetables
als Standard-Oszillator, nicht die Noctuary-Quellen. Der Noctuary-`SourceSlot` (Additiv-Bank,
Harmonic Table, Cloud, Spectral) wird **nicht** kopiert und nicht entkoppelt. Zwei Gründe:
- **Genre:** Psytrance-Flächen sind Wavetable-Pads der Virus-, Microwave- und Serum-Ästhetik
  (Vokal-, Chor-, Glas- und Sweep-Tabellen), keine mikrotonalen Teiltonwolken. Die 608 generierten
  Tabellen aus Noctuarys `Tools/WavetableLib` (2048-Sample-Frames, Serum-Layout) kommen mit.
- **Quest-Budget:** ein Noctuary-Additiv-Slot kostet gemessen 4,9 % eines Desktop-Kerns, auf der
  Quest also etwa 25 %; acht Pad-Stimmen wären dort unmöglich. Der Wavetable-Leser liegt dagegen
  im Lane-Muster von `Vec.h`.
Bewegung kommt aus Tabellen-Position (LFO, Hüllkurve, Sektionsbogen), `Ensemble` (Velvet-
Dekorrelation), Reverb und Convolver. Akkorde aus der Harmonieebene mit Stimmführungs-Constraint
(minimale Bewegung). Sidechain vom Kick ("pumpende" Pads im Drop) über den Ducker; Trance-Gate im
Kanalzug (5.9). Sollten die Breakdowns später sich entwickelnde Texturen brauchen, ist Noctuarys
`Cloud` (Granular ohne Sample, aus dem eigenen Pad-Bus) der Kandidat, nicht die Additiv-Bank.

### 5.8 SFX (psychedelische Effekte)
Ein Generator mit Ereignistypen, die der Komponist an Formpunkte setzt:

| Typ | Verfahren | Wo im Set |
|---|---|---|
| Riser / Downlifter | Rauschen + Pad-Ton mit Tonhöhen- und Cutoff-Rampe über N Takte, tempo-synchron | Build / nach dem Drop |
| Impact | Sub-Sinus + Rauschstoß + Reverb-Burst | Drop-Eins |
| Snare-Roll | Percussion-Lane mit Rate 1/16→1/32→1/64 und Crescendo | Build |
| Zap / Laser | FM mit schneller Tonhöhenhüllkurve, Resonanz | Sechzehntel-Füller |
| Sweep | resonanter Filter (Z-Plane) über Rauschen oder den Pad-Bus | Übergänge |
| Stutter / Beat-Repeat | `GrainRing`, Fenster 1/8 bis 1/64, tempo-synchron, Pitch optional | vor Drops, Infected-Stil |
| Reverse-Reverb | Reverb der kommenden Note vorab gerendert und rückwärts eingespielt (möglich, weil der Komponist voraus ist) | Breakdown → Build |
| Vocal-Formant | Puls/Sägezahn durch Formantfilter mit Vokalfolge; kein Sample, kein Text | Breakdowns |
| Tape-Stop / Pitch-Drop | Wiedergaberate-Rampe eines Bus-Puffers | Break-Anfang |
| Pre-Drop-Abriss | Reverse-Reverb-Fahne oder Formant-Schuss auf dem vierten Beat vor der Drop-Eins (6.2) | Build → Drop |
| Pitch-Delay | StereoDelay mit `Shifter` in der Rückkopplung (Quinte, Oktave) | Acid, Lead |
| Phaser / Flanger | ZDF-Phaser nach Zavalishin; Kiiski, Esqueda, Välimäki 2016 als Referenz für Zeitvarianz | Pads, Hats |
| Bitcrush / Downsample | mit Anti-Aliasing (Tiefpass vor Dezimation), Trocken/Nass | Hi-Tech |

### 5.9 Mixer und Master
- Kanalzug je Erzeuger (Percussion: je Lane + Gruppe): Gain, Pan, 3-Band-EQ (SVF), Hochpass,
  Sends A (Reverb kurz), B (Reverb lang / Convolver), C (Delay), Sidechain-Eingang.
- **Trance-Gate als Kanaleffekt** (Review 15.09.) für Pad und Lead: tempo-synchrone
  Amplitudenmaske (Sechzehntel, Achtel, Triolen, eigene Muster) mit Raised-Cosine-Flanken und
  einstellbarem Cutoff-Duck auf dem Kanalfilter. Der Komponist schaltet es an Formpunkten: ein
  schwebendes Pad, das im Breakdown plötzlich im Sechzehntel-Staccato pumpt.
- **Sidechain-Matrix:** Quelle Kick (oder Snare) → Ziele Bass, Pads, Leads, Delay-Return;
  Ducker mit Attack/Hold/Release/Tiefe, Hüllkurve statt echter Kompression (deterministischer
  und billiger). Kompressor für Busse nach Giannoulis, Massberg, Reiss (JAES 2012).
- **Master:** EQ → Bus-Kompressor → MidSide (Mono-Bass unter 120 Hz, Mono-Guard) → Lookahead-
  True-Peak-Limiter (Oversampling 4×, BS.1770-Messung) → Loudness-Meter (LUFS, LRA, True Peak,
  Sones). Zielwert per Stilprofil (Psytrance liegt meist bei −8 bis −6 LUFS integriert; Wert
  einstellbar).
- **Auto-Gain-Staging:** jedes Sound-Preset trägt eine offline gemessene Referenzlautheit, so dass
  ein neu gewürfeltes Set nicht erst ausgepegelt werden muss (Lehre aus Noctuary 2.0.3).

## 6. Der Komponist

### 6.1 Ebenen
1. **Set:** Länge, Trackzahl (Länge/8 min ± Streuung), Tempo-Verlauf, Tonartenreise, Energiebogen
   (vom Nutzer gezeichnete Kurve oder Dramaturgie-Preset: Warm-up, Peak-Time, Morning, Closing),
   Stilprofil oder Profil-Reise (z. B. Progressive → Full-On → Goa zum Sonnenaufgang).
2. **Track:** Form aus einer probabilistischen Grammatik über Sektionstypen mit Längen in
   Zweierpotenzen; Auswahl der aktiven Erzeuger je Sektion (Instrumentierungs-Matrix); Tonart,
   Skala, Akkordfolge; Motive (Lead, Arp) als Track-Identität, die wiederkehren.
3. **Sektion:** Dichte, Register, Filteröffnung, FX-Rate aus Energiebogen und Sektionstyp;
   Automationsrampen (Cutoff-Öffnung im Build, Reverb-Größe im Break).
4. **Pattern (1 bis 4 Takte):** Steps pro Erzeuger; Variationsoperatoren alle 2/4/8 Takte.

### 6.2 Form-Grammatik
Kontextfreie Grammatik mit Gewichten, pro Stilprofil andere Gewichte:
```
Track   → Intro Body Outro
Body    → Groove Build Drop Break Build Drop         (Full-On-Standard)
        | Groove Drop Break Drop Break Drop          (Progressive, flacher)
        | Intro2 Build Drop Break Build Drop Drop2   (Goa, langer zweiter Drop)
Drop    → Drop16 Drop16 Var | Drop32 Var             (Var = variierte Wiederholung)
```
Längen aus einer Verteilung über {8, 16, 32, 64}; harte Regeln als Constraints (kein Drop
kürzer als 16, Build 8 oder 16, Break-Anteil 15 bis 30 %).

**Pre-Drop-Vakuum** (Review 15.09., feste Regel): der letzte Takt eines Builds bündelt die
Spannung. Kick und Bass setzen aus, die Snare-Roll endet auf Beat 3, und Beat 4 ist leer bis auf
einen einzelnen Abriss (Reverse-Reverb-Fahne oder Formant-Schuss, 5.8). Varianten mit
Gewichten im Stilprofil: ganzer Takt leer, halber Takt, nur Beat 4, Kick allein auf Beat 4. Die
Kick-Ausklang-Grenze macht das Vakuum wirklich still: nach dem letzten Schlag ist innerhalb eines
Slots nichts mehr da. Referenz für grammatikbasierte
Formmodelle: Rohrmeier 2011 (Harmonie), Steedman 1984; für EDM-Struktur die eigene Messung an
Referenzmaterial (2.5).

### 6.3 Energiebogen
Der Bogen ist eine Kurve E(t) in [0, 1] über das Set; Sektionstypen werden so gewählt, dass die
Sektions-Energie (Drop 1.0, Build steigend, Break 0.3, Intro 0.4) der Kurve folgt. Das
Spannungsmodell nach Farbood (2012, "A parametric, temporal model of musical tension") liefert
die Abbildung von Lautheit, Dichte, Register und Dissonanz auf empfundene Spannung; der Bogen
steuert genau diese vier Größen pro Sektion.

### 6.4 Harmonie
Skalenbibliothek (äolisch, phrygisch, phrygisch-dominant, harmonisch Moll, doppelt-harmonisch,
Dorisch für Progressive), Akkordfolgen aus einer kleinen Übergangsmatrix pro Skala (i–bII,
i–bVI–bVII, i–v, i–iv), Tonartenreise zwischen Tracks per Quintverwandtschaft, Parallele oder
Halbtonrückung (Gewichte im Stilprofil). Kick-Tuning und Bassgrundton folgen der Tonart.

### 6.5 Melodik (Acid, Lead, Arp)
- **Constraint-Markov-Ketten** (Pachet und Roy 2011, "Markov constraints"): Übergangsstatistik
  über Skalenstufen, **aus dem MIDI-Korpus gelernt** (6.9, Stufe A) und je Stilprofil gewichtet,
  harte Constraints (Ambitus, Start-/Endton, Tonart, keine Kollision mit dem Bassregister), so
  dass jede gezogene Sequenz die Constraints erfüllt, statt nachträglich zu verwerfen.
- **Kontur:** eine Zielkontur (Bogen, Aufstieg, Pendel) als weiche Präferenz.
- **Motiv und Variation:** das Track-Motiv (2 bis 4 Takte) wird durch Transposition,
  Umkehrung, rhythmische Verschiebung, Verkürzung, Oktavierung variiert; ein
  Ähnlichkeitsmaß (Editierdistanz über Stufen/Rhythmus) hält Variationen im Zielfenster
  (erkennbar, aber nicht identisch).
- **Acid-Spezifika:** Akzent- und Slide-Wahrscheinlichkeiten pro Step-Position, Cutoff-Verlauf
  als eigene Sequenz.

### 6.6 Rhythmus und Groove
Euklid (Toussaint 2005) + Wahrscheinlichkeiten + Synkopenmaß (Longuet-Higgins und Lee 1984;
Sioros et al. 2014 zum Zusammenhang Synkope/Groove) als Zielgröße pro Lane und Sektion. Psytrance
ist weitgehend quantisiert; Mikro-Timing nur als kleine, feste Offsets pro Lane (Hats leicht
spät), Velocity-Muster als Akzentmodelle. GrooVAE (Gillick et al. 2019) ist die Referenz für
gelerntes Mikro-Timing, hier bewusst nicht nötig.

**Bass-Slot-Hüllkurve: Parameter, keine Regel.** Ein Review vom 15.09. empfahl, die erste Bassnote
nach der Kick kürzer (Gate 65 %) und 1,5 dB leiser zu setzen als die beiden folgenden ("Astrix-
Trick"). Nachgemessen an neun Referenztracks (60 s aus der Mitte, Band 40 bis 200 Hz, Tempo per
Autokorrelation, Energie in 16 Slices pro Beat gefaltet): Note 2 gegen Note 3 liegt bei Astrix
−0,7 und −0,6 dB, Astral Projection −0,1, Hallucinogen −0,2, Juno Reactor +1,2, 1200 Mics +0,4
und +0,5 dB. Alle neun innerhalb von ±1,2 dB; die scheinbar lautere Note 4 ist die anschwellende
nächste Kick in den letzten beiden Slices. Die Referenzen spielen drei gleich laute Noten, und die
phasengleichen, identischen Sechzehntel des Umsetzungsstands entsprechen genau dem. Die
Slot-Hüllkurve (Gate und Velocity je Slot, Velocity wirkt über `vel_to_cutoff` auch auf die
Helligkeit) kommt als Parameter des Stilprofils mit flachem Standard, damit sie formbar bleibt.

Nebenbefund derselben Messung: die Kick liegt im Bassband nach 40 bis 50 ms erst 3 bis 5 dB unter
ihrem Maximum, die Bassnoten liegen 6 bis 9 dB unter der Kick. Der Render zeigt dieselben
Verhältnisse; die Ausklang-Grenze ist nicht zu streng.

### 6.7 Übergänge zwischen Tracks
Da alle Tracks aus einer Hand kommen, sind Übergänge kompositorisch, nicht nur DJ-Blenden:
- Outro von A und Intro von B teilen Tempo, Kick und Phrasenraster; Bass-Swap an einer
  32er-Grenze (A-Bass aus, B-Bass ein), Hats von B kommen 16 Takte früher, Pads von A bleiben
  16 Takte länger.
- Tonartwechsel an der Swap-Stelle, per SFX-Sweep und Impact kaschiert.
- Tempo-Rampe über 16 bis 32 Takte, wenn das Stilprofil wechselt.
- Referenzen für automatische DJ-Übergänge: Ishizaki, Hoashi, Takishima (ISMIR 2009); Bittner
  et al. (ISMIR 2017, Playlist-Sequenzierung und Übergänge); Schwarz und Fourer (2021,
  DJ-Mix-Reverse-Engineering) für die Messung, wie lang echte Übergänge sind.

### 6.8 Sperren und Neuwürfeln
Jede Einheit (Set-Bogen, Track-Form, Sektion, Pattern, einzelne Lane) hat einen Sperr-Schalter
und einen eigenen Seed-Zweig. "Neu würfeln" ersetzt nur Ungesperrtes; der Rest bleibt bitgleich.
Das ist die wichtigste Bedienidee: der Nutzer kuratiert, statt zu programmieren.

### 6.9 Lernende Anteile: der symbolische Pattern-Generator

**Datenlage (gemessen 15.09.2026).** Unter `M:\Midi` liegen gekaufte MIDI-Packs, fast
ausschließlich kurze, einstimmige Loops mit Rollenbeschriftung im Pfad oder Dateinamen:

| Pack | Dateien | Inhalt |
|---|---|---|
| EMP Psytrance Bundle | 166 | Bass, Melodies, Riffs, Arps, Construction Kits; 4 bis 8 Takte, Format 0, 96 PPQ |
| PSYTRANCE MIDI BUNDLE (Sonicspore) | 824 | Basslines, Arpsy 1 bis 3, Step-and-Hold-Rhythmen, Microdimension-Arps; 2 bis 4 Takte, oft Tonart im Namen |
| TOTAL_MIDI_PSY_TRANCE | 274 | Basslines, Leads, Dark/Progressive-Loops, Kits mit Pads; Tempo im File (134 bis 170) |
| Unison Psytrance Drum Collection | 349 | 50 Kits × Lanes (Kick, Hat, Perc, Snare-Clap) + konsolidierte Drum-Spur, 130 BPM |
| VORTEX Trance Bundle | 6870 | Trance-Pads, -Melodien, 5500 Fan-Transkriptionen bekannter Trance-Tracks, teils mehrspurig und ganze Stücke |
| Fremdgenres (Piano 50k, Midi Klowd 13k, Atmos 5,7k, Star Samples > 1,6 Mio) | | für Psytrance-Patterns ungeeignet, höchstens als Vortraining für Tonhöhen-/Rhythmusstatistik |

Das ist genau die Granularität der Pattern-Ebene (6.1, Ebene 4), aber kein Korpus ganzer
Psytrance-Tracks. Konsequenz: **Patterns lernen, Form regeln.** Die Packs sind gekauft: sie
dienen nur dem lokalen Training; weder Dateien noch erkennbar reproduzierte Loops verlassen den
Rechner, und das Modell wird auf Memorisierung geprüft (Nächster-Nachbar-Abstand generierter
Patterns zum Korpus als Testmetrik).

**Aufbereitung (`Tools/corpus/`, Python).** Rolle aus Pfad/Name (bass, arp, riff/lead, pad,
drum-lane), Tonart per Krumhansl-Schmuckler-Profil oder aus dem Namen, Transposition nach
A-Moll-Bezug, Quantisierung auf ein 1/16-Raster (1/32 wo nötig, Triolen markiert), Takte 2/4/8,
Dubletten entfernt. Augmentierung: diatonische Verschiebung innerhalb der Skala, Oktavlage,
zyklische Rotation um ganze Takte. Ergebnis rund 1600 Psy- und 6900 Trance-Patterns vor
Augmentierung; Trance mit geringerem Gewicht.

**Stufe A: Markov variabler Ordnung + Constraints (Phase 5).** Pro Rolle ein
Vorhersagemodell variabler Ordnung (PPM/VMM nach Begleiter, El-Yaniv und Yona 2004) über
Tokens (Stufe relativ zur Tonart, Oktave, Dauer, Velocity-Klasse, Akzent/Slide) mit
Constraint-Dekodierung nach Pachet und Roy (2011): Skala, Ambitus, Kick-Lücke für den Bass,
Start-/Endton und Kontur als harte bzw. weiche Nebenbedingungen. Tabellen als Daten im
Programm (einige hundert KB), läuft auf der Quest trivial. Das ersetzt die handgeschriebenen
Übergangsmatrizen aus 6.5 durch gemessene.

**Stufe B: kleiner Transformer (Phase 8).** Token-Strom im REMI-/Compound-Word-Stil (Hsiao et
al. 2021), bedingt auf Rolle, Stilprofil-Klasse, Taktzahl und Tonart; etwa 4 Schichten,
Breite 128 bis 256, 1 bis 3 Mio. Parameter, Training in PyTorch auf dem PC mit starker
Regularisierung (der Korpus ist klein), Export der Gewichte in int8 und eine eigene C++-
Inferenz über `Vec.h` (Matmul, Softmax, Attention über höchstens 512 Tokens). Ein 8-Takt-
Pattern sind wenige hundert Tokens; auf einem kleinen Quest-Kern ein Bruchteil einer Sekunde,
weit innerhalb der Vorlaufzeit des Komponisten. Dekodierung mit denselben Constraint-Masken wie
Stufe A. Bewertung: Held-out-NLL, Memorisierungsabstand, die Metriken aus 11.4, und der Ranker.
Referenzen: Music Transformer (Huang et al. 2018), Compound Word Transformer (Hsiao et al.
2021), Multitrack Music Transformer (Dong et al. 2023).

**Ranker.** Ein kleines Modell (Gradient Boosting oder MLP über Merkmale: Dichte, Synkope,
Ambitus, Wiederholungsrate, Kontur, Modell-NLL), trainiert auf Daumen hoch/runter des Nutzers
je Pattern/Sektion, wählt aus k Kandidaten. Läuft auf dem Composer-Thread, auch auf der Quest.

**Kalibrierung aus Referenzaudio.** 40 Psytrance-Tracks (5,4 h) und 35 Trance-Tracks liegen
lokal vor (Album-Tag "Psytrance Collection" bzw. "Trance Collection"), überwiegend Goa und
Full-On der Referenzkünstler. `Tools/analyze_ref.py` misst Tempo, Sektionsgrenzen
(Neuheitskurve nach Foote 2000 über Lautheit und Spektrum), Kick-Grundton (Tonhöhenverfolgung
im Band 40 bis 120 Hz), Bass-Onset-Muster (Onsets im Band 40 bis 200 Hz relativ zum
Kick-Raster), Hat-Dichte, Lautheit und Crest. Daraus die Stilprofile für Goa und Full-On; für
Progressive und Dark/Forest fehlen Referenzen, dort bleiben die Profile zunächst aus der
Literatur und werden nachkalibriert, sobald der Nutzer Stücke nachlegt. Nur Statistiken werden
gespeichert.

**Was bewusst nicht kommt.** Neuronale Klangsynthese (DrumGAN, Nistal, Lattner, Richard 2020;
DDSP, Engel et al. 2020) ist für die Quest zu teuer und für synthetische Psytrance-Klänge
unnötig; die Klangerzeuger bleiben parametrisch.

## 7. MIDI-Export und Dateiformate
- **SMF Format 1**, PPQ 960, Tempo-Karte als Meta-Events, Taktart, Tonart (FF 59), Marker (FF 06)
  für Sektionen und Trackgrenzen, Track-Namen.
- Ein Track je Erzeuger; Percussion als ein Track mit GM-Notenzuordnung (36 Kick, 38 Snare,
  42/46 Hats, 49 Crash, 51 Ride ...) plus Zuordnungstabelle für die Lanes ohne GM-Entsprechung.
- Acid: Akzent = Velocity ≥ 100, Slide = überlappende Noten + CC 65 (Portamento an), Cutoff
  CC 74, Resonanz CC 71 (Konvention der gängigen 303-Emulationen).
- Automationsrampen als CC-Verläufe (Cutoff, Sends, Makros) mit 1/32-Auflösung.
- Export als ein Set-MIDI, als MIDI je Track, und als **WAV/FLAC-Stems** je Erzeuger aus dem
  Offline-Render (Noctuary `WavFile`, dr_flac).
- **`.phosset`** (Text, Zeilenform wie `.ambientset`): Seed, Stilprofil(e), Bogen, Sperren, und
  auf Wunsch die ausgerollte Partitur, damit ein Set auch nach Regeländerungen exakt wieder
  abspielbar ist.
- **Presets:** Sound-Presets je Erzeuger, Kits (Percussion), Stilprofile, Set-Dramaturgien,
  alle als `key=value`-Text im Noctuary-Stil, in Packs bündelbar.

## 8. GUI

### 8.1 Desktop (JUCE 9, wie Noctuary: Layout aus Parametern, `PHOS_SHOT`-Screenshot-Modus, Sprache Englisch)
Globale Tabs (Vorschlag, erweitert gegenüber der Aufgabenstellung):

| Tab | Inhalt |
|---|---|
| **Set** | Länge, Trackzahl, Tempo-Bereich, Stilprofil(e) mit Morph, Energiebogen (zeichenbar), Dramaturgie-Preset, Seed, Generieren / Play / Stop, Fortschritt, Loudness-Meter |
| **Arrange** | Zeitleiste des Sets: Tracks → Sektionen als Blöcke, Instrumentierungs-Matrix (welcher Erzeuger in welcher Sektion), Sperren, Neuwürfeln je Block, Sprung zur Sektion, Marker |
| **Kick** | Engine-Wahl, Sweep-Parameter, Sättigung, Klick, Layer, Tuning-Regel, Sample-Analyse |
| **Bass** | Pattern-Familie und Variationsrate (Noten-Hälfte), Oszillator, Leiter, Hüllkurve, Drive, Sidechain (Klang-Hälfte), Step-Anzeige |
| **Percussion** | 12 Lanes × (Engine, Sound-Parameter, Pattern-Regeln), Kit-Presets, Fill-Bank, Synkopen-Regler |
| **Acid** | Sequenz-Regeln (Ambitus, Akzent-/Slide-Wahrscheinlichkeit, Kontur), Diodenleiter, Akzent-Sweep, Verzerrung, Delay |
| **Lead** | Motiv-Regeln (Länge, Variationsoperatoren, Skala), `Poly`-Engine (Osc-Typen, Supersaw, Filter, Hüllkurven), Breite |
| **Arp** | Muster, Oktaven, Gate, Trance-Gate, Maskierung gegen Lead, eigene `Poly`-Instanz |
| **Pad / Atmos** | Noctuary-Quellen, Akkordregeln, Stimmführung, Sends, Sidechain |
| **SFX** | Ereignistypen mit Wahrscheinlichkeit je Formpunkt, Parameter je Typ, Vorhören |
| **Groove** | Mikro-Timing, Akzentmodelle, Swing, Synkopenziel je Sektionstyp |
| **Mod** | LFOs, Hüllkurven, Matrix, Makros (aus Noctuary) |
| **Mixer** | Kanalzüge, Sends, Sidechain-Matrix, Bus-Kompressor, Master-Kette, Meter |
| **Perform** | Live-Makros (Filter-Sweep, FX-Throw, Break jetzt, Drop jetzt, Stutter), Hand-Zuordnung für Quest, MIDI-Learn |
| **Export** | MIDI, Stems, `.phosset`, Offline-Render mit Fortschritt und Loudness-Bericht |
| **Style** | Stilprofile ansehen und editieren (die 60 Gewichte), aus Referenz kalibrieren, Ranker-Training (Daumen) |

Jeder Erzeuger-Tab ist zweigeteilt: links "Noten" (Pattern-Generator), rechts "Klang" (Synth),
darunter eine Step-/Piano-Roll-Vorschau der aktuellen Sektion mit Würfel- und Sperr-Knopf.

### 8.2 Quest 2
Entscheidung: **der komplette Generator läuft auf dem Gerät** (Komponist auf einem kleinen Kern,
Synthese auf einem großen, Qualitätsstufe `Quest`). Die Quest-Oberfläche ist Spieler und
Performer, nicht Editor: Set laden oder mit Standardprofil generieren,
Energiebogen mit der Hand zeichnen, Hände als Makros (Höhe → Cutoff, Abstand → FX-Menge, Pinch →
Stutter, Geste → Break/Drop), Handmenü wie in Noctuary Quest (Preset, Stil, Aufnahme). Bild:
die Kaleidoscope-Regeln (keine Kamerabewegung auf Audio, alles stetig); Beat/Sektion kommen
als Ereignisse aus der Partitur, nicht aus einer Audioanalyse.

### 8.3 Kaleidoscope-Kopplung (optional, klein)
OSC `/phos/beat i`, `/phos/bar i`, `/phos/section s f` (Typ, Energie), `/phos/key s`, `/phos/drop`.
Kaleidoscope bekommt einen kleinen Empfänger, der diese Cues dem Scheduler gibt (Sektionswechsel
→ Szenenwechsel, Drop → Flash-Cut). Der Scheduler muss dann nicht mehr aus dem Audio raten.

### 8.4 Parametersystem
Parameter in Blöcken pro Modulinstanz (`kick.sweep_start`, `perc3.decay`, `lead.osc1.detune`),
Deskriptor-Tabellen je Modul (Name, Bereich, Kurve, Einheit, Default, Automatisierbar), daraus
JUCE-Parameter, OSC-Adressen, Preset-Textform, Handbuch und `--list` generiert. Der Komponist
schreibt Parameter nur über Automationsrampen der Partitur, nie direkt.

## 9. Vektorisierung

- **Prinzip:** Structure-of-Arrays über Stimmen/Lanes. Ein Filter ist pro Sample rekursiv, aber
  acht Filter (AVX2) oder vier (NEON) laufen in Lanes. Alle Engines werden so gebaut, dass eine
  Lane-Gruppe der natürliche Verarbeitungsblock ist: Percussion-Lanes, Unisono-Lanes, Poly-Stimmen,
  Kick-Layer.
- **`phos/Vec.h`:** ein dünner Wrapper (`Vec8f` auf AVX2, `Vec4f` auf NEON, skalarer Rückfall,
  Compile-Zeit-Breite `Vec::N`): Laden/Speichern, +−×, FMA, min/max, Vergleich → Maske,
  `select`, `sqrt`, `rsqrt` + Newton, Horizontalsumme, Gather-Ersatz durch skalare Schleife
  (NEON hat kein Gather, Lehre aus Noctuarys Grain-Loop). Polynomiale `tanh`/`exp` für Lanes
  (Hüllkurven, Sättigung) mit gemessener Genauigkeit. Etwa 400 Zeilen, im Stil von `Simd.h`,
  mit dem erweiterten NEON-Shim für x86-Tests.
- **Was vektorisiert wird (Reihenfolge nach erwarteter Ersparnis):**
  1. Percussion-Kit: 12 Lanes Rauschen/Hüllkurve/SVF/Modal.
  2. `Poly`: Unisono-Oszillatoren mit zweiglosem PolyBLEP, Filter über Stimmen, Hüllkurven.
  3. ZDF-Leitern (Bass, Acid, Layer) 2× oversampled: Halbband-FIR als Skalarprodukt, vier
     nichtlineare Stufen mit polynomialem tanh in Lanes.
  4. Effekte: FDN-Householder 8×8 = ein AVX-Register (Noctuary-Reverb prüfen, ob schon so),
     Delays, Convolver (fertig), Limiter-Lookahead-Maximum.
  5. Kick-Layer.
- **Regeln:** skalarer Referenzpfad bleibt immer und ist das Orakel für die Lane-Pfade (Toleranz
  im Selbsttest); Messung vor und nach jedem Schritt mit `phos_render --bench` (Desktop, VTune wie
  in Noctuary; Quest per `adb shell`). Keine Vektorisierung ohne Messung.
- **Quest 2:** Snapdragon XR2 (Kryo 585: 1 + 3 große Cortex-A77-Kerne, 4 kleine A55; NEON 128
  Bit mit FMA, kein SVE). Audio auf einem großen Kern, Composer auf einem kleinen. Ziel ≤ 30 %
  eines großen Kerns bei 48 kHz / 256 für ein volles Drop-Arrangement. Qualitätsstufen
  (`Quality::Desktop / Quest`): Unisono 7 → 3, Oversampling 2× → 1× außer Bass, Pad-Polyphonie
  8 → 4, Reverb-Modus Classic, Convolver aus. Werte auf dem Gerät nachmessen.

Grober CPU-Rahmen (Desktop, ein Kern, 48 kHz, zu verifizieren in Phase 1/2):

| Modul | Ziel |
|---|---|
| Kick + Layer | < 0,5 % |
| Bass (2× OS) | < 1 % |
| Percussion 12 Lanes | < 2 % |
| Acid (2× OS) | < 1 % |
| Lead + Arp (`Poly`, 2 × 8 × 7 Unisono) | < 5 % |
| Pad (Noctuary-Quellen, 4 bis 8 Stimmen) | < 3 % |
| SFX + Sends + Master | < 3 % |
| Summe Drop | < 15 % |

## 10. Plattformen und Build

- **Repo:** `Core/` (Kern, statisch), `Plugin/` (JUCE 9 VST3 + Standalone), `Quest/` (NDK,
  OpenXR, Oboe; Gerüst aus Noctuary), `Tools/render` (`phos_render`: Offline-Render, `--bench`,
  `--midi`, `--stems`, `--set-file`, `--seed`, `--style`, `--hour` klemmt nichts fest, Lehre aus
  Noctuary: Zeit ist Beat-Position), `Tools/*.py` (Analyse, Stilkalibrierung, Handbuch),
  `Tests/` (selftest, hosttest/pluginval, racetest/TSan, Vec-Varianten AVX2/NEON-Shim/skalar),
  `Deploy/` (Inno Setup 7, `build_release.ps1` mit Intel-Baum wie Noctuary), `docs/`.
- **CMake:** `PHOS_BUILD_PLUGIN`, `PHOS_BUILD_TOOLS`, `PHOS_AVX2` (Default an auf x86-64),
  `PHOS_STATIC_RUNTIME` für Distribution, kein Fast-Math, LTO optional.
- **VST3:** Host-Playhead als Clock-Quelle (Tempo und Position vom Host, der Komponist folgt),
  MIDI-Out aus dem Plugin (VST3 erlaubt MIDI-Ausgabe; Prüfen, welche Hosts sie annehmen),
  Zustand = `.phosset` + alle Parameter.
- **Standalone:** eigene Clock, ASIO/WASAPI via JUCE, Aufnahme, MIDI-Clock-Ausgabe optional.
- **Quest:** `PHOS_MUTE=1` für Tests (Regel: stumm starten bei Tests).

## 11. Tests und Messungen

### 11.1 Selbsttest (`phos_selftest`, Muster Noctuary)
- Oszillatoren: Alias-Abstand PolyBLEP-Säge gegen ideale Säge (SNR > 60 dB bei 2 kHz).
- Leitern: Cutoff-Genauigkeit, Selbstoszillationsfrequenz, Stabilität bei Vollmodulation,
  Übereinstimmung Lane-Pfad gegen skalar (Toleranz).
- Kick: gemessene Tonhöhenhüllkurve gegen Sollkurve; Klick-Anteil; DC-Freiheit.
- Sequencer: sample-genaue Ereignisse an Blockgrenzen, Tempo-Rampen, Host-Sync-Sprünge.
- Komponist: alle Grammatik-Constraints (Zweierpotenzen, Drop ≥ 16, Break-Anteil), Skalentreue,
  Ambitus, Kick–Bass-Abstand, Determinismus (zwei Läufe bitgleich), Sperren (gesperrte Blöcke
  bitgleich nach Neuwürfeln).
- MIDI: Schreiben → Lesen → gleiche Partitur; Marker und Tempo-Karte.
- Master: True Peak ≤ −1 dBTP, LUFS im Zielfenster des Profils.

### 11.2 Vektor-Varianten
Wie Noctuarys `convtest`/`banktest`: dieselben Prüfungen dreimal gebaut (AVX2, NEON-Shim auf
x86, skalar) und gegeneinander verglichen; auf dem Gerät `phos_selftest` per `adb`.

### 11.3 Host- und Race-Tests
pluginval (Strictness 10) auf dem VST3, TSan-Build (WSL, Lehre aus Noctuary: die TSan-Falle im
Deployment-Memo beachten), Lasttest: 3-Stunden-Set offline mit Speicher-Hochwasserstand.

### 11.4 Hörprüfung und Metriken
- Solo-Renders je Erzeuger je Sektionstyp (Regel aus Noctuary: hört man jede Quelle?).
- `Tools/scene_metrics`-Analogon `Tools/set_metrics.py`: Onset-Dichten je Lane, Kick–Bass-
  Überlappungsenergie, Spektralschwerpunkt über Leistung (nicht Betrag, Messfalle 10.09.),
  Wiederholungsrate, LUFS-Verlauf gegen Energiebogen.
- `Tools/analyze_ref.py`: dieselben Metriken auf Referenzmaterial des Nutzers, für die
  Kalibrierung der Stilprofile (nur Statistiken werden gespeichert).
- A/B-Blindtest-Skript: zwei Seeds, Nutzer bewertet, Ergebnis füttert den Ranker.

## 12. Phasen und Meilensteine

Aufwand in Arbeitstagen ist eine Schätzung nach Noctuary-Erfahrung; die Reihenfolge ist
verbindlicher als die Zahlen.

| Phase | Inhalt | Ergebnis / Prüfstein | Tage |
|---|---|---|---|
| **0 Gerüst** | Repo, CMake, Noctuary-Modulkopie, `Vec.h` + Shim, Parametersystem, Clock/Sequencer, Partitur-Datenmodell, `phos_render`, Selbsttest-Skelett, `SourceSlot` von der Engine lösen | `phos_render` gibt Stille mit Tempo-Karte aus; Vec-Tests grün auf drei Pfaden | 2 |
| **1 Fundament** | Kick (parametrisch + Bridged-T), Bass (PolyBLEP + ZDF-Leiter + OS), Ducker, Bass-Pattern-Familien, Referenzmessung an Nutzer-Tracks | 4-Takt-Loop, der rollt; Kick–Bass-Metrik im Referenzfenster; erste CPU-Zahlen | 4 |
| **2 Rhythmus** | Percussion-Kit (5 Engines, 12 Lanes, SoA), Euklid/Synkope, Fill-Bank, Groove | 16-Takt-Groove mit Fills; Kit-Presets; Lane-Pfad = skalar | 4 |
| **3 Melodik** | Acid (Diodenleiter, Akzent, Slide, Squelch-Modus), `Poly` (Supersaw als Standard mit dynamischem Detune, Wavetable, VA, 4-op FM, PD), Key-Tracking-Hochpass für alles außer Kick/Bass, Lead-/Arp-Generatoren, Harmonieebene, Constraint-Markov, Squelch-Kalibrierung an Hallucinogen | 32-Takt-Drop mit allen Erzeugern; Skalentreue-Test; Bassband unter 140 Hz nur Kick und Bass | 5 |
| **4 Fläche + FX** | Pad als Wavetable-`Poly` mit WavetableLib, SFX-Generator inkl. Pre-Drop-Abriss, Trance-Gate im Kanalzug, Sends, Sidechain-Matrix, Bus-Kompressor, Master-Limiter, Meter, Auto-Gain-Staging | kompletter Track 8 min offline, LUFS im Ziel | 3 |
| **5 Komponist** | Korpus-Aufbereitung (`Tools/corpus`), Markov-Stufe A je Rolle, Form-Grammatik mit Pre-Drop-Vakuum, Bass-Slot-Hüllkurve im Stilprofil, Energiebogen, Tonartenreise, Übergänge, Sperren/Neuwürfeln, Stilprofile (5, Goa/Full-On aus `analyze_ref.py`), `.phosset`, MIDI-Export | 60-Minuten-Set aus einem Seed; MIDI in einer DAW geöffnet; Determinismus-Test; Memorisierungsabstand | 6 |
| **6 GUI** | JUCE-Tabs, Arrange-Zeitleiste, Step-Vorschauen, Perform-Makros, Export-Tab, Screenshot-Modus, Handbuch-Generator | Standalone + VST3 bedienbar; pluginval grün | 6 |
| **7 Quest** | NDK-Build, Qualitätsstufen, NEON-Messung auf Gerät, Performer-UI, Hand-Makros, OSC-Bridge | Set läuft auf der Quest 2 unter 30 % eines Kerns | 4 |
| **8 Transformer** | Tokenisierung, Training (PyTorch, PC), int8-Export, C++-Inferenz über `Vec.h`, Constraint-Dekodierung, A/B gegen Stufe A, Ranker | **Training fertig 16.09.**: Held-out-NLL 1,3457 gegen 2,0833 (SSM) und 2,5084 (Stufe A), `.phosmdl` und Orakel liegen bei; offen: Quest-Messung, Hörvergleich, Ranker | 5 |
| **9 Qualität** | Hörrunden je Erzeuger, Nachkalibrierung, Kaleidoscope-Cues, Release (Inno Setup, README, PDF-Handbuch) | v1.0 | 5 |

Nach Phase 1 gibt es den ersten hörbaren Prüfstein; nach Phase 5 ist das Produkt inhaltlich
komplett (ohne GUI), was für die Bewertung der musikalischen Qualität reicht. Die GUI kommt
bewusst spät: Layout aus Parametern (Noctuary-Lehre) macht sie billig, sobald die Parameter
stehen. Der Transformer kommt nach der Quest, weil Stufe A denselben Token-Raum und dieselben
Constraints benutzt: Stufe B ersetzt nur das Vorhersagemodell, nichts drumherum.

## 13. Risiken

1. **Musikalische Qualität des Regelwerks** (größtes Risiko). Gegenmittel: früher Prüfstein
   (Phase 1), Metriken gegen Referenz, Sperren/Neuwürfeln als Kuratierungsschleife, Ranker.
2. **Kopplung an `NoctuaryCore`**: `Modulation.h` hängt an Noctuarys `ParamId`. Gegenmittel:
   beim Kopieren auf das Block-Parametersystem umschreiben. `SourceSlot` ist kein Risiko mehr,
   weil er nicht kopiert wird (5.7).
3. **Quest-2-Budget**: Oversampling und Unisono sind teuer. Gegenmittel: Qualitätsstufen von
   Anfang an im Code, Messung auf dem Gerät ab Phase 1 (`adb`).
4. **Determinismus zwischen Pfaden**: AVX2/NEON/skalar unterscheiden sich im letzten Bit; ein
   Komponist, der von Audio-Messwerten abhinge, würde divergieren. Gegenmittel: der Komponist
   liest nie Audio, nur die Partitur und den RNG.
5. **Rechtliches**: Künstlernamen nur als Referenz in der Dokumentation, keine Fremd-Samples,
   keine Preset-Namen mit Künstlern.
6. **MIDI-Out aus VST3**: Host-Unterstützung uneinheitlich; Export als Datei ist der sichere Weg.

## 14. Entscheidungen des Nutzers (15.09.2026)

1. **Name:** Phosphene.
2. **Noctuary:** Modulkopie, weil Phosphene sich in eine andere Richtung entwickeln kann. Jede
   kopierte Datei nennt ihre Herkunft und den Noctuary-Stand.
3. **Quest:** der komplette Generator läuft auf dem Gerät.
4. **Samples:** keine; vollständige Synthese. Höchstens später Sprach-Samples für Breakdowns.
5. **Lernende Anteile:** symbolischer Generator erwünscht; umgesetzt als Stufe A (Markov aus dem
   Korpus, Phase 5) und Stufe B (Transformer, Phase 8), siehe 6.9.
6. **Sprache:** GUI und Handbuch Englisch.
7. **Referenzmaterial:** 40 Psytrance- und 35 Trance-Tracks in
   `C:\Users\Rene\Desktop\Kandidaten\Pop - Kopie` (Album-Tags), MIDI-Korpus unter `M:\Midi`.
   Noch offen: einige Progressive- und Dark/Forest-Stücke, sobald der Nutzer sie findet, für
   die Kalibrierung dieser beiden Profile.

## 15. Literatur (Auswahl, je Baustein)

- Välimäki, V.; Huovilainen, A. (2007). Antialiasing Oscillators in Subtractive Synthesis. IEEE Signal Processing Magazine 24(2). (PolyBLEP)
- Välimäki, V. (2005). Discrete-Time Synthesis of the Sawtooth Waveform with Reduced Aliasing. IEEE SPL. (DPW)
- Zavalishin, V. The Art of VA Filter Design (Native Instruments, Rev. 2.x). (TPT/ZDF, Moog- und Diodenleiter, Phaser)
- Huovilainen, A. (2004). Non-Linear Digital Implementation of the Moog Ladder Filter. DAFx.
- D'Angelo, S.; Välimäki, V. (2013). An Improved Virtual Analog Model of the Moog Ladder Filter. ICASSP.
- Parker, J.; Zavalishin, V.; Le Bihan, E. (2016). Reducing the Aliasing of Nonlinear Waveshaping Using Continuous-Time Convolution. DAFx. (ADAA)
- Werner, K. J.; Abel, J. S.; Smith, J. O. (2014). A Physically-Informed, Circuit-Bendable, Digital Model of the Roland TR-808 Bass Drum Circuit. DAFx. Dieselben Autoren 2014: TR-808 Cymbal (ICMC/SMC) und Cowbell (AES 137).
- Szabo, A. (2010). How to Emulate the Super Saw. Thesis, Linköping.
- Serra, X.; Smith, J. O. (1990). Spectral Modeling Synthesis. Computer Music Journal.
- Aramaki, M. et al. (2006). A Percussive Sound Synthesizer Based on Physical and Perceptual Attributes. CMJ.
- Smith, J. O. Physical Audio Signal Processing; Bilbao, S. Numerical Sound Synthesis. (Modal)
- Välimäki, V.; Alary, B.; Politis, A. (2017). Velvet-Noise Decorrelation.
- Kiiski, R.; Esqueda, F.; Välimäki, V. (2016). Time-Variant Gray-Box Modeling of a Phaser Pedal. DAFx.
- Giannoulis, D.; Massberg, M.; Reiss, J. D. (2012). Digital Dynamic Range Compressor Design: A Tutorial and Analysis. JAES.
- ITU-R BS.1770-4; EBU R 128. (Lautheit, True Peak)
- Toussaint, G. (2005). The Euclidean Algorithm Generates Traditional Musical Rhythms. BRIDGES.
- Longuet-Higgins, H. C.; Lee, C. S. (1984). The Rhythmic Interpretation of Monophonic Music. Music Perception. (Synkopenmaß)
- Sioros, G. et al. (2014). Syncopation Creates the Sensation of Groove in Synthesized Music Examples. Frontiers in Psychology.
- Pachet, F.; Roy, P. (2011). Markov Constraints: Steerable Generation of Markov Sequences. Constraints.
- Rohrmeier, M. (2011). Towards a Generative Syntax of Tonal Harmony. Journal of Mathematics and Music. Steedman, M. (1984). A Generative Grammar for Jazz Chord Sequences. Music Perception.
- Farbood, M. (2012). A Parametric, Temporal Model of Musical Tension. Music Perception.
- Ishizaki, H.; Hoashi, K.; Takishima, Y. (2009). Full-Automatic DJ Mixing System. ISMIR. Bittner, R. et al. (2017). Automatic Playlist Sequencing and Transitions. ISMIR. Schwarz, D.; Fourer, D. (2021). Methods and Datasets for DJ-Mix Reverse Engineering.
- Gillick, J. et al. (2019). Learning to Groove with Inverse Sequence Transformations. ICML.
- Huang, C.-Z. A. et al. (2018). Music Transformer. Hsiao, W.-Y. et al. (2021). Compound Word Transformer. AAAI. Dong, H.-W. et al. (2023). Multitrack Music Transformer. ICASSP.
- Begleiter, R.; El-Yaniv, R.; Yona, G. (2004). On Prediction Using Variable Order Markov Models. JAIR. (PPM/VMM für Stufe A)
- Krumhansl, C. L. (1990). Cognitive Foundations of Musical Pitch. (Tonartprofile für die Korpus-Aufbereitung)
- Foote, J. (2000). Automatic Audio Segmentation Using a Measure of Novelty. ICME. (Sektionsgrenzen im Referenzaudio)
- Nistal, J.; Lattner, S.; Richard, G. (2020). DrumGAN. ISMIR. Engel, J. et al. (2020). DDSP. ICLR.
- Schlecht, S.; Habets, E. (2015, 2020); Dal Santo et al. (Colourless FDN): bereits in Noctuarys Reverb umgesetzt.
