# Speech test fixture

`speech.wav` is a short, synthetic English voice clip created for this project.
It contains no recording of a person and no user media. The original test text is:

> The weather is clear today. We will meet at the station at nine.

The fixture is mono, 16 kHz, signed 16-bit PCM and lasts approximately 4.62 seconds.
It is used to exercise the real local recognition engines and the complete editor
workflow. Recognition tests check successful, valid timed output, not an exact
transcript or a benchmark accuracy score.

SHA-256: `a9f460b0dbf0637b444d52d30b8879f05223334de9640f50030b51461e45b454`

The text and synthesized clip are Copyright (c) 2026 davoda1 and distributed
under the application's GNU Affero General Public License v3 (AGPL-3.0-only).
Synthesis used [eSpeak NG](https://github.com/espeak-ng/espeak-ng) 1.51
with its `en-us` voice at 145 words per minute. eSpeak NG itself is a separate
GPL-licensed tool and is not included in the application or this source tree.

To make an equivalent fixture with eSpeak NG and FFmpeg installed:

```sh
espeak-ng -v en-us -s 145 -w speech-original.wav \
  'The weather is clear today. We will meet at the station at nine.'
ffmpeg -i speech-original.wav -ar 16000 -ac 1 -c:a pcm_s16le \
  -map_metadata -1 speech.wav
```

Different tool or voice-data versions can change the output bytes. CI uses the
committed fixture and does not synthesize or download speech during a test.
