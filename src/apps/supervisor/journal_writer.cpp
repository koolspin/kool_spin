// SPDX-License-Identifier: GPL-3.0-or-later
#include "journal_writer.h"

#include <ks/supervisor/journal.h>

#include <pthread.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace ks::supervisor {

std::unique_ptr<JournalWriter> JournalWriter::start(std::filesystem::path dir,
                                                    std::optional<std::uint32_t> stall_after) {
    auto shared = std::make_shared<Shared>();
    std::unique_ptr<JournalWriter> writer{new JournalWriter{shared}};
    // The thread inherits this mask: every signal goes to the poll loop.
    sigset_t all{};
    sigset_t before{};
    sigfillset(&all);
    if (::pthread_sigmask(SIG_BLOCK, &all, &before) != 0) {
        return nullptr;
    }
    try {
        writer->thread_ = std::thread{
            [shared, dir = std::move(dir), stall_after] { run(shared, dir, stall_after); }};
    } catch (const std::system_error&) {
        writer.reset();
    }
    ::pthread_sigmask(SIG_SETMASK, &before, nullptr);
    return writer;
}

JournalWriter::~JournalWriter() {
    bool idle = false;
    {
        const std::scoped_lock lock{shared_->mutex};
        shared_->quit = true;
        idle = !shared_->busy && !shared_->pending;
    }
    shared_->changed.notify_all();
    if (!thread_.joinable()) {
        return;
    }
    if (idle) {
        thread_.join();
    } else {
        // It's stuck on the disk: it ends with the process.
        thread_.detach();
    }
}

void JournalWriter::submit(std::string text) {
    {
        const std::scoped_lock lock{shared_->mutex};
        shared_->pending = std::move(text);
    }
    shared_->changed.notify_all();
}

std::optional<JournalWriter::Result> JournalWriter::take_result() {
    const std::scoped_lock lock{shared_->mutex};
    return std::exchange(shared_->result, std::nullopt);
}

bool JournalWriter::wait_idle(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock{shared_->mutex};
    return shared_->changed.wait_until(lock, deadline,
                                       [&] { return !shared_->busy && !shared_->pending; });
}

void JournalWriter::run(const std::shared_ptr<Shared>& shared, const std::filesystem::path& dir,
                        std::optional<std::uint32_t> stall_after) {
    for (;;) {
        std::string text;
        std::uint64_t write = 0;
        {
            std::unique_lock lock{shared->mutex};
            shared->changed.wait(lock, [&] { return shared->quit || shared->pending; });
            if (!shared->pending) {
                return;
            }
            text = std::move(*shared->pending);
            shared->pending.reset();
            shared->busy = true;
            write = ++shared->writes;
        }
        if (stall_after && write == *stall_after) {
            // A disk that hangs, for tests (requirement 31).
            for (;;) {
                std::this_thread::sleep_for(std::chrono::hours{1});
            }
        }
        const std::expected<void, std::error_code> written = write_journal(dir, text);
        {
            const std::scoped_lock lock{shared->mutex};
            shared->busy = false;
            shared->result = Result{
                .write = write,
                .error = written ? std::nullopt : std::optional<std::error_code>{written.error()}};
        }
        shared->changed.notify_all();
    }
}

} // namespace ks::supervisor
