# Tests and verification

This document describes the scope and limits of Subtitle Desk's automated checks. Results for a particular commit are available in [GitHub Actions](https://github.com/Davoda1/Subtitle-Desk/actions); a test's presence in the source is not evidence that it ran for every build.

## Coverage

| Area | Checks |
| --- | --- |
| Document formats | SRT, WebVTT, ASS, LRC, TXT, CSV, JSON/project, Unicode, malformed input, stable identifiers, atomic writing, and formula-like CSV cells. |
| Timing and editing | Cue boundaries, chronological ordering, overlap/readability hints, split/merge/shift operations, and undo/redo. |
| Native editor | Model/language choices, text edits, review marks, active-cue highlighting independent of selection, keyboard shortcuts, saving, lifecycle behavior, and compact layout. |
| Media decoding | Common containers/codecs, stereo resampling, Unicode paths, source offsets, nonzero container origin, timestamp gaps, and first-audio-stream selection. |
| Decoder failure handling | Corrupt inputs, files without audio, excessive timelines, cancellation, superseded imports, and destruction during decoding. |
| Real recognition | Each bundled model returns valid timed cues for a small speech fixture; cancellation preserves the existing document. |
| Application workflow | Load and decode media, transcribe, seek/highlight, edit, mark reviewed, save/reopen, and export. |
| Packaging and provenance | Runtime archive and model checksums, source-archive boundaries, native package layout, and required metadata/notices. |
| Packaged application startup | Launch the staged executable with its own runtime and capture the native demonstration editor. |

## Run the checks

Follow [BUILDING.md](BUILDING.md) for platform prerequisites and commands. Supplying `SUBTITLEDESK_RUNTIME_ROOT` at CMake configuration enables the runtime-dependent checks with the bundled test speech fixture. Run CTest with `--output-on-failure` to see failures without losing their diagnostics.

Document and editor tests can be built without downloading the models. The editor's real-media integration case skips when its required runtime/media inputs are unavailable. A test-only build without the runtime therefore does not verify the decoder or speech engines. The source includes a deterministic audio fixture generator and separate native decoder and transcription test drivers for focused checks.

## What these checks establish

Format and editor tests check behavior and data preservation. Decoder fixtures check the sample stream and its timing. Speech smoke tests establish that a model can load and return usable cue data; they are not accuracy benchmarks. Synthetic speech and tones are used for repeatable tests rather than user recordings.

GUI tests run with Qt's offscreen platform. They exercise application widgets and playback state without establishing sound output on a physical device. Test actual speakers or headphones on the target desktop when evaluating a release. A successful compilation or package inspection also does not, by itself, establish an interactive desktop launch on another operating system.

Screenshots are captured from the native running application with demonstration content. They illustrate the layout and themes; they are not evidence of recognition accuracy.

## Product limits to keep in mind

- Recognition quality depends on the recording, language, and model. Music, singing, overlapping speech, and reverberation can require substantial correction. Every generated cue starts as unreviewed.
- Whistle's recognized word times are grouped into cues; Whisper returns segment times. All recognition boundaries are estimates. Automatic word-level karaoke alignment and speaker diarization are not provided.
- The first audio stream is used. Review and recognition share a 16 kHz mono PCM track; the application does not display video or select among multiple audio tracks.
- Source timeline offsets and gaps are retained. Timestamp-free input uses continuous sample timing. Tracks beyond the roughly 37-hour PCM/RIFF processing limit must be split.
- Saved projects link to the source media and do not embed it. Recovery protects unsaved project edits; it is not a backup of the original audio.
