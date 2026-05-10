#pragma once
// LiveDashboard.hpp ────────────────────────────────────────────────────────
// Tiny ANSI-escape-driven terminal dashboard. Header-only.
//
// DESIGN CHOICES
// ──────────────
//
// * No ncurses, no termbox, no external TUI library. We just emit a
//   small set of well-supported ANSI sequences (cursor up, erase line)
//   that work on every modern Linux/macOS terminal and on Windows
//   Terminal / VS Code's integrated terminal. This keeps the project's
//   dependency surface unchanged (still: stdlib + pthreads).
//
// * "Render frame, jump cursor up, render again" instead of full screen
//   clears. Full clear (\033[2J) flickers on slow terminals; per-line
//   redraw is steady. Same technique used in `top` and `htop`.
//
// * The dashboard reads stats from the simulator + an external
//   DashboardStats struct holding the consumer's atomics. We don't put
//   the consumer's counters inside RequestSimulator because the
//   simulator's contract (the diagram) doesn't include the predictor —
//   the simulator only owns its own producer-side counts.
//
// * Throughput is a sliding 1-second window: (produced_now -
//   produced_one_second_ago) / 1 sec. More useful than a cumulative
//   rate for spotting hardware-config-induced stalls.

#include "RequestSimulator.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

namespace pec {

struct DashboardStats {
    std::atomic<std::size_t> consumed{0};
    std::atomic<std::size_t> feedback_count{0};
    std::atomic<std::size_t> feedback_hits{0};
};

class LiveDashboard {
public:
    LiveDashboard(const RequestSimulator& sim,
                  const DashboardStats&   stats,
                  std::string             mode_label,
                  std::string             config_label,
                  std::size_t             queue_capacity,
                  std::size_t             history_capacity,
                  std::size_t             max_requests)
        : sim_(sim), stats_(stats),
          mode_label_(std::move(mode_label)),
          config_label_(std::move(config_label)),
          queue_cap_(queue_capacity),
          history_cap_(history_capacity),
          max_requests_(max_requests) {}

    // Render frames at `period` until *stop becomes true. Designed to
    // run on its own thread.
    void run(std::atomic<bool>& stop, std::chrono::milliseconds period) {
        const auto t0 = std::chrono::steady_clock::now();
        produced_window_ = sim_.produced();
        window_t0_       = t0;

        // Initial blank lines so the first frame's cursor-up has room
        // to land. We always render exactly H lines; H is fixed.
        for (int i = 0; i < kHeight; ++i) std::cout << '\n';

        while (!stop.load()) {
            auto now = std::chrono::steady_clock::now();
            double elapsed_s =
                std::chrono::duration<double>(now - t0).count();
            render(elapsed_s);
            std::this_thread::sleep_for(period);
        }
        // Final frame — capture end-of-run state.
        auto now = std::chrono::steady_clock::now();
        double elapsed_s = std::chrono::duration<double>(now - t0).count();
        render(elapsed_s);
        std::cout << "\n" << std::flush;
    }

private:
    static constexpr int kHeight = 13;
    static constexpr int kWidth  = 64;

    void render(double elapsed_s) {
        // Throughput: 1-second sliding window once we've accumulated a
        // second; cumulative produced/elapsed before that. Avoids the
        // dashboard showing 0 req/s on short or fast runs.
        std::size_t now_produced = sim_.produced();
        auto now = std::chrono::steady_clock::now();
        double window_s = std::chrono::duration<double>(now - window_t0_).count();
        double throughput;
        if (window_s >= 1.0) {
            throughput = (now_produced - produced_window_) / window_s;
            produced_window_ = now_produced;
            window_t0_       = now;
            last_throughput_ = throughput;
        } else if (elapsed_s > 0.05) {
            throughput = now_produced / elapsed_s;  // cumulative fallback
        } else {
            throughput = last_throughput_;
        }

        std::size_t consumed = stats_.consumed.load();
        std::size_t dropped  = sim_.dropped();
        std::size_t fb_total = stats_.feedback_count.load();
        std::size_t fb_hits  = stats_.feedback_hits.load();
        std::size_t hist_now = sim_.get_history().size();

        double hit_rate = fb_total ? (100.0 * fb_hits / fb_total) : 0.0;

        // Cursor-up to top of the dashboard region, then redraw.
        std::cout << "\033[" << kHeight << "A";

        line_top();
        line_kv("Predictive Edge Caching — Request Simulator", "");
        line_sep();
        line_kv("Config",      config_label_);
        line_kv("Mode",        mode_label_);
        line_kv("Hardware",
                "queue=" + std::to_string(queue_cap_) +
                " history=" + std::to_string(history_cap_));
        line_sep();
        line_kv("Produced",    fmt_count(now_produced));
        line_kv("Consumed",    fmt_progress(consumed, now_produced));
        line_kv("Dropped",     fmt_count(dropped));
        line_kv("Throughput",  fmt_rate(throughput));
        line_kv("History",     fmt_progress(hist_now, history_cap_));
        line_kv("Hit rate",    fmt_pct(hit_rate, fb_total));
        line_kv("Elapsed",     fmt_secs(elapsed_s) +
                               (max_requests_
                                  ? "  (target " + fmt_count(max_requests_) + " req)"
                                  : ""));
        std::cout << std::flush;
    }

    void line_top()  { print_line(""); }
    void line_sep()  { print_line(std::string(kWidth - 2, '-')); }

    void line_kv(const std::string& k, const std::string& v) {
        std::string body;
        if (v.empty()) {
            // Title line: centered.
            int pad_left = (kWidth - 2 - (int)k.size()) / 2;
            if (pad_left < 0) pad_left = 0;
            body = std::string(pad_left, ' ') + k;
        } else {
            body = k + std::string(12 - (int)std::min<size_t>(k.size(), 12), ' ')
                 + ": " + v;
        }
        print_line(body);
    }

    void print_line(const std::string& body) {
        std::cout << "\033[2K";    // erase entire current line
        std::cout << "  ";
        std::cout << body;
        // pad to width to overwrite any prior longer content
        int n = (int)body.size();
        if (n + 2 < kWidth) std::cout << std::string(kWidth - 2 - n, ' ');
        std::cout << "\n";
    }

    // ── formatters ───────────────────────────────────────────────────
    static std::string fmt_count(std::size_t n) {
        // Thousands separators using locale-free manual insertion.
        std::string s = std::to_string(n);
        for (int p = (int)s.size() - 3; p > 0; p -= 3) s.insert(p, ",");
        return s;
    }
    static std::string fmt_rate(double rps) {
        char buf[64];
        if (rps >= 1e6) std::snprintf(buf, sizeof buf, "%.2f Mreq/s", rps / 1e6);
        else if (rps >= 1e3) std::snprintf(buf, sizeof buf, "%.2f kreq/s", rps / 1e3);
        else std::snprintf(buf, sizeof buf, "%.0f req/s", rps);
        return buf;
    }
    static std::string fmt_secs(double s) {
        char buf[32]; std::snprintf(buf, sizeof buf, "%.1fs", s); return buf;
    }
    std::string fmt_progress(std::size_t cur, std::size_t total) {
        std::string s = fmt_count(cur) + " / " + fmt_count(total) + "  ";
        s += bar(cur, total, 24);
        return s;
    }
    static std::string fmt_pct(double pct, std::size_t denom) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%5.1f%%  (n=%zu)", pct, denom);
        return buf;
    }
    static std::string bar(std::size_t cur, std::size_t total, int width) {
        if (!total) return std::string(width, '.');
        int filled = (int)((double)cur / (double)total * width);
        if (filled > width) filled = width;
        if (filled < 0)     filled = 0;
        return "[" + std::string(filled, '#') +
               std::string(width - filled, '.') + "]";
    }

    const RequestSimulator& sim_;
    const DashboardStats&   stats_;
    std::string mode_label_;
    std::string config_label_;
    std::size_t queue_cap_;
    std::size_t history_cap_;
    std::size_t max_requests_;

    // Throughput window state.
    std::size_t produced_window_ = 0;
    std::chrono::steady_clock::time_point window_t0_;
    double last_throughput_ = 0.0;
};

}  // namespace pec
