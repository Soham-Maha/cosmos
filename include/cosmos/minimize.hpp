#pragma once

// The 1-minimal minimizer (docs/fault-injection.md §11.2, §17.5): replay one recorded trace while
// withholding its fires one at a time, and keep only the fires the finding cannot do without.
// Each replay runs in a forked child, so a crash is an answer rather than the end of the loop.
#include "cosmos/campaign.hpp"
#include "cosmos/scenario.hpp"
#include "cosmos/trace.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace cosmos {

// Drives one Scenario the minimizer built: run, quiesce, check. The body must not build its own.
using ScenarioBody = std::function<void(Scenario&)>;

struct MinimizeConfig {
    FaultConfig plan;       // the failing run's plan; replay needs the same gate verdicts
    std::string finding_id; // the check that must keep failing
    uint32_t node_count = 1;
    std::string program; // named in the repro line
};

struct MinimizeReport {
    uint64_t seed = 0;
    std::string finding_id;
    std::string program;
    bool refused = false;
    std::string reason;                 // set when refused
    std::vector<TraceEntry> candidates; // every fire in the source trace, seq order
    std::vector<TraceEntry> kept;       // the 1-minimal set, seq order
    uint64_t passes = 0;
    uint64_t replays = 0; // every trial, baseline included
    uint64_t crashed_trials = 0;
};

namespace detail {

struct TrialResult {
    bool setup_failed = false;
    bool reproduced = false;
    bool crashed = false;
    int signal = 0;
    std::string first_failure; // check id, throw, or how the child ended
};

inline void write_all(int fd, const std::string& bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t written = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (written <= 0) return;
        done += static_cast<size_t>(written);
    }
}

inline std::string serialize_trial(const TrialResult& trial) {
    std::string bytes;
    put_field(bytes, static_cast<uint8_t>(trial.setup_failed ? 1 : 0));
    put_field(bytes, static_cast<uint8_t>(trial.reproduced ? 1 : 0));
    put_text(bytes, trial.first_failure);
    return bytes;
}

// False on a short payload, which a crashed or silent child leaves behind.
inline bool read_trial(const std::string& bytes, TrialResult& trial) {
    size_t offset = 0;
    auto take = [&](auto& value) {
        if (offset + sizeof(value) > bytes.size()) return false;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        offset += sizeof(value);
        return true;
    };
    auto take_text = [&](std::string& text) {
        uint64_t length = 0;
        if (!take(length) || length > bytes.size() - offset) return false;
        text.assign(bytes, offset, static_cast<size_t>(length));
        offset += static_cast<size_t>(length);
        return true;
    };
    uint8_t setup_failed = 0;
    uint8_t reproduced = 0;
    if (!take(setup_failed) || !take(reproduced) || !take_text(trial.first_failure)) return false;
    trial.setup_failed = setup_failed != 0;
    trial.reproduced = reproduced != 0;
    return true;
}

inline std::string config_problem_text(const ConfigProblem& problem) {
    std::string text = name_of(problem.error);
    if (problem.site) text += " (site=" + std::string(name_of(*problem.site)) + ")";
    return text;
}

inline bool has_failed_check(const ScenarioReport& report, const std::string& id) {
    for (const CheckResult& check : report.checks) {
        if (check.id == id && !check.passed) return true;
    }
    return false;
}

inline std::string first_failed_check(const ScenarioReport& report) {
    for (const CheckResult& check : report.checks) {
        if (!check.passed) return check.id;
    }
    return {};
}

// One replay in its own process: a crash must not end the reduction.
inline TrialResult run_trial(const FaultTrace& source, TraceFilter filter, const FaultConfig& plan,
                             uint32_t node_count, const std::string& finding_id,
                             const ScenarioBody& body) {
    TrialResult trial;
    const int fd = open_scratch_file();
    if (fd < 0) {
        trial.setup_failed = true;
        trial.first_failure = "could not create a scratch file";
        return trial;
    }

    const pid_t pid = ::fork();
    if (pid == 0) {
        TrialResult answer;
        auto scenario = Scenario::replay(source, std::move(filter), plan, node_count);
        if (!scenario) {
            answer.setup_failed = true;
            answer.first_failure = config_problem_text(scenario.error());
        } else {
            try {
                body(*scenario);
                answer.reproduced = has_failed_check(scenario->report(), finding_id);
                answer.first_failure = first_failed_check(scenario->report());
            } catch (const std::exception& error) {
                answer.first_failure = std::string("threw: ") + error.what();
            } catch (...) {
                answer.first_failure = "threw: unknown exception";
            }
        }
        write_all(fd, serialize_trial(answer));
        ::_exit(0);
    }

    if (pid < 0) {
        ::close(fd);
        trial.setup_failed = true;
        trial.first_failure = "fork() failed; minimization needs process containment";
        return trial;
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    const std::string bytes = read_scratch_file(fd);
    ::close(fd);
    if (!read_trial(bytes, trial)) {
        trial = TrialResult{};
        trial.crashed = WIFSIGNALED(status);
        trial.signal = trial.crashed ? WTERMSIG(status) : 0;
        trial.first_failure = trial.crashed ? "killed by signal " + std::to_string(trial.signal)
                                            : "exited without reporting";
    }
    return trial;
}

inline std::vector<TraceEntry> candidates_of(const FaultTrace& source) {
    std::vector<TraceEntry> candidates;
    for (const TraceEntry& entry : source) {
        if (entry.outcome != FaultKind::None || is_event_site(entry.site)) {
            candidates.push_back(entry);
        }
    }
    return candidates;
}

inline MinimizeReport refused_minimize(MinimizeReport report, std::string reason) {
    report.refused = true;
    report.reason = std::move(reason);
    report.kept.clear();
    return report;
}

} // namespace detail

// Greedy one-at-a-time suppression; passes repeat until one drops nothing, which is exactly the
// 1-minimal guarantee (docs/fault-injection.md §11.2). Global (ddmin) minimality is out of scope.
[[nodiscard]] inline MinimizeReport minimize(const FaultTrace& source, const MinimizeConfig& config,
                                             const ScenarioBody& body) {
    MinimizeReport report;
    report.seed = source.universe_seed();
    report.finding_id = config.finding_id;
    report.program = config.program;

    if (!body) return detail::refused_minimize(std::move(report), "no scenario body");
    if (config.finding_id.empty())
        return detail::refused_minimize(std::move(report), "empty finding id");
    if (is_harness_id(config.finding_id))
        return detail::refused_minimize(std::move(report),
                                        "the harness's own findings have no repro");
    // Forking with a universe current would track the replay's trace as application memory.
    if (Simulator::current() != nullptr)
        return detail::refused_minimize(std::move(report), "called inside a live universe");
    if (source.dropped() > 0) {
        return detail::refused_minimize(std::move(report), "the trace is truncated (" +
                                                               std::to_string(source.dropped()) +
                                                               " entries dropped)");
    }
    for (const TraceEntry& entry : source) {
        if (entry.status != DecisionStatus::Fired)
            return detail::refused_minimize(std::move(report),
                                            "the trace is itself a replay; minimize the recording");
    }

    report.candidates = detail::candidates_of(source);

    const detail::TrialResult baseline = detail::run_trial(
        source, TraceFilter::honour_all(), config.plan, config.node_count, config.finding_id, body);
    ++report.replays;
    if (baseline.crashed) ++report.crashed_trials;
    if (baseline.setup_failed) {
        return detail::refused_minimize(std::move(report),
                                        "replay setup failed: " + baseline.first_failure);
    }
    if (!baseline.reproduced) {
        std::string reason = "the trace does not reproduce \"" + config.finding_id + "\"";
        if (!baseline.first_failure.empty()) {
            reason += " (first failure: " + baseline.first_failure + ")";
        }
        return detail::refused_minimize(std::move(report), std::move(reason));
    }

    std::vector<uint64_t> kept;
    kept.reserve(report.candidates.size());
    for (const TraceEntry& entry : report.candidates)
        kept.push_back(entry.seq);

    bool dropped = true;
    while (dropped) {
        dropped = false;
        ++report.passes;
        for (size_t index = 0; index < kept.size();) {
            std::vector<uint64_t> without;
            without.reserve(kept.size() - 1);
            for (size_t other = 0; other < kept.size(); ++other) {
                if (other != index) without.push_back(kept[other]);
            }
            const detail::TrialResult trial =
                detail::run_trial(source, TraceFilter::only_faults(source, std::move(without)),
                                  config.plan, config.node_count, config.finding_id, body);
            ++report.replays;
            if (trial.crashed) ++report.crashed_trials;
            if (trial.setup_failed) {
                return detail::refused_minimize(std::move(report),
                                                "replay setup failed: " + trial.first_failure);
            }
            if (trial.reproduced) {
                kept.erase(kept.begin() + static_cast<std::ptrdiff_t>(index));
                dropped = true;
            } else {
                ++index;
            }
        }
    }

    // kept is ascending because candidates are in seq order; binary_search keeps the copy simple.
    for (const TraceEntry& entry : report.candidates) {
        if (std::binary_search(kept.begin(), kept.end(), entry.seq)) report.kept.push_back(entry);
    }
    return report;
}

// §17.5's shape, with the 1-minimal guarantee and the exact `--only-faults` seqs it needs.
inline void print_minimize_report(std::ostream& out, const MinimizeReport& report) {
    if (report.refused) {
        out << "Minimization REFUSED: " << report.reason << "\n";
        return;
    }

    out << "Minimal fault set for \"" << report.finding_id << "\"  (1-minimal, " << report.replays
        << (report.replays == 1 ? " replay" : " replays") << ", seed " << report.seed;
    if (report.crashed_trials > 0) out << ", " << report.crashed_trials << " crashed";
    out << "):\n";

    if (report.kept.empty()) {
        out << "  (empty: the finding reproduces with every fault withheld)\n";
    } else {
        size_t time_width = 9;
        for (const TraceEntry& entry : report.kept) {
            time_width = std::max(time_width, detail::format_time(entry.at).size() + 2);
        }
        std::ostringstream body;
        body << std::left;
        for (const TraceEntry& entry : report.kept) {
            body << "  " << std::setw(static_cast<int>(time_width)) << detail::format_time(entry.at)
                 << std::setw(8) << name_of(class_of(entry.site)) << std::setw(20)
                 << ("site=" + std::string(name_of(entry.site))) << name_of(entry.outcome)
                 << "  drew=" << (entry.drew ? "yes" : "no");
            // Time and Duration share the nanosecond layout, so one formatter serves both.
            if (entry.amount.ns != 0)
                body << "  amount=" << detail::format_ms(Time{entry.amount.ns});
            body << "\n";
        }
        out << body.str();
    }

    out << "Repro: ";
    if (!report.program.empty()) out << report.program << " ";
    out << "--seed " << report.seed << " --only-faults=";
    for (size_t index = 0; index < report.kept.size(); ++index) {
        if (index != 0) out << ",";
        out << report.kept[index].seq;
    }
    out << "   (" << report.kept.size() << (report.kept.size() == 1 ? " entry" : " entries")
        << ", nothing else)\n";
}

} // namespace cosmos
