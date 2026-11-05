# Contributing to Subtitle Desk

Subtitle Desk focuses on local, file-based transcription and careful subtitle review. Contributions should preserve that workflow and keep the application usable without a network connection.

## Report a problem

Search [existing issues](https://github.com/Davoda1/Subtitle-Desk/issues) before opening a report. Include the application version, operating system, package type, exact steps, and expected versus observed behavior. For recognition issues, include the selected model and language; for import or export issues, name the file format and codec when known.

Use a small, non-sensitive example that you have permission to share. Projects, subtitles, logs, and screenshots can contain transcript text, local paths, or identifying information. Remove those details before attaching them. Recognition quality alone is not necessarily an editor defect; examples of reproducible timing, decoding, or model failures are especially useful.

Follow [SECURITY.md](SECURITY.md) for a suspected vulnerability.

## Build and test

Follow [docs/BUILDING.md](docs/BUILDING.md) to install the matching toolchain and acquire checksum-pinned runtime components. Configure the runtime path to exercise decoding and all four speech models, then run:

```sh
cmake --build build/linux --parallel
ctest --test-dir build/linux --output-on-failure
```

Use your Windows build directory for the equivalent Windows commands. [docs/Verification.md](docs/Verification.md) explains which checks require the runtime and what automated tests cannot establish.

## Submit a change

Target pull requests at `master`. Explain the problem, the resulting behavior, and the verification performed. Keep changes focused, match the surrounding C++17/Python style, and update documentation when behavior or build instructions change. Add regression coverage when fixing a defect that existing tests would miss.

Preserve the current file layout in release packages: a real Windows executable at the ZIP root, a real Linux executable at the portable archive root, and a real `/usr/bin/subtitledesk` executable in Debian packages. Keep third-party license and provenance records with the components they describe.

Do not commit generated build output, downloaded runtimes, model weights, credentials, local settings, or user projects. Use only sample media and text that may be redistributed. Do not add new network services or telemetry as an incidental part of another change.

Contributions are submitted under the project's [GNU Affero General Public License, version 3 only](LICENSE) (`AGPL-3.0-only`). The application copyright is **Copyright (c) 2026 davoda1**; see [NOTICE](NOTICE). Third-party material must retain its applicable copyright notices, licenses, and compatible redistribution terms.
