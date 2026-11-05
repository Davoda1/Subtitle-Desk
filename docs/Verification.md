# Tests and verification

This document describes the scope and limits of Subtitle Desk's automated checks. The repository runs native Linux/Windows tests and CodeQL for changes to master. Check [GitHub Actions](https://github.com/Davoda1/Subtitle-Desk/actions) for the results associated with the matching release commit; a test's presence in the source is not evidence that it ran for every build.

## 1.1.0 pre-release verification — 2026-11-05

The local pre-release record covers Linux execution and Windows cross-compilation. Native Windows execution results are provided by the repository workflow for the matching release commit. The complete native Linux suite passed after the final model-use locking and cancellation fixes: **15 of 15 CTest groups passed, with no failures**. The configured real-model cases ran; none of the catalog or editor cases were skipped.

| Verification | Result |
| --- | --- |
| Linux application and test build | Passed with GCC 13.3 and Qt 6.8.3. |
| Document formats | 262/262 checks passed. |
| Native audio decoding | 82/82 checks passed. |
| Package-boundary Python tests | 9/9 tests passed. |
| Model catalog | 32 reported QtTest passes; zero failures and zero skips. |
| Editor workflow | 20 reported QtTest passes; zero failures and zero skips. |
| Bundled recognition | Whistle, Tiny, Base, and Small returned valid timed cues; cancellation passed. |
| Imported recognition | Additional Tiny.en Q5_1 and an imported Whistle file both installed and returned valid timed cues. Imported Whisper cancellation and lock cleanup passed. |
| Linux staged package | Started with its own runtime and rendered the native editor. Both imported model families also passed import/transcription/removal using the staged runtime. |
| Linux and Windows staged contents | Native executable layout, all four pinned model hashes, decoder/engine files, and application/dependency notices passed package validation. |
| Windows x64 build | Application and test executables cross-compiled with MinGW-w64 GCC 13.0 and Qt 6.8.3. Application version resources are 1.1.0/1.1.0.0 and copyright is davoda1. Required non-system application DLL imports and symbols resolved against the supplied runtime. |
| Native Windows execution | See the Native tests (Windows) job for the matching release commit in [Actions](https://github.com/Davoda1/Subtitle-Desk/actions). The local cross-build record above does not substitute for that job. |
| Physical audio output | Not verified by the offscreen test environment; check speakers or headphones on the target desktop. |

QtTest totals include its setup and cleanup entries. Recognition tests use the redistributable synthetic speech fixture. The extra English-only model is the pinned input in `tests/fetch_test_model.py`; it is used for verification and is not distributed as a fifth bundled model.

Native screenshots were inspected in both themes at the 680 × 540 editor minimum and the 560 × 440 Models dialog minimum, including import progress. The imported-model screenshot shows an actual successful installation. The four bundled model files and canonical AGPL license remain unchanged from 1.0.0.

## Coverage

| Area | Checks |
| --- | --- |
| Document formats | SRT, WebVTT, ASS, LRC, TXT, CSV, JSON/project, Unicode, malformed input, stable identifiers, atomic writing, and formula-like CSV cells. |
| Timing and editing | Cue boundaries, chronological ordering, overlap/readability hints, split/merge/shift operations, and undo/redo. |
| Native editor | Model/language choices, text edits, review marks, active-cue highlighting independent of selection, keyboard shortcuts, saving, lifecycle behavior, and compact layout. |
| Media decoding | Common containers/codecs, stereo resampling, Unicode paths, source offsets, nonzero container origin, timestamp gaps, and first-audio-stream selection. |
| Decoder failure handling | Corrupt inputs, files without audio, excessive timelines, cancellation, superseded imports, and destruction during decoding. |
| Real recognition | Each bundled model returns valid timed cues for a small speech fixture; cancellation preserves the existing document. The optional Tiny.en fixture also checks an imported model and cancellation. |
| Model installation | Unsupported/truncated files, size and header checks, path confinement, symlinks, atomic failure and cancellation, duplicates, persistence, English-only detection, and removal protection while a model is active, including separate application instances, and cancellation/destruction during the engine probe. |
| Model UI | Bundled choices, imported choices and language restrictions, persistent model selection, and unavailable project models. |
| Application workflow | Load and decode media, transcribe, seek/highlight, edit, mark reviewed, save/reopen, and export. |
| Packaging and provenance | Runtime archive and model checksums, source-archive boundaries, native package layout, and required metadata/notices. |
| Packaged application startup | Launch the staged executable with its own runtime and capture the native demonstration editor. |

## Run the checks

Follow [BUILDING.md](BUILDING.md) for platform prerequisites and commands. Supplying `SUBTITLEDESK_RUNTIME_ROOT` at CMake configuration enables the runtime-dependent checks with the bundled test speech fixture. Also supply `SUBTITLEDESK_TEST_IMPORT_MODEL` with the pinned English-only Tiny.en file to enable successful custom-import integration and imported-model transcription cases. Run CTest with `--output-on-failure` to see failures without losing their diagnostics.

Document and editor tests can be built without downloading the models. The editor's real-media integration case skips when its required runtime/media inputs are unavailable, and real custom-import cases skip without their additional model fixture. A test-only build without the runtime therefore does not verify the decoder or speech engines. The source includes a deterministic audio fixture generator and separate native decoder and transcription test drivers for focused checks.

## What these checks establish

Format and editor tests check behavior and data preservation. Decoder fixtures check the sample stream and its timing. Speech smoke tests establish that a model can load and return usable cue data; they are not accuracy benchmarks. Synthetic speech and tones are used for repeatable tests rather than user recordings.

GUI tests run with Qt's offscreen platform. They exercise application widgets and playback state without establishing sound output on a physical device. Test actual speakers or headphones on the target desktop when evaluating a release. A successful compilation or package inspection also does not, by itself, establish an interactive desktop launch on another operating system.

Screenshots are captured from the native running application with demonstration content. They illustrate the layout and themes; they are not evidence of recognition accuracy.

## Product limits to keep in mind

- Recognition quality depends on the recording, language, and model. Music, singing, overlapping speech, and reverberation can require substantial correction. Every generated cue starts as unreviewed.
- Whistle's recognized word times are grouped into cues; Whisper returns segment times. All recognition boundaries are estimates. Automatic word-level karaoke alignment and speaker diarization are not provided.
- The first audio stream is used. Review and recognition share a 16 kHz mono PCM track; the application does not display video or select among multiple audio tracks.
- Source timeline offsets and gaps are retained. Timestamp-free input uses continuous sample timing. Tracks beyond the roughly 37-hour PCM/RIFF processing limit must be split.
- Saved projects link to the source media and do not embed it. Imported models are referenced by content ID and are not embedded either. Recovery protects unsaved project edits; it is not a backup of the original audio or model files.
- A successful import establishes that the bundled recognizer can load a model, not its transcription quality or compatibility with every recording. Models must be in the supported raw formats, at most 8 GiB, and load within the five-minute validation window.
