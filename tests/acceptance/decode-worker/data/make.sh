#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Makes the decode-worker acceptance tests' fixtures (specs/decode-worker/spec.md)
# with the ffmpeg command-line tool, built with libmp3lame, libvorbis, and
# libopus. Run it from anywhere; it writes next to itself. The fixtures are
# committed, so the tests don't need ffmpeg.
#
# Each is a 1-second, 1 kHz sine tone at -6 dBFS, stereo at 44.1 kHz, unless
# its name says otherwise. The tests write their own WAV files, and make the
# damaged and overlong files from these.
set -euo pipefail
cd "$(dirname "$0")"

# The tone at `rate`, through the encoder options that follow. It's made by
# aevalsrc, at an amplitude of 0.5: lavfi's sine source peaks at 1/8 of full
# scale.
tone() {
    local out=$1 rate=$2
    shift 2
    ffmpeg -nostdin -v error -y -f lavfi \
        -i "aevalsrc=exprs=0.5*sin(2*PI*1000*t):c=stereo:s=$rate:d=1" \
        -map_metadata -1 -fflags +bitexact "$@" "$out"
}

tone tone.aiff 44100 -c:a pcm_s16be
tone tone.flac 44100 -c:a flac
tone tone_alac.m4a 44100 -c:a alac
tone tone.mp3 44100 -c:a libmp3lame -b:a 192k
tone tone_aac.m4a 44100 -c:a aac -b:a 192k
tone tone.ogg 44100 -c:a libvorbis -q:a 6
tone tone.opus 48000 -c:a libopus -b:a 128k
tone tone_96k.flac 96000 -c:a flac

# 5.1 at 44.1 kHz, 3 seconds: the tone only in the front left for a second,
# then only in the front right, then only in the centre.
ffmpeg -nostdin -v error -y -f lavfi \
    -i "aevalsrc=exprs=0.5*sin(2*PI*1000*t)*lt(t\,1)|0.5*sin(2*PI*1000*t)*gte(t\,1)*lt(t\,2)|0.5*sin(2*PI*1000*t)*gte(t\,2)|0|0|0:c=5.1:s=44100:d=3" \
    -map_metadata -1 -fflags +bitexact -c:a flac surround.flac

# The MP3 with a small picture attached, as cover art.
ffmpeg -nostdin -v error -y -f lavfi -i "color=c=red:s=64x64:d=1" -frames:v 1 -update 1 cover.png
ffmpeg -nostdin -v error -y -i tone.mp3 -i cover.png -map 0:a -map 1:v -c copy \
    -id3v2_version 3 -metadata:s:v title="Cover" -metadata:s:v comment="Cover (front)" \
    -disposition:v attached_pic picture.mp3
rm cover.png
