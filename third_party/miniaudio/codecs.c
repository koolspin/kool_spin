// SPDX-License-Identifier: GPL-3.0-or-later
// A private copy of miniaudio with its decoders and WAV encoder, for the
// development harness only (specs/harness/spec.md requirements 30 and 31).
//
// MA_API static gives every miniaudio function internal linkage, so this copy
// links beside ks::miniaudio's. The one global that ignores MA_API is renamed.
// Device I/O is compiled out: ks::miniaudio does that.
#define ma_atomic_global_lock ks_codecs_ma_atomic_global_lock
#define MA_API static
#define MA_NO_DEVICE_IO
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "ks_codecs.h"

#include <stdlib.h>
#include <string.h>

// Frames decoded per read.
#define KS_CODECS_CHUNK_FRAMES 4096U

const char* ks_codecs_describe(int result) {
    switch (result) {
    case KS_CODECS_OK:
        return "no error";
    case KS_CODECS_OPEN_FAILED:
        return "it couldn't be opened as WAV, FLAC, or MP3";
    case KS_CODECS_DECODE_FAILED:
        return "decoding failed";
    case KS_CODECS_TOO_LONG:
        return "it's too long";
    case KS_CODECS_OUT_OF_MEMORY:
        return "out of memory";
    case KS_CODECS_WRITE_FAILED:
        return "it couldn't be written";
    case KS_CODECS_INVALID_ARGUMENT:
        return "invalid argument";
    default:
        return "unknown error";
    }
}

int ks_codecs_decode_file(const char* path, uint64_t max_frames, ks_codecs_audio* audio) {
    if (path == NULL || audio == NULL) {
        return KS_CODECS_INVALID_ARGUMENT;
    }
    memset(audio, 0, sizeof *audio);

    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 2, 0); // the file's own rate
    ma_decoder decoder;
    if (ma_decoder_init_file(path, &config, &decoder) != MA_SUCCESS) {
        return KS_CODECS_OPEN_FAILED;
    }

    float* samples = NULL;
    uint64_t frames = 0;
    uint64_t capacity = 0;
    int result = KS_CODECS_OK;
    for (;;) {
        if (frames + KS_CODECS_CHUNK_FRAMES > capacity) {
            const uint64_t grown = capacity == 0 ? 65536U : capacity * 2U;
            if (grown > SIZE_MAX / (2U * sizeof(float))) {
                result = KS_CODECS_OUT_OF_MEMORY;
                break;
            }
            float* const bigger = (float*)realloc(samples, (size_t)grown * 2U * sizeof(float));
            if (bigger == NULL) {
                result = KS_CODECS_OUT_OF_MEMORY;
                break;
            }
            samples = bigger;
            capacity = grown;
        }
        ma_uint64 read = 0;
        const ma_result status = ma_decoder_read_pcm_frames(
            &decoder, samples + (frames * 2U), KS_CODECS_CHUNK_FRAMES, &read);
        frames += read;
        if (frames > max_frames) {
            result = KS_CODECS_TOO_LONG;
            break;
        }
        if (status == MA_AT_END || read == 0) {
            break;
        }
        if (status != MA_SUCCESS) {
            result = KS_CODECS_DECODE_FAILED;
            break;
        }
    }
    const uint32_t sample_rate = decoder.outputSampleRate;
    ma_decoder_uninit(&decoder);

    if (result != KS_CODECS_OK) {
        free(samples);
        return result;
    }
    if (frames == 0) {
        free(samples);
        samples = NULL;
    }
    audio->samples = samples;
    audio->frames = frames;
    audio->sample_rate = sample_rate;
    return KS_CODECS_OK;
}

void ks_codecs_free_audio(ks_codecs_audio* audio) {
    if (audio == NULL) {
        return;
    }
    free(audio->samples);
    memset(audio, 0, sizeof *audio);
}

struct ks_codecs_wav_writer {
    ma_encoder encoder;
    uint32_t channels;
};

int ks_codecs_wav_open(const char* path, uint32_t channels, uint32_t sample_rate,
                       ks_codecs_wav_writer** writer) {
    if (path == NULL || writer == NULL || channels == 0 || sample_rate == 0) {
        return KS_CODECS_INVALID_ARGUMENT;
    }
    *writer = NULL;
    ks_codecs_wav_writer* const opened = (ks_codecs_wav_writer*)calloc(1, sizeof *opened);
    if (opened == NULL) {
        return KS_CODECS_OUT_OF_MEMORY;
    }
    const ma_encoder_config config =
        ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, channels, sample_rate);
    if (ma_encoder_init_file(path, &config, &opened->encoder) != MA_SUCCESS) {
        free(opened);
        return KS_CODECS_WRITE_FAILED;
    }
    opened->channels = channels;
    *writer = opened;
    return KS_CODECS_OK;
}

int ks_codecs_wav_write(ks_codecs_wav_writer* writer, const float* samples, uint64_t frames) {
    if (writer == NULL || (samples == NULL && frames != 0)) {
        return KS_CODECS_INVALID_ARGUMENT;
    }
    ma_uint64 written = 0;
    if (ma_encoder_write_pcm_frames(&writer->encoder, samples, frames, &written) != MA_SUCCESS ||
        written != frames) {
        return KS_CODECS_WRITE_FAILED;
    }
    return KS_CODECS_OK;
}

int ks_codecs_wav_close(ks_codecs_wav_writer* writer) {
    if (writer == NULL) {
        return KS_CODECS_INVALID_ARGUMENT;
    }
    ma_encoder_uninit(&writer->encoder);
    free(writer);
    return KS_CODECS_OK;
}
