# Subtitle Desk 1.1.0

Subtitle Desk turns local audio and video into editable subtitles and synchronized lyric lines. Recognize speech, then listen while correcting words and timing. The Windows and Linux packages include four CPU models, and the Models manager can install additional compatible local files. After installation, the application needs no account, API key, network connection, or model download.

## Start the application

**Windows 10/11 x64:** extract the complete portable ZIP into a writable folder and open `SubtitleDesk.exe` at the archive root. Keep the adjacent runtime, plugin, model, and license files together.

**GNU/Linux x86-64:** extract the complete TAR.GZ and run the `subtitledesk` binary at its root. A graphical desktop and working system audio are required. The Debian package installs the actual executable at `/usr/bin/subtitledesk`; supporting files live under `/usr/lib/subtitledesk`. No launcher script is needed.

The minimum window is 680 × 540. Compact layouts hide some secondary controls; menu commands and shortcuts remain available. Only individual editing panels scroll.

## 1. Open audio or video

Click **Open media…**, use **File → Open media**, or drop one local file onto the window. Common inputs include MP4, MKV, WebM, MOV, AVI, MP3, WAV, FLAC, M4A, AAC, OGG, and Opus. Actual decoding depends on the codec inside the file; damaged, encrypted, or unsupported streams cannot be transcribed.

Subtitle Desk reads the first audio stream. It does not display video or offer an audio-track selector. Review playback and recognition use the same temporary 16 kHz mono PCM track, giving them one shared timeline. Your original media file is never rewritten. Temporary audio requires about 115 MB of disk space per hour and is removed when its session ends normally.

Audio start offsets relative to the container's playback origin, and gaps, are retained as silence so exported subtitles stay aligned with the original video. If a source has no timestamps, continuous sample timing is used.

If you already have subtitles, open the media and choose **File → Import subtitles / lyrics…**. SRT, WebVTT, ASS/SSA, and LRC imports are supported. Import replaces the cue list after confirmation and can be undone. You can also open timed text first and use **File → Link / replace audio…** afterward.

## 2. Choose a model and transcribe

| Model | Practical starting point | Language support |
| --- | --- | --- |
| Cactus Whistle | Compact recognizer; recognized word times are grouped into subtitle cues | English, German, French, Spanish, Italian, Dutch, Polish |
| Whisper Tiny multilingual Q5_1 | Fast initial draft | Multilingual |
| Whisper Base multilingual Q5_1 | Balanced default | Multilingual |
| Whisper Small multilingual Q5_1 | Larger, generally slower alternative to compare on difficult material | Multilingual |

Choose the language or leave **Auto language** selected. Use Whisper for languages outside Whistle's supported set. Click **Transcribe**; the status area shows progress and **Cancel** stops the job. Existing cues remain intact if recognition fails, is cancelled, or returns no speech. A successful replacement can be undone.

Singing, music, reverberation, overlapping voices, and unfamiliar names can produce incorrect words. Review the result before sharing it. Whistle supplies recognized word timing; Whisper supplies segment timing. All recognition timings are estimates. Editing and exports use complete cues; automatic word-by-word karaoke alignment and speaker diarization are not provided.

## Install and manage additional models

Click **Models…** next to the main model selector, or choose **View → Models…**. The manager lists bundled and imported models with their recognizer family and file size. Choose **Import model…**, select a file, and optionally set its display name.

The supported inputs are raw **whisper.cpp GGML `.bin` speech models** and compatible **Whistle `.cact` audio models**. A compatible Whisper model can be quantized, multilingual, or English-only. A `.cact` file must contain the Whistle audio manifest; a general Needle text model is not compatible. Files must work with the bundled engine versions. Renaming a PyTorch `.pt`, Safetensors, general-purpose GGUF/LLM, ZIP, or executable file does not make it a supported model.

The app copies the file into its own model storage, validates the structure, calculates a SHA-256 content identifier, and runs the bundled recognizer against short silence to check that it loads. Validation does not assess recognition accuracy. **Files must be between 1 MB and 8 GiB**, and engine validation stops after **five minutes**. Keep enough free disk space for a complete copy and enough memory for the model. Larger models can be much slower on a CPU.

Progress appears in the manager. **Cancel** stops an in-progress import and removes its unfinished copy. An error leaves the existing catalog intact. Your original file is never modified or deleted. A model becomes selectable only after installation succeeds; importing the same content again selects the existing installation. The application does not download models or convert other model formats.

Select an imported model in the main model selector to use it for transcription. English-only Whisper files are identified from their headers and offer only Auto/English; multilingual Whisper uses the normal language list, and Whistle offers its seven supported languages. Any recognition result should still be checked against the audio.

To uninstall an imported model, select it in the manager and choose **Remove…**, then confirm. This removes the installed copy and its metadata, not the original file. Bundled models cannot be removed here. A model used by an active transcription job cannot be removed until that job ends or is cancelled. This protection also applies when another copy of Subtitle Desk is open. An imported model can be used for recognition by one application instance at a time; if another instance is using it, wait for that job to finish.

| Storage mode | Installed models |
| --- | --- |
| Windows | `%AppData%\SubtitleDesk\models` |
| Linux | `~/.config/SubtitleDesk/models`, respecting `XDG_CONFIG_HOME` |
| Portable, with `portable.flag` | `data/models` beside the executable |

Projects store the selected model's content-based identifier, not its weights or an absolute installation path. When moving a project to another computer, install the same model there or choose a different available model. If the saved model is unavailable, the app shows that state and disables transcription until a model is available; existing subtitle text and timing remain editable. Reinstalling identical content restores the same identifier.

Only install models from sources you trust. Imported model weights retain their own licenses; the application's AGPL license does not replace them. They are not embedded in projects, subtitle exports, or the application's distributed packages.

## 3. Listen and review

Click the waveform to seek. The **Full track**, **30 seconds**, and **10 seconds** views help with navigation. Playback speed ranges from 0.5× to 2×; the adjacent buttons move three seconds backward or forward.

Blue rows show cues active at the playhead. Highlighting is independent of editing selection, so listening does not move your text cursor or replace your editing text. **Follow audio** scrolls the active cue into view when you are not typing.

Double-click a row or choose **Replay cue** to listen from just before its start. Enable **Loop cue** to repeat the selected interval with a short lead-in. Use the arrow buttons to visit the previous or next visible cue.

Click **Mark reviewed** after checking a cue. Editing clears the mark. **To review** hides reviewed rows; search filters cue text. Projects preserve review marks.

## 4. Correct text and timing

Select a cue and edit its words on the right. Press Enter for a subtitle line break. Start and end fields accept `HH:MM:SS.mmm` or `MM:SS.mmm`. **Set at playhead** copies the current playback position into the corresponding boundary. The end must remain later than the start.

**Add** creates a blank cue at the playhead. **Split** divides text at the editor cursor and timing at the playhead when it is inside the cue; otherwise it uses the cue midpoint. Place the text cursor between words before splitting. **Merge** combines consecutive selected cues, preserves their line breaks, and covers their combined time range. If only one cue is selected, Merge includes the next cue. Ctrl-click or Shift-click selects multiple rows.

**Edit → Shift cue times…** applies a millisecond offset to selected cues, or all cues when none are selected. Negative offsets move them earlier. The same offset is applied throughout the selection and is limited to prevent negative starts. Use **Sort cues by start time** after substantial rearrangement.

**Edit → Check timing and readability…** reports overlaps, empty text, cues past the audio end, lines longer than 42 characters, more than two lines, and reading speeds above 20 characters per second. These are review hints, not rigid subtitle standards. Invalid intervals block export; overlaps and readability warnings do not. Undo and redo cover text edits, timing changes, splits, merges, deletions, and transcription replacement.

## 5. Save a project and export

Save a `.sdesk` project before ending a review session. It preserves cue text, exact millisecond boundaries, stable IDs, review marks, the selected recognition model and language, and the media link. Audio is not embedded. Keep the original media available; use **Link / replace audio…** if you move or rename it. Relative links let a project and its media move together.

| Export | What it preserves and where to use it |
| --- | --- |
| SRT | Start/end times to milliseconds; common video-player subtitles |
| WebVTT | Start/end times to milliseconds; web video and compatible players |
| ASS | Start/end times to centiseconds and a neutral default subtitle style |
| LRC | Line start times to centiseconds; music players supporting synchronized lyrics |
| TXT | Transcript words and line breaks, without timing |
| CSV | Millisecond timing columns, text, IDs, and review marks for spreadsheets |
| JSON | The full editable project data, equivalent to `.sdesk` |

Exports use UTF-8 and chronological cue order. ASS rounds starts down and ends up to its 10 ms precision. LRC has no explicit end times: importing it infers each end from the next distinct start, with a three-second final cue unless a length tag supplies an end. Duplicate LRC timestamps retain all lyric lines. Use SRT, VTT, or a project when both exact boundaries matter.

CSV prefixes formula-like cell values with an apostrophe so spreadsheet software treats them as text. This does not change the text in your project or other exports. Imported subtitle styling is converted to plain editable text; this is not an ASS effects or typesetting editor. UTF-8 and BOM-marked UTF-16 subtitle imports are supported.

## Shortcuts

| Action | Shortcut |
| --- | --- |
| Open media / open project | Ctrl+O / Ctrl+Shift+O |
| Import timed text / save project / export | Ctrl+I / Ctrl+S / Ctrl+E |
| Play or pause / replay cue | Ctrl+Space / Ctrl+P |
| Previous or next cue | Alt+Up / Alt+Down |
| Set start or end at playhead | Ctrl+[ / Ctrl+] |
| Add / split / merge cue | Ctrl+Enter / Ctrl+K / Ctrl+M |
| Delete selected cues / toggle reviewed | Ctrl+Delete / Ctrl+R |
| Undo / redo | Ctrl+Z / Ctrl+Y or Ctrl+Shift+Z |
| Shift timing / check timing | Ctrl+Shift+T / Ctrl+Shift+C |
| Settings / quick guide | Ctrl+, / F1 |

## Settings, recovery, and privacy

**View → Settings…** controls System, Light, or Dark appearance and recognition CPU threads. Window geometry, panel sizes, model, language, and volume are remembered under `%AppData%\SubtitleDesk` on Windows or `~/.config/SubtitleDesk` on Linux (respecting `XDG_CONFIG_HOME`). To keep these files beside a portable application, create an empty `portable.flag` beside the executable before launch; settings, recovery, and imported models then use its writable `data` subfolder.

Unsaved edits are written to a recovery project approximately every 20 seconds. After an interrupted session, the next launch offers to restore it. Save a project explicitly before moving between computers; recovery does not copy your original audio. The application has no telemetry, cloud transcription, or credential storage. Projects and exports contain your text and can contain media paths; review those files before sharing them.

## Building and licenses

The source uses C++17, CMake 3.22 or later, and Qt 6.8 or later with Core, Gui, Widgets, Multimedia, and Test for GUI tests. Use Qt 6.8.3 when packaging with the pinned runtime. Configure with your Qt kit, build, and run the registered tests:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/your/Qt/kit
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Source builds also need the native engines, models, and multimedia components. See [BUILDING.md](BUILDING.md) for the pinned runtime fetch, native Linux/Windows commands, and real-engine checks. Configuring with `SUBTITLEDESK_RUNTIME_ROOT` enables runtime-dependent tests; the basic commands above do not exercise the speech engines by themselves. The source archive omits model weights and the Qt SDK.

Subtitle Desk is licensed under the GNU Affero General Public License, version 3 only (`AGPL-3.0-only`). Copyright (c) 2026 davoda1. The source includes `LICENSE` and `NOTICE`; runtime packages include `LICENSE.txt` and `NOTICE.txt`.

Qt, recognition engines, models, and multimedia libraries retain their respective copyright notices and licenses. Keep the included `licenses` directory, application license, and copyright notices with redistributed binaries. The corresponding 1.1.0 application source is supplied in `SubtitleDesk-1.1.0-Source.zip` and `SubtitleDesk-1.1.0-Source.tar.gz` alongside the binary packages. The [Subtitle Desk repository](https://github.com/Davoda1/Subtitle-Desk) is the project home.
