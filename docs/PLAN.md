# Phosphene: Plan für den Psytrance-Set-Generator

Spezifikation, Stand 15.09.2026 nach den Entscheidungen des Nutzers (Abschnitt 14); was seither
gebaut wurde, steht im Journal (`docs/rounds/`). Name **Phosphene**, Repo
`G:\Tools\VRAudio\PsytranceGenerator`, Namensraum `phos::`, Präfix `PHOS_`, Werkzeuge `phos_render`,
`phos_selftest`. Code-Kommentare im Doxygen-Format (`docs/Doxyfile`).

## Stand der Umsetzung

Phasen 0 bis 9 sind gebaut (Abschnitt 12), dazu Plugin, Quest-App, Release-Weg und die Runden danach.
Was jede Runde gebaut, gemessen und entschieden hat, steht im Journal
[docs/rounds/2026-09.md](rounds/2026-09.md), ein Block pro Runde mit Datum und Namen. Dieses Dokument ist
die Spezifikation: es beschreibt den Zielzustand und die Gründe, das Journal den Weg.

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
- **Keine kleine None in der Fläche** (Regel des Nutzers, 25.09.2026): kein Pad- oder Drone-Ton einen Halbton
  über dem Grundton, den Bass und Drone halten, und keiner über dem eigenen Akkordgrundton -- auch nicht im
  DJ-Übergang über dem Bass des alten Tracks. Ebenso kein gehaltener Leitton unter der Tonika und kein
  Tritonus über dem Pad-Grundton; Dissonanz in Flächen entsteht über den Klang (Cluster, Rauschen, Drones),
  nicht über Voicings. maj7 nur selten und nur auf bVI/bIII.
- **Bass ist Orgelpunkt:** b2 und Leitton im Bass nur als Auftakt (höchstens zwei Sechzehntel am Taktende).
  "Bass Follows Chords" folgt dem Grundton, den das Pad hält, nur wo das Pad spielt, und die Drone geht mit.
- **Die b2 ist in Phrygisch eine Stufe** für Acid und Lead (E–F–E), auch betont (Lead: Zählzeit 2 und 4) und
  gehalten, höchstens ein Viertel der Noten; übrige Farbtöne bleiben Nachbartöne. Lead und Counter schlagen auf
  1 und 3 nie im Halbton- oder Nonenabstand zusammen. Der Tonanteil eines Effekts landet nie auf b2 oder
  Leitton. Die phrygische b2 gehört
  Lead und Arp als kurzer Akzent (Farbton-Slots); auf dem bII des Pendels hält das Pad die Tonika mit kleiner Sexte.
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
- **Tiefe, Weite, Klarheit** (Mix-Leitfaden des Nutzers, 25.09.2026; Zahlen gegen seine 40 Referenzen
  gemessen, `Tools/mix_audit.py`, wo der Leitfaden nur Erfahrungswerte nennt):
  - *Drei Ebenen, drei Räume* (Nachtrag "Lehren aus dem Dark-Ambient-Guide", 25.09.2026): vorn trocken
    (Kick, Bass; der Lead etwas im Nahraum A, 0,4–0,9 s), Mitte in der Plate B (Acid, Counter, Arp, Stab;
    1,5 s im Drop, 4 s im Break), hinten ebenfalls in B mit mehr Anteil (Pads, Drone); B speist seriell den
    Fernraum C (Hall, 10 s, 400 Hz–3 kHz), den Bett und Stimmen direkt erreichen. C klingt nur in Intro und
    Break. Pre-Delays sind Entfernungshinweise, kein Tempowert: vorn 40–60 ms, Mitte 20–30, hinten 0–10.
    Jeder Raum hat eigene Send-Filter.
  - *Distance* je Stimme (0 nah … 1 fern, Vorgabe die eigene Ebene) koppelt Pegel, Tiefpass, Plate-Anteil
    und Breite; der Lead geht im Break nach hinten, der Counter nähert sich im Haupt-Break über 16 Takte.
    Pads und Drone bewegen sich zusätzlich frei (13/21/34 s, Verhältnisse des Goldenen Schnitts).
  - *Gehaltene Pads:* in Intro und Break Unison ≤ 6 Cent; solange der Bass läuft, keine Terz im Pad.
  - *Ducking-Matrix* von der Kick: Bass 2–4 dB (Release 40–70 ms), Linien 1–2 dB (50–80 ms), hintere
    Ebene 4–8 dB (100–150 ms), Returns 3–6 dB (80–120 ms); Effekte nicht (ein Impact fällt auf die Kick).
    Vom Lead: der Counter 1–3 dB, das Präsenzband des Pads (500 Hz–3 kHz) 2–4 dB, solange eine Lead-Note
    klingt.
  - *Mix nach Phase:* Break 7 dB Gain unter dem Drop (nach Kompressor und Limiter rund 5 dB Lautheit),
    Pad dort etwas lauter, Lead nach hinten; Drop mit Lead vorn, Pad voll breit (die Referenzen sind
    breiter als der Leitfaden), tieferem Ducking; Build-up zieht den Mix zusammen (Pad auf 60 %, Plate auf 1,2 s, Fernraum aus,
    Hochpass der Linien +180 Hz) und öffnet sich im Drop; Übergänge ein Takt in den Drop, vier in den
    Break. Vor jedem Achttaktwechsel werden die Effekte einen Schlag lang in die Plate geworfen.
  - *Monitor-Schalter* (Master): Mono, Sub (Tiefpass 80 Hz), Side -- nach dem Meter.
  - *Prüfungen nach dem Render* (ctest `mixaudit`): Korrelation, Mono-Verlust, Spektralneigung,
    Energie unter 120 Hz je Stimme, Kick-Bass-Lücke, Kontrast Drop/Break, Stimmung Kick/Bass; dazu der
    Ablations-Bericht (jede Stimme weggelassen, spektrale Distanz zum vollen Mix).

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
