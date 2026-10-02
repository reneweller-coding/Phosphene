/**
 * @file PluginProcessor.cpp
 * @brief Implementation of the Phosphene processor: parameters, threads, transport and MIDI out.
 */
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "phos/FieldLibrary.h"
#include "phos/FieldPresets.h"
#include "phos/Probe.h"
#include <thread>
#include "phos/Clock.h"
#include "phos/Model.h"
#include "phos/SetFile.h"
#include "phos/Rating.h"
#include "phos/WaveTableFile.h"
#include <algorithm>
#include <chrono>
#include <cmath>

using namespace phos;

namespace {

/** @brief How far ahead of the play position the rings are kept filled (PLAN 3: never under 8 bars). */
constexpr double kHorizonBeats = 8.0 * kBeatsPerBar;

/** @brief The shipped wavetable pack, installed beside the artefacts by Plugin/CMakeLists.txt. */
constexpr const char* kWaveTablePack = "library.phoswt";

/**
 * @brief The directory a shipped resource lies in, looked for the way a plugin has to look.
 *
 * A plugin has no working directory worth the name -- it is whatever the host was started in -- and
 * no source tree, so `PHOS_SOURCE_DATA_DIR` (Core/CMakeLists.txt) only ever helps a development
 * build. What a plugin does have is its own module: JUCE hands the loaded binary back through
 * `currentExecutableFile`, and the bundle around it through `currentApplicationFile` (on macOS
 * these differ, on Windows they are the same file). From there the candidates are the three places
 * a resource can sit in the formats this plugin is built in:
 *
 * | Format | Binary | The resources |
 * |---|---|---|
 * | Standalone (Windows, Linux) | `Phosphene.exe` | beside it |
 * | Standalone (macOS) | `Phosphene.app/Contents/MacOS/Phosphene` | `Contents/Resources` |
 * | VST3 (Windows) | `Phosphene.vst3/Contents/x86_64-win/Phosphene.vst3` | `Contents/Resources` |
 * | VST3 (macOS) | `Phosphene.vst3/Contents/MacOS/Phosphene` | `Contents/Resources` |
 *
 * `Contents/Resources` is where the VST3 specification puts a plugin's data, and it is the one
 * place a host will not strip when it copies a bundle. The directory beside the binary is kept as
 * the first candidate because it is what the standalone and a plain copy of the build tree have.
 *
 * Nothing is loaded here and nothing fails here: an absent file is not an error -- it is the
 * built-in tables (WaveTableFile.h) or the Markov composer (Model.h) -- and the editor says so.
 *
 * @param file the bare name to look for, e.g. `library.phoswt` or `melody.phosmdl`
 * @return the directory holding it, or an empty string
 */
juce::String resolveResourceDirectory(const char* file)
{
    juce::Array<juce::File> dirs;
    const juce::File binaries[] = { juce::File::getSpecialLocation(juce::File::currentExecutableFile),
                                    juce::File::getSpecialLocation(juce::File::currentApplicationFile) };
    for (const juce::File& f : binaries) {
        if (f == juce::File()) continue;
        const juce::File dir = f.isDirectory() ? f : f.getParentDirectory();
        dirs.addIfNotAlreadyThere(dir);                                        // beside the binary
        dirs.addIfNotAlreadyThere(dir.getParentDirectory().getChildFile("Resources"));   // .../Contents/<arch>/..
        dirs.addIfNotAlreadyThere(dir.getChildFile("Contents").getChildFile("Resources"));   // the bundle itself
    }
    for (const juce::File& d : dirs)
        if (d.getChildFile(file).existsAsFile()) return d.getFullPathName();
    return {};
}

juce::String gWaveTablePackDirectory;   ///< where installSearchPaths() found the pack, or empty
PhospheneProcessor::LearnedModels gLearnedModels;   ///< what installSearchPaths() made of Phase 8

/**
 * @brief Points the core at the shipped resources and settles Phase 8, once per process.
 *
 * Called from the processor's constructor, before its composer thread starts: that thread prepares
 * probe engines of its own, and the first Engine::prepare() anywhere in the process is the one that
 * loads the wavetable library, while the first compose pass is the one that loads the two models.
 * Setting either path afterwards would be both a race and too late.
 *
 * **Why the models are opened here and not left to the composer.** They would load by themselves on
 * the composer's thread the first time the knob asks for them, and the Set tab could ask afterwards
 * -- but then the Set tab's answer would be "not yet" for as long as nobody had switched the models
 * on, which is precisely the state in which a user needs to be told that the files are missing. So
 * the question is asked once, eagerly, on the thread that constructs the processor. It costs about
 * 13 MB of packed weights and a tenth of a second per process, once, against Phase 8 disappearing
 * without a word. The load is idempotent (phos::sharedMelodyModel), so the composer later finds the
 * work already done rather than doing it twice.
 */
void installSearchPaths()
{
    static std::once_flag once;
    std::call_once(once, [] {
        gWaveTablePackDirectory = resolveResourceDirectory(kWaveTablePack);
        if (gWaveTablePackDirectory.isNotEmpty()) setWaveTableSearchPath(gWaveTablePackDirectory.toStdString());

        // The models are looked for under the melody file: the installer and Plugin/CMakeLists.txt
        // put both into the same directory, and when only one of them is there the core's own
        // message below names the one that is missing, which is more use than a second directory.
        gLearnedModels.directory = resolveResourceDirectory(kMelodyModelFile);
        if (gLearnedModels.directory.isNotEmpty()) setModelSearchPath(gLearnedModels.directory.toStdString());

        std::string note;
        if (const NeuralModel* m = sharedMelodyModel(&note)) {
            gLearnedModels.melody = true;
            gLearnedModels.melodyNll = m->info().nll;
        } else {
            gLearnedModels.melodyNote = juce::String(note);
        }
        if (const NeuralModel* m = sharedBassModel(&note)) {
            gLearnedModels.bass = true;
            gLearnedModels.bassNll = m->info().nll;
        } else {
            gLearnedModels.bassNote = juce::String(note);
        }
        // Loaded, but from nowhere this plugin pointed at: a development build's PHOS_SOURCE_DATA_DIR
        // (Core/CMakeLists.txt) or the working directory it happened to be started in. Worth saying,
        // because it is the one case that works on this machine and on no other.
        gLearnedModels.fromSourceTree = gLearnedModels.directory.isEmpty()
                                     && (gLearnedModels.melody || gLearnedModels.bass);
    });
}

/**
 * @brief "kick" -> "Kick", "perc3" -> "Perc 3", "compose" -> "Set".
 *
 * The host's parameter list is flat, so every name has to say which module it belongs to. The
 * prefix of the text key is the only source for that, which keeps the naming generated.
 */
juce::String prettyPrefix(const std::string& key)
{
    const size_t dot = key.find('.');
    juce::String p = juce::String(key.substr(0, dot == std::string::npos ? key.size() : dot));
    if (p == "compose") return "Set";
    if (p == "sfx") return "SFX";
    if (p == "fx") return "FX";
    juce::String head, digits;
    for (int i = 0; i < p.length(); ++i) {
        const juce::juce_wchar c = p[i];
        if (c >= '0' && c <= '9') digits += juce::String::charToString(c);
        else head += juce::String::charToString(c);
    }
    head = head.substring(0, 1).toUpperCase() + head.substring(1);
    return digits.isEmpty() ? head : head + " " + digits;
}


} // namespace

const char* const kMacroNames[kNumMacros] = { "Filter Sweep", "Gate Depth", "Drop-out", "Stutter" };
const char* const kMacroHelp[kNumMacros] = {
    "Opens or closes the filters of acid, lead, counter-lead, arp and stab together, by up to a third of their range in "
    "each direction. Centre is neutral.",
    "Switches the trance gate on for lead, counter-lead, arp, stab and pad and sets its depth; at 1.0 the gate closes "
    "completely between its steps.",
    "Takes kick and bass out until the next bar line and lets them back in on the downbeat. One "
    "press, one bar.",
    "Held: lead, counter-lead, arp, stab and pad run through the gate on sixteenths at full depth and the shortest "
    "duty. Phosphene has no buffer repeat, so this is the gate's stutter, not a tape one.",
};

bool macroIsMomentary(Macro m) { return m == Macro::DropOut || m == Macro::Stutter; }

// ==================================================================== PhospheneProcessor

PhospheneProcessor::PhospheneProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    for (auto& a : shownIndex_) a.store(-1, std::memory_order_relaxed);   // no composer preset shown yet (26.09.2026)
    // Before anything prepares an engine -- this instance's, or one of the composer thread's probe
    // renders -- the core has to know where the shipped tables and the two learned models are. The
    // table load itself happens in the first Engine::prepare() and only once for the process.
    installSearchPaths();
    // 22.09.2026, the user: "der Start des Programms dauert relativ lange". Measured, from Play to
    // the first sample: 12.8 s, and almost all of it is one thing -- the first track's level match,
    // which renders the foundation, each melodic part, the two presence probes and the mix twice.
    // The core's own default is a single probe thread (Probe.cpp), and no host ever raised it, so the
    // plugin planned its first track serially on a machine with 24 of them.
    //
    // Parallel probes are not an approximation: testProbeSchedule renders the same plans with one,
    // three and eight threads and compares them bit for bit, cold library included.
    //
    // Probe.h left this off for the plugin on purpose, and the reason it gives is the right one:
    // "in the plugin the plan is made next to a running audio thread, where eight busy worker
    // threads for several seconds were a risk nobody had shown to be harmless". The host
    // test can show it -- it renders live, at a device's rate, while the composer thread plans ahead,
    // and it measures both the audio thread's own cost and the longest gap in the output. With this
    // on: 14.0 % of a core against 13.5 % before, no gap over 150 ms. But that is one machine with
    // twenty-four cores, and the concern is a small one, so half of them is what is taken and the
    // other half stays for the host. Probe.cpp clamps to kMaxThreads.
    //
    // It happens here rather than in prepareToPlay because the composer thread starts with the
    // processor, and a test that wants another number sets it after construction, as they all do.
    const int cores = static_cast<int>(std::thread::hardware_concurrency());
    phos::probe::setThreads(juce::jmax(1, cores / 2));
    engine_ = std::make_unique<Engine>();
    engine_->setLive(true);   // the keyboard's Replace and the composer switch act here, never in an export (01.10.2026)
    composer_ = std::make_unique<Composer>(seed_.load());
    // The plugin plans without Auto Gain and measures it once the music is running (serviceComposer).
    composer_->setDeferMasterGain(true);
    conductor_ = std::make_unique<PlugConductor>(*engine_, *composer_);
    conductor_->setCueMarks(&cueMarks_);
    buildParameters();
    // The live ring's values start at the knobs, so nothing shows a ring before the first block has played.
    playedCount_ = params().count();
    played_ = std::make_unique<std::atomic<float>[]>(static_cast<size_t>(playedCount_));
    playedScratch_.assign(static_cast<size_t>(playedCount_), 0.0f);
    for (int id = 0; id < playedCount_; ++id) played_[static_cast<size_t>(id)].store(params().get(id), std::memory_order_relaxed);
    planKnobIds_ = Composer::planKnobIds(params());
    composeParams_.copyValuesFrom(params());
    // The listener's learned preferences (23.09.2026, phos/Preferences.h): process-wide, so every instance plans
    // with the same taste; loaded before the composer thread plans its first track.
    if (preferencesFile().existsAsFile()) {
        auto prefs = std::make_shared<Preferences>();
        if (prefs->parse(preferencesFile().loadFileAsString().toStdString())) setPreferences(prefs);
    }

    // The cue bridge (PLAN 8.3) is off until `cue.send` is switched on. `PHOS_CUE_HOST` and
    // `PHOS_CUE_PORT` are for an automated run, which has no state file to carry a destination.
    const juce::String envHost = juce::SystemStats::getEnvironmentVariable("PHOS_CUE_HOST", "");
    if (envHost.isNotEmpty()) cueHost_ = envHost;
    const juce::String envPort = juce::SystemStats::getEnvironmentVariable("PHOS_CUE_PORT", "");
    if (envPort.isNotEmpty()) params().set(params().base(Module::Cue) + cue::Port, static_cast<float>(envPort.getIntValue()));

    // PHOS_MUTE=1: silent from the first sample and never unmuted from inside. The house rule for
    // every automated run -- tests, the screenshot export -- is that nothing makes a sound.
    forceMute_ = juce::SystemStats::getEnvironmentVariable("PHOS_MUTE", "").isNotEmpty()
              || juce::SystemStats::getEnvironmentVariable("PHOS_SHOT", "").isNotEmpty()
              || juce::SystemStats::getEnvironmentVariable("PHOS_SHOT_ALL", "").isNotEmpty()
              || juce::SystemStats::getEnvironmentVariable("PHOS_MANUAL", "").isNotEmpty();
    mute_.store(forceMute_);
    trace_ = juce::SystemStats::getEnvironmentVariable("PHOS_TRACE", "").isNotEmpty();
    gPlugTrace = trace_;

    followHost_ = wrapperType != wrapperType_Standalone;
    followHostAtomic_.store(followHost_);

    bindDefaultControllers();

    composerRun_.store(true);
    composerThread_ = std::thread([this] { composerLoop(); });
    startTimer(33);
}

PhospheneProcessor::~PhospheneProcessor()
{
    stopTimer();
    stopRecording();
    // Before the composer thread ends: the sender's thread is the only other one that outlives the
    // audio thread, and it must not be running while the queue it reads is destroyed.
    cues_.stop();
    composerRun_.store(false, std::memory_order_release);
    if (composerThread_.joinable()) composerThread_.join();
    stopMeasureJob();
}

void PhospheneProcessor::buildParameters()
{
    ParamStore& p = params();
    byId_.assign(static_cast<size_t>(p.count()), nullptr);
    for (int i = 0; i < p.count(); ++i) {
        const juce::String name = prettyPrefix(p.key(i)) + " " + p.desc(i).name;
        auto* param = new StoreParameter(p, i, name);
        param->addListener(this);   // undo: the editor's gestures (parameterGestureChanged)
        byId_[static_cast<size_t>(i)] = param;
        addParameter(param);
    }
    // The four macros as host parameters (23.09.2026), after every store parameter so that no id before them
    // moves. A DAW automates them like any knob and a controller can be learned onto them; serviceMacros()
    // carries a change of the host value into the macro and a macro that lets go by itself (a drop-out, one
    // bar) back into the host value.
    static const char* const kMacroIds[kNumMacros] = { "macro_filter_sweep", "macro_gate_depth", "macro_drop_out", "macro_stutter" };
    for (int i = 0; i < kNumMacros; ++i) {
        const Macro m = static_cast<Macro>(i);
        const juce::ParameterID pid(kMacroIds[i], 1);
        const juce::String name = juce::String("Macro ") + kMacroNames[i];
        juce::RangedAudioParameter* hp = nullptr;
        if (macroIsMomentary(m)) hp = new juce::AudioParameterBool(pid, name, false);
        else hp = new juce::AudioParameterFloat(pid, name, juce::NormalisableRange<float>(m == Macro::FilterSweep ? -1.0f : 0.0f, 1.0f), 0.0f);
        macroParam_[i] = hp;
        addParameter(hp);
    }
}

juce::RangedAudioParameter* PhospheneProcessor::hostParameter(int target) const
{
    const int n = static_cast<int>(byId_.size());
    if (target >= 0 && target < n) return byId_[static_cast<size_t>(target)];
    if (target >= n && target < n + kNumMacros) return macroParam_[target - n];
    return nullptr;
}

juce::String PhospheneProcessor::midiTargetName(int target) const
{
    if (auto* hp = hostParameter(target)) return hp->getName(64);
    return {};
}

std::string PhospheneProcessor::midiTargetKey(int target) const
{
    static const char* const kMacroKeys[kNumMacros] = { "macro.filter_sweep", "macro.gate_depth", "macro.drop_out", "macro.stutter" };
    const int n = params().count();
    if (target >= 0 && target < n) return params().key(target);
    if (target >= n && target < n + kNumMacros) return kMacroKeys[target - n];
    return {};
}

int PhospheneProcessor::midiTargetFind(const std::string& key) const
{
    for (int i = 0; i < kNumMacros; ++i) if (midiTargetKey(params().count() + i) == key) return params().count() + i;
    return params().find(key.c_str());
}

PhospheneProcessor::WaveTableLibrary PhospheneProcessor::waveTableLibrary() const
{
    WaveTableLibrary s;
    s.directory = gWaveTablePackDirectory;
    s.shipped = kNumLibraryWaveTables;
    s.loaded = waveTablesIndexed();
    s.built = waveTablesBuilt();
    return s;
}

PhospheneProcessor::LearnedModels PhospheneProcessor::learnedModels() const
{
    // Settled by installSearchPaths() in the constructor, so this is a copy and never a load.
    return gLearnedModels;
}

bool PhospheneProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainInputChannels() != 0) return false;
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void PhospheneProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    {
        // The engine allocates here and never again. Only the composer thread's short pushing
        // section is locked out; its planning, which can take seconds, is not -- a host must not
        // wait for a level-match probe to finish before it can open a plugin.
        const std::lock_guard<std::mutex> lock(engineLock_);
        sampleRate_ = sampleRate;
        scratch_.setSize(2, juce::jmax(64, samplesPerBlock), false, true, true);
        engine_->prepare(sampleRate, samplesPerBlock);
        engine_->setMetering(true);   // reading only: the mix is the same to the bit (Engine.h)
        for (HeldNote& h : held_) h = HeldNote{};
        NoteEvent junk;
        while (midiRing_.pop(junk)) {}
    }
    // A fresh generation: the conductor repositions and the rings are filled again before the next
    // sample is rendered. Nothing here writes the composer thread's own counter.
    wasPlaying_ = false;
    genWanted_.fetch_add(1, std::memory_order_release);
    setLatencySamples(engine_->latencySamples());
}

void PhospheneProcessor::timerCallback()
{
    // The fast job is the macros: a drop-out has to let go on the bar line, and 33 ms is a
    // sixty-fourth note at 145 BPM. Everything else here is housekeeping and runs every eighth tick.
    pollHeadset();   // the headset's hands move macros too (01.10.2026)
    serviceMacros();
    if (++macroTicks_ % 8 != 0) return;
    serviceCueBridge();
    // A table the *user* picked (the Table box, a loaded set) has to be expanded by somebody, and
    // the audio thread must not do it (22.09.2026, the on-demand library). Six atomic loads when
    // nothing changed, on the message thread, a quarter of a second after the click at worst --
    // until then the voice sounds the built-in fallback, which is what a machine without the pack
    // hears anyway.
    engine_->ensureVoiceTables();
    const int latency = engine_->latencySamples();
    if (latency != getLatencySamples()) setLatencySamples(latency);
    // The composer's own knobs decide how the set is planned, and the composer throws its cached
    // plans away when they move. The published copy the editor draws has to follow, so a cheap
    // fingerprint of those parameters is watched here rather than a listener on six hundred of them.
    const ParamStore& p = params();
    const int cb = p.base(Module::Compose);
    float sum = 0.0f;
    for (int i = 0; i < ParamStore::moduleCount(Module::Compose); ++i) sum += (i + 1) * p.get(cb + i);
    if (sum != composeFingerprint_) {
        composeFingerprint_ = sum;
        plansStale_.store(true, std::memory_order_release);
    }
    refreshPresetDisplay();
}

int PhospheneProcessor::soundSynthOfId(int id, int& local) const
{
    const ParamStore& p = params();
    for (int k = 0; k < kSoundSynths; ++k) {
        const Module m = k == 0 ? Module::Kick : k == 1 ? Module::Bass : k == 2 ? Module::Acid : Module::Poly;
        const int b = p.base(m, k >= 3 ? k - 3 : 0);
        if (id >= b && id < b + ParamStore::moduleCount(m)) { local = id - b; return k; }
    }
    local = -1;
    return -1;
}

void PhospheneProcessor::refreshPresetDisplay()
{
    const TransportView t = transport();
    TrackPlan plan;
    if (!tryReadTrack(t.track, plan)) return;
    ParamStore& p = params();
    const int ownBase = p.base(Module::Mix) + mix::KickOwn;
    for (int k = 0; k < kSoundSynths; ++k) {
        if (p.getBool(ownBase + k)) { shownPreset_[k] = -2; shownIndex_[k].store(-1, std::memory_order_relaxed); continue; }
        const SoundPreset* sp = Composer::presetOf(p, plan, k);
        const int idx = sp != nullptr ? plan.soundPreset[k] : -1;
        shownPreset_[k] = idx;
        shownIndex_[k].store(idx, std::memory_order_relaxed);
    }
}

void PhospheneProcessor::takeOverPreset(int k)
{
    // The knobs of the synth take the preset's values (26.09.2026): what the user starts turning from is what sounded.
    // Inside the gesture, so the undo step that records the turn takes this back with it. Not the plan knobs.
    const int idx = k >= 0 && k < kSoundSynths ? shownPreset_[k] : -1;
    if (idx < 0) return;
    const Module m = k == 0 ? Module::Kick : k == 1 ? Module::Bass : k == 2 ? Module::Acid : Module::Poly;
    const std::vector<SoundPreset>& bank = factoryPresets(m, k >= 3 ? k - 3 : 0);
    if (idx >= static_cast<int>(bank.size())) return;
    ParamStore& p = params();
    const int b = p.base(m, k >= 3 ? k - 3 : 0);
    for (const auto& kv : bank[static_cast<size_t>(idx)].values) {
        const int id = b + kv.first;
        if (std::find(planKnobIds_.begin(), planKnobIds_.end(), id) != planKnobIds_.end() || p.get(id) == kv.second) continue;
        if (StoreParameter* sp = parameterFor(id)) sp->setValueNotifyingHost(p.toNormalised(id, kv.second));
    }
}

// ------------------------------------------------------------------ the cue bridge (PLAN 8.3)

juce::String PhospheneProcessor::cueHost() const
{
    const std::lock_guard<std::mutex> lock(cueHostLock_);
    return cueHost_;
}

void PhospheneProcessor::setCueHost(const juce::String& host)
{
    const std::lock_guard<std::mutex> lock(cueHostLock_);
    cueHost_ = host.trim().isEmpty() ? juce::String(kCueDefaultHost) : host.trim();
}

void PhospheneProcessor::serviceCueBridge()
{
    const ParamStore& p = params();
    const int base = p.base(Module::Cue);
    cueBeats_.store(p.getBool(base + cue::Beats), std::memory_order_relaxed);
    cueLeadMs_.store(p.get(base + cue::LeadMs), std::memory_order_relaxed);

    const bool want = p.getBool(base + cue::Send);
    const int port = p.getInt(base + cue::Port);
    const juce::String host = cueHost();
    if (want == cueWanted_ && port == cuePort_ && host == cuePortHost_) return;
    cueWanted_ = want;
    cuePort_ = port;
    cuePortHost_ = host;

    cues_.stop();
    if (!want) return;
    // A destination that cannot be resolved is not an error and is not reported to the user: the
    // whole contract of this bridge is that a visualiser which is not there changes nothing. It is
    // traced, because a silent failure that nobody can see is the other way to get this wrong.
    // Offline (a bounce, the demo) the due times describe a timeline nobody is living through, so
    // the sender does not wait for them.
    const bool ok = cues_.start(host.toStdString(), port, !isNonRealtime());
    if (ok) cueAnnounce_.store(true, std::memory_order_release);
    if (trace_)
        std::fprintf(stderr, "[phos cues] %s %s:%d\n", ok ? "sending to" : "cannot reach",
                     host.toRawUTF8(), port);
}

// ------------------------------------------------------------------ the composer thread

void PhospheneProcessor::setNonRealtime(bool offline) noexcept
{
    juce::AudioProcessor::setNonRealtime(offline);
    offline_.store(offline, std::memory_order_release);
    // Offline is a bounce, and a bounce has to be the same samples phos_render writes -- the whole
    // determinism contract of this project rests on it (`the plugin equals phos_render bit for bit`,
    // Tests/hosttest.cpp). Deferring Auto Gain is a *live* convenience: it lets the music start
    // before the offset is measured and rides it in over a ramp, which is a different signal. So the
    // deferral is switched off here, and the plans made under it are thrown away so the bounce plans
    // whole. (22.09.2026.)
    const std::lock_guard<std::mutex> lock(composeLock_);
    if (offline) stopMeasureJob();   // a bounce measures whole, on its own; a live measurement would only compete
    if (composer_->defersMasterGain() == offline) {
        composer_->setDeferMasterGain(!offline);
        composer_->setSeed(seed_.load(std::memory_order_relaxed));   // clears the cached plans
        plansStale_.store(true, std::memory_order_release);
    }
}

void PhospheneProcessor::composerLoop()
{
    unsigned turns = 0;
    while (composerRun_.load(std::memory_order_acquire)) {
        if (trace_ && ++turns % 500 == 0)
            std::fprintf(stderr, "[phos composer] turn %u, generation %d, next bar %d, %d tracks planned\n",
                         turns, genLocal_, conductor_->nextBar(), publishedTracks_);
        if (!offline_.load(std::memory_order_acquire)) serviceComposer(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

bool PhospheneProcessor::drainCuration()
{
    CurationCommand c;
    bool any = false;
    while (curation_.pop(c)) {
        const LockUnit unit = static_cast<LockUnit>(juce::jlimit(0, kNumLockUnits - 1, static_cast<int>(c.unit)));
        switch (c.op) {
        case 0: composer_->setLock(unit, c.index, false); break;
        case 1: composer_->setLock(unit, c.index, true); break;
        case 2: composer_->reroll(unit, c.index); break;
        case 4: composer_->setVariation(unit, c.index, c.value); break;
        default: composer_->clearLocks(); break;
        }
        any = true;
    }
    if (!any) return false;
    // Every cached plan is gone (Composer::setLock clears them), so the editor's published copy has
    // to be built again, and the bars already in the rings were composed under the old decisions:
    // the transport restarts at the bar the editor marked when the button was pressed.
    plansStale_.store(true, std::memory_order_release);
    restartRequest_.store(true, std::memory_order_release);
    restarts_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void PhospheneProcessor::refreshComposeParams(bool restart)
{
    // What the plans were made with, for the plan knobs whose change has to wait.
    const size_t n = planKnobIds_.size();
    std::vector<float> held(n), now(n);
    for (size_t k = 0; k < n; ++k) held[k] = composeParams_.get(planKnobIds_[k]);
    composeParams_.copyValuesFrom(params());
    for (size_t k = 0; k < n; ++k) now[k] = composeParams_.get(planKnobIds_[k]);
    // The clock of the rule is the music's, so that a bounce takes a change over on the same bar every time.
    const double beat = musicalBeat_.load(std::memory_order_relaxed);
    if (now != planKnobsSeen_ || beat < planKnobsSeenBeat_) {
        planKnobsSeen_ = now;
        planKnobsSeenBeat_ = beat;
    }
    if (now == held) return;
    const bool running = playRequest_.load(std::memory_order_relaxed) || hostSyncNow_.load(std::memory_order_relaxed);
    if (restart || !running || beat - planKnobsSeenBeat_ >= kPlanKnobSettleBeats) {
        plansStale_.store(true, std::memory_order_release);   // taken over: the composer plans again, the editor's list follows
        if (trace_) std::fprintf(stderr, "[phos composer] plan knobs taken over at beat %.1f\n", beat);
        return;
    }
    for (size_t k = 0; k < n; ++k) composeParams_.set(planKnobIds_[k], held[k]);   // still moving: the plans stay
}

void PhospheneProcessor::serviceComposer(bool warmUp)
{
    const std::lock_guard<std::mutex> lock(composeLock_);
    drainCuration();
    const int want = genWanted_.load(std::memory_order_acquire);
    // One set of knobs for the whole turn (composeParams_, PluginProcessor.h); a restart takes every knob over.
    refreshComposeParams(want != genLocal_);
    const ParamStore& knobs = composeParams_;
    if (trace_ && want != genLocal_)
        std::fprintf(stderr, "[phos composer] generation %d wanted, was %d, engine reset for %d\n",
                     want, genLocal_, genReset_.load());
    if (want != genLocal_) {
        // Step 2 of the handshake: position the conductor. The engine's own tempo map is never
        // used -- the tempo arrives as control events (PlugConductor::tempoControls) -- so nothing
        // here allocates inside the engine, and a seek costs one composed bar.
        const int bar = pendingBar_.load(std::memory_order_relaxed);
        const double offset = pendingOffset_.load(std::memory_order_relaxed);
        const bool ownClock = !(followHostAtomic_.load(std::memory_order_relaxed)
                                && hostSyncNow_.load(std::memory_order_relaxed));
        {
            const std::lock_guard<std::mutex> eng(engineLock_);
            engine_->clearTempoMap();
        }
        conductor_->seek(knobs, bar, offset, ownClock, engineLock_);
        genLocal_ = want;
        genAck_.store(want, std::memory_order_release);
        return;
    }
    if (genReset_.load(std::memory_order_acquire) != genLocal_) return;   // waiting for step 3
    conductor_->pump(knobs, kHorizonBeats, &midiRing_, engineLock_);
    genPrimed_.store(genLocal_, std::memory_order_release);

    // ---------------------------------------------------------------- measuring behind the music
    //
    // 22.09.2026, at the user's decision: Auto Gain after the start -- two whole-mix renders that used to put nine
    // seconds of silence between the button and the first sound, measured while the music runs and ridden in over
    // a phrase (`ControlEvent::Kind::Offset` ramps with a raised cosine over `length` beats).
    //
    // 23.09.2026, round "Planung": the first track's level, presence and audibility probes too (the composer plans
    // it without any, Composer::completeMeasurement), and all of it on a thread of its own, on a copy of the
    // composer. Done here, on this thread, the measurement held composeLock_ for 8 to 11 seconds, and a second
    // restart right after Play -- a click into the Arrange tab, a host's locate -- waited for all of them; the host
    // test measured 9.4 s. Now this thread is never busy for longer than a pump, and the job's result is taken over
    // when it arrives (Composer::adoptMeasurement, refused if the plans were thrown away meanwhile).
    //
    // A track opens with sixteen bars without a kick and without the lines -- some twenty-six seconds at 145 BPM --
    // and the job takes about fifteen, so the levels settle long before anyone hears a line or the first drop.
    const bool running = playRequest_.load(std::memory_order_relaxed) || hostSyncNow_.load(std::memory_order_relaxed);
    if (composer_->defersMasterGain()) {
        const int playingTrack = composer_->trackOfBar(knobs, juce::jmax(0, static_cast<int>(
            musicalBeat_.load(std::memory_order_relaxed) / kBeatsPerBar)));
        // The corrections of a plan as the engine should now have them: every melodic part's level, and the
        // master offset, both over a phrase. Sent again when unchanged, which a ramp to the value it holds is.
        auto sendCorrections = [&](int index) {
            const phos::TrackPlan& now = composer_->track(knobs, index);
            std::vector<ControlEvent> events;
            const double at = engine_->beatPosition() + 1.0;   // a beat ahead of the play head, never behind it
            const float phrase = 4.0f * static_cast<float>(kBeatsPerBar);
            composer_->levelControls(knobs, now, at, phrase, events);
            if (!now.masterDeferred) {
                const ParamStore& ps = knobs;
                const ParamDesc& mg = ps.desc(ps.base(Module::Master) + master::Gain);
                ControlEvent c;
                c.beat = at;
                c.param = static_cast<int16_t>(ps.base(Module::Master) + master::Gain);
                c.kind = ControlEvent::Kind::Offset;
                c.value = now.masterGainDb / (mg.maxValue - mg.minValue);
                c.length = phrase;
                events.push_back(c);
            }
            {
                const std::lock_guard<std::mutex> eng(engineLock_);
                for (const ControlEvent& c : events) engine_->pushImmediate(c);   // not behind eight queued bars
            }
            composer_->clearCorrectionsPending(index);
            const std::lock_guard<std::mutex> pl(plansLock_);
            if (index < static_cast<int>(plans_.size())) plans_[static_cast<size_t>(index)] = now;
        };
        // A finished job: take it over and send what it measured.
        if (measureJob_ != nullptr && measureJob_->done.load(std::memory_order_acquire)) {
            if (measureThread_.joinable()) measureThread_.join();
            const bool adopted = composer_->adoptMeasurement(measureJob_->index, measureJob_->result, measureJob_->generation);
            if (trace_) std::fprintf(stderr, "[phos composer] background measurement of track %d %s\n", measureJob_->index + 1,
                                     adopted ? "taken over" : "dropped (the plans changed meanwhile)");
            const int index = measureJob_->index;
            measureJob_.reset();
            if (adopted && index == playingTrack) {
                sendCorrections(index);
            } else if (adopted) {
                // Measured ahead of its turn: its first bars will be composed with it, and the editor's list shows it.
                const phos::TrackPlan& now = composer_->track(knobs, index);
                const std::lock_guard<std::mutex> pl(plansLock_);
                if (index < static_cast<int>(plans_.size())) plans_[static_cast<size_t>(index)] = now;
            }
        }
        // Measured already, by the planning of a later track that needed the first one's numbers.
        if (composer_->correctionsPending(playingTrack)) sendCorrections(playingTrack);
        // Start one for the track that plays, while the set plays and no restart waits; that one measured, for the
        // next track, so that it arrives with its levels (26.09.2026: every live plan is made without its probes,
        // Composer::measureTrack). Stopped, nothing is measured at all: a probe render's CPU is only worth spending
        // on what is about to be heard.
        if (measureJob_ == nullptr && running && genWanted_.load(std::memory_order_acquire) == genLocal_) {
            for (const int index : { playingTrack, playingTrack + 1 }) {
                const phos::TrackPlan& plan = composer_->track(knobs, index);
                if (plan.measureDeferred || plan.masterDeferred) { startMeasureJob(index); break; }
            }
        }
    }

    // With the rings filled, the spare time goes into planning the tracks ahead: the editor's list
    // of tracks comes from here, and a seek into a track that is already planned is instant. One
    // track per turn, because the first plan of a track measures its level by rendering it.
    // Only the composer thread does this. Offline, serviceComposer runs on the audio thread, and
    // planning a track there would stall the render for seconds per track for no gain.
    if (!warmUp) return;
    // Not while a restart is waiting: planning a track takes seconds, and the transport would
    // stand silent for all of them. The warm-up is what there is spare time for, nothing more.
    if (genWanted_.load(std::memory_order_acquire) != genLocal_) return;
    if (plansStale_.exchange(false, std::memory_order_acq_rel)) publishedTracks_ = 0;
    // Live, a plan renders no probes (Composer::measureTrack, 26.09.2026) and takes milliseconds, so the list is
    // planned whether the set plays or not; until then this stopped at the first track while stopped or measuring,
    // because every later plan measured the first one again, here, for seconds.
    if (publishedTracks_ < kPublishedTracks) {
        const TrackPlan plan = composer_->track(knobs, publishedTracks_);
        const std::lock_guard<std::mutex> pl(plansLock_);
        if (publishedTracks_ == 0) plans_.clear();
        plans_.push_back(plan);
        ++publishedTracks_;
    }
}

void PhospheneProcessor::startMeasureJob(int index)
{
    auto job = std::make_unique<MeasureJob>();
    job->composer = std::make_unique<phos::Composer>(*composer_);
    job->composer->setAbortFlag(&job->abort);
    job->params.copyValuesFrom(composeParams_);   // the knobs the plan was made with
    job->index = index;
    job->generation = composer_->planGeneration();
    MeasureJob* j = job.get();
    measureJob_ = std::move(job);
    measureThread_ = std::thread([j] {
        j->composer->completeMeasurement(j->params, j->index);   // level, presence, audibility (the first track)
        if (!j->abort.load(std::memory_order_relaxed)) j->composer->completeMasterGain(j->params, j->index);   // Auto Gain
        j->result = j->composer->track(j->params, j->index);
        j->done.store(true, std::memory_order_release);
    });
    if (trace_) std::fprintf(stderr, "[phos composer] measuring track %d behind the music\n", index + 1);
}

void PhospheneProcessor::stopMeasureJob()
{
    if (measureJob_ != nullptr) measureJob_->abort.store(true, std::memory_order_relaxed);
    if (measureThread_.joinable()) measureThread_.join();
    measureJob_.reset();
}

void PhospheneProcessor::requestSeek(int bar, double beatOffset)
{
    pendingBar_.store(bar, std::memory_order_relaxed);
    pendingOffset_.store(beatOffset, std::memory_order_relaxed);
    genWanted_.fetch_add(1, std::memory_order_release);
}

// ------------------------------------------------------------------ transport

void PhospheneProcessor::play() { playRequest_.store(true, std::memory_order_release); }

void PhospheneProcessor::bindDefaultControllers()
{
    // The controllers every generator of the family has by default (01.10.2026): controller 74 (brightness) the filter
    // sweep, the expression pedal (11) the gate depth, the sustain pedal (64) the stutter while it is held. The mod
    // wheel (1) is left free.
    midiMap_.bind(0, 74, params().count() + static_cast<int>(Macro::FilterSweep));
    midiMap_.bind(0, 11, params().count() + static_cast<int>(Macro::GateDepth));
    midiMap_.bind(0, 64, params().count() + static_cast<int>(Macro::Stutter));
}

void PhospheneProcessor::composeSet()
{
    // Every knob taken over at once (refreshComposeParams: a restart takes them all), the plans made again, the set
    // from bar 1.
    plansStale_.store(true, std::memory_order_release);
    seekToBar(0);
}

void PhospheneProcessor::resetToDefault(int id)
{
    if (id < 0 || id >= params().count()) return;
    StoreParameter* p = parameterFor(id);
    if (p == nullptr) return;
    undoable(juce::String(params().desc(id).name) + " to its default", [p] {
        p->beginChangeGesture();
        p->setValueNotifyingHost(p->getDefaultValue());
        p->endChangeGesture();
    });
}

void PhospheneProcessor::pollHeadset()
{
    frame::Settings& st = frame::Settings::of("Phosphene");
    headset_.listen(st.headset() == frame::Settings::HeadsetMode::Off ? 0 : st.headsetPort());
    const frame::HeadsetEvents e = headset_.poll();
    if (e.playStop) { if (isPlaying()) stop(); else play(); }
    if (e.next) {
        // The next track, as the Quest app has it: the conductor goes to its first bar.
        phos::TrackPlan plan;
        if (tryReadTrack(transport().track + 1, plan)) seekToBar(plan.firstBar);
    }
    if (e.action) setMacro(Macro::DropOut, 1.0f);
    if (e.hold) setMacro(Macro::Stutter, 1.0f);
    if (e.holdEnded) setMacro(Macro::Stutter, 0.0f);
    if (e.filterMoved) setMacro(Macro::FilterSweep, e.filter);
    if (e.throwMoved) setMacro(Macro::GateDepth, e.throwAmount);
}

void PhospheneProcessor::stop()
{
    playRequest_.store(false, std::memory_order_release);
    seekToBar(0);
}

void PhospheneProcessor::seekToBar(int bar)
{
    pendingBar_.store(juce::jmax(0, bar), std::memory_order_relaxed);
    pendingOffset_.store(static_cast<double>(juce::jmax(0, bar)) * kBeatsPerBar, std::memory_order_relaxed);
    restartRequest_.store(true, std::memory_order_release);
    restarts_.fetch_add(1, std::memory_order_relaxed);
}

void PhospheneProcessor::setFollowHost(bool on)
{
    followHost_ = on;
    followHostAtomic_.store(on, std::memory_order_release);
    requestRestart();
}

// ------------------------------------------------------------------ undo (23.09.2026)

namespace {

/** @brief One undo step: the state before and after an edit. perform() is a no-op the first time -- the edit has
 *         already happened when the step is recorded. */
class UndoStep final : public juce::UndoableAction {
public:
    /** @brief A step of @p p from @p before to @p after. */
    UndoStep(PhospheneProcessor& p, PhospheneProcessor::UndoState before, PhospheneProcessor::UndoState after)
        : p_(p), before_(std::move(before)), after_(std::move(after)) {}
    /** @brief Applies after (the first time: it is applied already, so nothing happens). */
    bool perform() override
    {
        if (first_) { first_ = false; return true; }
        p_.applyUndoState(after_);
        return true;
    }
    /** @brief Puts before back. */
    bool undo() override { p_.applyUndoState(before_); return true; }
    /** @brief The memory the step takes, roughly (the history's limit). */
    int getSizeInUnits() override { return static_cast<int>(before_.knobs.size() + after_.knobs.size()) + 64; }

private:
    PhospheneProcessor& p_;   ///< the processor
    PhospheneProcessor::UndoState before_;   ///< the state before the step
    PhospheneProcessor::UndoState after_;   ///< the state after it
    bool first_ = true;   ///< perform() has not run yet (the change is made already)
};

} // namespace

bool PhospheneProcessor::UndoState::operator==(const UndoState& o) const
{
    if (knobs != o.knobs || seed != o.seed) return false;
    for (int u = 0; u < kNumLockUnits; ++u) if (locks[u] != o.locks[u] || variations[u] != o.variations[u]) return false;
    return true;
}

PhospheneProcessor::UndoState PhospheneProcessor::captureUndoState() const
{
    UndoState s;
    s.knobs = params().toText(false);
    s.seed = seed_.load(std::memory_order_relaxed);
    const std::lock_guard<std::mutex> lock(curationLock_);
    for (int u = 0; u < kNumLockUnits; ++u) { s.locks[u] = lockMirror_[u]; s.variations[u] = variationMirror_[u]; }
    return s;
}

void PhospheneProcessor::applyUndoState(const UndoState& s)
{
    const bool was = restoringUndo_;
    restoringUndo_ = true;
    params().parseText(s.knobs);
    if (s.seed != seed_.load(std::memory_order_relaxed)) setSeed(s.seed);
    // The curation as a difference against the mirror, sent to the composer like any lock or reroll; a reroll
    // counter that moves moves notes, so the transport restarts at the bar that is playing, as a reroll does.
    std::vector<CurationCommand> cmds;
    bool moved = false;
    {
        const std::lock_guard<std::mutex> lock(curationLock_);
        for (int u = 0; u < kNumLockUnits; ++u) {
            for (const auto& kv : lockMirror_[u]) if (s.locks[u].count(kv.first) == 0) cmds.push_back({ static_cast<uint8_t>(u), 0, kv.first, 0 });
            for (const auto& kv : s.locks[u]) if (lockMirror_[u].count(kv.first) == 0) cmds.push_back({ static_cast<uint8_t>(u), 1, kv.first, 0 });
            std::map<int, uint32_t> keys = variationMirror_[u];
            for (const auto& kv : s.variations[u]) keys[kv.first] = 0;
            for (const auto& kv : keys) {
                const auto a = variationMirror_[u].find(kv.first);
                const auto b = s.variations[u].find(kv.first);
                const uint32_t now = a == variationMirror_[u].end() ? 0u : a->second, want = b == s.variations[u].end() ? 0u : b->second;
                if (now != want) { cmds.push_back({ static_cast<uint8_t>(u), 4, kv.first, want }); moved = true; }
            }
            lockMirror_[u] = s.locks[u];
            variationMirror_[u] = s.variations[u];
        }
    }
    if (moved) markCurrentPosition();
    for (const CurationCommand& c : cmds) curation_.push(c);
    restoringUndo_ = was;
}

void PhospheneProcessor::recordUndo(const juce::String& name, const UndoState& before)
{
    if (restoringUndo_) return;
    UndoState after = captureUndoState();
    if (after == before) return;
    undo_.beginNewTransaction(name);
    undo_.perform(new UndoStep(*this, before, std::move(after)), name);
}

void PhospheneProcessor::undoable(const juce::String& name, const std::function<void()>& action)
{
    const UndoState before = captureUndoState();
    action();
    recordUndo(name, before);
}

bool PhospheneProcessor::undo() { return undo_.undo(); }

bool PhospheneProcessor::redo() { return undo_.redo(); }

void PhospheneProcessor::parameterGestureChanged(int parameterIndex, bool gestureIsStarting)
{
    // Only the editor's gestures, which come on the message thread; a host touching an automation lane may call
    // this from its own threads, and automation is the host's history, not this one.
    if (restoringUndo_ || !juce::MessageManager::existsAndIsCurrentThread()) return;
    if (gestureIsStarting) {
        if (gestureDepth_++ == 0) {
            gestureBefore_ = captureUndoState();
            const auto& all = getParameters();
            gestureName_ = parameterIndex >= 0 && parameterIndex < all.size() ? all[parameterIndex]->getName(64) : juce::String("knob");
            // A synth playing the composer's preset (26.09.2026): the knob the user takes becomes the sound's owner --
            // Own Sound on, the knobs standing at the preset's values -- in the same undo step as the turn.
            if (parameterIndex >= 0 && parameterIndex < all.size())
                if (auto* spar = dynamic_cast<StoreParameter*>(all[parameterIndex])) {
                    int local = -1;
                    const int k = soundSynthOfId(spar->storeId(), local);
                    const Module m = k == 0 ? Module::Kick : k == 1 ? Module::Bass : k == 2 ? Module::Acid : Module::Poly;
                    const int own = params().base(Module::Mix) + mix::KickOwn + k;
                    if (k >= 0 && !presetLeaves(m, local) && !params().getBool(own) && shownPreset_[k] >= 0) {
                        takeOverPreset(k);
                        if (StoreParameter* o = parameterFor(own)) o->setValueNotifyingHost(1.0f);
                    }
                }
        }
    } else if (gestureDepth_ > 0 && --gestureDepth_ == 0) {
        recordUndo(gestureName_, gestureBefore_);
    }
}

void PhospheneProcessor::setSeed(uint64_t s)
{
    {
        const std::lock_guard<std::mutex> lock(composeLock_);
        seed_.store(s, std::memory_order_relaxed);
        composer_->setSeed(s);
    }
    plansStale_.store(true, std::memory_order_release);
    seekToBar(0);
}

void PhospheneProcessor::randomiseSeed()
{
    // The clock and the address of this object: two things that differ between runs, so "another
    // set" never gives the same one twice in a session.
    const uint64_t t = static_cast<uint64_t>(juce::Time::getHighResolutionTicks());
    uint64_t x = t ^ (reinterpret_cast<uintptr_t>(this) * 0x9E3779B97F4A7C15ull);
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
    setSeed(1 + (x % 1000000000ull));
}

bool PhospheneProcessor::tryReadTrack(int index, TrackPlan& out) const
{
    // The published copy, not the composer itself: the editor must never wait on a probe render.
    const std::lock_guard<std::mutex> lock(plansLock_);
    if (index < 0 || index >= static_cast<int>(plans_.size())) return false;
    out = plans_[static_cast<size_t>(index)];
    return true;
}

// ------------------------------------------------------------------ curation (PLAN 6.8)

void PhospheneProcessor::markCurrentPosition()
{
    // Where the music is now, so that the restart a curation command triggers lands here again
    // rather than at the top of the set. Under a host the drift check corrects it a block later.
    const int bar = juce::jmax(0, static_cast<int>(musicalBeat_.load(std::memory_order_relaxed) / kBeatsPerBar));
    pendingBar_.store(bar, std::memory_order_relaxed);
    pendingOffset_.store(static_cast<double>(bar) * kBeatsPerBar, std::memory_order_relaxed);
}

void PhospheneProcessor::setLock(LockUnit unit, int index, bool locked)
{
    {
        const std::lock_guard<std::mutex> lock(curationLock_);
        auto& m = lockMirror_[static_cast<int>(unit)];
        if (locked) m[index] = 1;
        else m.erase(index);
    }
    // A lock changes no note by itself -- it only decides what a later reroll may move -- so it
    // does not restart the transport; the mirror is enough for the editor, and the composer picks
    // the command up on its next turn.
    curation_.push(CurationCommand{ static_cast<uint8_t>(unit), static_cast<uint8_t>(locked ? 1 : 0), index });
}

bool PhospheneProcessor::isLocked(LockUnit unit, int index) const
{
    const std::lock_guard<std::mutex> lock(curationLock_);
    const auto& m = lockMirror_[static_cast<int>(unit)];
    return m.find(index) != m.end();
}

void PhospheneProcessor::reroll(LockUnit unit, int index)
{
    {
        const std::lock_guard<std::mutex> lock(curationLock_);
        if (lockMirror_[static_cast<int>(unit)].count(index) != 0) return;   // a locked unit does not move
        ++variationMirror_[static_cast<int>(unit)][index];
    }
    markCurrentPosition();
    curation_.push(CurationCommand{ static_cast<uint8_t>(unit), 2, index });
}

uint32_t PhospheneProcessor::variation(LockUnit unit, int index) const
{
    const std::lock_guard<std::mutex> lock(curationLock_);
    const auto& m = variationMirror_[static_cast<int>(unit)];
    const auto it = m.find(index);
    return it == m.end() ? 0u : it->second;
}

void PhospheneProcessor::clearCuration()
{
    {
        const std::lock_guard<std::mutex> lock(curationLock_);
        for (int u = 0; u < kNumLockUnits; ++u) { lockMirror_[u].clear(); variationMirror_[u].clear(); }
    }
    markCurrentPosition();
    curation_.push(CurationCommand{ 0, 3, 0 });
}

void PhospheneProcessor::syncCurationMirror()
{
    // After a .phosset has been read the composer holds locks the editor has never seen.
    const std::lock_guard<std::mutex> lock(curationLock_);
    for (int u = 0; u < kNumLockUnits; ++u) {
        lockMirror_[u] = composer_->locks(static_cast<LockUnit>(u));
        variationMirror_[u] = composer_->variations(static_cast<LockUnit>(u));
    }
}

// ------------------------------------------------------------------ perform macros

int PhospheneProcessor::macroTargets(Macro m, float value, MacroTarget* out) const
{
    const ParamStore& p = params();
    const int acidBase = p.base(Module::Acid);
    const int lead = p.base(PolyInstance::Lead), counter = p.base(PolyInstance::Counter);
    const int arp = p.base(PolyInstance::Arp), stab = p.base(PolyInstance::Stab);
    const int pad = p.base(PolyInstance::Pad);
    const int mixBase = p.base(Module::Mix);
    int n = 0;
    auto add = [&](int id, float v, bool absolute) {
        if (id >= 0 && n < kMaxMacroTargets) out[n++] = MacroTarget{ id, v, absolute };
    };
    if (value == 0.0f) return 0;   // neutral: the macro wants nothing, and its knobs go back
    switch (m) {
    case Macro::FilterSweep: {
        // A third of the normalised range each way. The three melodic filters move together,
        // which is what makes it one gesture instead of three knobs.
        const float d = juce::jlimit(-1.0f, 1.0f, value) * 0.35f;
        add(acidBase + acid::Cutoff, d, false);
        add(lead + poly::Cutoff, d, false);
        add(arp + poly::Cutoff, d, false);
        add(counter + poly::Cutoff, d, false);   // 19.09.2026: the counter-lead and the stab move with them
        add(stab + poly::Cutoff, d, false);
        break;
    }
    case Macro::GateDepth: {
        const float v = juce::jlimit(0.0f, 1.0f, value);
        for (int base : { lead, counter, arp, stab, pad }) {
            add(base + poly::Gate, 1.0f, true);
            add(base + poly::GateDepth, v, true);
        }
        break;
    }
    case Macro::DropOut:
        add(mixBase + mix::KickMute, 1.0f, true);
        add(mixBase + mix::BassMute, 1.0f, true);
        break;
    case Macro::Stutter:
        for (int base : { lead, counter, arp, stab, pad }) {
            add(base + poly::Gate, 1.0f, true);
            add(base + poly::GatePattern, 0.0f, true);   // Sixteenths
            add(base + poly::GateDepth, 1.0f, true);
            add(base + poly::GateDuty, 0.0f, true);      // the shortest opening the knob allows
        }
        break;
    default: break;
    }
    return n;
}

void PhospheneProcessor::setMacro(Macro m, float value)
{
    const int i = static_cast<int>(m);
    if (i < 0 || i >= kNumMacros) return;
    macro_[i].value = juce::jlimit(m == Macro::FilterSweep ? -1.0f : 0.0f, 1.0f, value);
    // A drop-out is one press and one bar: it lets go on the next bar line, wherever the press
    // fell. The release is a message-thread job, so it lands within a tick of the downbeat.
    if (m == Macro::DropOut && macro_[i].value > 0.0f) {
        const double beat = musicalBeat_.load(std::memory_order_relaxed);
        macro_[i].releaseBeat = (std::floor(beat / kBeatsPerBar) + 1.0) * kBeatsPerBar;
    } else if (macro_[i].value == 0.0f) {
        macro_[i].releaseBeat = -1.0;
    }
    // The host parameter follows (MIDI learn round): the editor's knob and the DAW's lane show the same thing.
    if (auto* hp = macroParam_[i]) {
        macroHostSeen_[i] = macro_[i].value;
        const float norm = hp->convertTo0to1(macroIsMomentary(m) ? (macro_[i].value > 0.0f ? 1.0f : 0.0f) : macro_[i].value);
        if (std::fabs(hp->getValue() - norm) > 1.0e-6f) hp->setValueNotifyingHost(norm);
    }
    serviceMacros();
}

void PhospheneProcessor::serviceMacros()
{
    const std::lock_guard<std::mutex> lock(macroBaseLock_);
    // A macro's host parameter moved (DAW automation, a learned controller): the macro follows. Compared with
    // what was seen last, not with the macro's own value, so a drop-out that let go by itself is not pressed
    // again by a host value that still says 1.
    for (int i = 0; i < kNumMacros; ++i) {
        auto* hp = macroParam_[i];
        if (hp == nullptr) continue;
        const Macro m = static_cast<Macro>(i);
        float v = hp->convertFrom0to1(hp->getValue());
        if (macroIsMomentary(m)) v = v > 0.5f ? 1.0f : 0.0f;
        if (std::fabs(v - macroHostSeen_[i]) > 1.0e-6f) {
            macroHostSeen_[i] = v;
            macro_[static_cast<size_t>(i)].value = juce::jlimit(m == Macro::FilterSweep ? -1.0f : 0.0f, 1.0f, v);
            if (m == Macro::DropOut && v > 0.0f) {
                const double b = musicalBeat_.load(std::memory_order_relaxed);
                macro_[static_cast<size_t>(i)].releaseBeat = (std::floor(b / kBeatsPerBar) + 1.0) * kBeatsPerBar;
            } else if (v == 0.0f) {
                macro_[static_cast<size_t>(i)].releaseBeat = -1.0;
            }
        }
    }
    const double beat = musicalBeat_.load(std::memory_order_relaxed);
    const bool playing = playRequest_.load(std::memory_order_relaxed) || hostSyncNow_.load(std::memory_order_relaxed);
    for (MacroState& s : macro_) {
        if (s.releaseBeat < 0.0) continue;
        // A transport that is not running has no bar line to wait for.
        if (beat >= s.releaseBeat || !playing) { s.value = 0.0f; s.releaseBeat = -1.0; }
    }
    // A macro that let go by itself takes its host parameter with it.
    for (int i = 0; i < kNumMacros; ++i) {
        auto* hp = macroParam_[i];
        if (hp == nullptr || macro_[static_cast<size_t>(i)].value != 0.0f || macroHostSeen_[i] == 0.0f) continue;
        macroHostSeen_[i] = 0.0f;
        hp->setValueNotifyingHost(hp->convertTo0to1(0.0f));
    }

    // What the macros want, summed: two macros may ask for the same knob (gate depth and stutter
    // both do), and the stronger wish wins rather than the later one.
    MacroTarget want[kNumMacros * kMaxMacroTargets];
    int n = 0;
    for (int i = 0; i < kNumMacros; ++i) {
        MacroTarget t[kMaxMacroTargets];
        const int c = macroTargets(static_cast<Macro>(i), macro_[static_cast<size_t>(i)].value, t);
        for (int k = 0; k < c; ++k) {
            int found = -1;
            for (int j = 0; j < n; ++j) if (want[j].param == t[k].param) { found = j; break; }
            if (found < 0) { want[n++] = t[k]; continue; }
            if (t[k].absolute && want[found].absolute) want[found].value = juce::jmax(want[found].value, t[k].value);
            else if (t[k].absolute) want[found] = t[k];
            else want[found].value += t[k].value;
        }
    }

    ParamStore& p = params();
    for (int i = 0; i < n; ++i) {
        const int id = want[i].param;
        // The value is remembered as the store keeps it, not as a normalised position: putting a
        // knob back must be exact, and a round trip through a logarithmic mapping is not.
        const auto it = macroBase_.find(id);
        const float base = it != macroBase_.end() ? it->second : (macroBase_[id] = p.get(id));
        const float target = juce::jlimit(0.0f, 1.0f, want[i].absolute ? want[i].value
                                                                       : p.toNormalised(id, base) + want[i].value);
        p.setNormalised(id, target);
    }
    // Everything no macro wants any more goes back to the value it had before any macro touched it.
    for (auto it = macroBase_.begin(); it != macroBase_.end();) {
        bool wanted = false;
        for (int i = 0; i < n; ++i) if (want[i].param == it->first) { wanted = true; break; }
        if (wanted) { ++it; continue; }
        p.set(it->first, it->second);
        it = macroBase_.erase(it);
    }
}

TransportView PhospheneProcessor::transport() const
{
    TransportView v;
    v.playing = playRequest_.load(std::memory_order_relaxed) || hostSyncNow_.load(std::memory_order_relaxed);
    v.hostSync = hostSyncNow_.load(std::memory_order_relaxed);
    v.musicalBeat = musicalBeat_.load(std::memory_order_relaxed);
    v.bpm = bpmNow_.load(std::memory_order_relaxed);
    v.bar = static_cast<int>(v.musicalBeat / kBeatsPerBar);
    v.restarting = genPrimed_.load(std::memory_order_relaxed) != genWanted_.load(std::memory_order_relaxed);
    // Which track that bar belongs to, from the published plans rather than from the composer: the
    // editor asks for this at every tick and must never wait behind a probe render.
    {
        const std::lock_guard<std::mutex> lock(plansLock_);
        for (const TrackPlan& p : plans_)
            if (v.bar >= p.firstBar && v.bar < p.firstBar + p.bars) { v.track = p.index; v.barInTrack = v.bar - p.firstBar; break; }
    }
    return v;
}

// ------------------------------------------------------------------ MIDI out

void PhospheneProcessor::emitMidi(juce::MidiBuffer& midi, double beatAtStart, double beatsPerSample, int n)
{
    const double endBeat = beatAtStart + beatsPerSample * n;
    auto sampleOf = [&](double beat) {
        if (beatsPerSample <= 0.0) return 0;
        return juce::jlimit(0, n - 1, static_cast<int>((beat - beatAtStart) / beatsPerSample));
    };
    // Note-offs that fall in this block, first: a re-used slot must be free before a note-on takes it.
    for (HeldNote& h : held_) {
        if (!h.active || h.endBeat >= endBeat) continue;
        midi.addEvent(juce::MidiMessage::noteOff(h.channel + 1, h.pitch), sampleOf(h.endBeat));
        h.active = false;
    }
    NoteEvent e;
    while (midiRing_.peek(e)) {
        if (e.beat >= endBeat) break;
        midiRing_.pop(e);
        if (engine_->generatedSilenced(e.part)) continue;   // the keyboard plays this voice (Replace)
        const int channel = midiChannelOf(e.part);
        const int at = sampleOf(e.beat);
        midi.addEvent(juce::MidiMessage::noteOn(channel + 1, e.pitch, static_cast<juce::uint8>(juce::jlimit(1, 127, static_cast<int>(e.velocity)))), at);
        const double off = e.beat + juce::jmax(0.02, static_cast<double>(e.length));
        if (off < endBeat) {
            midi.addEvent(juce::MidiMessage::noteOff(channel + 1, e.pitch), juce::jmax(at, sampleOf(off)));
            continue;
        }
        HeldNote* slot = nullptr;
        for (HeldNote& h : held_) if (!h.active) { slot = &h; break; }
        if (slot == nullptr) continue;   // more than 128 notes sounding at once: the off would be lost anyway
        slot->active = true;
        slot->endBeat = off;
        slot->channel = channel;
        slot->pitch = e.pitch;
    }
}

// ------------------------------------------------------------------ the audio thread

void PhospheneProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    const juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    buffer.clear();
    // MIDI in (23.09.2026): controllers drive whatever was learned onto them (phos/MidiMap.h); notes go to the
    // voice mix.keyboard_part names (Engine::liveNoteOn) at their sample, below. The buffer is then cleared,
    // because it is also the generator's MIDI out.
    int liveCount = 0;
    for (const juce::MidiMessageMetadata meta : midiMessages) {
        const juce::MidiMessage msg = meta.getMessage();
        if ((msg.isNoteOn() || msg.isNoteOff()) && liveCount < kMaxLive) {
            // The keyboard's options (02.10.2026): the keys under the split play the lower voice, Scale Lock moves a key
            // to the nearest note of the track's key and scale (not on the kit, whose keys are its instruments), the
            // velocity goes through its curve. Where a press went, its release goes too.
            const int ch = msg.getChannel() - 1, key = msg.getNoteNumber();
            LiveNote ln{ juce::jlimit(0, juce::jmax(0, n - 1), meta.samplePosition), key, msg.getVelocity(), ch, msg.isNoteOn() };
            if (ln.on) {
                const int mb = params().base(Module::Mix);
                const int lower = static_cast<int>(std::lround(params().get(mb + phos::mix::KeyboardLower)));
                const int split = 36 + 12 * static_cast<int>(std::lround(params().get(mb + phos::mix::KeyboardSplit)));
                ln.part = lower > 0 && key < split ? (lower <= 7 ? lower : lower + 1) : -1;   // Bass and Kit sit past By channel
                const int part = ln.part > 0 ? ln.part : static_cast<int>(std::lround(params().get(mb + phos::mix::KeyboardPart)));
                const bool kit = part == 10 || (part == 8 && ch == 9);
                if (params().get(mb + phos::mix::KeyboardScale) >= 0.5f && !kit)
                    ln.pitch = frame::snapToScale(key, engine_->keyRoot(),
                                                  frame::scaleMask(phos::kScaleNames[juce::jlimit(0, 5, engine_->scale())]));
                ln.velocity = frame::shapeVelocity(ln.velocity, static_cast<int>(std::lround(params().get(mb + phos::mix::KeyboardVelocity))));
                keyMemory_.press(ch, key, ln.part, ln.pitch);
            } else {
                keyMemory_.release(ch, key, ln.part, ln.pitch);
            }
            live_[liveCount++] = ln;
            continue;
        }
        if (msg.isAllNotesOff() || msg.isAllSoundOff()) { engine_->liveAllOff(); keyMemory_.clear(); continue; }
        if (!msg.isController()) continue;
        float norm = 0.0f;
        const int target = midiMap_.handleCc(msg.getChannel() - 1, msg.getControllerNumber(), msg.getControllerValue(), norm);
        if (auto* hp = hostParameter(target)) hp->setValueNotifyingHost(norm);
    }
    midiMessages.clear();
    if (n <= 0) return;

    // ---------------------------------------------------------------- where the clock comes from
    bool playing = playRequest_.load(std::memory_order_relaxed);
    bool hostSync = false;
    double hostBeat = 0.0;
    if (followHostAtomic_.load(std::memory_order_relaxed)) {
        if (auto* ph = getPlayHead()) {
            if (const auto pos = ph->getPosition()) {
                if (const auto ppq = pos->getPpqPosition()) {
                    hostSync = true;
                    hostBeat = *ppq;
                    playing = pos->getIsPlaying();
                    if (const auto bpm = pos->getBpm()) if (*bpm > 1.0) hostBpm_.store(*bpm, std::memory_order_relaxed);
                }
            }
        }
    }
    hostSyncNow_.store(hostSync, std::memory_order_relaxed);
    // PHOS_TRACE=1: a line every hundred blocks on stderr saying what the transport and the
    // handshake are doing. The only way to see inside a plugin that a host has loaded and that is
    // not making a sound.
    if (trace_ && ++traceCount_ % 100 == 0)
        std::fprintf(stderr, "[phos] play=%d sync=%d beat=%.2f want=%d ack=%d reset=%d primed=%d mute=%d follow=%d wrapper=%d\n",
                     static_cast<int>(playing), static_cast<int>(hostSync), hostBeat,
                     genWanted_.load(), genAck_.load(), genReset_.load(), genPrimed_.load(),
                     static_cast<int>(mute_.load()), static_cast<int>(followHostAtomic_.load()),
                     static_cast<int>(wrapperType));
    // Under a host the tempo is the host's. compose.bpm is written directly rather than through the
    // host parameter, so the value the user sees follows the transport without an automation fight;
    // "Follow host" in the Set tab switches the whole arrangement off.
    if (hostSync) {
        const double bpm = hostBpm_.load(std::memory_order_relaxed);
        const int id = params().base(Module::Compose) + compose::Bpm;
        if (bpm > 1.0 && std::fabs(static_cast<double>(params().get(id)) - bpm) > 0.005) params().set(id, static_cast<float>(bpm));
    }

    // ---------------------------------------------------------------- restarts
    //
    // Never ask for a second restart while one is in flight. The composer can need seconds for the
    // first bar of a track it has not planned yet, and the host's play head keeps running through
    // all of them -- so the distance between the two clocks grows while we wait, and asking again
    // for every block of that wait would cancel the answer just before it arrived, over and over.
    // That livelock is what the VST3 test caught: the plugin planned forever and never played.
    const bool restarting = genPrimed_.load(std::memory_order_acquire) != genWanted_.load(std::memory_order_acquire);
    const bool readyNow = genPrimed_.load(std::memory_order_acquire) == genWanted_.load(std::memory_order_acquire)
                       && genReset_.load(std::memory_order_relaxed) == genWanted_.load(std::memory_order_acquire);
    if (restartRequest_.exchange(false, std::memory_order_acq_rel)) {
        // 22.09.2026, the user: "Klicken direkt im Arrangement-View fuehrt zu schlimmsten
        // Stoergeraeuschen". A click there is a seek, and a seek used to take effect inside this very
        // block: genWanted_ moved, the `ready` test below failed, and the block came out silent --
        // from whatever level the waveform happened to be at. The host test measured a step of
        // 0.0235 on a quiet passage, and the size of that step is simply the instantaneous signal, so
        // on a kick it is most of full scale. A step is broadband; once per mouse click it is exactly
        // what the user described.
        //
        // So the seek waits for the end of this block: the block renders as it would have, a fade to
        // zero is laid over it, and only then is the request made. The composer thread cannot be
        // repositioning while we render, because it waits for genWanted_ -- which is the reason the
        // request has to go *after* the render and not before it. When nothing is sounding (stopped,
        // or a restart already in flight) there is nothing to fade and the seek goes at once.
        if (playing && readyNow) {
            fadeOutThenSeek_ = true;
        } else {
            requestSeek(pendingBar_.load(std::memory_order_relaxed), pendingOffset_.load(std::memory_order_relaxed));
            wasPlaying_ = false;
        }
    } else if (hostSync && !restarting) {
        const double expected = engine_->beatPosition() + conductor_->beatOffset();
        const double drift = hostBeat - expected;
        if (playing && (!wasPlaying_ || std::fabs(drift) > 0.25)) {
            // The transport started, or the host moved the play head. Everything below a bar is
            // cheap to redo, because composeBars is deterministic per bar. If the plan for that bar
            // is not cached yet this takes a moment, and the next block's drift check picks up
            // whatever the host moved on by in the meantime.
            requestSeek(static_cast<int>(std::floor(hostBeat / kBeatsPerBar)), hostBeat);
        } else if (playing && std::fabs(drift) > 1.0e-9) {
            // Rounding between two clocks, not a jump: slide the offset instead of resetting, so
            // the events stay where the host expects them and nothing clicks.
            conductor_->nudgeOffset(drift);
        }
    } else if (!hostSync && playing && !wasPlaying_ && !restarting) {
        requestSeek(pendingBar_.load(std::memory_order_relaxed), pendingOffset_.load(std::memory_order_relaxed));
    }
    const bool justStopped = wasPlaying_ && !playing;
    wasPlaying_ = playing;
    // A played note belongs to the voice it went to; a transport that stops, or a keyboard moved to another voice,
    // must not leave it hanging there.
    const int keyboardPart = static_cast<int>(std::lround(params().get(params().base(Module::Mix) + phos::mix::KeyboardPart)));
    if (justStopped || keyboardPart != keyboardPartSeen_) engine_->liveAllOff();
    keyboardPartSeen_ = keyboardPart;
    if (justStopped) {
        // A transport that stops in the middle of a note leaves it sounding in whatever the MIDI
        // output is wired to; the notes we started are the notes we end.
        for (HeldNote& h : held_) {
            if (!h.active) continue;
            midiMessages.addEvent(juce::MidiMessage::noteOff(h.channel + 1, h.pitch), 0);
            h.active = false;
        }
    }

    // Step 3: the composer has positioned itself, so the engine may be cleared for the new start.
    const int want = genWanted_.load(std::memory_order_acquire);
    auto resetForNewStart = [this, want] {
        if (genAck_.load(std::memory_order_acquire) != want || genReset_.load(std::memory_order_relaxed) == want) return;
        engine_->reset();
        NoteEvent junk;
        while (midiRing_.pop(junk)) {}
        for (HeldNote& h : held_) h = HeldNote{};
        genReset_.store(want, std::memory_order_release);
    };
    resetForNewStart();
    // Offline (a bounce, or the host test): this thread does the composer's work itself, once per
    // block, exactly where phos_render does it -- so the block is self-contained and the render is
    // bit-identical. The loop is for the handshake, which needs up to three steps on a restart.
    if (isNonRealtime()) {
        for (int i = 0; i < 3; ++i) {
            serviceComposer(false);
            resetForNewStart();
            if (genPrimed_.load(std::memory_order_relaxed) == want) break;
        }
    }

    const bool ready = genPrimed_.load(std::memory_order_acquire) == want && genReset_.load(std::memory_order_relaxed) == want;
    if (!playing || !ready) {
        musicalBeat_.store(engine_->beatPosition() + conductor_->beatOffset(), std::memory_order_relaxed);
        return;   // silence: either stopped, or a restart is in flight
    }

    // ---------------------------------------------------------------- render
    const double beatAtStart = engine_->beatPosition();
    float* L = buffer.getWritePointer(0);
    float* R = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
    auto render = [&](int from, int count) {
        if (count <= 0) return;
        if (R == nullptr) {
            // Mono device: render the stereo pair into the scratch and fold it down.
            const int m = juce::jmin(count, scratch_.getNumSamples() - from);
            if (m <= 0) return;
            engine_->process(scratch_.getWritePointer(0, from), scratch_.getWritePointer(1, from), m);
            for (int i = from; i < from + m; ++i) L[i] = 0.5f * (scratch_.getReadPointer(0)[i] + scratch_.getReadPointer(1)[i]);
        } else {
            engine_->process(L + from, R + from, count);
        }
    };
    // The effects page's audition (previewSfx): a live action like a played note, at the block's start.
    if (const int pv = sfxPreview_.exchange(-1, std::memory_order_acq_rel); pv >= 0) engine_->previewSfx(pv >> 12, pv & 0xFFF);
    // Played notes split the block at their samples; a block without one is a single process() call, exactly as
    // before, so a set nobody plays along to stays bit-identical to phos_render.
    int done = 0;
    for (int k = 0; k < liveCount; ++k) {
        const LiveNote& ln = live_[k];
        render(done, ln.at - done);
        done = juce::jmax(done, ln.at);
        if (ln.on) engine_->liveNoteOn(ln.pitch, ln.velocity, ln.channel, ln.part);
        else engine_->liveNoteOff(ln.pitch, ln.channel);
    }
    render(done, n - done);
    {
        // The channel meters: raised here, taken by the mixer page (takeChannelMeters). One writer, so a load and a
        // store will do; a block the editor takes in between is at worst counted in the next reading.
        float pk[phos::kNumParts];
        double ss[phos::kNumParts];
        const int got = engine_->takeMeters(pk, ss);
        // What every parameter played at, for the editor's live ring (26.09.2026): one relaxed store each, a few
        // microseconds a block.
        engine_->playedValues(playedScratch_.data());   // with the modulation of the voice one hears (Engine.h)
        for (int id = 0; id < playedCount_; ++id)
            played_[static_cast<size_t>(id)].store(playedScratch_[static_cast<size_t>(id)], std::memory_order_relaxed);
        if (got > 0) {
            for (int k = 0; k < phos::kNumParts; ++k) {
                if (pk[k] > meterPeak_[static_cast<size_t>(k)].load(std::memory_order_relaxed))
                    meterPeak_[static_cast<size_t>(k)].store(pk[k], std::memory_order_relaxed);
                meterSum_[static_cast<size_t>(k)].store(meterSum_[static_cast<size_t>(k)].load(std::memory_order_relaxed) + ss[k],
                                                        std::memory_order_relaxed);
            }
            meterCount_.fetch_add(got, std::memory_order_release);
        }
    }
    if (fadeOutThenSeek_) {
        // A raised cosine rather than a straight line: its first derivative is zero at both ends, so
        // neither the start of the fade nor the arrival at zero is itself a corner.
        for (int c = 0; c < buffer.getNumChannels(); ++c) {
            float* d = buffer.getWritePointer(c);
            for (int i = 0; i < n; ++i)
                d[i] *= static_cast<float>(0.5 * (1.0 + std::cos(juce::MathConstants<double>::pi * (i + 1) / n)));
        }
        fadeOutThenSeek_ = false;
        requestSeek(pendingBar_.load(std::memory_order_relaxed), pendingOffset_.load(std::memory_order_relaxed));
        wasPlaying_ = false;
    }
    const double beatAfter = engine_->beatPosition();
    const double beatsPerSample = n > 0 ? (beatAfter - beatAtStart) / n : 0.0;
    musicalBeat_.store(beatAfter + conductor_->beatOffset(), std::memory_order_relaxed);
    bpmNow_.store(beatsPerSample * sampleRate_ * 60.0, std::memory_order_relaxed);

    emitMidi(midiMessages, beatAtStart, beatsPerSample, n);

    // ---------------------------------------------------------------- the cue bridge (PLAN 8.3)
    //
    // Here, and nowhere earlier. The composer knows every boundary bars in advance -- which is why
    // the marks travel ahead in a ring -- but a cue is an instant, and the only instant a beat has
    // is the one at which its samples are rendered. What is rendered now is not what is heard now,
    // by two known amounts, and both make the cue early rather than late: the limiter's lookahead
    // (engine->latencySamples()) and the buffers between this block and the device. The portable
    // part of the second is the block itself -- on the usual double-buffered stream the block being
    // filled is the one after the block being played -- and `cue.lead_ms` trims whatever the
    // driver's own queue adds, the one number a plugin is never told. The tap stamps every cue with
    // that instant; the sender's thread waits for it.
    {
        cueTap_.setSendBeats(cueBeats_.load(std::memory_order_relaxed));
        const double offset = conductor_->beatOffset();
        const int64_t now = phos::cueNowNanos();
        const double leadSeconds = static_cast<double>(n + engine_->latencySamples()) / sampleRate_
                                 + 0.001 * static_cast<double>(cueLeadMs_.load(std::memory_order_relaxed));
        const int64_t leadNanos = static_cast<int64_t>(leadSeconds * 1.0e9);
        const int64_t blockNanos = static_cast<int64_t>(static_cast<double>(n) / sampleRate_ * 1.0e9);
        if (cueAnnounce_.exchange(false, std::memory_order_acq_rel))
            cueTap_.announce(now + leadNanos, cues_.queue());
        cueTap_.scan(beatAtStart + offset, beatAfter + offset, now, leadNanos, blockNanos, cueMarks_, cues_.queue());
        // With the bridge off the tap still runs -- it costs a handful of comparisons, it keeps the
        // mark ring empty, and it remembers the section that is playing, so switching the bridge on
        // says where we are at once instead of waiting for the next boundary.
        if (!cues_.running()) cues_.queue().clear();
    }

    {
        const juce::ScopedTryLock lock(recordLock_);
        if (lock.isLocked() && recordWriter_ != nullptr) {
            recordWriter_->write(buffer.getArrayOfReadPointers(), n);
            recordedSamples_.fetch_add(n, std::memory_order_relaxed);
        }
    }
    if (mute_.load(std::memory_order_relaxed)) buffer.clear();
}

// ------------------------------------------------------------------ state

void PhospheneProcessor::writeStateTo(juce::MemoryBlock& dest) const
{
    juce::XmlElement xml("PHOSPHENE");
    // The values travel as "key=value" text, so the order of the parameters never reaches a state; the
    // version says what the *content* means (PluginProcessor.h, kStateVersion).
    xml.setAttribute("version", kStateVersion);
    xml.setAttribute("seed", juce::String(seed_.load()));
    xml.setAttribute("followHost", followHost_);
    // The cue destination is not a parameter (a parameter is a float); the port and the switch are.
    xml.setAttribute("cueHost", cueHost());
    xml.setAttribute("fieldFolder", fieldFolder_);   // 27.09.2026: the user's own field recordings
    // 20.09.2026, round "dialogue": only the knobs that differ from *this* build's defaults. A state
    // that held every value silently undid every default a later round recalibrated -- see kStateVersion
    // for the whole reasoning and for what it costs. "%.9g" still round-trips a float exactly, so a
    // knob that is in the file comes back bit for bit.
    //
    // A knob a macro is holding is saved at the value it goes back to, not where the macro has it (23.09.2026: the
    // VST3 test randomised the four macro host parameters, a filter sweep moved the cutoffs, and the state no longer
    // came back byte for byte). The macros themselves are performance, not part of the set.
    std::string knobs;
    {
        const std::lock_guard<std::mutex> lock(macroBaseLock_);
        if (macroBase_.empty()) {
            knobs = params().toText(true);
        } else {
            ParamStore base;
            base.copyValuesFrom(params());
            for (const auto& [id, value] : macroBase_) base.set(id, value);
            knobs = base.toText(true);
        }
    }
    xml.createNewChildElement("params")->addTextElement(juce::String(knobs));
    // The learned controllers, by parameter key (23.09.2026): a map outlives parameters being added.
    xml.createNewChildElement("midimap")->addTextElement(juce::String(midiMap_.toText([this](int t) { return midiTargetKey(t); })));
    juce::AudioProcessor::copyXmlToBinary(xml, dest);
}

void PhospheneProcessor::getStateInformation(juce::MemoryBlock& destData) { writeStateTo(destData); }

void PhospheneProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml == nullptr || !xml->hasTagName("PHOSPHENE")) return;
    // Every knob back to its default first, then the state's own values on top. With version 3 the
    // state names only the knobs the user moved, so this is what makes a recalibrated default reach an
    // old session at all (PluginProcessor.h, kStateVersion).
    params().resetDefaults();
    legacyKnobs_.clear();
    lastStateVersion_ = xml->getIntAttribute("version", 1);
    const juce::String knobs = xml->getChildByName("params") != nullptr
                             ? xml->getChildByName("params")->getAllSubText() : juce::String();
    if (lastStateVersion_ >= 3) {
        params().parseText(knobs.toStdString());
        // A macro still holding a knob now holds it from the loaded value: that is where it goes back to.
        const std::lock_guard<std::mutex> lock(macroBaseLock_);
        for (auto& [id, value] : macroBase_) value = params().get(id);
    } else {
        // A version-1 or version-2 state holds *every* value of its session and cannot say which of them
        // the user chose. Applying it is exactly the failure the state version 3 exists to stop: the user's own
        // standalone state carried sfx.level -12, bass.cutoff 140, mix.lead_level 0 and kick.level -2
        // from an older calibration and silently undid four rounds of work.
        //
        // The decision, and it is a decision: such a state sets **no knob at all**. The engine comes up
        // on today's factory defaults, which is the safe half of the choice and the one a listener
        // wants by default, and the old text is kept so the editor can offer it ("Load the saved knobs
        // anyway", adoptLegacyState). What is *not* a calibrated value is restored as always -- the
        // seed, the host-sync switch and the cue destination are the user's set, not our tuning.
        //
        // The cost is real and is stated rather than hidden: a DAW project saved with an older build
        // opens with its sound on the defaults until the user takes the offer.
        legacyKnobs_ = knobs;
    }
    // A loaded set lets go of every macro (01.10.2026). The macros are performance, not part of the set
    // (writeStateTo), so a state cannot say where they stand -- and leaving them where they were did not bring them
    // back either: pluginval's state test pressed the drop-out, the timer let it go by itself (no transport, no bar
    // line to wait for), and the next state handed the host a value it had not had before. Neutral is the one value
    // a state can promise; the knobs a macro held go back to what was just loaded.
    for (int i = 0; i < kNumMacros; ++i) setMacro(static_cast<Macro>(i), 0.0f);
    if (auto* mm = xml->getChildByName("midimap"))
        midiMap_.fromText(mm->getAllSubText().toStdString(), [this](const std::string& k) { return midiTargetFind(k); });
    if (midiMap_.bindings().empty()) bindDefaultControllers();   // a state from before 01.10.2026 had none at all
    followHost_ = xml->getBoolAttribute("followHost", followHost_);
    setCueHost(xml->getStringAttribute("cueHost", cueHost()));
    if (xml->getStringAttribute("fieldFolder") != fieldFolder_) setFieldFolder(xml->getStringAttribute("fieldFolder"));
    followHostAtomic_.store(followHost_, std::memory_order_release);
    const uint64_t s = static_cast<uint64_t>(xml->getStringAttribute("seed", "1").getLargeIntValue());
    setSeed(juce::jmax<uint64_t>(1, s));
}

void PhospheneProcessor::adoptLegacyState()
{
    if (legacyKnobs_.isEmpty()) return;
    params().parseText(legacyKnobs_.toStdString());
    legacyKnobs_.clear();
}

void PhospheneProcessor::resetToFactoryDefaults()
{
    // The host's own parameter objects read the store, so putting the store back is enough for the
    // sound; the editor refreshes from the same place on its timer.
    params().resetDefaults();
    legacyKnobs_.clear();
    lastStateVersion_ = kStateVersion;
}

// ------------------------------------------------------------------ export

bool PhospheneProcessor::exportMidi(const juce::File& file, int bars)
{
    // Blocks on the composer, which is what a user action with a file dialog in front of it may do.
    const std::lock_guard<std::mutex> lock(composeLock_);
    // The knobs as they stand, every one of them: the file is the set the user sees (composeParams_, 25.09.2026).
    refreshComposeParams(true);
    const ParamStore& knobs = composeParams_;
    const int total = juce::jlimit(1, 4096, bars);
    Score score;
    score.tempo = composer_->tempoMap(knobs, total);
    score.keyRoot = composer_->track(knobs, 0).key;
    score.scale = composer_->track(knobs, 0).scale;
    for (int t = 0; composer_->track(knobs, t).firstBar < total; ++t) {
        const TrackPlan p = composer_->track(knobs, t);
        const double beat = static_cast<double>(p.firstBar) * kBeatsPerBar;
        // The key signature changes where the bass does: at the hand-over of the DJ overlap (19.09.2026).
        const double keyBeat = static_cast<double>(handoverBar(p)) * kBeatsPerBar;
        if (t > 0 && p.key != composer_->track(knobs, t - 1).key) score.keyChanges.push_back(KeyChange{ keyBeat, p.key });
        score.sections.push_back(SectionMark{ beat, SectionType::Groove, 0.5f, t });
    }
    std::vector<NoteEvent> notes;
    composer_->composeBars(knobs, 0, total, notes, nullptr);
    score.notes = std::move(notes);
    score.sort();
    return writeMidiFile(score, file.getFullPathName().toRawUTF8());
}

juce::File PhospheneProcessor::galleryFolder() const
{
    const juce::String env = juce::SystemStats::getEnvironmentVariable("PHOS_GALLERY_DIR", "");
    const juce::File dir = env.isNotEmpty() ? juce::File(env)
                                            : juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Phosphene").getChildFile("Sets");
    // Not created here: the gallery page asks for the path just by opening (23.09.2026 -- the VST3 test, which opens
    // the editor, left an empty Sets folder in the user's real data folder). Saving creates it.
    return dir;
}

juce::File PhospheneProcessor::userFolder() const
{
    // PHOS_USER_DIR (tests): a folder of their own for the ratings, the preferences and the user presets. The host
    // test presses every button of the editor, "Good here" and "Learn from my ratings..." among them, and must never
    // write into the user's real files -- its first runs of 23.09.2026 left sixteen verdicts in them before this existed.
    const juce::String env = juce::SystemStats::getEnvironmentVariable("PHOS_USER_DIR", "");
    const juce::File dir = env.isNotEmpty() ? juce::File(env)
                                            : juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Phosphene");
    dir.createDirectory();
    return dir;
}

juce::File PhospheneProcessor::ratingsFile() const
{
    return userFolder().getChildFile("ratings.tsv");
}

// ------------------------------------------------------------------ sound presets

juce::File PhospheneProcessor::userPresetFolder(Module module, int instance) const
{
    const std::string key = params().key(params().base(module, instance));
    return userFolder().getChildFile("Presets").getChildFile(juce::String(key.substr(0, key.find('.'))));
}

std::vector<SoundPreset> PhospheneProcessor::userPresets(Module module, int instance) const
{
    std::vector<SoundPreset> out;
    const juce::File dir = userPresetFolder(module, instance);
    if (!dir.isDirectory()) return out;
    for (const juce::File& f : dir.findChildFiles(juce::File::findFiles, false, "*.txt"))
        out.push_back({ "User", f.getFileNameWithoutExtension().toStdString(), f.loadFileAsString().removeCharacters("\r").toStdString() });
    std::sort(out.begin(), out.end(), [](const SoundPreset& a, const SoundPreset& b) { return a.name < b.name; });
    return out;
}

void PhospheneProcessor::previewSfx(int choice, int preset)
{
    if (choice < 0 || choice >= phos::sfx::kNumPresetChoices) return;
    sfxPreview_.store((choice << 12) | juce::jlimit(0, 0xFFF, preset), std::memory_order_release);
}

void PhospheneProcessor::takeChannelMeters(float* peak, float* rms)
{
    const int n = meterCount_.exchange(0, std::memory_order_acquire);
    for (int k = 0; k < phos::kNumParts; ++k) {
        peak[k] = meterPeak_[static_cast<size_t>(k)].exchange(0.0f, std::memory_order_relaxed);
        const double s = meterSum_[static_cast<size_t>(k)].exchange(0.0, std::memory_order_relaxed);
        rms[k] = n > 0 ? static_cast<float>(std::sqrt(s / n)) : 0.0f;
    }
}

void PhospheneProcessor::applyPreset(Module module, int instance, const SoundPreset& preset)
{
    // The synth's owner number is the order of the own-sound switches (Params.h, mix::KickOwn): kick, bass, acid, the voices.
    const int owner = module == Module::Kick ? 0 : module == Module::Bass ? 1 : module == Module::Acid ? 2 : 3 + instance;
    undoable("Preset " + juce::String(preset.name), [&] {
        applySoundPreset(params(), module, instance, preset.text);
        params().set(params().base(Module::Mix) + mix::KickOwn + owner, 1.0f);
    });
}

void PhospheneProcessor::applyFieldPreset(int index)
{
    const std::vector<FieldPreset>& all = fieldPresets();
    if (index < 0 || index >= static_cast<int>(all.size())) return;
    const FieldPreset& fp = all[static_cast<size_t>(index)];
    // A preset loaded by hand is the user's choice: its category is no longer Auto, so the composer leaves it alone.
    undoable("Field preset " + juce::String(fp.name), [&] {
        const int b = params().base(Module::Field);
        for (const auto& kv : fieldPresetValues(params(), fp)) params().set(b + kv.first, kv.second);
    });
}

void PhospheneProcessor::setFieldFolder(const juce::String& folder)
{
    fieldFolder_ = folder.trim();
    setFieldUserFolder(fieldFolder_.toStdString());
    scanFieldLibrary();
}

juce::File PhospheneProcessor::saveUserPreset(Module module, int instance, const juce::String& name)
{
    const juce::String clean = juce::File::createLegalFileName(name.trim());
    if (clean.isEmpty()) return {};
    const juce::File dir = userPresetFolder(module, instance);
    dir.createDirectory();
    const juce::File f = dir.getChildFile(clean + ".txt");
    // "\n" line ends: JUCE's default is "\r\n", and the preset must read back as the text it was.
    return f.replaceWithText(juce::String(moduleText(params(), module, instance)), false, false, "\n") ? f : juce::File();
}

juce::File PhospheneProcessor::saveToGallery(const juce::String& name)
{
    GalleryEntry e;
    e.name = name.trim().toStdString();
    e.saved = juce::Time::getCurrentTime().formatted("%Y-%m-%dT%H:%M:%S").toStdString();
    {
        // The published plans: the tracks the editor has seen planned, which is what the thumbnail should show.
        const std::lock_guard<std::mutex> lock(plansLock_);
        for (const TrackPlan& p : plans_) e.tracks.push_back(galleryTrackOf(p));
    }
    std::string text;
    {
        const std::lock_guard<std::mutex> lock(composeLock_);
        text = writeSetText(*composer_, params());
    }
    text = withGalleryComment(text, e);
    juce::String base = juce::Time::getCurrentTime().formatted("%Y-%m-%d_%H%M") + "_";
    base += name.trim().isNotEmpty() ? juce::File::createLegalFileName(name.trim()) : juce::String("seed") + juce::String(seed_.load());
    galleryFolder().createDirectory();
    const juce::File f = galleryFolder().getNonexistentChildFile(base, ".phosset", false);
    return f.replaceWithText(juce::String(text)) ? f : juce::File();
}

juce::File PhospheneProcessor::preferencesFile() const
{
    return ratingsFile().getSiblingFile("preferences.txt");
}

Preferences PhospheneProcessor::fitFromRatings(int* verdicts) const
{
    std::vector<RatingEntry> ratings;
    juce::StringArray lines;
    ratingsFile().readLines(lines);
    for (const juce::String& l : lines) {
        RatingEntry r;
        if (parseRating(l.toStdString(), r) && r.verdict != 0 && !r.features.empty()) ratings.push_back(r);
    }
    if (verdicts != nullptr) *verdicts = static_cast<int>(ratings.size());
    return fitPreferences(ratings);
}

bool PhospheneProcessor::applyPreferences(const Preferences& prefs)
{
    const bool written = preferencesFile().replaceWithText(juce::String(prefs.toText()));
    setPreferences(std::make_shared<Preferences>(prefs));
    // The plans are made again (Composer::validate watches the revision); the bars in the rings were composed under
    // the old taste, so the transport restarts at the bar that is playing, as a reroll does.
    markCurrentPosition();
    plansStale_.store(true, std::memory_order_release);
    restartRequest_.store(true, std::memory_order_release);
    restarts_.fetch_add(1, std::memory_order_relaxed);
    return written;
}

void PhospheneProcessor::forgetPreferences()
{
    const juce::File f = preferencesFile();
    if (f.existsAsFile()) f.moveFileTo(f.getSiblingFile("preferences.txt.old"));
    setPreferences(nullptr);
    markCurrentPosition();
    plansStale_.store(true, std::memory_order_release);
    restartRequest_.store(true, std::memory_order_release);
    restarts_.fetch_add(1, std::memory_order_relaxed);
}

juce::String PhospheneProcessor::preferencesSummary() const
{
    const std::shared_ptr<const Preferences> p = preferences();
    if (p == nullptr || p->empty()) return "No learned preferences: every choice is the style's own.";
    // The strongest three, read back from the text form (one line per weight).
    juce::StringArray lines = juce::StringArray::fromLines(juce::String(p->toText()));
    std::vector<std::pair<double, juce::String>> w;
    for (const juce::String& l : lines) {
        if (l.startsWithChar('#') || l.trim().isEmpty()) continue;
        w.emplace_back(std::fabs(l.fromLastOccurrenceOf(" ", false, false).getDoubleValue()), l.trim());
    }
    std::sort(w.begin(), w.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    juce::String s;
    s << static_cast<int>(p->size()) << " learned weights in force, strongest: ";
    for (size_t i = 0; i < std::min<size_t>(3, w.size()); ++i) s << (i > 0 ? ",  " : "") << w[i].second;
    return s;
}

bool PhospheneProcessor::exportSet(const juce::File& file)
{
    // The core's own `.phosset` writer (SetFile.h), not a private format: seed, style, arc, the
    // knobs that differ from their defaults and -- since the curation loop exists in the editor --
    // every lock and every reroll counter. What this writes, `phos_render --set-file` plays.
    const std::lock_guard<std::mutex> lock(composeLock_);
    return writeSetFile(file.getFullPathName().toRawUTF8(), *composer_, params());
}

bool PhospheneProcessor::importSet(const juce::File& file)
{
    bool ok = false;
    {
        // Blocks on the composer, which a user action with a file dialog in front of it may do.
        const std::lock_guard<std::mutex> lock(composeLock_);
        std::string error;
        ok = readSetFile(file.getFullPathName().toRawUTF8(), *composer_, params(), &error);
        if (!ok && !error.empty()) juce::Logger::writeToLog("phosset: " + juce::String(error));
        seed_.store(composer_->seed(), std::memory_order_relaxed);
    }
    syncCurationMirror();
    plansStale_.store(true, std::memory_order_release);
    seekToBar(0);
    return ok;
}

// ------------------------------------------------------------------ recording

bool PhospheneProcessor::startRecording(const juce::File& file)
{
    stopRecording();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr) return false;
    juce::WavAudioFormat wav;
    // 32-bit float, as the offline renderer writes: a recording of a set is a master, not a mixdown.
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriter::Options{}
                                                  .withSampleRate(sampleRate_)
                                                  .withNumChannels(2)
                                                  .withBitsPerSample(32)
                                                  .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (writer == nullptr) return false;
    if (!recordThread_.isThreadRunning()) recordThread_.startThread();
    auto threaded = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(writer.release(), recordThread_, 1 << 16);
    recordedSamples_.store(0);
    {
        const juce::ScopedLock lock(recordLock_);
        recordWriter_ = std::move(threaded);
    }
    recording_.store(true);
    return true;
}

void PhospheneProcessor::stopRecording()
{
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> gone;
    {
        const juce::ScopedLock lock(recordLock_);
        gone = std::move(recordWriter_);
    }
    gone.reset();
    recording_.store(false);
}

// ------------------------------------------------------------------ editor and entry point

juce::AudioProcessorEditor* PhospheneProcessor::createEditor() { return new PhospheneEditor(*this); }

/** @brief JUCE's entry point: one processor per instance. */
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PhospheneProcessor(); }
