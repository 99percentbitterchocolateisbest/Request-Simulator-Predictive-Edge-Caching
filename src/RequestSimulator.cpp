// RequestSimulator.cpp ─────────────────────────────────────────────────────

#include "RequestSimulator.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace pec {

RequestSimulator::RequestSimulator(std::unique_ptr<TrafficEngine> engine,
                                   SimulatorConfig                cfg)
    : engine_(std::move(engine)),
      cfg_(cfg),
      stream_(cfg.stream_queue_capacity),
      history_(cfg.history_window) {}

RequestSimulator::~RequestSimulator() {
    stop();
}

void RequestSimulator::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    producer_ = std::thread([this]{ producer_loop_(); });
}

void RequestSimulator::stop() {
    // We must join the producer thread on every stop(), even if it
    // already finished naturally (e.g., trace exhausted or max_requests
    // hit). If we returned early when running_ is already false, the
    // std::thread destructor would later run on a still-joinable thread
    // and call std::terminate. Two-phase shutdown:
    //   (1) flip running_ to false and close the stream so any in-flight
    //       producer iteration exits its loop cleanly,
    //   (2) join unconditionally if joinable.
    running_.store(false);
    stream_.close();
    if (producer_.joinable()) producer_.join();
}

void RequestSimulator::producer_loop_() {
    using clock = std::chrono::steady_clock;
    const auto wall_start = clock::now();
    int64_t sim_start_ns = -1;

    while (running_.load(std::memory_order_relaxed)) {
        auto maybe_req = engine_->generate();
        if (!maybe_req) break;                       // trace exhausted
        Request req = *maybe_req;

        // Real-time pacing: sleep until wall-clock catches the request's
        // simulated arrival time. We anchor on the first request's
        // arrival_ns so simulations whose traces start at t = 10^9 don't
        // sleep for half an hour before emitting their first record.
        if (cfg_.real_time_pacing) {
            if (sim_start_ns < 0) sim_start_ns = req.arrival_ns;
            const auto target = wall_start +
                std::chrono::nanoseconds(req.arrival_ns - sim_start_ns);
            std::this_thread::sleep_until(target);
        }

        // Feed both downstream channels. History is updated first so a
        // consumer reacting to the stream will see the request already
        // present in the working set window.
        history_.record(req);
        if (!stream_.push(req)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            break;  // queue closed
        }
        produced_.fetch_add(1, std::memory_order_relaxed);

        if (cfg_.max_requests &&
            produced_.load() >= cfg_.max_requests) break;
    }

    stream_.close();
    running_.store(false);
}

std::size_t RequestSimulator::pop_request_stream(std::vector<Request>& out,
                                                 std::size_t max) {
    return stream_.pop_batch(out, max);
}

std::vector<Request> RequestSimulator::get_history() const {
    return history_.snapshot();
}

void RequestSimulator::submit_feedback(const Feedback& fb) {
    std::lock_guard<std::mutex> lk(fb_m_);
    auto [it, inserted] = feedback_.insert_or_assign(fb.request_id, fb);
    if (inserted) fb_order_.push_back(fb.request_id);
    purge_old_feedback_locked_();
}

std::optional<Feedback>
RequestSimulator::lookup_feedback(uint64_t request_id) const {
    std::lock_guard<std::mutex> lk(fb_m_);
    auto it = feedback_.find(request_id);
    if (it == feedback_.end()) return std::nullopt;
    return it->second;
}

void RequestSimulator::purge_old_feedback_locked_() {
    // Bounded-memory invariant: keep only the most recent N feedback
    // records. Simple FIFO eviction by insertion order. This trades
    // exact lookups of very old request IDs (which the ML Predictor
    // shouldn't be asking about anyway) for predictable RSS.
    while (fb_order_.size() > cfg_.feedback_retention) {
        feedback_.erase(fb_order_.front());
        fb_order_.erase(fb_order_.begin());
    }
}

}  // namespace pec
