#pragma once
// RequestSimulator.hpp ─────────────────────────────────────────────────────
// Top-level orchestrator. Owns the Dataset Loader (indirectly, via the
// chosen TrafficEngine), the I/O Optimizer (BoundedQueue + HistoryBuffer),
// and runs the producer thread that emits the request_stream channel.
//
// EXTERNAL CONTRACT (matches the architecture diagram exactly)
// ────────────────────────────────────────────────────────────
//
//   IN  ← User Equipment file requests   (delivered by the TrafficEngine,
//                                         which encapsulates the UE model)
//
//   OUT → request_stream  to ML Predictor       — pop_request_stream()
//   OUT → history         to ML Predictor       — get_history()
//
//   IN  ← cache response / feedback loop from
//         the Edge Cache Controller             — submit_feedback()
//
// DESIGN CHOICES
// ──────────────
//
// * The simulator is decoupled from both the ML Predictor and the Cache
//   Controller — it never *calls* them; it only exposes endpoints they
//   can poll/post into. Dependency Inversion (Martin, "The Dependency
//   Inversion Principle", C++ Report, 1996). This is what makes the
//   feedback loop in the diagram an actual loop and not a tight coupling.
//
// * The producer thread sleeps to honor inter-arrival timing only when
//   real_time_pacing_ is true. For batch experiments we run as fast as
//   possible; for live demos we pace to wall-clock. Discrete-event
//   simulators standardly support both modes (Banks et al. 2010, §1.4).
//
// * Feedback is stored in an unordered_map<request_id, Feedback>. We
//   purge entries older than a configurable retention horizon to keep
//   memory bounded — without a horizon, long simulations leak.

#include "IOOptimizer.hpp"
#include "Request.hpp"
#include "TrafficEngine.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pec {

struct SimulatorConfig {
    std::size_t stream_queue_capacity = 4096;   // backpressure threshold
    std::size_t history_window        = 256;    // K — feature window size
    std::size_t feedback_retention    = 65536;  // max feedback entries
    bool        real_time_pacing      = false;  // honor arrival_ns vs wall
    std::size_t max_requests          = 0;      // 0 = run forever / EOT
};

class RequestSimulator {
public:
    RequestSimulator(std::unique_ptr<TrafficEngine> engine,
                     SimulatorConfig               cfg = {});
    ~RequestSimulator();

    // Non-copyable, non-movable: owns a std::thread.
    RequestSimulator(const RequestSimulator&)            = delete;
    RequestSimulator& operator=(const RequestSimulator&) = delete;

    // Lifecycle.
    void start();
    void stop();

    // ── ML Predictor side ────────────────────────────────────────────
    //
    // Pop up to `max` requests off the request_stream. Blocks if the
    // queue is empty until either at least one item arrives or the
    // simulator is stopped (in which case the returned count may be 0).
    std::size_t pop_request_stream(std::vector<Request>& out,
                                   std::size_t max);

    // Snapshot of the last K requests, in chronological order.
    std::vector<Request> get_history() const;

    // ── Edge Cache Controller side ───────────────────────────────────
    //
    // Post a feedback record back into the simulator. Thread-safe.
    void submit_feedback(const Feedback& fb);

    // Optional: query the recorded outcome for a specific request.
    // Returns nullopt if the record has been purged or never arrived.
    std::optional<Feedback> lookup_feedback(uint64_t request_id) const;

    // ── Observability ────────────────────────────────────────────────
    std::size_t produced() const { return produced_.load(); }
    std::size_t dropped() const  { return dropped_.load(); }

private:
    void producer_loop_();
    void purge_old_feedback_locked_();

    std::unique_ptr<TrafficEngine> engine_;
    SimulatorConfig                cfg_;

    BoundedQueue<Request>          stream_;
    HistoryBuffer                  history_;

    mutable std::mutex                                    fb_m_;
    std::unordered_map<uint64_t, Feedback>                feedback_;
    std::vector<uint64_t>                                 fb_order_;

    std::thread        producer_;
    std::atomic<bool>  running_{false};
    std::atomic<std::size_t> produced_{0};
    std::atomic<std::size_t> dropped_{0};
};

}  // namespace pec
