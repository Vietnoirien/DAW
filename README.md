# LiBeDAW

> **Li**nux **Be**at **DAW** — A professional-grade, open-source Digital Audio Workstation built for Linux using C++20 and the JUCE 8 framework.

---

## Overview

LiBeDAW is an Ableton Live–inspired Session View DAW built with C++20 and JUCE 8. It prioritizes **real-time audio safety**, **sample-accurate sequencing**, and a **lock-free architecture** between the UI and audio engine. Every design decision is made to guarantee deterministic, jitter-free playback at professional studio quality.

LiBeDAW is developed and tested on **Linux**. The codebase uses only cross-platform JUCE APIs and the build system has been structured to support **macOS** and **Windows**, but those platforms have not yet been tested. Contributions and build reports are welcome.

---

## Features

- 🎛️ **Session View** — Ableton-style clip grid with independent per-track launch control. Track and pattern renaming, color assignment, and mixer strips with live RMS metering.
- 🎞️ **Arrangement View** — Timeline-based linear sequencing with drag-and-drop clip placement, dynamic horizontal scrolling, warp-marker diamonds, and take-lane expand/collapse.
- 🥁 **Euclidean Rhythm Sequencer** — Bjorklund's algorithm for polyrhythmic pattern generation with visual circle editor.
- 🎹 **MIDI Pattern Editor** — Piano-roll style note editing per clip slot with sample-accurate rendering, `allNotesOff` glitch-free transitions, and **MPE expression visualization** (pressure colour gradient, pitch-bend indicator).
- 🎹 **Built-in Instruments**:
  - **FMSynth**: 4-operator FM synthesizer with all 32 DX7 algorithms, per-operator controls, 8-voice polyphony, multi-mode filter, and **MPE mapping panel** (Pressure → Amplitude / Filter, Timbre Depth, Bend Range ±48 st).
  - **WavetableSynth**: 2 independent oscillators morphing across 8 waveforms, 8-voice unison, sub oscillator, noise generator, multi-mode filter, and **MPE mapping panel** (Pressure → Amplitude / Filter, Timbre → wavetable position, Bend Range ±48 st).
  - **KarplusStrong**: Physical modeling synthesizer via fractional delay-line interpolation.
  - **DrumRack**: Multi-pad sampler for drum programming.
  - **Simpler**: Drag-and-drop single-sample audio playback.
  - **Oscillator**: Basic subtractive synthesizer.
- 🎚️ **Modular Audio Effects System**:
  - Full Device Chain with Reverb, Delay, Chorus, Filter, Compressor, Limiter, Phaser, Saturation, and ParametricEQ (with real-time spectrum display).
  - Send/Return routing (post-fader) with dedicated master and return track effect chains.
  - **Sidechain routing** — select a sidechain source directly in the **Compressor editor** (`Sidechain Source` combo box). Real-time gain-reduction meter. Source index persists in project XML. *(Noise Gate sidechain API is stubbed; DSP not yet wired — v0.2.)*
  - **Parallel compression** — Compressor `Mix %` knob blends dry and compressed signal. `Makeup` gain applied post-compression.
- 🗂️ **Group Tracks (Folder Bus)** — Ableton-style inline folder columns in the Session View. Fold/expand arrow hides child tracks and compacts the scroll width. Audio is summed through the group bus effect chain before reaching master. Right-click a track header to assign to a group or create a new one on the fly.
- 🎙️ **Real-Time Time-Stretching (Warp)** — Audio clips in the Arrangement View can be warp-enabled to follow project tempo using the **Rubber Band Library v3.3.0** (R3 Phase Vocoder engine). Lock-free decoder → input ring → stretcher → output ring pipeline; zero allocations on the render thread. Right-click an audio clip to enable/disable warp, set the clip's original BPM, or add warp markers.
- 🎵 **Non-Destructive Comping** — Multi-take recording with loop-record support. Up to 8 takes per clip. Right-click a clip in Arrangement View → "Show Take Lanes" to reveal a `TakeLaneOverlay` with waveform thumbnails; swipe across lanes to define comp regions. Equal-power crossfades with background zero-crossing search. Takes and comp regions fully persist in project XML.
- 🎮 **MPE (MIDI Polyphonic Expression)** — Full MPE zone management via `MpeZoneManager` (RPN auto-detection + permissive multi-channel fallback). Per-note pitch bend (±48 semitones), channel pressure, and CC74 timbre are routed through the lock-free `TrackCommand` FIFO as `MpeExpression` payloads. WavetableSynth and FMSynth apply expression per voice on the render thread without heap allocation.
- 🔌 **Plugin Delay Compensation (PDC)** — `EffectProcessor::getLatencySamples()` virtual hook; `recalculatePDC()` computes per-track compensation and writes a lock-free `PdcDelayLine` ring-buffer into each track atomically. Any plugin reporting latency is automatically compensated.
- 📁 **Asset Browser** — Drag-and-drop file browser with persistent folder bookmarks and dynamically generated drag tiles for instruments and effects.
- 💾 **Project Management** — Full save/load via `juce::ValueTree` serialization (custom `.LBD` project file format), including warp markers, takes, comp regions, sidechain assignments, group tracks, and MPE config.
- 📤 **Audio Export** — High-quality offline rendering bounce engine supporting WAV, MP3, FLAC, and OGG formats (via FFmpeg integration).
- 🔌 **Plugin Hosting (VST3 / LV2 / AU)** — Drag any VST3 plugin (all platforms), LV2 plugin (Linux), or Audio Unit (macOS) from the **Plugins** browser tab directly onto a track. The plugin editor opens in a floating window toggled by a button in the Device View chain. Platform-specific default search paths are seeded automatically; additional directories are persisted across sessions.
  > **CLAP hosting** — The `clap-juce-extensions` submodule (providing the CLAP SDK headers) is included for future readiness. Native CLAP *host* support requires JUCE 9; until then, use the VST3 version of your plugins (e.g. `Vital.vst3`).
- ⚙️ **Audio Device Settings** — Runtime soundcard/buffer/sample-rate configuration.
- 🕹️ **Global Transport** — Sample-accurate BPM clock driving all sequencers.

---

## Architecture

LiBeDAW is built on strict real-time safety principles:

| Concern | Solution |
|---|---|
| UI ↔ Audio communication | `std::atomic` + Lock-Free SPSC queues (`juce::AbstractFifo`) |
| State management | `juce::ValueTree` (MVC — UI as pure listener) |
| Sequencing | Sample-accurate global transport clock |
| Memory on audio thread | **Zero allocation** — pre-allocated pools only |
| Plugin hot-swapping | Garbage collection queue on UI thread |
| Track commands | Lock-free `TrackCommand` FIFO ring buffer (`PlayPattern`, `FlushNotes`, `MpeExpression`, …) |
| Plugin Delay Compensation | `PdcDelayLine` lock-free ring buffer per track; `recalculatePDC()` on message thread |
| Sidechain routing | Pre-render scratch buffer pointer passed to `CompressorEffect::setSidechainBuffer()` — render thread only, no atomics |
| Audio time-stretching | Rubber Band Library R3 Phase Vocoder; lock-free decoder thread → ring → stretcher → ring → render thread |
| Multi-take comping | `CompPlayer` owns ≤8 `AudioClipPlayer` instances; equal-power crossfade table; `loopWrapCounter` atomic for seamless loop-record |
| MPE expression | `MpeZoneManager` on message thread; `MpeExpression` command through FIFO; per-voice state on render thread — no extra atomics |
| Offline Rendering | Rapid-drain loop over `AppAudioBuffer` bypassing RT hardware limits |

### Key Components

```
Source/
├── Core/
│   ├── Main.cpp                  # JUCE application entry point
│   ├── MainComponent.{h,cpp}     # Root component, audio engine, export bounce
│   ├── AppAudioBuffer.h          # Lock-free audio buffer abstraction
│   ├── LockFreeQueue.h           # SPSC ring buffer implementation
│   ├── TrackCommand.h            # Audio-thread command types (incl. MpeExpression)
│   ├── GlobalTransport.h         # Sample-accurate BPM clock
│   ├── ProjectManager.h          # ValueTree save/load serialization
│   ├── ClipData.h                # Clip, WarpMarker, Take, CompRegion data models
│   ├── AudioClipPlayer.{h,cpp}   # Lock-free warp-enabled audio file player (RBL)
│   ├── CompPlayer.{h,cpp}        # Multi-take comp player with equal-power crossfades
│   └── MpeZoneManager.h          # MPE zone detection & per-note expression routing
├── Instruments/
│   ├── Instrument.h              # Base InstrumentProcessor interface
│   ├── InstrumentFactory.h       # Dynamic instrument instantiation
│   ├── FMSynthProcessor.h        # FM Synthesizer DSP engine (MPE-aware)
│   ├── WavetableSynthProcessor.h # Wavetable Synth DSP engine (MPE-aware)
│   ├── KarplusStrongProcessor.cpp # Physical modeling DSP engine
│   ├── DrumRackProcessor.cpp     # Drum machine DSP engine
│   ├── SimplerProcessor.cpp      # Sampler DSP engine
│   └── OscProcessor.cpp          # Synthesizer DSP engine
├── Effects/
│   ├── EffectProcessor.h         # Base Effect interface (getLatencySamples, sidechain API)
│   ├── EffectFactory.h           # Modular effect instantiation
│   └── BuiltInEffects.cpp        # All DSP algorithms (Reverb, Comp+sidechain, etc)
├── Sequencing/
│   ├── EuclideanPattern.h        # Bjorklund Euclidean rhythm generator
│   ├── MidiPattern.h             # MIDI note sequence container
│   ├── Pattern.h                 # Polymorphic pattern base
│   ├── PatternEditor.h           # Piano-roll / step editor UI
│   ├── PianoRollEditor.h         # Piano roll UI (MPE pressure colour + bend indicator)
│   └── PatternPool.h             # Pre-allocated pattern memory pool
└── UI/
    ├── SessionView.h             # Clip grid UI (TrackColumn, GroupColumn, sidechain menu)
    ├── ArrangementView.h         # Timeline (TakeLaneOverlay, warp-marker UI, comp menu)
    ├── BrowserComponent.h        # Asset file browser with bookmarks
    ├── DeviceView.h              # Bottom device rack & effect chains
    └── TopBarComponent.h         # Transport controls & project management
```

---

## Building

> **JUCE is fetched automatically** via CMake `FetchContent` (JUCE **8.0.12**) — no manual download needed.

---

### 🐧 Linux

#### Prerequisites

| Tool | Minimum version |
|------|-----------------|
| GCC or Clang | GCC 14+ / Clang 18+ (C++20 required) |
| CMake | 3.21+ |
| Git | any recent version |

Install system dependencies (Debian 13 / Ubuntu 24.04):

```bash
sudo apt update
sudo apt install cmake build-essential git pkg-config \
  libasound2-dev libfreetype6-dev libfontconfig1-dev \
  libcurl4-openssl-dev libx11-dev libxrandr-dev libxinerama-dev \
  libxcursor-dev libgl1-mesa-dev libglu1-mesa-dev \
  libwebkit2gtk-4.1-dev ffmpeg
```

> **Debian 12 (Bookworm)**: replace `libwebkit2gtk-4.1-dev` with `libwebkit2gtk-4.0-dev`.

#### Build

```bash
git clone https://github.com/Vietnoirien/LiBeDAW.git
cd DAW
git submodule update --init --recursive
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

#### Run

```bash
# Via convenience symlink (always points to the last-built config)
./build/LiBeDAW_artefacts/LiBeDAW

# Or directly
./build/LiBeDAW_artefacts/Release/LiBeDAW
```

#### Plugin formats

- **VST3** — standard locations (`/usr/lib/vst3`, `~/.vst3`) are scanned automatically
- **LV2** — standard locations (`/usr/lib/lv2`, `~/.lv2`) are scanned automatically

---

### 🍎 macOS *(untested — build reports welcome)*

> **⚠️ Untested platform.** The code changes required for macOS compatibility have been applied (platform-guarded LV2/AU plugin formats, default search paths), but LiBeDAW has not yet been compiled or run on macOS. The instructions below follow JUCE 8's documented requirements and should work, but may need adjustment.

#### Prerequisites

| Tool | Minimum version |
|------|-----------------|
| Xcode | 14+ (includes Apple Clang with C++20) |
| Xcode Command Line Tools | required even if using Ninja |
| CMake | 3.21+ |
| Git | any recent version |
| ffmpeg | for MP3/FLAC/OGG export (via Homebrew) |

```bash
# Install Xcode Command Line Tools (if not already installed)
xcode-select --install

# Install Homebrew dependencies
brew install cmake git ffmpeg
```

#### Build

```bash
git clone https://github.com/Vietnoirien/LiBeDAW.git
cd DAW
git submodule update --init --recursive

# Intel Mac
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Apple Silicon + Intel Universal Binary
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"

cmake --build build -j$(nproc)
```

> **Xcode generator (optional):** replace the first cmake line with
> `cmake -B build -G Xcode` to open the project in Xcode instead.

#### Run

```bash
open build/LiBeDAW_artefacts/Release/LiBeDAW.app
# or
./build/LiBeDAW_artefacts/Release/LiBeDAW.app/Contents/MacOS/LiBeDAW
```

#### Plugin formats

- **VST3** — `/Library/Audio/Plug-Ins/VST3` and `~/Library/Audio/Plug-Ins/VST3` are scanned automatically
- **Audio Units (AU)** — `/Library/Audio/Plug-Ins/Components` and `~/Library/Audio/Plug-Ins/Components` are scanned automatically

> **Code signing:** for local development no signing is required. For distribution, sign with `codesign --deep --sign - LiBeDAW.app` or configure a Developer ID certificate.

---

### 🪟 Windows *(untested — build reports welcome)*

> **⚠️ Untested platform.** The code changes required for Windows compatibility have been applied (platform-guarded LV2 format, symlink guard, default VST3 paths), but LiBeDAW has not yet been compiled or run on Windows. The instructions below follow JUCE 8's documented requirements and should work, but may need adjustment.

#### Prerequisites

| Tool | Minimum version | Notes |
|------|-----------------|-------|
| Visual Studio | 2022 (MSVC v143) | Community edition is free; select **"Desktop development with C++"** |
| CMake | 3.21+ | Bundled with Visual Studio or install from cmake.org |
| Git | any recent version | |
| ffmpeg | any | Add `ffmpeg.exe` to `PATH` for MP3/FLAC/OGG export |

> **MinGW is not supported.** JUCE uses Windows COM/WinRT APIs for audio (WASAPI) and VST3 hosting that require MSVC.

#### Build (Developer Command Prompt or PowerShell)

```powershell
git clone https://github.com/Vietnoirien/LiBeDAW.git
cd DAW
git submodule update --init --recursive

cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

> **Ninja (faster):** if you have the "Ninja" component installed via Visual Studio,
> open a **"x64 Native Tools Command Prompt for VS 2022"** and use:
> ```powershell
> cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
> cmake --build build
> ```

#### Run

```powershell
.\build\LiBeDAW_artefacts\Release\LiBeDAW.exe
```

#### Plugin formats

- **VST3** — `C:\Program Files\Common Files\VST3` and `%APPDATA%\VST3` are scanned automatically
- LV2 and AU are not available on Windows

> **Low-latency audio:** Windows' default audio stack (WASAPI) works out of the box. For sub-10 ms latency, install an **ASIO driver** (e.g. [ASIO4ALL](https://www.asio4all.org/) for generic hardware or your audio interface's native driver) and select it in *Settings → Audio Device*.

---

### Binary locations (all platforms)

| Platform | Release binary |
|----------|----------------|
| Linux | `build/LiBeDAW_artefacts/Release/LiBeDAW` |
| Linux (symlink) | `build/LiBeDAW_artefacts/LiBeDAW` |
| macOS | `build/LiBeDAW_artefacts/Release/LiBeDAW.app` |
| Windows | `build\LiBeDAW_artefacts\Release\LiBeDAW.exe` |

---

## Usage

1. **Add samples** — Click `+ Add Folder` in the Asset Browser (left panel) to index your sample library.
2. **Load an instrument** — Drag an instrument (like WavetableSynth or FMSynth) from the Browser onto a track, or drag a `.wav` file to instantiate a `Simpler` engine.
3. **Add effects** — Switch to the Effects tab in the Browser and drag any effect into the Device View chain at the bottom of the screen.
4. **Program a pattern** — Click a clip slot to open the Pattern Editor. Toggle steps or use the Euclidean circle to generate rhythms.
5. **Organize** — Right-click track headers or clips to assign custom names and colors.
6. **Group tracks** — Right-click any track header → *Assign to Group ▸* to route it into an inline folder bus. Fold/expand the group column with the ▶/▼ arrow.
7. **Sidechain compression** — Right-click a track header → *Set Sidechain Source ▸*, select any other track. Open the Compressor editor in the Device View to adjust Threshold, Ratio, Makeup, Mix, and monitor the gain-reduction meter.
8. **Timeline Sequencing** — Switch to the **Arrangement View** to lay out clips along a continuous horizontal timeline.
9. **Warp audio to tempo** — Right-click an audio clip in Arrangement View → *Enable Warp* → *Set Clip BPM*. The Rubber Band Library will stretch the clip to follow the project tempo in real time.
10. **Non-destructive comping** — Record multiple takes in loop mode. Right-click a clip → *Show Take Lanes (N takes)* to open the TakeLaneOverlay. Swipe across lanes to define your comp; crossfades are computed automatically.
11. **MPE expression** — Connect an MPE-capable controller. Expression is automatically routed per-note to the armed track's WavetableSynth or FMSynth. Use the **MPE MAPPING** panel on the instrument to route Pressure to Amplitude or Filter Cutoff, adjust Timbre Depth, and set the Bend Range.
12. **Launch clips** — Press ▶ on any clip slot to queue it for launch on the next bar.
13. **Play** — Hit the global Transport Play button. All queued clips launch sample-accurately.
14. **Export Audio** — Click `Export Audio` in the top bar to render your composition to WAV, MP3, FLAC, or OGG format.
15. **Load a VST3 plugin** — Switch to the **Plugins** tab in the browser, click **Add Folder…** to point it at your VST3 directory (e.g. `/usr/lib/vst3` or the Vital installer folder), then drag a plugin tile onto a track. Click **Show / Hide Editor** in the Device View to open its native GUI.
16. **Save** — `File → Save Project` to serialize the full session as an `.LBD` file.

---

## Roadmap

- [x] Arrangement View
- [x] VST3 plugin rack in Device View (Plugins browser tab, per-track, show/hide editor)
- [x] Piano Roll — keyboard stays fixed when scrolling horizontally; vertical scroll syncs
- [x] Euclidean Sequencer — configurable pattern duration (1 / 2 / 4 bars)
- [x] Plugin Delay Compensation (PDC) — lock-free `PdcDelayLine` per track
- [x] Sidechain routing — Compressor editor with source selector, makeup gain, parallel mix, GR meter
- [x] Group tracks (inline folder bus, Ableton-style fold/expand)
- [x] Real-time audio warping — Rubber Band Library R3 Phase Vocoder, warp-marker UI
- [x] Non-destructive comping — `TakeLaneOverlay`, 8-take cap, loop-record, crossfade
- [x] MPE engine integration — `MpeZoneManager`, per-note expression in WavetableSynth & FMSynth, Piano Roll visualization
- [ ] CLAP plugin hosting (pending JUCE 9 native support)
- [ ] Piano-roll quantization & velocity editing
- [ ] MIDI controller mapping (CC mapping panel)
- [ ] MIDI recording with per-note MPE snapshot capture
- [ ] Audio clip recording (non-MPE tracks)

---

## License

MIT License — see [LICENSE](LICENSE) for details.

---

## Acknowledgements

Built with [JUCE 8.0.12](https://juce.com/) — the cross-platform C++ audio framework.

CLAP SDK headers provided by [clap-juce-extensions](https://github.com/free-audio/clap-juce-extensions) (free-audio / clap team).
