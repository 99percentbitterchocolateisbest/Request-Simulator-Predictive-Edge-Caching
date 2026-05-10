#pragma once
// TrafficEngine.hpp ────────────────────────────────────────────────────────
// Decides *when* a Request is emitted and *with what statistics* — the
// "Traffic Engine" box. Strategy pattern: the simulator holds a
// std::unique_ptr<TrafficEngine> and never knows whether traffic is being
// replayed from a trace or synthesized.
//
// DESIGN CHOICES
// ──────────────
//
// * Two concrete strategies, both useful and well-grounded:
//
//   1) TraceReplayEngine — wraps a DatasetLoader for deterministic
//      reproducibility. Replay is the gold standard for cache benchmarks
//      (Almeida, Bestavros, Crovella & de Oliveira, "Characterizing
//      reference locality in the WWW", SIGMETRICS 1996, §2: "a trace-
//      driven approach … is the only one that can capture the actual
//      reference patterns of real users").
//
//   2) SyntheticEngine — generates fresh requests from analytic
//      distributions for parameter sweeps and ablations:
//
//        - File popularity ~ Zipf(α) over a catalog of M files.
//          Breslau, Cao, Fan, Phillips & Shenker, "Web caching and
//          Zipf-like distributions: evidence and implications",
//          INFOCOM '99 — measured α ≈ 0.6–0.8 for web objects. We
//          default α = 0.8.
//
//        - Inter-arrival times ~ Exponential(λ) (Poisson process).
//          M/M/1 textbook baseline. We expose the λ knob as
//          mean_iat_ns; a future Pareto mode would address the
//          bursty heavy-tail behaviour reported in Paxson & Floyd,
//          "Wide-area traffic: the failure of Poisson modeling",
//          IEEE/ACM ToN 3(3), 1995. We do not implement Pareto here
//          to keep the reference implementation small, but the
//          interface is designed to admit it cleanly.
//
//        - User assignment uniform over [1, N]. A per-user popularity
//          mixture is straightforward to add; we keep the default
//          minimal so the math in tests stays clean.
//
// * std::mt19937_64 chosen as the PRNG. Mersenne Twister, Matsumoto &
//   Nishimura, "Mersenne twister: a 623-dimensionally equidistributed
//   uniform pseudo-random number generator", ACM ToMACS 8(1), 1998. The
//   _64 variant is preferred for 64-bit IDs to avoid distribution skew.
//   Seeded explicitly for reproducibility — Knuth, *TAOCP Vol. 2*, §3.6:
//   "anyone who attempts to generate random numbers by deterministic
//   means is, of course, living in a state of sin." We embrace that sin
//   and seed deliberately so experiments are repeatable.
//
// * Zipf sampling uses a precomputed CDF. Direct CDF inversion is
//   O(log M) per draw via binary search — fine for catalogs up to ~10⁶.
//   For larger catalogs, switch to Hörmann & Derflinger's rejection
//   method (ACM ToMS 22(4), 1996); not needed at our scale.

#include "DatasetLoader.hpp"
#include "Request.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <vector>

namespace pec {

class TrafficEngine {
public:
    virtual ~TrafficEngine() = default;

    // Produce the next Request, or nullopt if the source is exhausted
    // (only TraceReplayEngine ever returns nullopt; SyntheticEngine is
    // an infinite generator).
    virtual std::optional<Request> generate() = 0;
};

// ──────────────────────────────────────────────────────────────────────
// Trace replay
// ──────────────────────────────────────────────────────────────────────

class TraceReplayEngine final : public TrafficEngine {
public:
    explicit TraceReplayEngine(std::unique_ptr<DatasetLoader> loader);

    std::optional<Request> generate() override;

private:
    std::unique_ptr<DatasetLoader> loader_;
    uint64_t                       reissue_id_ = 0;
};

// ──────────────────────────────────────────────────────────────────────
// Synthetic Zipf + Poisson
// ──────────────────────────────────────────────────────────────────────

struct SyntheticConfig {
    uint64_t catalog_size   = 10'000;     // M  — number of distinct files
    double   zipf_alpha     = 0.80;       // Breslau et al. INFOCOM '99
    uint32_t num_users      = 100;        // N  — UE 1..UE N
    double   mean_iat_ns    = 1'000'000;  // λ  — 1 ms mean inter-arrival
    uint64_t mean_file_size = 1'048'576;  // 1 MiB; lognormal in practice
    uint64_t seed           = 0xC0FFEEULL;
};

class SyntheticEngine final : public TrafficEngine {
public:
    explicit SyntheticEngine(SyntheticConfig cfg = {});

    std::optional<Request> generate() override;

private:
    SyntheticConfig          cfg_;
    std::mt19937_64          rng_;
    std::vector<double>      zipf_cdf_;       // size = catalog_size
    std::exponential_distribution<double>  iat_dist_;
    std::uniform_int_distribution<uint32_t> user_dist_;

    uint64_t next_request_id_ = 1;
    int64_t  current_time_ns_ = 0;

    uint64_t sample_file_id_();
};

}  // namespace pec
