#pragma once
// Request.hpp ──────────────────────────────────────────────────────────────
// Canonical data type that every component in the architecture passes
// around. Lives in the `pec` (predictive edge caching) namespace.
//
// DESIGN CHOICES
// ──────────────
//
// * Plain-old-data struct — no virtuals, no atomics, trivially copyable.
//   Rationale: a POD type composes freely with lock-free queues and
//   binary serialization. Stroustrup, *The C++ Programming Language*,
//   4th ed. (2013), §8.2.6: "trivially copyable types … may be safely
//   copied with std::memcpy." If we embedded an std::atomic for cache
//   feedback inside the Request, the type would lose copy-constructibility
//   and could not sit inside our SPSC ring buffer. Feedback is therefore
//   a *separate* type keyed by request_id (see Feedback below).
//
// * 64-bit IDs everywhere. The Wikipedia 2007 request trace alone has
//   ≈10⁹ requests (Urdaneta, Pierre & van Steen, "Wikipedia workload
//   analysis for decentralized hosting", Computer Networks 53(11), 2009).
//   32-bit IDs would overflow on a multi-day simulation.
//
// * arrival_ns measures *simulation time*, not wall-clock. Standard
//   discrete-event-simulation practice — Banks, Carson, Nelson & Nicol,
//   *Discrete-Event System Simulation*, 5th ed. (2010), §1.2. Lets us
//   run faster-than-real-time without distorting inter-arrival statistics.
//
// * size_bytes is first-class because the Edge Cache Controller's
//   admission policy needs it (variable-size object caching — Cherkasova,
//   "Improving WWW proxies performance with Greedy-Dual-Size-Frequency
//   caching policy", HP Labs Tech. Report, 1998).
//
// * user_id is first-class because per-user embeddings are common in
//   modern ML predictors (Narayanan et al., "DeepCache: A deep learning
//   based framework for content caching", NetAI '18 @ SIGCOMM 2018).
//   Burying it in a metadata blob would force every consumer to reparse.

#include <cstdint>

namespace pec {

struct Request {
    uint64_t request_id;   // monotonically increasing, globally unique
    uint32_t user_id;      // identifies UE 1..UE N
    uint64_t file_id;      // content identifier
    uint64_t size_bytes;   // payload size; needed for size-aware caching
    int64_t  arrival_ns;   // simulation-time arrival in nanoseconds
};

enum class CacheOutcome : uint8_t {
    PENDING = 0,
    HIT     = 1,
    MISS    = 2
};

// Feedback is intentionally a *separate* type so the Request itself stays
// trivially copyable. The Edge Cache Controller posts these back through
// the feedback loop in the architecture diagram; the simulator stores
// them in an unordered_map<request_id, Feedback>.
struct Feedback {
    uint64_t     request_id;
    CacheOutcome outcome;
    int64_t      served_ns;   // simulation time at which the cache responded
};

}  // namespace pec
