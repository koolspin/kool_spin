// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/deck.h>

#include <ks/engine/block.h>

#include <algorithm>
#include <cmath>

namespace ks::engine {

namespace {

/// The rate snaps to its target once this close (spec requirement 10).
constexpr double kRateSnap = 1e-9;

/// `frames` whole frames, as a position.
TrackPosition to_position(std::uint64_t frames) noexcept KS_NONBLOCKING {
    return static_cast<TrackPosition>(frames) << kPositionFractionBits;
}

/// A frame from a control, clamped to [0, capacity], as a position.
TrackPosition to_position(double frame, const Track& track) noexcept KS_NONBLOCKING {
    const double clamped = std::clamp(frame, 0.0, static_cast<double>(track.capacity()));
    return static_cast<TrackPosition>(std::llround(clamped * static_cast<double>(kOneFrame)));
}

bool is_valid(const Track& track) noexcept KS_NONBLOCKING {
    return track.sample_rate() >= kMinTrackRate && track.sample_rate() <= kMaxTrackRate &&
           track.capacity() <= kMaxTrackFrames;
}

} // namespace

Deck::Deck(const Interpolator& interpolator) noexcept : interpolator_{&interpolator} {}

// Controls.

bool Deck::load(const Track& track) noexcept KS_NONBLOCKING {
    if (!is_valid(track)) {
        return false;
    }
    request_load(&track);
    return true;
}

void Deck::unload() noexcept KS_NONBLOCKING {
    request_load(nullptr);
}

void Deck::request_load(const Track* track) noexcept KS_NONBLOCKING {
    playing_ = false;
    cue_ = 0;
    seek_pending_ = false; // a load cancels a waiting seek
    if (move_main(track, 0)) {
        load_pending_ = false;
        pending_track_ = nullptr;
    } else {
        load_pending_ = true;
        pending_track_ = track;
    }
}

void Deck::set_playing(bool playing) noexcept KS_NONBLOCKING {
    if (control_track() != nullptr) {
        playing_ = playing;
    }
}

void Deck::set_rate(double rate) noexcept KS_NONBLOCKING {
    if (std::isfinite(rate)) {
        target_rate_ = std::clamp(rate, -kMaxRate, kMaxRate);
    }
}

void Deck::seek(double frame) noexcept KS_NONBLOCKING {
    const Track* const track = control_track();
    if (track == nullptr || !std::isfinite(frame)) {
        return;
    }
    const TrackPosition position = to_position(frame, *track);
    // After a waiting load, or during a crossfade, the seek waits. A later
    // seek replaces a waiting one, whether it waits or applies.
    if (!load_pending_ && move_main(main_.track, position)) {
        seek_pending_ = false;
    } else {
        seek_pending_ = true;
        pending_seek_ = position;
    }
}

void Deck::set_cue(double frame) noexcept KS_NONBLOCKING {
    const Track* const track = control_track();
    if (track != nullptr && std::isfinite(frame)) {
        cue_ = to_position(frame, *track);
    }
}

const Track* Deck::control_track() const noexcept KS_NONBLOCKING {
    return load_pending_ ? pending_track_ : main_.track;
}

bool Deck::move_main(const Track* track, TrackPosition position) noexcept KS_NONBLOCKING {
    if (main_.gain == 0) {
        main_.track = track;
        main_.position = position;
    } else if (!has_outgoing_) {
        // A crossfade: the main head fades out from where it is, and a new
        // one fades in.
        outgoing_ = main_;
        has_outgoing_ = true;
        main_ = Head{.track = track, .position = position, .gain = 0};
    } else {
        return false;
    }
    waiting_ = false;
    return true;
}

void Deck::apply_waiting_controls() noexcept KS_NONBLOCKING {
    if (load_pending_) {
        load_pending_ = false;
        static_cast<void>(move_main(pending_track_, 0));
        if (pending_track_ == nullptr) {
            playing_ = false; // unloaded: nothing left to play
        }
        pending_track_ = nullptr;
    }
    if (seek_pending_) {
        seek_pending_ = false;
        if (main_.track != nullptr) {
            static_cast<void>(move_main(main_.track, pending_seek_));
        }
    }
}

// Rendering.

void Deck::set_sample_rate(std::uint32_t sample_rate) noexcept KS_NONBLOCKING {
    sample_rate_ = sample_rate;
    const auto ms = static_cast<std::uint64_t>(kDeclickTime.count());
    ramp_frames_ = ((ms * sample_rate) + 500) / 1000;
    const double tau = std::chrono::duration<double>(kRateSmoothing).count();
    smoothing_ = -std::expm1(-1.0 / (tau * static_cast<double>(sample_rate)));
    main_.gain = std::min(main_.gain, ramp_frames_);
    outgoing_.gain = std::min(outgoing_.gain, ramp_frames_);
}

void Deck::render(std::span<float> left, std::span<float> right,
                  std::uint32_t sample_rate) noexcept KS_NONBLOCKING {
    if (left.size() != right.size() || left.empty() || left.size() > kMaxBlockFrames ||
        sample_rate == 0) {
        return;
    }
    if (sample_rate != sample_rate_) {
        set_sample_rate(sample_rate);
    }
    ready_count_ = 0;

    // A position past the end of a complete track moves to the end.
    if (main_.track != nullptr) {
        const TrackReady ready = ready_of(*main_.track);
        if (ready.complete) {
            main_.position = std::min(main_.position, to_position(ready.frames));
        }
    }

    for (std::size_t i = 0; i < left.size(); ++i) {
        const StereoSample frame = render_frame();
        left[i] = frame.left;
        right[i] = frame.right;
    }
}

StereoSample Deck::render_frame() noexcept KS_NONBLOCKING {
    // The rate: at once while silent, otherwise smoothed.
    if (main_.gain == 0 && !has_outgoing_) {
        rate_ = target_rate_;
    } else if (rate_ != target_rate_) {
        rate_ += (target_rate_ - rate_) * smoothing_;
        if (std::abs(target_rate_ - rate_) <= kRateSnap) {
            rate_ = target_rate_;
        }
    }

    // The steps, in frames and in fixed point, for a head's track.
    const auto step_of = [this](const Head& head) noexcept KS_NONBLOCKING {
        return rate_ * static_cast<double>(head.track->sample_rate()) /
               static_cast<double>(sample_rate_);
    };
    const auto fixed = [](double step) noexcept KS_NONBLOCKING {
        return static_cast<TrackPosition>(std::llround(step * static_cast<double>(kOneFrame)));
    };

    // The output: each head's value, times its gain.
    StereoSample out{};
    if (main_.track != nullptr) {
        const double step = step_of(main_);
        const float gain = gain_of(main_, true);
        if (gain > 0.0F) {
            const StereoSample value = value_at(main_, step);
            out.left = gain * value.left;
            out.right = gain * value.right;
        }
        advance_main(fixed(step));
    } else {
        waiting_ = false;
    }
    if (has_outgoing_ && outgoing_.track != nullptr) {
        const double step = step_of(outgoing_);
        const float gain = gain_of(outgoing_, false);
        const StereoSample value = value_at(outgoing_, step);
        out.left += gain * value.left;
        out.right += gain * value.right;
        outgoing_.position += fixed(step);
    }

    // The gains take one step toward their targets.
    const std::uint64_t target = playing_ && !waiting_ && main_.track != nullptr ? ramp_frames_ : 0;
    if (main_.gain < target) {
        ++main_.gain;
    } else if (main_.gain > target) {
        --main_.gain;
    }
    if (has_outgoing_) {
        if (outgoing_.gain > 0) {
            --outgoing_.gain;
        }
        if (outgoing_.gain == 0) {
            has_outgoing_ = false;
            outgoing_ = Head{};
            apply_waiting_controls();
        }
    }
    return out;
}

StereoSample Deck::value_at(const Head& head, double step) noexcept KS_NONBLOCKING {
    const TrackReady ready = ready_of(*head.track);
    return interpolator_->value(head.track->samples(), ready.frames, head.position, std::abs(step));
}

void Deck::advance_main(TrackPosition step) noexcept KS_NONBLOCKING {
    waiting_ = false;
    // It moves while playing, or while fading out after a pause.
    if (!playing_ && main_.gain == 0) {
        return;
    }
    const TrackPosition next = main_.position + step;
    if (next < 0) {
        main_.position = 0;
        playing_ = false;
        return;
    }
    const TrackReady ready = ready_of(*main_.track);
    const TrackPosition end = to_position(ready.frames);
    if (ready.complete) {
        if (next > end) {
            main_.position = end;
            playing_ = false;
            return;
        }
    } else {
        const TrackPosition limit = end - (kInterpolatorReach * kOneFrame);
        if (next > limit) {
            // Stops at the limit, or stays put if it's already past it.
            main_.position = std::max(main_.position, limit);
            waiting_ = true;
            return;
        }
    }
    main_.position = next;
}

float Deck::gain_of(const Head& head, bool main) const noexcept KS_NONBLOCKING {
    if (ramp_frames_ == 0) {
        return main && playing_ && !waiting_ ? 1.0F : 0.0F;
    }
    return static_cast<float>(head.gain) / static_cast<float>(ramp_frames_);
}

TrackReady Deck::ready_of(const Track& track) noexcept KS_NONBLOCKING {
    const std::span<Ready> read = std::span{ready_}.first(ready_count_);
    for (const Ready& entry : read) {
        if (entry.track == &track) {
            return entry.ready;
        }
    }
    const TrackReady ready = track.ready();
    if (ready_count_ < ready_.size()) {
        std::span{ready_}[ready_count_++] = Ready{.track = &track, .ready = ready};
    }
    return ready;
}

// State.

const Track* Deck::track() const noexcept KS_NONBLOCKING {
    return main_.track;
}

bool Deck::holds(const Track& track) const noexcept KS_NONBLOCKING {
    return main_.track == &track || (has_outgoing_ && outgoing_.track == &track) ||
           (load_pending_ && pending_track_ == &track);
}

bool Deck::playing() const noexcept KS_NONBLOCKING {
    return playing_;
}

bool Deck::waiting() const noexcept KS_NONBLOCKING {
    return waiting_;
}

double Deck::rate() const noexcept KS_NONBLOCKING {
    return target_rate_;
}

double Deck::position() const noexcept KS_NONBLOCKING {
    return static_cast<double>(main_.position) / static_cast<double>(kOneFrame);
}

double Deck::cue() const noexcept KS_NONBLOCKING {
    return static_cast<double>(cue_) / static_cast<double>(kOneFrame);
}

} // namespace ks::engine
