// main.cpp ─────────────────────────────────────────────────────────────────
// End-to-end demo. Wires three threads that mirror the architecture:
// Usage:
//   ./request_sim                        # uses data/sim_config.json if present
//   ./request_sim path/to/config.json    # explicit config
//   ./request_sim trace path/to/file.csv # legacy CLI: trace replay shorthand
//
// Three threads still mirror the architecture:
//   producer  — owned by RequestSimulator (Traffic Engine pushing)
//   consumer  — pretends to be the ML Predictor
//   feedback  — pretends to be the Edge Cache Controller
// And optionally a fourth: the LiveDashboard renderer.


#include "DatasetLoader.hpp"
#include "RequestSimulator.hpp"
#include "TrafficEngine.hpp"
#include "LiveDashboard.hpp"
#include "SimConfig.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace pec;
using clock_t_ = std::chrono::steady_clock;

namespace {

SimConfig resolve_config(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "trace") {
        SimConfig c;
        c.mode         = WorkloadMode::Trace;
        c.trace_path   = argv[2];
        c.config_label = "cli-trace";
        return c;
    }
    std::string path = (argc >= 2) ? argv[1] : "data/sim_config.json";
    return load_sim_config(path);
}

std::unique_ptr<TrafficEngine> build_engine(const SimConfig& c) {
    if (c.mode == WorkloadMode::Trace) {
        auto loader = std::make_unique<CsvTraceLoader>(c.trace_path);
        return std::make_unique<TraceReplayEngine>(std::move(loader));
    }
    return std::make_unique<SyntheticEngine>(c.synthetic);
}

std::string mode_label(const SimConfig& c) {
    if (c.mode == WorkloadMode::Trace)
        return "trace replay: " + c.trace_path;
    return "synthetic Zipf(a=" +
           std::to_string(c.synthetic.zipf_alpha).substr(0, 4) +
           ") + Poisson";
}

}  // namespace

int main(int argc, char** argv) {
    SimConfig cfg = resolve_config(argc, argv);
    print_sim_config(cfg);
    std::cout << std::endl;

    auto engine = build_engine(cfg);
    RequestSimulator sim(std::move(engine), cfg.simulator);
    sim.start();

    DashboardStats stats;

    // ── Mock ML Predictor consumer ─────────────────────────────────
    std::atomic<bool> stop_consumer{false};
    std::thread predictor([&]{
        std::vector<Request> batch;
        while (!stop_consumer.load()) {
            batch.clear();
            const auto n = sim.pop_request_stream(batch, cfg.consumer.batch_size);
            if (n == 0) break;
            stats.consumed.fetch_add(n);

            if (stats.consumed.load() % 1024 < n) {
                auto hist = sim.get_history();
                (void)hist;
            }
        }
    });

    // ── Mock Edge Cache Controller posting feedback ────────────────
    std::atomic<bool> stop_feedback{false};
    std::thread cache_ctrl;
    if (cfg.consumer.emit_feedback) {
        cache_ctrl = std::thread([&]{
            uint64_t rid = 1;
            while (!stop_feedback.load()) {
                Feedback fb;
                fb.request_id = rid;
                fb.outcome    = (rid % 10 < 7) ? CacheOutcome::HIT
                                               : CacheOutcome::MISS;
                fb.served_ns  = 0;
                sim.submit_feedback(fb);
                stats.feedback_count.fetch_add(1);
                if (fb.outcome == CacheOutcome::HIT)
                    stats.feedback_hits.fetch_add(1);
                rid += 1;
                std::this_thread::sleep_for(
                    std::chrono::microseconds(cfg.consumer.feedback_period_us));
            }
        });
    }

    // ── Optional live dashboard ────────────────────────────────────
    std::atomic<bool> stop_dashboard{false};
    std::thread dashboard;
    std::unique_ptr<LiveDashboard> dash;
    if (cfg.ui.live_dashboard) {
        dash = std::make_unique<LiveDashboard>(
            sim, stats,
            mode_label(cfg),
            cfg.config_label,
            cfg.simulator.stream_queue_capacity,
            cfg.simulator.history_window,
            cfg.simulator.max_requests);
        dashboard = std::thread([&]{
            dash->run(stop_dashboard,
                      std::chrono::milliseconds(cfg.ui.refresh_ms));
        });
    }

    // ── Run for the configured wall-clock window, then shut down ───
    const auto t0 = clock_t_::now();
    while (clock_t_::now() - t0 < std::chrono::seconds(cfg.ui.run_seconds)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (cfg.simulator.max_requests &&
            sim.produced() >= cfg.simulator.max_requests) break;
    }

    sim.stop();
    stop_consumer.store(true);
    stop_feedback.store(true);
    predictor.join();
    if (cache_ctrl.joinable()) cache_ctrl.join();

    stop_dashboard.store(true);
    if (dashboard.joinable()) dashboard.join();

    // ── Final report (always, even with dashboard) ─────────────────
    std::cout << "\n[final] config    = " << cfg.config_label << "\n"
              << "[final] produced  = " << sim.produced()  << "\n"
              << "[final] consumed  = " << stats.consumed.load() << "\n"
              << "[final] dropped   = " << sim.dropped()   << "\n"
              << "[final] history   = " << sim.get_history().size()
              << " / " << cfg.simulator.history_window << "\n";
    if (stats.feedback_count.load()) {
        double hr = 100.0 * stats.feedback_hits.load() / stats.feedback_count.load();
        std::cout << "[final] hit rate  = " << hr << "%\n";
    }
    return 0;
}

