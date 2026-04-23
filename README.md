# LiBeDAW

> **Li**nux **Be**at **DAW** — A professional-grade, open-source Digital Audio Workstation built for Linux using C++20 and the JUCE 8 framework.

---

## Overview

LiBeDAW is an Ableton Live–inspired Session View DAW engineered from first principles on Linux. It prioritizes **real-time audio safety**, **sample-accurate sequencing**, and a **lock-free architecture** between the UI and audio engine. Every design decision is made to guarantee deterministic, jitter-free playback at professional studio quality.

---

## Features

- 🎛️ **Session View** — Ableton-style clip grid with independent per-track launch control. Track and pattern renaming, color assignment, and mixer strips with live RMS metering.
- 🥁 **Euclidean Rhythm Sequencer** — Bjorklund's algorithm for polyrhythmic pattern generation with visual circle editor.
- 🎹 **MIDI Pattern Editor** — Piano-roll style note editing per clip slot with sample-accurate rendering and `allNotesOff` handling for glitch-free transitions.
- 🎹 **Built-in Instruments**:
  - **FMSynth**: 4-operator FM synthesizer with all 32 DX7 algorithms, per-operator controls, 8-voice polyphony, and multi-mode filter.
  - **WavetableSynth**: 2 independent oscillators morphing across 8 waveforms, 8-voice unison, sub oscillator, noise generator, and multi-mode filter.
  - **KarplusStrong**: Physical modeling synthesizer via fractional delay-line interpolation.
  - **DrumRack**: Multi-pad sampler for drum programming.
  - **Simpler**: Drag-and-drop single-sample audio playback.
  - **Oscillator**: Basic subtractive synthesizer.
- 🎚️ **Modular Audio Effects System**:
  - Full Device Chain with Reverb, Delay, Chorus, Filter, Compressor, Limiter, Phaser, Saturation, and ParametricEQ (with real-time spectrum display).
  - Send/Return routing (post-fader) with dedicated master and return track effect chains.
- 📁 **Asset Browser** — Drag-and-drop file browser with persistent folder bookmarks and dynamically generated drag tiles for instruments and effects.
- 💾 **Project Management** — Full save/load via `juce::ValueTree` serialization (custom `.LBD` project file format).
- 🔌 **Plugin Hosting** — VST3 and LV2 support via JUCE.
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
| Track commands | Lock-free `TrackCommand` FIFO ring buffer |

### Key Components

```
Source/
├── Core/
│   ├── Main.cpp                  # JUCE application entry point
│   ├── MainComponent.{h,cpp}     # Root component, audio engine, transport
│   ├── AppAudioBuffer.h          # Lock-free audio buffer abstraction
│   ├── LockFreeQueue.h           # SPSC ring buffer implementation
│   ├── TrackCommand.h            # Audio-thread command message types
│   ├── GlobalTransport.h         # Sample-accurate BPM clock
│   ├── ProjectManager.h          # ValueTree save/load serialization
│   └── ClipData.h                # Clip state data model
├── Instruments/
│   ├── Instrument.h              # Base InstrumentProcessor interface
│   ├── InstrumentFactory.h       # Dynamic instrument instantiation
│   ├── FMSynthProcessor.cpp      # FM Synthesizer DSP engine
│   ├── WavetableSynthProcessor.cpp # Wavetable Synth DSP engine
│   ├── KarplusStrongProcessor.cpp # Physical modeling DSP engine
│   ├── DrumRackProcessor.cpp     # Drum machine DSP engine
│   ├── SimplerProcessor.cpp      # Sampler DSP engine
│   └── OscProcessor.cpp          # Synthesizer DSP engine
├── Effects/
│   ├── EffectProcessor.h         # Base Effect interface
│   ├── EffectFactory.h           # Modular effect instantiation
│   └── BuiltInEffects.cpp        # All DSP algorithms (Reverb, Comp, etc)
├── Sequencing/
│   ├── EuclideanPattern.h        # Bjorklund Euclidean rhythm generator
│   ├── MidiPattern.h             # MIDI note sequence container
│   ├── Pattern.h                 # Polymorphic pattern base
│   ├── PatternEditor.h           # Piano-roll / step editor UI
│   ├── PianoRollEditor.h         # Dedicated piano roll UI components
│   └── PatternPool.h             # Pre-allocated pattern memory pool
└── UI/
    ├── SessionView.h             # Clip grid UI (Track columns, clip slots)
    ├── BrowserComponent.h        # Asset file browser with bookmarks
    ├── DeviceView.h              # Bottom device rack & effect chains
    └── TopBarComponent.h         # Transport controls & project management
```

---

## Building

### Prerequisites

- **OS**: Linux (Debian Sid / Ubuntu 22.04+ recommended)
- **Compiler**: GCC 12+ or Clang 15+ (C++20 required)
- **CMake**: 3.20+
- **Dependencies**: `pkg-config`, `libasound2-dev`, `libfreetype6-dev`, `libfontconfig1-dev`, `libcurl4-openssl-dev`, `libx11-dev`, `libxrandr-dev`, `libxinerama-dev`, `libxcursor-dev`, `libgl1-mesa-dev`, `libglu1-mesa-dev`, `libwebkit2gtk-4.0-dev`

Install dependencies on Debian/Ubuntu:
```bash
sudo apt install cmake build-essential pkg-config libasound2-dev libfreetype6-dev \
  libfontconfig1-dev libcurl4-openssl-dev libx11-dev libxrandr-dev libxinerama-dev \
  libxcursor-dev libgl1-mesa-dev libglu1-mesa-dev libwebkit2gtk-4.0-dev
```

### Build

```bash
git clone https://github.com/Vietnoirien/DAW.git
cd DAW
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The binary will be at `build/LiBeDAW_artefacts/Release/LiBeDAW`.

### Run

```bash
./build/LiBeDAW_artefacts/Release/LiBeDAW
```

---

## Usage

1. **Add samples** — Click `+ Add Folder` in the Asset Browser (left panel) to index your sample library.
2. **Load an instrument** — Drag an instrument (like WavetableSynth or FMSynth) from the Browser onto a track, or drag a `.wav` file to instantiate a `Simpler` engine.
3. **Add effects** — Switch to the Effects tab in the Browser and drag any effect into the Device View chain at the bottom of the screen.
4. **Program a pattern** — Click a clip slot to open the Pattern Editor. Toggle steps or use the Euclidean circle to generate rhythms.
5. **Organize** — Right-click track headers or clips to assign custom names and colors.
6. **Launch clips** — Press ▶ on any clip slot to queue it for launch on the next bar.
7. **Play** — Hit the global Transport Play button. All queued clips launch sample-accurately.
8. **Save** — `File → Save Project` to serialize the full session as an `.LBD` file.

---

## Roadmap

- [ ] Piano-roll quantization & velocity editing
- [ ] VST3 plugin rack in Device View
- [ ] MIDI controller mapping
- [ ] Audio clip recording
- [ ] Arrangement View

---

## License

MIT License — see [LICENSE](LICENSE) for details.

---

## Acknowledgements

Built with [JUCE 8](https://juce.com/) — the cross-platform C++ audio framework.
