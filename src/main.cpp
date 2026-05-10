// main.cpp ─────────────────────────────────────────────────────────────────
// End-to-end demo. Wires three threads that mirror the architecture:
//
//   producer thread — owned by RequestSimulator (Traffic Engine pushing)
//   consumer thread — pretends to be the ML Predictor
//   feedback thread — pretends to be the Edge Cache Controller
//
// Run:  ./request_sim                 (synthetic Zipf+Poisson)
//       ./request_sim trace path.csv  (replay a trace file)
//
// We deliberately keep the "ML Predictor" and "Cache Controller" mocks
// trivial — the point is to exercise every channel in the diagram from
// the simulator's side: request_stream out, history out, feedback in.

#include "DatasetLoader.hpp"
#include "RequestSimulator.hpp"
#include "TrafficEngine.hpp"

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

int main(int argc, char** argv) {
    // ── Choose a TrafficEngine based on CLI args ────────────────────
    std::unique_ptr<TrafficEngine> engine;
    if (argc >= 3 && std::string(argv[1]) == "trace") {
        auto loader = std::make_unique<CsvTraceLoader>(argv[2]);
        engine = std::make_unique<TraceReplayEngine>(std::move(loader));
        std::cout << "[mode] trace replay: " << argv[2] << "\n";
    } else {
        SyntheticConfig sc;
        sc.catalog_size = 5'000;
        sc.zipf_alpha   = 0.80;
        sc.num_users    = 50;
        sc.mean_iat_ns  = 250'000;        // 4 kreq/s nominal
        engine = std::make_unique<SyntheticEngine>(sc);
        std::cout << "[mode] synthetic Zipf(α=0.8) + Poisson\n";
    }

    SimulatorConfig sim_cfg;
    sim_cfg.stream_queue_capacity = 2048;
    sim_cfg.history_window        = 128;
    sim_cfg.max_requests          = 50'000;   // bound the demo run
    sim_cfg.real_time_pacing      = false;    // batch mode

    RequestSimulator sim(std::move(engine), sim_cfg);
    sim.start();

    // ── Mock ML Predictor consumer ─────────────────────────────────
    std::atomic<bool> stop_consumer{false};
    std::atomic<std::size_t> consumed{0};
    std::thread predictor([&]{
        std::vector<Request> batch;
        while (!stop_consumer.load()) {
            batch.clear();
            const auto n = sim.pop_request_stream(batch, 128);
            if (n == 0) break;  // simulator stopped + queue drained
            consumed.fetch_add(n);

            // Demonstrate the history channel: every 1024 requests the
            // Feature Extractor would call this exactly once per
            // prediction step.
            if (consumed.load() % 1024 < n) {
                auto hist = sim.get_history();
                (void)hist;  // ML logic would consume this
            }
        }
    });

    // ── Mock Edge Cache Controller posting feedback ────────────────
    std::atomic<bool> stop_feedback{false};
    std::thread cache_ctrl([&]{
        uint64_t rid = 1;
        while (!stop_feedback.load()) {
            // Pretend ~70% hit rate, alternate hit/miss.
            Feedback fb;
            fb.request_id = rid;
            fb.outcome    = (rid % 10 < 7) ? CacheOutcome::HIT
                                           : CacheOutcome::MISS;
            fb.served_ns  = 0;
            sim.submit_feedback(fb);
            rid += 1;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    // ── Run for ~2 wall-clock seconds, then shut down ──────────────
    const auto t0 = clock_t_::now();
    while (clock_t_::now() - t0 < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (sim.produced() >= sim_cfg.max_requests) break;
    }

    sim.stop();
    stop_consumer.store(true);
    stop_feedback.store(true);
    predictor.join();
    cache_ctrl.join();

    // ── Report ─────────────────────────────────────────────────────
    std::cout << "[stats] produced  = " << sim.produced()  << "\n"
              << "[stats] consumed  = " << consumed.load() << "\n"
              << "[stats] dropped   = " << sim.dropped()   << "\n"
              << "[stats] history   = " << sim.get_history().size()
              << " / " << sim_cfg.history_window << "\n";
    return 0;
}
