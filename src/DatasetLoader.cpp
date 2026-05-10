// DatasetLoader.cpp ────────────────────────────────────────────────────────
// Implementation notes:
//
// * Parsing uses std::from_chars (C++17, <charconv>). Lemire's 2021 study
//   ("Number parsing at a gigabyte per second", SP&E 51(8)) measured it
//   at 3-5x faster than std::stoull / atoll for the same workload. This
//   matters when streaming 10^9-line traces: parsing time dominates
//   simulator throughput.
//
// * We accept missing fields gracefully (returning std::nullopt) rather
//   than crashing. Real-world traces are messy — MSR Cambridge traces
//   in particular have occasional malformed lines from log rotation
//   races (Narayanan, Donnelly & Rowstron, FAST '08, §3.1).

#include "DatasetLoader.hpp"

#include <charconv>
#include <string_view>
#include <utility>

namespace pec {

namespace {

// Parse the next comma-separated token from `sv` into `out`. On success,
// advance `sv` past the token and its trailing comma (if any) and return
// true. On any malformed input return false.
template <typename Int>
bool consume_int(std::string_view& sv, Int& out) {
    if (sv.empty()) return false;
    auto comma = sv.find(',');
    auto tok = sv.substr(0, comma);
    auto [ptr, ec] = std::from_chars(tok.data(),
                                     tok.data() + tok.size(),
                                     out);
    if (ec != std::errc{}) return false;
    sv.remove_prefix(comma == std::string_view::npos ? sv.size()
                                                     : comma + 1);
    return true;
}

}  // namespace

CsvTraceLoader::CsvTraceLoader(std::string path)
    : path_(std::move(path)), stream_(path_) {}

void CsvTraceLoader::reset() {
    stream_.clear();
    stream_.seekg(0);
}

std::optional<Request> CsvTraceLoader::next() {
    if (!stream_.is_open()) return std::nullopt;

    std::string line;
    while (std::getline(stream_, line)) {
        if (line.empty() || line[0] == '#') continue;

        // Skip an optional header row by looking for non-digit start.
        // (Cheap heuristic — avoids a separate "header_consumed" flag.)
        if (!std::isdigit(static_cast<unsigned char>(line[0]))) continue;

        std::string_view sv{line};
        Request r{};

        // Field-by-field parse. If any field fails we skip the line
        // rather than abort — see header note about messy real traces.
        if (!consume_int(sv, r.request_id)) continue;
        if (!consume_int(sv, r.user_id))    continue;
        if (!consume_int(sv, r.file_id))    continue;
        if (!consume_int(sv, r.size_bytes)) continue;
        if (!consume_int(sv, r.arrival_ns)) continue;

        return r;
    }

    return std::nullopt;
}

}  // namespace pec
