// SPDX-License-Identifier: GPL-3.0-or-later
#include <ks/engine/engine.h>

namespace ks::engine {

// Stubs: the acceptance tests compile against this API before it's
// implemented (specs/commands/spec.md). The engine owns its mixer, but gives
// and collects nothing, and process() renders nothing.

Engine::Engine(const BusMap& map) : mixer_{interpolator_, map} {}

// NOLINTBEGIN(readability-convert-member-functions-to-static): stubs; the
// implementation uses the engine's members.

bool Engine::load(std::size_t /*deck*/, const Track& /*track*/) noexcept {
    return false;
}

bool Engine::unload(std::size_t /*deck*/) noexcept {
    return false;
}

std::optional<std::size_t> Engine::attach(ipc::CommandRing& /*ring*/) noexcept {
    return std::nullopt;
}

bool Engine::detach(std::size_t /*client*/) noexcept {
    return false;
}

std::optional<Released> Engine::collect() noexcept {
    return std::nullopt;
}

void Engine::process(const Block& /*block*/) noexcept KS_NONBLOCKING {}

const Mixer& Engine::mixer() const noexcept KS_NONBLOCKING {
    return mixer_;
}

ClientStats Engine::client_stats(std::size_t /*client*/) const noexcept KS_NONBLOCKING {
    return {};
}

// NOLINTEND(readability-convert-member-functions-to-static)

} // namespace ks::engine
