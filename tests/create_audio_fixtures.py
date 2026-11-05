#!/usr/bin/env python3
"""Create small, deterministic media fixtures for the native decoder tests."""
from pathlib import Path
import argparse
import json
import math
import shutil
import struct
import subprocess
import wave

parser=argparse.ArgumentParser()
parser.add_argument('output',type=Path)
parser.add_argument('--ffmpeg',default='ffmpeg')
parser.add_argument('--ffprobe',default='ffprobe')
args=parser.parse_args()
root=args.output.resolve();root.mkdir(parents=True,exist_ok=True)
for name,rate,channels,frequency in [('tone.wav',16000,1,440),('stereo.wav',44100,2,440),('second.wav',16000,1,880)]:
    with wave.open(str(root/name),'wb') as f:
        f.setnchannels(channels);f.setsampwidth(2);f.setframerate(rate)
        f.writeframes(b''.join(struct.pack('<h',int(0.3*32767*math.sin(2*math.pi*frequency*i/rate)))*channels for i in range(rate*2)))

def ffmpeg(name,*options):
    subprocess.run([args.ffmpeg,'-hide_banner','-loglevel','error','-y',*map(str,options),str(root/name)],check=True)

for name,codec in [('tone.mp4','aac'),('tone.mkv','libopus'),('tone.mp3','libmp3lame'),('tone.flac','flac'),('tone.ogg','libvorbis')]:
    ffmpeg(name,'-i',root/'tone.wav','-c:a',codec)
ffmpeg('delayed.mp4','-f','lavfi','-i','color=c=black:s=64x64:r=25:d=5','-itsoffset','3','-i',root/'tone.wav','-map','0:v','-map','1:a','-c:v','libx264','-c:a','aac','-shortest')
ffmpeg('shifted.mp4','-i',root/'delayed.mp4','-c','copy','-output_ts_offset','7')
ffmpeg('gap.mkv','-i',root/'tone.wav','-af',r'asetpts=PTS+gte(T\,1)/TB','-c:a','pcm_s16le')
ffmpeg('too-long.mkv','-i',root/'tone.wav','-af',r'asetpts=PTS+gte(T\,1)*144000/TB','-c:a','pcm_s16le')
ffmpeg('video-only.mp4','-f','lavfi','-i','color=c=black:s=64x64:r=25:d=0.2','-c:v','libx264','-an')
ffmpeg('two-tracks.mkv','-i',root/'tone.wav','-i',root/'second.wav','-map','0:a','-map','1:a','-c:a','pcm_s16le','-disposition:a:0','0','-disposition:a:1','default')
(root/'corrupt.mp4').write_bytes(b'Not a media file\x00\x01')
(root/'remote.m3u8').write_text('#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\nhttps://127.0.0.1:9/should-never-be-opened.ts\n#EXT-X-ENDLIST\n')
shutil.copy2(root/'tone.wav',root/'Árvíztűrő tükörfúrógép 日本語.wav')
expectations={}
for name in ['delayed.mp4','shifted.mp4']:
    probe=subprocess.run([args.ffprobe,'-v','error','-show_entries','format=start_time,duration','-of','json',str(root/name)],capture_output=True,text=True,check=True)
    metadata=json.loads(probe.stdout)['format']
    expectations[name]={'duration_ms':round(float(metadata['duration'])*1000),'origin_ms':round(float(metadata.get('start_time',0))*1000)}
(root/'expectations.json').write_text(json.dumps(expectations,indent=2))
print(f'Created audio fixtures in {root}')
