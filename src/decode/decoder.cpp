// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/decode/decoder.h>

#include <ks/ipc/fd.h>
#include <ks/ipc/track_segment.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ks::decode {

namespace {

/// The size of the buffer FFmpeg reads the file through.
constexpr int kInputBufferSize = 32 * 1'024;

/// FFmpeg's text for `error`.
std::string av_error_text(int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    if (av_strerror(error, text.data(), text.size()) != 0) {
        return std::format("error {}", error);
    }
    return std::string{text.data()};
}

/// The system's text for errno value `error`.
std::string errno_text(int error) {
    return std::error_code{error, std::generic_category()}.message();
}

/// Turns FFmpeg's logging off, once: a decoder reports what matters through
/// its refusals, errors, and skipped packets, and the worker's standard error
/// is for starting failures only (specs/decode-worker/spec.md requirement
/// 20).
void quiet_ffmpeg() {
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_level(AV_LOG_QUIET); });
}

/// `frames` at `rate` as minutes and seconds, "m:ss", rounded to the second.
std::string minutes_and_seconds(std::uint64_t frames, std::uint32_t rate) {
    const auto seconds = static_cast<std::uint64_t>(
        std::llround(static_cast<double>(frames) / static_cast<double>(rate)));
    return std::format("{}:{:02}", seconds / 60, seconds % 60);
}

/// What FFmpeg reads through: the source, the position it has reached, and
/// the first error the source gave.
struct Input {
    std::unique_ptr<Source> source;
    std::uint64_t position = 0;
    std::optional<int> error;
};

/// FFmpeg's read callback: the next bytes from the source.
int read_input(void* opaque, std::uint8_t* buffer, int size) {
    auto& input = *static_cast<Input*>(opaque);
    if (input.error) {
        return AVERROR(*input.error);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): FFmpeg's bytes
    const std::span<std::byte> bytes{reinterpret_cast<std::byte*>(buffer),
                                     static_cast<std::size_t>(std::max(size, 0))};
    const std::expected<std::size_t, int> read = input.source->read_at(input.position, bytes);
    if (!read) {
        input.error = read.error();
        return AVERROR(read.error());
    }
    if (*read == 0) {
        return AVERROR_EOF;
    }
    input.position += *read;
    return static_cast<int>(*read);
}

/// FFmpeg's seek callback: moves the position, or gives the size for
/// AVSEEK_SIZE.
std::int64_t seek_input(void* opaque, std::int64_t offset, int whence) {
    auto& input = *static_cast<Input*>(opaque);
    const auto size = [&input]() -> std::expected<std::int64_t, int> {
        const std::expected<std::uint64_t, int> bytes = input.source->size();
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        return static_cast<std::int64_t>(*bytes);
    };
    std::int64_t from = 0;
    switch (whence & ~AVSEEK_FORCE) {
    case AVSEEK_SIZE: {
        const std::expected<std::int64_t, int> bytes = size();
        return bytes ? *bytes : AVERROR(bytes.error());
    }
    case SEEK_SET:
        break;
    case SEEK_CUR:
        from = static_cast<std::int64_t>(input.position);
        break;
    case SEEK_END: {
        const std::expected<std::int64_t, int> bytes = size();
        if (!bytes) {
            return AVERROR(bytes.error());
        }
        from = *bytes;
        break;
    }
    default:
        return AVERROR(EINVAL);
    }
    const std::int64_t position = from + offset;
    if (position < 0) {
        return AVERROR(EINVAL);
    }
    input.position = static_cast<std::uint64_t>(position);
    return position;
}

/// Frees an AVIOContext and its buffer, which FFmpeg may have replaced.
struct AvioDeleter {
    void operator()(AVIOContext* avio) const noexcept {
        av_freep(static_cast<void*>(&avio->buffer));
        avio_context_free(&avio);
    }
};

struct FormatDeleter {
    void operator()(AVFormatContext* format) const noexcept { avformat_close_input(&format); }
};

struct CodecDeleter {
    void operator()(AVCodecContext* codec) const noexcept { avcodec_free_context(&codec); }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};

struct SwrDeleter {
    void operator()(SwrContext* swr) const noexcept { swr_free(&swr); }
};

/// An AVChannelLayout that uninitializes itself.
class ChannelLayout {
public:
    ChannelLayout() = default;
    ChannelLayout(const ChannelLayout&) = delete;
    ChannelLayout& operator=(const ChannelLayout&) = delete;
    ChannelLayout(ChannelLayout&&) = delete;
    ChannelLayout& operator=(ChannelLayout&&) = delete;
    ~ChannelLayout() { av_channel_layout_uninit(&layout_); }

    /// `layout`, or the default layout for its channel count when its order
    /// isn't known, as swresample needs. False if it can't be copied.
    bool set(const AVChannelLayout& layout) {
        av_channel_layout_uninit(&layout_);
        if (layout.order == AV_CHANNEL_ORDER_UNSPEC) {
            av_channel_layout_default(&layout_, layout.nb_channels);
            return true;
        }
        return av_channel_layout_copy(&layout_, &layout) == 0;
    }

    [[nodiscard]] const AVChannelLayout& get() const noexcept { return layout_; }

private:
    AVChannelLayout layout_{};
};

} // namespace

/// Everything a Decoder holds: FFmpeg's contexts, and the frames decoded but
/// not yet read. At a fixed address, since FFmpeg holds a pointer to `input`.
struct Decoder::State {
    Input input;
    std::unique_ptr<AVIOContext, AvioDeleter> avio;
    std::unique_ptr<AVFormatContext, FormatDeleter> format;
    std::unique_ptr<AVCodecContext, CodecDeleter> codec;
    std::unique_ptr<AVPacket, PacketDeleter> packet;
    std::unique_ptr<AVFrame, FrameDeleter> frame;
    int stream = -1;
    DecoderInfo info;

    /// The converter, and the input layout and format it was made for.
    std::unique_ptr<SwrContext, SwrDeleter> swr;
    ChannelLayout swr_layout;
    int swr_format = -1;

    /// Interleaved stereo frames decoded but not yet read, from
    /// `pending_offset` (in samples).
    std::vector<float> pending;
    std::size_t pending_offset = 0;
    /// The demuxer has ended, and the decoder has been told.
    bool input_ended = false;
    /// The decoder has given its last frame.
    bool finished = false;
    /// The error read() keeps returning, once there's been one.
    std::optional<DecodeError> error;
    std::uint64_t skipped = 0;

    /// Opens the file through the source: FFmpeg's I/O and its demuxer.
    std::optional<Refusal> open_input();
    /// Finds the best audio stream, and opens its decoder.
    std::optional<Refusal> open_codec();
    /// Checks the stream's rate and channels, and fills `info`.
    std::optional<Refusal> describe();

    /// Decodes until `pending` holds at least one frame, or the decoder has
    /// finished. Returns the error that stopped it, if one did.
    std::optional<DecodeError> decode_more();
    /// Reads the next packet and gives it to the decoder; at the end of the
    /// file, tells the decoder so. Returns the error that stopped it, if one
    /// did.
    std::optional<DecodeError> feed();

    /// Converts `frame` to interleaved stereo float, into `pending`.
    std::optional<DecodeError> convert();

    /// Why the file can't be opened, when FFmpeg reports `result`: the
    /// source's own error if it had one.
    [[nodiscard]] Refusal unreadable(int result) const {
        return Refusal{.reason = RefusalReason::Unreadable,
                       .text = input.error
                                   ? "can't read the file: " + errno_text(*input.error)
                                   : "not an audio file it can read: " + av_error_text(result)};
    }

    /// The error to report when FFmpeg reports `result`: the source's own
    /// error if it had one.
    [[nodiscard]] DecodeError failure(int result) const {
        if (input.error) {
            return DecodeError{.text = "reading the file failed: " + errno_text(*input.error)};
        }
        return DecodeError{.text = "decoding failed: " + av_error_text(result)};
    }
};

FdSource::FdSource(ipc::Fd fd) noexcept : fd_{std::move(fd)} {}

std::expected<std::size_t, int> FdSource::read_at(std::uint64_t offset,
                                                  std::span<std::byte> buffer) {
    if (!size()) {
        return std::unexpected{EINVAL};
    }
    std::size_t done = 0;
    while (done < buffer.size()) {
        const std::span<std::byte> rest = buffer.subspan(done);
        const ssize_t read =
            ::pread(fd_.get(), rest.data(), rest.size(), static_cast<off_t>(offset + done));
        if (read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected{errno};
        }
        if (read == 0) {
            break;
        }
        done += static_cast<std::size_t>(read);
    }
    return done;
}

std::expected<std::uint64_t, int> FdSource::size() {
    struct stat status{};
    if (::fstat(fd_.get(), &status) != 0) {
        return std::unexpected{errno};
    }
    if (!S_ISREG(status.st_mode)) {
        return std::unexpected{EINVAL};
    }
    return static_cast<std::uint64_t>(status.st_size);
}

std::expected<Decoder, Refusal> Decoder::open(std::unique_ptr<Source> source) {
    quiet_ffmpeg();
    auto state = std::make_unique<State>();
    state->input.source = std::move(source);
    for (std::optional<Refusal> (State::* const step)() :
         {&State::open_input, &State::open_codec, &State::describe}) {
        if (std::optional<Refusal> refused = ((*state).*step)()) {
            return std::unexpected{std::move(*refused)};
        }
    }
    state->packet.reset(av_packet_alloc());
    state->frame.reset(av_frame_alloc());
    if (!state->packet || !state->frame) {
        return std::unexpected{
            Refusal{.reason = RefusalReason::Unreadable, .text = "out of memory"}};
    }
    return Decoder{std::move(state)};
}

std::optional<Refusal> Decoder::State::open_input() {
    if (const std::expected<std::uint64_t, int> size = input.source->size(); !size) {
        return Refusal{.reason = RefusalReason::Unreadable,
                       .text = "can't read the file: " + errno_text(size.error())};
    }
    Refusal out_of_memory{.reason = RefusalReason::Unreadable, .text = "out of memory"};
    // FFmpeg reads through the source, from a buffer it owns.
    auto* const buffer = static_cast<unsigned char*>(av_malloc(kInputBufferSize));
    if (buffer == nullptr) {
        return out_of_memory;
    }
    avio.reset(
        avio_alloc_context(buffer, kInputBufferSize, 0, &input, read_input, nullptr, seek_input));
    if (!avio) {
        av_free(buffer);
        return out_of_memory;
    }
    AVFormatContext* opened = avformat_alloc_context();
    if (opened == nullptr) {
        return out_of_memory;
    }
    opened->pb = avio.get();
    opened->flags |= AVFMT_FLAG_CUSTOM_IO;
    // avformat_open_input() frees `opened` if it fails.
    if (const int result = avformat_open_input(&opened, nullptr, nullptr, nullptr); result < 0) {
        return unreadable(result);
    }
    format.reset(opened);
    if (const int result = avformat_find_stream_info(format.get(), nullptr); result < 0) {
        return unreadable(result);
    }
    return std::nullopt;
}

std::optional<Refusal> Decoder::State::open_codec() {
    const AVCodec* decoder = nullptr;
    stream = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (stream == AVERROR_STREAM_NOT_FOUND) {
        return Refusal{.reason = RefusalReason::NoAudio, .text = "it has no audio"};
    }
    if (stream < 0 || decoder == nullptr) {
        return Refusal{.reason = RefusalReason::Unsupported,
                       .text = "there's no decoder for its audio"};
    }
    const std::span<AVStream*> streams{format->streams, format->nb_streams};
    const AVStream& audio = *streams[static_cast<std::size_t>(stream)];
    Refusal unsupported{.reason = RefusalReason::Unsupported,
                        .text = std::format("its {} audio can't be decoded",
                                            avcodec_get_name(audio.codecpar->codec_id))};
    codec.reset(avcodec_alloc_context3(decoder));
    if (!codec || avcodec_parameters_to_context(codec.get(), audio.codecpar) < 0) {
        return unsupported;
    }
    // Decoding on the calling thread keeps it deterministic (requirement 2).
    codec->thread_count = 1;
    codec->pkt_timebase = audio.time_base;
    if (avcodec_open2(codec.get(), decoder, nullptr) < 0) {
        return unsupported;
    }
    return std::nullopt;
}

std::optional<Refusal> Decoder::State::describe() {
    const int rate = codec->sample_rate;
    if (std::cmp_less(rate, ipc::kMinTrackRate) || std::cmp_greater(rate, ipc::kMaxTrackRate)) {
        return Refusal{.reason = RefusalReason::BadRate,
                       .text = std::format("its sample rate, {} Hz, is outside {} to {} Hz", rate,
                                           ipc::kMinTrackRate, ipc::kMaxTrackRate)};
    }
    const int channels = codec->ch_layout.nb_channels;
    if (channels < 1 || std::cmp_greater(channels, kMaxChannels)) {
        return Refusal{.reason = RefusalReason::BadChannels,
                       .text = std::format("it has {} channels: 1 to {} can be decoded", channels,
                                           kMaxChannels)};
    }

    // The estimate: the stream's duration, or else the file's.
    const std::span<AVStream*> streams{format->streams, format->nb_streams};
    const AVStream& audio = *streams[static_cast<std::size_t>(stream)];
    std::int64_t estimated = 0;
    if (audio.duration != AV_NOPTS_VALUE && audio.duration > 0) {
        estimated = av_rescale_q_rnd(audio.duration, audio.time_base,
                                     AVRational{.num = 1, .den = rate}, AV_ROUND_NEAR_INF);
    } else if (format->duration != AV_NOPTS_VALUE && format->duration > 0) {
        estimated = av_rescale_rnd(format->duration, rate, AV_TIME_BASE, AV_ROUND_NEAR_INF);
    }
    const auto unsigned_rate = static_cast<std::uint32_t>(rate);
    info = DecoderInfo{
        .sample_rate = unsigned_rate,
        .channels = static_cast<std::uint32_t>(channels),
        .estimated_frames = static_cast<std::uint64_t>(std::max<std::int64_t>(estimated, 0)),
        .codec = avcodec_get_name(audio.codecpar->codec_id),
    };
    if (info.estimated_frames > ipc::kMaxTrackFrames) {
        return Refusal{
            .reason = RefusalReason::TooLong,
            .text = std::format("it's {} long, longer than the {} a track can hold at {} Hz",
                                minutes_and_seconds(info.estimated_frames, unsigned_rate),
                                minutes_and_seconds(ipc::kMaxTrackFrames, unsigned_rate), rate)};
    }
    return std::nullopt;
}

Decoder::Decoder(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}

Decoder::Decoder(Decoder&& other) noexcept = default;

Decoder& Decoder::operator=(Decoder&& other) noexcept = default;

Decoder::~Decoder() = default;

const DecoderInfo& Decoder::info() const noexcept {
    return state_->info;
}

std::expected<std::size_t, DecodeError> Decoder::read(std::span<float> out) {
    State& state = *state_;
    if (state.error) {
        return std::unexpected{*state.error};
    }
    const std::size_t wanted = (out.size() / 2) * 2;
    std::size_t written = 0;
    while (written < wanted) {
        if (state.pending_offset < state.pending.size()) {
            const std::size_t count =
                std::min(wanted - written, state.pending.size() - state.pending_offset);
            const std::span<const float> from =
                std::span{state.pending}.subspan(state.pending_offset, count);
            std::ranges::copy(from, out.subspan(written).begin());
            written += count;
            state.pending_offset += count;
            continue;
        }
        if (state.finished) {
            break;
        }
        if (std::optional<DecodeError> failed = state.decode_more()) {
            // The frames this call copied are dropped: a read returns fewer
            // frames than it has room for only at the end of the file.
            state.error = std::move(failed);
            return std::unexpected{*state.error};
        }
    }
    return written / 2;
}

std::uint64_t Decoder::skipped_packets() const noexcept {
    return state_->skipped;
}

std::optional<DecodeError> Decoder::State::decode_more() {
    pending.clear();
    pending_offset = 0;
    while (!finished) {
        const int received = avcodec_receive_frame(codec.get(), frame.get());
        if (received == 0) {
            std::optional<DecodeError> failed = convert();
            av_frame_unref(frame.get());
            if (failed || !pending.empty()) {
                return failed;
            }
        } else if (received == AVERROR_EOF) {
            finished = true;
            // A source that failed can look like the end of the file.
            return input.error ? std::optional{failure(received)} : std::nullopt;
        } else if (received == AVERROR(EAGAIN)) {
            if (std::optional<DecodeError> failed = feed()) {
                return failed;
            }
        } else {
            // A frame that didn't decode: the next packets still can.
            ++skipped;
        }
    }
    return std::nullopt;
}

std::optional<DecodeError> Decoder::State::feed() {
    if (input_ended) {
        // The decoder was told, and still wants more: nothing more comes.
        finished = true;
        return std::nullopt;
    }
    const int read = av_read_frame(format.get(), packet.get());
    if (read == AVERROR_EOF && !input.error) {
        input_ended = true;
        // Drains the decoder: what it holds comes out, then AVERROR_EOF.
        static_cast<void>(avcodec_send_packet(codec.get(), nullptr));
        return std::nullopt;
    }
    if (read < 0) {
        return failure(read);
    }
    if (packet->stream_index == stream && avcodec_send_packet(codec.get(), packet.get()) < 0) {
        ++skipped;
    }
    av_packet_unref(packet.get());
    return std::nullopt;
}

std::optional<DecodeError> Decoder::State::convert() {
    if (std::cmp_not_equal(frame->sample_rate, info.sample_rate)) {
        return DecodeError{.text = std::format("the sample rate changed from {} to {} Hz",
                                               info.sample_rate, frame->sample_rate)};
    }
    const int channels = frame->ch_layout.nb_channels;
    if (channels < 1 || std::cmp_greater(channels, kMaxChannels)) {
        return DecodeError{.text = std::format("a frame has {} channels", channels)};
    }
    // Mono is converted as mono, then copied to both channels unchanged.
    const bool mono = channels == 1;

    ChannelLayout layout;
    if (!layout.set(frame->ch_layout)) {
        return DecodeError{.text = "a frame's channel layout can't be read"};
    }
    if (!swr || swr_format != frame->format ||
        av_channel_layout_compare(&swr_layout.get(), &layout.get()) != 0) {
        AVChannelLayout out_layout{};
        av_channel_layout_default(&out_layout, mono ? 1 : 2);
        SwrContext* made = nullptr;
        const int set = swr_alloc_set_opts2(
            &made, &out_layout, AV_SAMPLE_FMT_FLT, frame->sample_rate, &layout.get(),
            static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&out_layout);
        swr.reset(made);
        if (set < 0 || swr_init(swr.get()) < 0) {
            swr.reset();
            return DecodeError{.text = "a frame's samples can't be converted"};
        }
        if (!swr_layout.set(layout.get())) {
            swr.reset();
            return DecodeError{.text = "a frame's channel layout can't be read"};
        }
        swr_format = frame->format;
    }

    const int out_channels = mono ? 1 : 2;
    const int room = swr_get_out_samples(swr.get(), frame->nb_samples);
    if (room < 0) {
        return DecodeError{.text = "a frame's samples can't be converted"};
    }
    std::vector<float> converted(static_cast<std::size_t>(room) *
                                 static_cast<std::size_t>(out_channels));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): swresample's bytes
    std::array<std::uint8_t*, 1> out{reinterpret_cast<std::uint8_t*>(converted.data())};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): swresample only reads the input
    const auto* const* in = const_cast<const std::uint8_t* const*>(frame->extended_data);
    const int frames = swr_convert(swr.get(), out.data(), room, in, frame->nb_samples);
    if (frames < 0) {
        return DecodeError{.text =
                               "a frame's samples can't be converted: " + av_error_text(frames)};
    }
    converted.resize(static_cast<std::size_t>(frames) * static_cast<std::size_t>(out_channels));
    if (mono) {
        pending.reserve(pending.size() + (converted.size() * 2));
        for (const float sample : converted) {
            pending.push_back(sample);
            pending.push_back(sample);
        }
    } else {
        pending.insert(pending.end(), converted.begin(), converted.end());
    }
    return std::nullopt;
}

DecodedTrack decode_track(Decoder& decoder, ipc::TrackWriter& writer, std::uint64_t max_frames,
                          const std::function<bool()>& stopping) {
    writer.start(decoder.info().sample_rate);
    const std::uint64_t limit = std::min(max_frames, writer.capacity());
    const std::span<float> samples = writer.samples();
    // Each read goes here first, so a read that fails writes nothing to the
    // segment past the frames published.
    std::vector<float> chunk(kDecodeChunkFrames * 2);
    std::uint64_t frames = 0;
    const auto finish = [&writer, &frames](ipc::TrackStatus status) {
        writer.finish(status, frames);
        return DecodedTrack{.frames = frames, .status = status};
    };
    for (;;) {
        if (stopping()) {
            return finish(ipc::TrackStatus::Failed);
        }
        if (frames == limit) {
            // At the limit: Complete if the file ends here too.
            std::array<float, 2> one{};
            const std::expected<std::size_t, DecodeError> more = decoder.read(one);
            return finish(more && *more == 0 ? ipc::TrackStatus::Complete
                                             : ipc::TrackStatus::Truncated);
        }
        const auto room =
            static_cast<std::size_t>(std::min<std::uint64_t>(kDecodeChunkFrames, limit - frames));
        const std::expected<std::size_t, DecodeError> read =
            decoder.read(std::span{chunk}.first(room * 2));
        if (!read) {
            return finish(ipc::TrackStatus::Failed);
        }
        if (*read == 0) {
            return finish(ipc::TrackStatus::Complete);
        }
        std::ranges::copy(std::span{chunk}.first(*read * 2),
                          samples.subspan(static_cast<std::size_t>(frames) * 2).begin());
        frames += *read;
        writer.publish(frames);
    }
}

} // namespace ks::decode
