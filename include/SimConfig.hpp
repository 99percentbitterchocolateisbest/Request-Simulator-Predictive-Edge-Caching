#pragma once
// SimConfig.hpp ────────────────────────────────────────────────────────────
// Typed configuration that bundles the SimulatorConfig (hardware knobs)
// with the workload selection and synthetic parameters. Loaded from JSON
// at startup so a sweep across hardware configs is just a directory of
// config files instead of recompilation.
//
// DESIGN CHOICES
// ──────────────
//
// * One flat struct, fields grouped by responsibility. The user-facing
//   knob list is short enough that introducing a class hierarchy would
//   be ceremony for its own sake.
//
// * Defaults match what main.cpp had hard-coded before, so a missing
//   config file or any missing key behaves identically to the prior
//   demo. This makes the JSON path additive: opt-in, never breaking.
//
// * No "schema validation" pass — every key is read with get_or() and
//   a default. Misspelled keys silently fall through to defaults; we
//   print the resolved config at startup so the user sees what was
//   actually applied. Easier to read than a validator's error spew.

#include "RequestSimulator.hpp"
#include "TrafficEngine.hpp"

#include <string>

namespace pec {

enum class WorkloadMode { Synthetic, Trace };

struct UiConfig {
    bool        live_dashboard = true;
    std::size_t refresh_ms     = 100;
    std::size_t run_seconds    = 2;     // demo wall-clock cap
};

struct ConsumerConfig {
    std::size_t batch_size = 128;
    bool        emit_feedback = true;   // run the mock cache controller
    std::size_t feedback_period_us = 50;
};

struct SimConfig {
    SimulatorConfig simulator;       // queue cap, history window, retention
    SyntheticConfig synthetic;       // catalog, alpha, num_users, ...
    ConsumerConfig  consumer;
    UiConfig        ui;

    WorkloadMode mode       = WorkloadMode::Synthetic;
    std::string  trace_path = "data/sample_trace.csv";
    std::string  config_label = "default";  // shown in the dashboard
};

// Load a config JSON file. Missing files / missing keys → defaults.
// Throws std::runtime_error only on JSON syntax errors in a file that
// does exist.
SimConfig load_sim_config(const std::string& path);

// Pretty-print to stderr/stdout for the startup banner.
void print_sim_config(const SimConfig& c);

}  // namespace pec
