# Changelog

## 1.1.0 — 2026-11-05

### Additional speech models

- Add a Models manager for installing compatible local whisper.cpp GGML `.bin` and Whistle `.cact` audio models alongside the four bundled choices.
- Copy, hash, and validate imports in the background with progress and cancellation; install only after the bundled engine successfully loads the model.
- Preserve the original file, reuse duplicate content, and remove imported models with confirmation while protecting bundled and actively used models, including use from another running application instance.
- Persist imported models in application data and remember their stable content identifiers in settings and projects.
- Detect English-only Whisper models from their headers and apply the appropriate language choices.
- Show missing project models explicitly until they are reinstalled or replaced with an available selection.

### Verification and documentation

- Add regression coverage for malformed files, unsafe storage paths, interrupted imports, duplicate content, catalog reload, removal locks, and UI model selection.
- Add optional pinned-model integration tests for importing an additional English-only Whisper model, transcribing with it, and cancelling a job safely.
- Document compatibility, storage, removal, model licensing, and reproducible model-import tests.
- Preserve the four original bundled model files and their dependency notices. Application licensing remains AGPL-3.0-only; copyright (c) 2026 davoda1.

## 1.0.0 — 2026-11-05

Initial release of Subtitle Desk, a native offline editor for subtitles and synchronized lyrics.

### Transcription and review

- Import local audio and video, using the first audio stream without displaying video.
- Transcribe on the CPU with bundled Cactus Whistle or multilingual Whisper Tiny, Base, and Small models.
- Review a waveform with seeking, cue replay, cue looping, playback-speed control, and active-cue highlighting independent of editing selection.
- Search cue text, mark reviewed cues, and filter remaining review work.

### Editing and files

- Edit cue text and millisecond boundaries, set timing at the playhead, and add, split, merge, delete, sort, or shift cues.
- Undo and redo edits and successful transcription replacements.
- Check timing and readability, including overlap, empty text, long lines, and reading-speed hints.
- Save `.sdesk` projects and recover unsaved work after an interrupted session.
- Import SRT, WebVTT, ASS/SSA, and LRC; export SRT, WebVTT, ASS, LRC, TXT, CSV, and JSON.

### Distribution

- Windows x64 portable ZIP, Linux amd64 portable TAR.GZ, and Debian package.
- System, Light, and Dark themes, with a 680 × 540 minimum window size.
- User and build guides, bundled third-party notices, pinned runtime provenance, and automated Linux/Windows checks.
- Application licensed under the GNU Affero General Public License, version 3 only (`AGPL-3.0-only`), with copyright (c) 2026 davoda1.

Recognition quality varies by recording. Timing estimates require review; lyrics are line-timed, and imported subtitle effects are reduced to plain text. See the [user guide](docs/README.md) for format details and [verification guide](docs/Verification.md) for test scope.
