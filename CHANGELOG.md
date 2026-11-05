# Changelog

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

Recognition quality varies by recording. Timing estimates require review; lyrics are line-timed, and imported subtitle effects are reduced to plain text. See the [user guide](https://github.com/Davoda1/Subtitle-Desk/blob/master/docs/README.md) for format details and [verification guide](https://github.com/Davoda1/Subtitle-Desk/blob/master/docs/Verification.md) for test scope.
