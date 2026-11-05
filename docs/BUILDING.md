# Building Subtitle Desk 1.0.0

Run these commands from the extracted source directory containing `CMakeLists.txt`. The source archive contains the application, tests, build scripts, license notices, and the FFmpeg public headers it uses. It does **not** include model weights, recognition executables, a Qt SDK, or a compiler.

The application is native C++17. Python is used only by the explicit build and packaging scripts; the finished application does not require Python or download models.

## Toolchain and pinned runtime

Install CMake 3.22 or later, Ninja, Python 3.12 or later, and a C++17 compiler. Install the **Qt 6.8.3** SDK for your platform with Qt Base and Qt Multimedia. Qt Test, supplied with Qt Base, is needed when tests are enabled. Use the 64-bit GCC kit on Linux, or the 64-bit MinGW kit and its matching MinGW compiler on Windows. Linux staging also requires `patchelf`. For the complete test suite, install the `ffmpeg` and `ffprobe` command-line tools with AAC, Opus, MP3, Vorbis, and H.264 encoding support before configuring CMake.

Use Qt 6.8.3 when producing packages with the supplied runtime. The CMake minimum of Qt 6.8 does not mean a binary built with a newer Qt SDK can safely run with the pinned 6.8.3 libraries. Build on a Linux system compatible with your intended minimum glibc version; copying Qt libraries does not remove a newer compiler's system-library requirements.

`vendor/runtime-lock.json` records the exact distribution URLs, SHA-256 values, model sizes and hashes, Qt/FFmpeg versions, and recognizer identities. The runtime donor is the public Speech Desk 1.1.1 distribution. Its application executable and manuals are not included in Subtitle Desk staging.

| Platform | Pinned archive SHA-256 |
| --- | --- |
| Linux amd64 | `5c66c4d174f491d6dadf16b5a89a892619d7bcf9dd3bbc001687cbd45a71edc4` |
| Windows x64 | `845223eb28ef22b10b01a165b6ec5cffb5e824d363bbf226e876208257a695ec` |

The fetch helper downloads the selected archive, verifies the complete archive **before extraction**, and extracts it into `DESTINATION/linux` or `DESTINATION/windows`. Both archives have their application files, `resources`, `plugins`, and `licenses` directly at the archive root. There is no extra versioned directory to append to `--runtime-reference`.

The extraction directory must not already exist. An already downloaded archive is reused and checked again. If a download was interrupted or its checksum fails, remove that incomplete cached archive before retrying; do not disable the checksum check. For an offline build, put the correctly named, verified archive from the lock file in the destination directory before running the helper.

## Native Linux build

Set the Qt path to your installed GCC kit. On Debian-family build systems, the usual prerequisites include a compiler toolchain, CMake, Ninja, `patchelf`, and the desktop/OpenGL development dependencies required by your Qt SDK.

```sh
SUBTITLE_QT_KIT="/path/to/Qt/6.8.3/gcc_64"
python3 packaging/fetch_runtime.py linux build-assets
cmake -S . -B build/linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$SUBTITLE_QT_KIT" \
  -DSUBTITLEDESK_RUNTIME_ROOT="$PWD/build-assets/linux" \
  -DSUBTITLEDESK_BUILD_TESTS=ON
cmake --build build/linux --parallel

export SUBTITLEDESK_RESOURCES="$PWD/build-assets/linux/resources"
export QT_PLUGIN_PATH="$SUBTITLE_QT_KIT/plugins"
export LD_LIBRARY_PATH="$PWD/build-assets/linux/lib:$SUBTITLE_QT_KIT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
ctest --test-dir build/linux --output-on-failure

python3 packaging/package.py --platform linux \
  --binary build/linux/subtitledesk \
  --runtime-reference build-assets/linux \
  --output dist/linux
python3 packaging/validate_package.py --platform linux --path dist/linux
./dist/linux/subtitledesk
```

`dist/linux` must be absent or empty before staging. The executable is a real ELF binary at the folder root. The staging script copies the runtime, verifies all four model files again, includes notices and manuals, and sets relative ELF library lookup paths. Keep `lib`, `plugins`, `resources`, and `licenses` beside the executable.

To archive this layout without a wrapper directory:

```sh
tar -C dist/linux -czf SubtitleDesk-1.0.0-Linux-amd64.tar.gz .
```

The Debian layout uses a real `/usr/bin/subtitledesk` executable and `/usr/lib/subtitledesk/` for its libraries, plugins, and resources. With `dpkg-deb` installed, package the staged portable tree using:

```sh
python3 packaging/make_deb.py --portable dist/linux \
  --output dist/SubtitleDesk-1.0.0-Linux-amd64.deb
```

These commands create packages; they do not install files into the system.

## Native Windows build with MinGW

Use PowerShell, a matching Qt 6.8.3 MinGW 64-bit installation, and its MinGW toolchain. Adjust the two installation paths below. Do not mix MSVC Qt libraries with a MinGW build. CMake, Ninja, and the Python launcher must be available in `PATH`.

```powershell
$SubtitleQtKit = 'C:\Qt\6.8.3\mingw_64'
$SubtitleMingwBin = 'C:\Qt\Tools\mingw1310_64\bin'
$env:PATH = "$SubtitleMingwBin;$SubtitleQtKit\bin;$env:PATH"

py -3 packaging/fetch_runtime.py windows build-assets
$SubtitleRuntime = (Resolve-Path 'build-assets/windows').Path
cmake -S . -B build/windows -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH="$SubtitleQtKit" `
  -DCMAKE_CXX_COMPILER="$SubtitleMingwBin/g++.exe" `
  -DCMAKE_RC_COMPILER="$SubtitleMingwBin/windres.exe" `
  -DSUBTITLEDESK_RUNTIME_ROOT="$SubtitleRuntime" `
  -DSUBTITLEDESK_BUILD_TESTS=ON
cmake --build build/windows --parallel

$env:SUBTITLEDESK_RESOURCES = (Resolve-Path 'build-assets/windows/resources').Path
$env:QT_PLUGIN_PATH = "$SubtitleQtKit\plugins"
ctest --test-dir build/windows --output-on-failure

py -3 packaging/package.py --platform windows `
  --binary build/windows/SubtitleDesk.exe `
  --runtime-reference build-assets/windows `
  --output dist/windows `
  --compiler "$SubtitleMingwBin/g++.exe"
py -3 packaging/validate_package.py --platform windows --path dist/windows
& '.\dist\windows\SubtitleDesk.exe'
```

The compiler option locates and copies that compiler's GCC, C++, and threading runtime DLLs. If it reports a missing DLL, verify that the selected compiler is the complete matching Qt/MinGW kit. Use a fresh staging directory after correcting a failed staging run. The result has a real `SubtitleDesk.exe` at its root with the required DLLs and resource folders beside it.

```powershell
Compress-Archive -Path '.\dist\windows\*' `
  -DestinationPath '.\SubtitleDesk-1.0.0-Windows-x64.zip' `
  -CompressionLevel Optimal
```

See the [Actions results](https://github.com/Davoda1/Subtitle-Desk/actions) for the status of native Linux and Windows checks on a particular commit. Build success and offscreen testing do not replace an interactive desktop and audio-output check.

## Running the tests

The native Linux and Windows workflow builds the application and runs its CTest suite. Configure with `SUBTITLEDESK_RUNTIME_ROOT` as shown above to enable runtime-dependent checks. The suite then covers document formats, the editor's complete media workflow, synthetic media decoding, recognition with all four models, and cancellation. A short synthetic English speech fixture is included under `tests/fixtures`; it contains no user recording.

CTest also runs the Python package-boundary tests and verifies the pinned runtime. The workflow stages and validates a portable package, then launches it with `packaging/smoke_package.py` using its own runtime and captures the demonstration editor. This distinguishes a build that works with the development SDK from a package that can start with its bundled dependencies.

The GUI test uses Qt's offscreen platform. Without a configured runtime, the document/editor tests remain available, but the real-media integration case skips unless `SUBTITLEDESK_TEST_MEDIA` points to speech audio and `SUBTITLEDESK_RESOURCES` points to the extracted resources directory. Do not interpret a runtime-free test run as validation of the decoder or speech engines.

For decoder checks, install the `ffmpeg` and `ffprobe` command-line tools with AAC, Opus, MP3, Vorbis, and H.264 encoding support. Generate the small deterministic fixtures and run the native decoder test:

```sh
python3 tests/create_audio_fixtures.py build/audio-fixtures
build/linux/test_audio build/audio-fixtures
```

On Windows use `py -3` and `build/windows/test_audio.exe`. The generator accepts `--ffmpeg` and `--ffprobe` if those tools are not in `PATH`. Its synthetic tones check decoding, resampling, original container offsets/gaps, track selection, failure handling, and cancellation; they are not speech recordings for recognizer tests.

`test_transcription` requires a 16 kHz, mono, signed 16-bit PCM WAV. CTest runs it for each model when the runtime is configured. To check a different recording, run the native test driver directly:

```sh
build/linux/test_transcription "$SUBTITLEDESK_RESOURCES" /path/to/speech-16k-mono.wav whistle
build/linux/test_transcription "$SUBTITLEDESK_RESOURCES" /path/to/speech-16k-mono.wav tiny
build/linux/test_transcription "$SUBTITLEDESK_RESOURCES" /path/to/speech-16k-mono.wav base
build/linux/test_transcription "$SUBTITLEDESK_RESOURCES" /path/to/speech-16k-mono.wav small
```

On Windows use `build/windows/test_transcription.exe` with the same positional arguments. Adding `--cancel` checks cancellation without emitting a partial transcript. A normal non-silent speech test requires at least one valid returned cue. Also listen through actual playback on the target desktop; an offscreen test does not establish audio-device behavior or recognition accuracy on your material.

## Dependency sources and redistribution

The application dynamically links Qt. Its media decoder dynamically loads the bundled FFmpeg 7.1 shared libraries: `avformat`/`avcodec` ABI 61, `avutil` ABI 59, and `swresample` ABI 5. `vendor/ffmpeg/include` contains the matching public headers, not a statically linked FFmpeg implementation. All 36 header hashes are recorded in `vendor/ffmpeg/provenance.json`; `avconfig.h` describes the supported little-endian x86-64 targets.

Subtitle Desk is licensed under the GNU Affero General Public License, version 3 only (`AGPL-3.0-only`), with copyright (c) 2026 davoda1. The source distribution includes the canonical `LICENSE` and application `NOTICE`; runtime packages include these as `LICENSE.txt` and `NOTICE.txt`. Provide the corresponding application source with binary distributions as required by the license.

Retain the `licenses` directory, original copyright notices, the dependency lock, and source/provenance notices when redistributing. Application AGPL-3.0-only licensing does not replace dependency licenses. Compatible shared libraries remain replaceable; no application restriction prohibits debugging such replacements. The Linux staging script's library changes are limited to the relative lookup paths shown in its source. The inherited Windows Needle UTF-8 manifest change is documented in its existing modification notice.

Matching upstream source locations are also recorded in the included notices:

- [Qt 6.8.3 source archives](https://download.qt.io/archive/qt/6.8/6.8.3/single/)
- [FFmpeg n7.1 source](https://github.com/FFmpeg/FFmpeg/tree/n7.1)
- [Speech Desk dependency provenance](https://github.com/Davoda1/Speech-Desk/tree/fcd78d0)

For your own redistribution, preserve the applicable corresponding-source access and license requirements alongside the binary packages. Keep any changed dependency build configuration or patches with those sources. The build procedure pins runtime inputs; it does not claim bit-for-bit identical executables across different compiler or operating-system versions.
