// SPDX-License-Identifier: GPL-3.0-or-later

#include <ks/client/session.h>

#include <utility>

// The API exists before the code (specs/ks-play/spec.md): every function
// returns an error until the build loop writes it.

namespace ks::client {

struct Session::Impl {};

namespace {

Error not_written() {
    return Error{.kind = ErrorKind::Protocol, .code = ipc::ErrorCode::BadMessage, .text = "stub"};
}

} // namespace

Result<Session> Session::connect(std::string_view /*name*/,
                                 const std::filesystem::path& /*runtime_dir*/,
                                 Milliseconds /*timeout*/) {
    return std::unexpected{not_written()};
}

Result<Session> Session::connect_as(std::string_view /*name*/,
                                    const std::filesystem::path& /*runtime_dir*/,
                                    uid_t /*expected_uid*/, Milliseconds /*timeout*/) {
    return std::unexpected{not_written()};
}

Session::Session(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;
Session::~Session() = default;

Result<std::uint32_t> Session::open_ring(Milliseconds /*timeout*/) {
    static_cast<void>(impl_);
    return std::unexpected{not_written()};
}

Result<std::uint64_t> Session::send(const Command& /*command*/) {
    static_cast<void>(impl_);
    return std::unexpected{not_written()};
}

Result<Applied> Session::wait_applied(std::uint64_t /*sequence*/, Milliseconds /*timeout*/) {
    static_cast<void>(impl_);
    return std::unexpected{not_written()};
}

Result<Snapshot> Session::snapshot() const {
    static_cast<void>(impl_);
    return std::unexpected{not_written()};
}

Result<Snapshot> Session::snapshot_from(ipc::EngineState& /*state*/, Milliseconds /*budget*/) {
    return std::unexpected{not_written()};
}

Result<LoadedFile> Session::load_file(std::uint32_t /*deck*/, const std::string& /*path*/,
                                      Milliseconds /*timeout*/) {
    static_cast<void>(impl_);
    return std::unexpected{not_written()};
}

Result<void> Session::unload_deck(std::uint32_t /*deck*/, Milliseconds /*timeout*/) {
    static_cast<void>(impl_);
    return std::unexpected{not_written()};
}

Result<void> Session::stop(Milliseconds /*timeout*/) {
    static_cast<void>(impl_);
    return std::unexpected{not_written()};
}

Result<void> Session::ping(Milliseconds /*timeout*/) {
    static_cast<void>(impl_);
    return std::unexpected{not_written()};
}

Result<ipc::msg::DecodeEnded> Session::next_decode_ended(Milliseconds /*timeout*/) {
    static_cast<void>(impl_);
    return std::unexpected{not_written()};
}

void Session::close() noexcept {}

} // namespace ks::client
