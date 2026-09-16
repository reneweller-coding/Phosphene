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
| **8 Transformer** | Tokenisierung, Training (PyTorch, PC), int8-Export, C++-Inferenz über `Vec.h`, Constraint-Dekodierung, A/B gegen Stufe A, Ranker | Stufe B auf PC und Quest; Held-out-NLL und Hörvergleich dokumentiert | 5 |
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
