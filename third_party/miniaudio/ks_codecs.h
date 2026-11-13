/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Decoding and WAV writing for the development harness only
 * (specs/harness/spec.md requirements 30 and 31). The engine never links this.
 *
 * codecs.c compiles a private copy of miniaudio with MA_API defined as static,
 * so none of its symbols clash with ks::miniaudio's. This header doesn't
 * include miniaudio.h: ks::miniaudio's users see that header with the codecs
 * compiled out.
 */
#ifndef KS_CODECS_H
#define KS_CODECS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Results. */
enum {
    KS_CODECS_OK = 0,
    /* The file couldn't be opened, or isn't a format miniaudio decodes. */
    KS_CODECS_OPEN_FAILED = 1,
    /* Decoding failed part way through. */
    KS_CODECS_DECODE_FAILED = 2,
    /* The file has more frames than the caller allows. */
    KS_CODECS_TOO_LONG = 3,
    KS_CODECS_OUT_OF_MEMORY = 4,
    /* The WAV file couldn't be created or written. */
    KS_CODECS_WRITE_FAILED = 5,
    /* A null pointer, or 0 channels or sample rate. */
    KS_CODECS_INVALID_ARGUMENT = 6
};

/* A short description of a result, in English. Never null. */
const char* ks_codecs_describe(int result);

/* Audio decoded from a file. */
typedef struct ks_codecs_audio {
    /* Interleaved stereo 32-bit float: frames * 2 samples. Null if frames is
       0. Free with ks_codecs_free_audio(). */
    float* samples;
    uint64_t frames;
    /* The file's own sample rate. */
    uint32_t sample_rate;
} ks_codecs_audio;

/*
 * Decodes a whole WAV, FLAC, or MP3 file to interleaved stereo float at its
 * own sample rate: mono is copied to both channels, and miniaudio mixes more
 * channels down. Fails with KS_CODECS_TOO_LONG, keeping nothing, if the file
 * has more than max_frames frames. On failure, *audio is zeroed.
 */
int ks_codecs_decode_file(const char* path, uint64_t max_frames, ks_codecs_audio* audio);

/* Frees what ks_codecs_decode_file() allocated, and zeroes *audio. */
void ks_codecs_free_audio(ks_codecs_audio* audio);

/* A 32-bit float WAV file being written. */
typedef struct ks_codecs_wav_writer ks_codecs_wav_writer;

/* Creates (or replaces) a WAV file. On success, *writer must be closed with
   ks_codecs_wav_close(). */
int ks_codecs_wav_open(const char* path, uint32_t channels, uint32_t sample_rate,
                       ks_codecs_wav_writer** writer);

/* Appends frames: frames * channels interleaved samples. */
int ks_codecs_wav_write(ks_codecs_wav_writer* writer, const float* samples, uint64_t frames);

/* Finishes the file's header, closes it, and frees the writer. */
int ks_codecs_wav_close(ks_codecs_wav_writer* writer);

#ifdef __cplusplus
}
#endif

#endif /* KS_CODECS_H */
