# LiBeDAW v0.2 — Ideas & Deferred Features

This file captures features that were scoped out of v0.1 for complexity / time reasons.
They should be picked up after the v0.1 roadmap is fully completed and validated.

---

## Audio / Warping

### 3.1-ext — Additional Warp Modes (Beats & Tones)

Phase 3.1 ships **Complex** (Phase Vocoder via Rubber Band Library R3) as the sole warp mode.
The UI selector is present but only Complex is active. The following modes should be added in v0.2:

#### `Beats` Mode — Granular / Transient-Locked Time-Stretching
- **Algorithm:** Transient detection pass over the source audio. Grains are placed at detected transient boundaries rather than at fixed hop sizes. This preserves rhythmic accuracy (kick/snare attacks stay tight) at the cost of some tonal smearing in the sustain portion.
- **RBL option flags:** `OptionTransientsSmooth` → `OptionTransientsMixed` → `OptionTransientsCrisp` (expose a sub-mode: Smooth / Mixed / Crisp).
- **Best for:** Drum loops, percussion, rhythmic loops.
- **UI:** Add "Transients" sub-selector (Crisp / Mixed / Smooth) when Beats is active.
- **UX hint:** Auto-detect when the clip's peak-to-RMS ratio exceeds a threshold and suggest Beats mode.

#### `Tones` Mode — Pitch-Coherent Granular Stretching
- **Algorithm:** Longer grain windows tuned for stable pitched content. Phase locking between grains to reduce the "phasiness" of slow melodic lines. In RBL terms, enable `OptionPitchHighQuality` + `OptionWindowLong`.
- **Best for:** Monophonic vocals, bass lines, string solos.
- **UI:** No sub-mode needed; single toggle.

#### Implementation Notes
- Both modes are selectable via `ArrangementClip::warpMode` (already in the data model).
- In `AudioClipPlayer::load()`, map `WarpMode` → `RubberBandStretcher::Options` before constructing the stretcher. Changing mode mid-clip requires recreating the stretcher (with a brief pre-roll) — schedule this on the decoder thread, not the render thread.
- Add a "Warp Mode" right-click submenu entry that shows all three modes; gray out Beats/Tones until v0.2.

---

### 3.2-ext — Warp + Comping Integration

In v0.1, `warpEnabled` and `takes.size() > 0` are **mutually exclusive** on a given
`ArrangementClip`.  When takes are present, `CompPlayer` is used and the warp path is
skipped entirely.  Full integration would allow each individual take to have its own
warp markers (e.g. beat-align vocal takes before comping them together).

**Proposed design for v0.2:**
- `Take` gains its own `std::vector<WarpMarker> warpMarkers` and `double clipBpm`.
- Each take's `AudioClipPlayer` is initialised with its own warp markers.
- `CompRegion` crossfade boundaries are computed in stretched-sample space rather than
  raw-file-sample space.
- The right-click menu on a take lane row exposes "Set BPM" and "Enable Warp" per take.

---

## MIDI / Expression

### 4.1 — MPE (MIDI Polyphonic Expression) Integration
*(Deferred from Phase 4 — full details in the v0.1 roadmap)*

- Per-note pitch bend, channel pressure, and timbre via MPE zone manager.
- Refactor `PianoRollEditor` for per-note expression curves.
- Map MPE data to `WavetableSynthProcessor` and `FMSynthProcessor` macro parameters.

### 4.1-ext-A — Dedicated MPE Input Track Selector (Option B)

In v0.1, live MPE expression events are always routed to the **selected/armed track** (option A — simplest, same as standard MIDI arming).

For v0.2, add an explicit **MPE Input Track** dropdown in the Top Bar or Session View header:
- A `juce::ComboBox` listing all audio track names.
- Selection writes `MainComponent::mpeTargetTrack` independently of track selection/arming.
- Allows using the keyboard with one hand (expression) while editing on a different track with the mouse.
- Persist the target track name in the project XML (`<MpeTargetTrackIndex>`).

### 4.1-ext-B — Real-Time MIDI Recording + Manual MPE Expression Entry

In v0.1, `MidiNote::hasMpe` / `pressure` / `pitchBend` / `timbre` fields exist in the data model and display in the Piano Roll but cannot be populated via the UI.

For v0.2:
- **MIDI recording**: add a Record-armed MIDI capture path (analogous to `RecordingThread` for audio) that stamps each captured Note-On with the live MPE snapshot from `MpeZoneManager` into the `MidiNote` struct.
- **Manual entry**: right-click a note in the Piano Roll → "Set Expression…" → a small popover with three sliders (Pressure, PitchBend, Timbre) that write into `MidiNote.pressure`, `.pitchBend`, `.timbre` and set `hasMpe = true`.
- Both paths feed the existing Piano Roll colouring and XML persistence.

---

## Mixer / Routing

### Mid/Side Processing Track Mode
- Allow a track to operate in M/S mode: separate Mid and Side channels through independent effect chains.
- Useful for mastering workflows.

### Track Freeze
- "Freeze" a track: render its instrument + effect chain to a temp WAV, playback from file to save CPU.
- Requires integrating with `AudioClipPlayer` infrastructure from Phase 3.1.

---

## UI / UX

### Clip Colour Picker
- Right-click clip → choose from a swatch palette (currently only programmatic colour assignment).

### Browser Waveform Preview
- Hover over a WAV/FLAC in the browser → plays a 3-second preview with waveform thumbnail.

### Arrangement View Zoom-to-Fit
- Ctrl+Shift+Z → auto-zoom the arrangement timeline to show all placed clips.
