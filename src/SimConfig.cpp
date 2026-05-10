// SimConfig.cpp ────────────────────────────────────────────────────────────

#include "SimConfig.hpp"
#include "Json.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>

namespace pec {

namespace {

std::string slurp(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

SimConfig load_sim_config(const std::string& path) {
    SimConfig c;  // start with defaults

    const std::string text = slurp(path);
    if (text.empty()) {
        // Missing or empty file: silently keep defaults. main.cpp
        // prints the resolved config at startup so this is visible.
        return c;
    }

    JsonValue root = JsonValue::parse(text);

    c.config_label = root.get_or<std::string>("label", c.config_label);

    // ── hardware (SimulatorConfig) ──────────────────────────────────
    c.simulator.stream_queue_capacity =
        root.get_or<std::size_t>("hardware.stream_queue_capacity",
                                 c.simulator.stream_queue_capacity);
    c.simulator.history_window =
        root.get_or<std::size_t>("hardware.history_window",
                                 c.simulator.history_window);
    c.simulator.feedback_retention =
        root.get_or<std::size_t>("hardware.feedback_retention",
                                 c.simulator.feedback_retention);

    // ── workload (mode + synthetic params + trace path) ─────────────
    std::string mode_s = root.get_or<std::string>("workload.mode", "synthetic");
    c.mode = (mode_s == "trace") ? WorkloadMode::Trace
                                 : WorkloadMode::Synthetic;
    c.trace_path = root.get_or<std::string>("workload.trace_path", c.trace_path);

    c.simulator.max_requests =
        root.get_or<std::size_t>("workload.max_requests",
                                 c.simulator.max_requests);
    c.simulator.real_time_pacing =
        root.get_or<bool>("workload.real_time_pacing",
                         c.simulator.real_time_pacing);

    c.synthetic.catalog_size =
        root.get_or<std::uint64_t>("workload.synthetic.catalog_size",
                                   c.synthetic.catalog_size);
    c.synthetic.zipf_alpha =
        root.get_or<double>("workload.synthetic.zipf_alpha",
                            c.synthetic.zipf_alpha);
    c.synthetic.num_users =
        root.get_or<std::uint32_t>("workload.synthetic.num_users",
                                   c.synthetic.num_users);
    c.synthetic.mean_iat_ns =
        root.get_or<double>("workload.synthetic.mean_iat_ns",
                            c.synthetic.mean_iat_ns);
    c.synthetic.mean_file_size =
        root.get_or<std::uint64_t>("workload.synthetic.mean_file_size",
                                   c.synthetic.mean_file_size);
    c.synthetic.seed =
        root.get_or<std::uint64_t>("workload.synthetic.seed",
                                   c.synthetic.seed);

    // ── consumer (mock predictor / cache controller) ────────────────
    c.consumer.batch_size =
        root.get_or<std::size_t>("consumer.batch_size", c.consumer.batch_size);
    c.consumer.emit_feedback =
        root.get_or<bool>("consumer.emit_feedback", c.consumer.emit_feedback);
    c.consumer.feedback_period_us =
        root.get_or<std::size_t>("consumer.feedback_period_us",
                                 c.consumer.feedback_period_us);

    // ── ui ──────────────────────────────────────────────────────────
    c.ui.live_dashboard =
        root.get_or<bool>("ui.live_dashboard", c.ui.live_dashboard);
    c.ui.refresh_ms =
        root.get_or<std::size_t>("ui.refresh_ms", c.ui.refresh_ms);
    c.ui.run_seconds =
        root.get_or<std::size_t>("ui.run_seconds", c.ui.run_seconds);

    return c;
}

void print_sim_config(const SimConfig& c) {
    std::cout << "[config] label = " << c.config_label << "\n"
              << "  hardware:\n"
              << "    stream_queue_capacity = " << c.simulator.stream_queue_capacity << "\n"
              << "    history_window        = " << c.simulator.history_window << "\n"
              << "    feedback_retention    = " << c.simulator.feedback_retention << "\n"
              << "  workload:\n"
              << "    mode                  = "
              << (c.mode == WorkloadMode::Trace ? "trace" : "synthetic") << "\n";
    if (c.mode == WorkloadMode::Trace) {
        std::cout << "    trace_path            = " << c.trace_path << "\n";
    } else {
        std::cout << "    catalog_size          = " << c.synthetic.catalog_size << "\n"
                  << "    zipf_alpha            = " << c.synthetic.zipf_alpha << "\n"
                  << "    num_users             = " << c.synthetic.num_users << "\n"
                  << "    mean_iat_ns           = " << c.synthetic.mean_iat_ns << "\n"
                  << "    mean_file_size        = " << c.synthetic.mean_file_size << "\n";
    }
    std::cout << "    max_requests          = " << c.simulator.max_requests << "\n"
              << "    real_time_pacing      = "
              << (c.simulator.real_time_pacing ? "true" : "false") << "\n"
              << "  consumer:\n"
              << "    batch_size            = " << c.consumer.batch_size << "\n"
              << "    emit_feedback         = "
              << (c.consumer.emit_feedback ? "true" : "false") << "\n"
              << "  ui:\n"
              << "    live_dashboard        = "
              << (c.ui.live_dashboard ? "true" : "false") << "\n"
              << "    refresh_ms            = " << c.ui.refresh_ms << "\n"
              << "    run_seconds           = " << c.ui.run_seconds << "\n";
}

}  // namespace pec
