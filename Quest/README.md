# Phosphene for Meta Quest

The whole generator on the headset: the composer plans the set, the engine synthesizes it, and the
hands play it. Native OpenXR, no game engine — `NativeActivity` + `android_native_app_glue`, EGL,
GLES 3, the Khronos OpenXR loader, `XR_EXT_hand_tracking`, Oboe, and the unchanged core from
`../Core`.

```
Quest/
  CMakeLists.txt        NDK build of libphosquest.so (links PhospheneCore, oboe, openxr_loader)
  AndroidManifest.xml   NativeActivity, hasCode=false, hand-tracking permission/features, VR category
  src/main.cpp          the app: OpenXR session, composer thread, Oboe, hand macros, GLES panel, OSC cues
  res/mipmap-*/         the launcher icon at five densities
  fetch_thirdparty.ps1  OpenXR loader (prefab AAR) and Oboe into ../ThirdParty
  build_apk.ps1         CMake/NDK -> aapt2 -> jar -> zipalign -> apksigner (debug key)
```

## Build

```powershell
powershell -File Quest\fetch_thirdparty.ps1
powershell -File Quest\build_apk.ps1
adb install -r bin\quest\PhospheneQuest.apk
```

Needs NDK r27 (`C:\Android-Buildtools\sdk\ndk\27.2.12479018`), build-tools 34, platform android-34
and JDK 17 — the parameters at the top of `build_apk.ps1`. No Gradle and no ninja: the NDK's own
`make.exe` drives the CMake build.

`fetch_thirdparty.ps1` makes directory junctions into Noctuary's `ThirdParty` when that checkout is
present (the same OpenXR loader and the same Oboe), and downloads them only if it is not; `-Copy`
copies instead of linking, `-Download` ignores the sibling.

## Three threads

| Thread | Does |
|---|---|
| Audio (Oboe, low latency, exclusive) | `SetPlayer::process`: `Engine::process` and the play/stop fade. One compare-and-exchange, no lock, no allocation; while the composer holds the player for a track jump it writes silence. |
| Composer | plans tracks, composes bars, keeps the engine's rings filled eight bars ahead, and publishes what the panel shows. Meant for one of the XR2's small A55 cores. |
| Render (the glue thread) | OpenXR frame loop, hands, gestures, picture. |

`SetPlayer` is `Conductor` (Composer.h) with a bar offset: the engine always plays from its own beat
0, and every composed event and the tempo map are moved back by the beat of the first bar to play,
so "next track" can start the engine at bar 700 of the set. Nothing in the core had to change.

## Playing it

| Gesture | Effect |
|---|---|
| left pinch | play / stop — a 15 ms fade, the music pauses where it is |
| right pinch | drop-out: kick and bass gone until the next bar line |
| right pinch held 0.6 s | stutter: lead, counter, arp, stab and pad chopped on sixteenths while it is held |
| both hands pinched together | the next track: the conductor goes to that track's first bar |
| left hand height | filter sweep: the acid, lead, counter, arp and stab filters together, closed low, open high (a dead zone round the middle) |
| right hand height | gate depth: the trance gate of lead, counter, arp, stab and pad, from mid height up |

These are the plugin's four perform macros (`macroTargets`, copied into the app) in the grammar every generator's
headset shares (01.10.2026; until then the right pinch was the next track and the heights the track gain and the acid
cutoff): a pinch acts when it opens again, so a pinch of both hands never also counts as two single ones. A hand moves
its macro only while it is **not** pinching. Height is measured against the head (`(palm.y − (head.y − 1 m)) / 0.8 m`), so
it works in a STAGE space and in a LOCAL one and for any player's size. Both macros are smoothed
with a 0.15 s one-pole and both are centred: a hand at mid height plays exactly what the composer
wrote, and nothing ever jumps.

**The bridge.** With `bridge_host` set, the app sends its hands to Phosphene on that computer as well: OSC `/hands` with
six floats (left and right height, left and right pinch, left and right tracked), 30 times a second, to `bridge_port`
(9101 by default, the port in the plugin's settings under Headset). The plugin reads them with the same grammar and
the same numbers -- every generator of the family has them (its `Plugin/Frame.h`) -- and shows its headset controls
while they arrive. `audio=0` leaves the headset silent, so only the computer plays.

The panel is head-locked (yaw only) and drawn as points: track and bar, key and scale, tempo, the
16-bar block and what plays in it, loudness, both macro values, four beat lamps whose brightness is
a raised cosine of the distance to the beat, and a loudness row. No camera movement follows the
audio and no brightness steps (the Kaleidoscope rules, PLAN 8.2).

## Config

`phos.cfg` in the app's external data folder, every key optional:

```
adb push phos.cfg /sdcard/Android/data/com.reneweller.phosphene.quest/files/phos.cfg
```

```
mute=1                     start silent (the test rule); the engine still runs
seed=2026                  set seed
quality=quest              quest (the default here) or desktop
osc_host=192.168.1.20      Kaleidoscope cue bridge (PLAN 8.3), empty = off
osc_port=9000
bridge_host=192.168.1.20   the bridge: the hands to Phosphene on that computer; empty = off
bridge_port=9101                its headset port (the plugin's settings, Headset)
audio=0                    no sound on the headset, the computer plays (the same as mute=1)
set=compose.pad_amount=1;compose.acid_amount=1     any knobs, repeatable
```

The cue bridge sends `/phos/bar f f` (bar, BPM) once per bar and `/phos/track f f f`
(track, key, scale) at each track change — from the score, never from an analysis of the audio.

## The shipped data: the wavetable library and the two learned models

Three files ride in the APK as assets — `Core/data/library.phoswt` (750 KB, twelve tables) and the
two weight files of Phase 8, `melody.phosmdl` and `bass.phosmdl` (1.5 MB each). `build_apk.ps1`
stages them and `aapt2 link -A` packs them. The pack deflates to 662 KB; the two models are stored
uncompressed, because int8 weights are noise as far as deflate is concerned. Measured on this
machine: the APK goes from **4.59 MB to 7.63 MB**, 3.04 MB for the whole of Phase 8 on the headset.

The core opens its resources by name and an asset inside the APK has none, so the app unpacks each
one into its private directory on the first start and points `setWaveTableSearchPath()` and
`setModelSearchPath()` there (`prepareAsset` and `prepareModels` in `src/main.cpp`); afterwards a
copy is recognised by its length and nothing is written. That costs about 3.8 MB of the headset's
internal storage, once.

A file of the same name pushed into the external data folder beside `phos.cfg` wins over the one in
the APK, which is how another table selection or another trained model is tried on the device
without building an APK for it:

```
adb push library.phoswt /sdcard/Android/data/com.reneweller.phosphene.quest/files/library.phoswt
adb push melody.phosmdl /sdcard/Android/data/com.reneweller.phosphene.quest/files/melody.phosmdl
```

Without the two models the composer falls back to the Markov model and the pattern families — the
whole of Phase 8 — and the only word about it is a line in `logcat`. That is why `build_apk.ps1`
refuses to finish when an asset is missing or is the wrong length, and why the package check has a
section of its own for it (`Tools/release/check_package.ps1`, check H).

At the `quest` quality level the tables are built with 32 of their 64 frames
(`Core/include/phos/Quality.h`), which takes the expanded mip levels from 23.3 MB to 12.1 MB and the
load from 110 ms to 71 ms. With no pack at all the engine falls back to its six built-in tables and
says so in the log — it does not go silent.

## Checks on the headset

The NEON lane path is only run through an x86 stand-in on the desktop (`Tests/neonshim`). The arm64
build of `phos_vectest` runs the real one, and `phos_selftest` measures every building block on the
device's own arithmetic:

```powershell
cmake -S . -B build-android -G "Unix Makefiles" `
    -DCMAKE_MAKE_PROGRAM="C:\Android-Buildtools\sdk\ndk\27.2.12479018\prebuilt\windows-x86_64\bin\make.exe" `
    -DCMAKE_TOOLCHAIN_FILE="C:\Android-Buildtools\sdk\ndk\27.2.12479018\build\cmake\android.toolchain.cmake" `
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release -DPHOS_BUILD_TOOLS=ON
cmake --build build-android -j 6
adb push build-android\Tests\phos_vectest build-android\Tests\phos_selftest build-android\Tools\render\phos_render /data/local/tmp/
adb shell chmod +x /data/local/tmp/phos_vectest /data/local/tmp/phos_selftest /data/local/tmp/phos_render
adb shell /data/local/tmp/phos_vectest
adb shell /data/local/tmp/phos_selftest
```

The self test takes about half an hour on the desktop in one process (19.09.2026) and will take
longer here; `--only testAcid` (or any other section name, exact; `--list` prints them all) runs one
section at a time: `adb shell /data/local/tmp/phos_selftest --only testPoly`. There is no ctest on
the device, so the per-section tests of the desktop suite do not exist here.

`phos_vectest` must print `path neon` and every lane identical to the scalar path, bit for bit —
that is the point of running it here at all.

The CPU budget (≤ 30 % of a big core at 48 kHz / 256 for a full arrangement, PLAN 9), both quality
levels, 60 s of a full drop arrangement:

```
adb shell taskset f0 /data/local/tmp/phos_render --seconds 60 --sr 48000 --block 256 --bench --quality quest \
    --set "compose.pad_amount=1;compose.acid_amount=1;compose.lead_amount=1;compose.arp_amount=1"
adb shell taskset f0 /data/local/tmp/phos_render --seconds 60 --sr 48000 --block 256 --bench --quality desktop \
    --set "compose.pad_amount=1;compose.acid_amount=1;compose.lead_amount=1;compose.arp_amount=1"
```

`taskset f0` pins the render to the four big cores of the XR2 (mask 0xf0 = CPU 4–7); without it the
scheduler may leave it on an A55 and the number means nothing. If the device's toybox has no
`taskset`, drop it and say so with the number. `--bench` prints the percentage of a core directly.

Then the app:

```
adb install -r bin\quest\PhospheneQuest.apk
adb shell am start -n com.reneweller.phosphene.quest/android.app.NativeActivity
adb logcat -s Phosphene
```

`adb logcat -s Phosphene` shows the system name, whether hand tracking is there, the EGL and GL
versions, the swapchain sizes, the Oboe rate and burst, and every track jump.

## Status

Builds and packages (arm64-v8a, APK signed with a debug key), but **nothing has run on a device
yet**: no headset was attached to the build machine on 16.09.2026. Open, in this order: the vector
test's `path neon` line, the two `--bench` numbers against the 30 % budget, session state flow and
swapchain format, the hand-tracking permission prompt, the Oboe stream start, the
`wavetables: unpacked …` line of the first start (and that the second start does not repeat it),
and how the two pinch gestures feel in practice (the thresholds are 22 mm closed / 38 mm open, a
guess from Noctuary's).
