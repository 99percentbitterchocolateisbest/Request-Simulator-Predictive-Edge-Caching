// TrafficEngine.cpp ────────────────────────────────────────────────────────

#include "TrafficEngine.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pec {

// ──────────────────────────────────────────────────────────────────────
// TraceReplayEngine
// ──────────────────────────────────────────────────────────────────────

TraceReplayEngine::TraceReplayEngine(std::unique_ptr<DatasetLoader> loader)
    : loader_(std::move(loader)) {}

std::optional<Request> TraceReplayEngine::generate() {
    auto r = loader_->next();
    if (!r) return std::nullopt;
    // Even when the trace records an arrival_ns and a request_id, we
    // overwrite request_id with our own monotonic counter so multiple
    // simulator instances replaying the same trace can be distinguished
    // downstream. arrival_ns is left untouched — that *is* the workload.
    r->request_id = ++reissue_id_;
    return r;
}

// ──────────────────────────────────────────────────────────────────────
// SyntheticEngine
// ──────────────────────────────────────────────────────────────────────

SyntheticEngine::SyntheticEngine(SyntheticConfig cfg)
    : cfg_(cfg),
      rng_(cfg.seed),
      iat_dist_(1.0 / cfg.mean_iat_ns),
      user_dist_(1, cfg.num_users == 0 ? 1 : cfg.num_users) {

    // Precompute the Zipf CDF.
    //
    //   P(file = k) = (1/k^α) / H_{M,α},  k = 1..M
    //   H_{M,α}    = Σ_{j=1..M} 1/j^α            (generalized harmonic)
    //
    // This is the discrete analogue of Zipf with finite support, often
    // called Zipf-Mandelbrot in the limit. We store the CDF so each
    // sample is an O(log M) binary search via std::lower_bound.
    zipf_cdf_.resize(cfg.catalog_size);
    double H = 0.0;
    for (uint64_t k = 1; k <= cfg.catalog_size; ++k) {
        H += 1.0 / std::pow(static_cast<double>(k), cfg.zipf_alpha);
        zipf_cdf_[k - 1] = H;
    }
    // Normalize.
    for (auto& v : zipf_cdf_) v /= H;
}

uint64_t SyntheticEngine::sample_file_id_() {
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const double x = u(rng_);
    // First index with cdf[idx] >= x  →  rank ∈ [1, M]
    auto it = std::lower_bound(zipf_cdf_.begin(), zipf_cdf_.end(), x);
    auto idx = static_cast<uint64_t>(it - zipf_cdf_.begin());
    return idx + 1;  // file IDs are 1-indexed
}

std::optional<Request> SyntheticEngine::generate() {
    Request r{};
    r.request_id = next_request_id_++;
    r.user_id    = user_dist_(rng_);
    r.file_id    = sample_file_id_();

    // Lognormal-ish file sizes around the configured mean. Lognormal is
    // the canonical distribution for web object sizes (Crovella &
    // Bestavros, "Self-similarity in World Wide Web traffic", IEEE/ACM
    // ToN 5(6), 1997). We approximate with mean = cfg.mean_file_size,
    // sigma = 1.0 in log space — close enough for a reference impl.
    const double mu = std::log(static_cast<double>(cfg_.mean_file_size));
    std::lognormal_distribution<double> size_dist(mu, 1.0);
    double sz = size_dist(rng_);
    if (sz < 1.0) sz = 1.0;
    r.size_bytes = static_cast<uint64_t>(sz);

    // Advance simulation time by an exponential inter-arrival sample.
    // Exponential inter-arrivals ⇔ Poisson process. Cinlar,
    // *Introduction to Stochastic Processes* (1975), Thm. 4.3.
    const double iat = iat_dist_(rng_);
    current_time_ns_ += static_cast<int64_t>(iat);
    r.arrival_ns = current_time_ns_;

    return r;
}

}  // namespace pec
