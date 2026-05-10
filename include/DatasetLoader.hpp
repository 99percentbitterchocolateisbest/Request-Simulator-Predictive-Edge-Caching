#pragma once
// DatasetLoader.hpp ────────────────────────────────────────────────────────
// Streams Request objects off disk one at a time. The "Dataset Loader" box
// in the architecture diagram.
//
// DESIGN CHOICES
// ──────────────
//
// * Abstract base class with virtual next() / reset(). Real cache-research
//   traces come in many formats: MSR Cambridge (Narayanan, Donnelly &
//   Rowstron, FAST '08) is CSV-like; YouTube (Cha, Kwak, Rodriguez, Ahn &
//   Moon, IMC '07) is columnar; some labs use binary for speed. A virtual
//   interface lets us swap loaders without recompiling the rest of the
//   simulator — Open/Closed Principle (Meyer, *Object-Oriented Software
//   Construction*, 1988, ch. 3).
//
// * std::optional<Request> for next(). End-of-stream is a normal,
//   expected condition, not an error — std::optional is the C++17 idiom
//   for "maybe a value" (Sutter, "GotW #94", 2014). Throwing would be
//   wrong because reaching EOF is not exceptional.
//
// * Streaming, not slurping. We read one line at a time via getline().
//   Required because the Wikipedia 2007 trace and similar production
//   traces exceed RAM. Trade-off: per-line parse cost; mitigated below
//   by std::from_chars (Lemire, "Number parsing at a gigabyte per
//   second", Software: Practice & Experience 51(8), 2021).

#include "Request.hpp"

#include <fstream>
#include <optional>
#include <string>

namespace pec {

class DatasetLoader {
public:
    virtual ~DatasetLoader() = default;

    // Returns the next Request, or std::nullopt if the trace is exhausted.
    virtual std::optional<Request> next() = 0;

    // Rewinds the underlying source so next() restarts from the beginning.
    // Useful for warm-up runs and multi-epoch evaluation.
    virtual void reset() = 0;

    // Optional hint of total record count; 0 means unknown.
    virtual std::size_t size_hint() const { return 0; }
};

// CSV trace loader.
//
// Expected format (one record per line, header row optional):
//     request_id,user_id,file_id,size_bytes,arrival_ns
//
// Lines beginning with '#' are treated as comments and skipped — a
// convention borrowed from the SNIA IOTTA trace repository.
class CsvTraceLoader : public DatasetLoader {
public:
    explicit CsvTraceLoader(std::string path);

    std::optional<Request> next() override;
    void                   reset() override;

private:
    std::string   path_;
    std::ifstream stream_;
};

}  // namespace pec
