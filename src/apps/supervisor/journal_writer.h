// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The session journal's writer thread (specs/session-journal/spec.md
// requirements 11, 12, 14, and 31): the supervisor's poll loop hands it the
// newest journal, and never waits for the disk.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

namespace ks::supervisor {

class JournalWriter {
public:
    /// How a write ended: its number, from 1, and its error, if it failed.
    struct Result {
        std::uint64_t write = 0;
        std::optional<std::error_code> error;
    };

    /// Starts the thread, which writes to `dir`, with every signal blocked in
    /// it. With `stall_after`, that write stops before it opens the file,
    /// and never ends. Null if the thread can't start.
    static std::unique_ptr<JournalWriter> start(std::filesystem::path dir,
                                                std::optional<std::uint32_t> stall_after);

    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;
    JournalWriter(JournalWriter&&) = delete;
    JournalWriter& operator=(JournalWriter&&) = delete;
    /// Asks the thread to end. Joins it if it's idle; otherwise leaves it to
    /// end with the process.
    ~JournalWriter();

    /// Hands over `text` to write, replacing any text not yet started.
    void submit(std::string text);
    /// The newest result not yet taken.
    [[nodiscard]] std::optional<Result> take_result();
    /// Waits until nothing is being written or waits to be, or `deadline`.
    /// True if it's idle.
    [[nodiscard]] bool wait_idle(std::chrono::steady_clock::time_point deadline);

private:
    struct Shared {
        std::mutex mutex;
        std::condition_variable changed;
        std::optional<std::string> pending;
        bool busy = false;
        bool quit = false;
        std::uint64_t writes = 0;
        std::optional<Result> result;
    };

    explicit JournalWriter(std::shared_ptr<Shared> shared) : shared_{std::move(shared)} {}
    static void run(const std::shared_ptr<Shared>& shared, const std::filesystem::path& dir,
                    std::optional<std::uint32_t> stall_after);

    std::shared_ptr<Shared> shared_;
    std::thread thread_;
};

} // namespace ks::supervisor
