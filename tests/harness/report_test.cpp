// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the harness's report (specs/harness/spec.md requirement 28).

#include <ks/harness/run.h>

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>

namespace {

using ks::harness::flags_problem;
using ks::harness::print_report;
using ks::harness::Report;

Report clean_report() {
    Report report;
    report.played_seconds = 1.0;
    report.blocks_rendered = 188;
    report.peak_render_load = 0.012;
    report.commands_sent = 2;
    report.commands_read = 2;
    return report;
}

std::string printed(const Report& report) {
    std::ostringstream out;
    print_report(report, out);
    return out.str();
}

} // namespace

TEST_CASE("Report: printed in the documented format", "[harness]") {
    CHECK(printed(clean_report()) == "Played 1.000 s\n"
                                     "Blocks: 188 rendered, 0 late, peak load 0.012\n"
                                     "Samples: 0 non-finite, 0 limited\n"
                                     "Commands: 2 sent, 2 read, 0 rejected, 0 delayed\n"
                                     "Device losses: 0\n");
}

TEST_CASE("Report: every field is printed, and stopping early is noted", "[harness]") {
    Report report;
    report.played_seconds = 0.2004;
    report.stopped_early = true;
    report.blocks_rendered = 38;
    report.late_blocks = 1;
    report.peak_render_load = 1.2346;
    report.non_finite_samples = 3;
    report.limited_frames = 4;
    report.commands_sent = 300;
    report.commands_read = 299;
    report.commands_rejected = 5;
    report.events_delayed = 44;
    report.device_losses = 2;
    CHECK(printed(report) == "Played 0.200 s\n"
                             "Stopped early\n"
                             "Blocks: 38 rendered, 1 late, peak load 1.235\n"
                             "Samples: 3 non-finite, 4 limited\n"
                             "Commands: 300 sent, 299 read, 5 rejected, 44 delayed\n"
                             "Device losses: 2\n");
}

TEST_CASE("Report: which problems it flags", "[harness]") {
    CHECK_FALSE(flags_problem(Report{}));
    CHECK_FALSE(flags_problem(clean_report()));

    SECTION("late blocks, in real time") {
        Report report = clean_report();
        report.real_time = true;
        report.late_blocks = 1;
        CHECK(flags_problem(report));
    }
    SECTION("non-finite samples") {
        Report report = clean_report();
        report.non_finite_samples = 1;
        CHECK(flags_problem(report));
    }
    SECTION("rejected commands") {
        Report report = clean_report();
        report.commands_rejected = 1;
        CHECK(flags_problem(report));
    }
    SECTION("fewer commands read than sent") {
        Report report = clean_report();
        report.commands_read = 1;
        CHECK(flags_problem(report));
    }
    SECTION("what isn't a problem") {
        Report report = clean_report();
        report.stopped_early = true;
        report.limited_frames = 100;
        report.events_delayed = 10;
        report.device_losses = 1;
        report.peak_render_load = 0.9;
        // Late blocks, but not in real time: file mode.
        report.late_blocks = 3;
        CHECK_FALSE(flags_problem(report));
    }
}
