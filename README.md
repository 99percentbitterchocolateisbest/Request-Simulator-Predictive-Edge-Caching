# Request Simulator — Predictive Edge Caching (CSE 476/575, Spring 2026)

C++17 reference implementation of the **Request Simulator** block in the
*Predictive Edge Caching* architecture: the upstream half of the 5G/6G
edge server that feeds the ML Predictor with a `request_stream` and a
`history` window, and receives a `cache response / feedback loop` from
the Edge Cache Controller.

## Architecture mapping

| Box in diagram        | Code                                                                       |
| --------------------- | -------------------------------------------------------------------------- |
| User Equipment        | (external; modeled inside `TrafficEngine`)                                 |
| Dataset Loader        | `DatasetLoader` interface, `CsvTraceLoader` impl                           |
| Traffic Engine        | `TrafficEngine` interface, `TraceReplayEngine` and `SyntheticEngine` impls |
| I/O Optimizer         | `BoundedQueue<Request>` + `HistoryBuffer`                                  |
| `request_stream` out  | `RequestSimulator::pop_request_stream()`                                   |
| `history` out         | `RequestSimulator::get_history()`                                          |
| feedback loop in      | `RequestSimulator::submit_feedback()`                                      |

## Key design choices

| Choice                                      | Reason / citation                                                                                         |
|---------------------------------------------|-----------------------------------------------------------------------------------------------------------|
| `Request` is a trivially-copyable POD       | Stroustrup, *The C++ Programming Language* 4e §8.2.6 — POD types compose with lock-free queues and memcpy |
| `Feedback` lives outside `Request`          | keeps `Request` POD; closes the diagram's feedback loop without a circular dep                            |
| Strategy pattern for `TrafficEngine`        | Open/Closed Principle — Meyer, *OO Software Construction* (1988)                                          |
| Trace replay is the gold standard           | Almeida, Bestavros, Crovella, de Oliveira, "Characterizing reference locality in the WWW", SIGMETRICS '96 |
| Zipf(α=0.8) for synthetic file popularity   | Breslau, Cao, Fan, Phillips, Shenker, "Web caching and Zipf-like distributions", INFOCOM '99              |
| Poisson inter-arrivals                      | M/M/1 baseline; Çinlar, *Introduction to Stochastic Processes* (1975), Thm. 4.3                           |
| Lognormal file sizes                        | Crovella & Bestavros, "Self-similarity in WWW traffic", IEEE/ACM ToN 5(6), 1997                           |
| Mersenne Twister `mt19937_64`               | Matsumoto & Nishimura, ACM ToMACS 8(1), 1998                                                              |
| `std::from_chars` parsing                   | Lemire, "Number parsing at a gigabyte per second", SP&E 51(8), 2021                                       |
| Streaming dataset loader                    | trace sizes (Wikipedia 2007 ≈ 10⁹ rows — Urdaneta et al., Comp. Networks 53(11), 2009) exceed RAM         |
| Bounded queue backpressure                  | Reactive Manifesto (2014); also classic discrete-event SIM textbook (Banks et al. 2010 §1.4)              |
| Producer/consumer with mutex+CV             | clarity over Lamport ToPLAS '83 lock-free SPSC; same API, drop-in upgrade path                            |
| Batched dequeue (`pop_batch`)               | mutex-acquire amortization (Hennessy & Patterson 6e §5.10); aligns with mini-batch ML consumer grain      |
| HistoryBuffer = circular working-set window | Denning, "The working set model for program behavior", CACM 11(5), 1968                                   |
| Simulation time, not wall-clock             | Banks, Carson, Nelson, Nicol, *Discrete-Event System Simulation* 5e (2010) §1.2                           |
| Bounded feedback retention                  | prevents unbounded RSS growth on long simulations — engineering necessity, not from the literature        |
| Two-phase shutdown (set flag, close, join)  | found by running it: thread destructors that run while joinable call `std::terminate`                     |
| Hand-rolled JSON parser, no nlohmann/RapidJSON | preserves the stdlib + pthreads dependency surface; ~250 lines is cheaper than vendoring a header       |
| `std::variant` for `JsonValue`              | C++17 sum-type idiom; safer than tagged-union by hand                                                     |
| Single `if constexpr` template for `get_or<T>` | on x86_64 Linux `size_t` ≡ `uint64_t`; explicit specializations would collide                           |
| Resolved config printed at startup          | misspelled keys silently fall through to defaults; printing makes that visible without a separate validator |
| ANSI escapes, no ncurses/termbox            | every modern terminal supports CSI; zero new build deps                                                   |
| Render-then-cursor-up frame strategy        | technique used in `top` / `htop`; avoids `\033[2J` flicker on slow terminals                              |
| 1-second sliding throughput window with cumulative fallback | windowed rate is more diagnostic than cumulative; fallback prevents 0 req/s on sub-second runs |
| Consumer-side counters live outside `RequestSimulator` | the simulator's contract (the diagram) doesn't include the predictor — its counters belong to its owner |

## Configuration

Hardware and workload knobs are loaded from a JSON file at startup so a
config sweep is a directory of files, not a recompile. Defaults match
the prior hard-coded values exactly, so the JSON path is purely
additive — missing files or missing keys behave identically to the old
demo.

| Section              | Keys                                                                                            | Maps to                       |
|----------------------|-------------------------------------------------------------------------------------------------|-------------------------------|
| `hardware`           | `stream_queue_capacity`, `history_window`, `feedback_retention`                                 | `SimulatorConfig`             |
| `workload`           | `mode` (`synthetic`\|`trace`), `trace_path`, `max_requests`, `real_time_pacing`                 | `SimulatorConfig` + engine    |
| `workload.synthetic` | `catalog_size`, `zipf_alpha`, `num_users`, `mean_iat_ns`, `mean_file_size`, `seed`              | `SyntheticConfig`             |
| `consumer`           | `batch_size`, `emit_feedback`, `feedback_period_us`                                             | mock ML / cache controller    |
| `ui`                 | `live_dashboard`, `refresh_ms`, `run_seconds`                                                   | `LiveDashboard` + run cap     |

Three sample configs ship in `data/`:

| File                       | Hardware                                | Use                                  |
|----------------------------|-----------------------------------------|--------------------------------------|
| `sim_config.json`          | queue=2048, history=128                 | default demo                         |
| `sim_config_tiny.json`     | queue=32, history=16, batch=4           | exercises backpressure / saturation  |
| `sim_config_fat.json`      | queue=64k, history=2048, batch=512      | high-throughput run (~1.16 Mreq/s)   |

## Build & run

```bash
# CMake (preferred)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

./build/request_sim                              # default config: data/sim_config.json
./build/request_sim data/sim_config_fat.json     # explicit config
./build/request_sim trace data/sample_trace.csv  # legacy CLI: trace replay shorthand

# Or directly with g++ (C++17, pthreads)
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow \
    -Iinclude src/*.cpp -pthread -o request_sim
```

Demo output (synthetic mode, 50k requests):

```
[mode] synthetic Zipf(α=0.8) + Poisson
[stats] produced  = 50000
[stats] consumed  = 50000
[stats] dropped   = 0
[stats] history   = 128 / 128
```

Verified statistically: empirical Zipf rank-1 mass = 0.1235 vs theoretical
0.1229 over 200 000 samples (α = 0.8, M = 100); ratio P(1)/P(2) measured
1.750 vs 2^0.8 ≈ 1.741.

## File layout

```
include/
  Request.hpp             POD request + feedback types
  DatasetLoader.hpp       streaming-loader interface + CSV impl
  TrafficEngine.hpp       Strategy interface + TraceReplay + Synthetic
  IOOptimizer.hpp         BoundedQueue + HistoryBuffer
  RequestSimulator.hpp    top-level orchestrator
  Json.hpp                minimal recursive-descent JSON parser
  SimConfig.hpp           typed config struct + JSON loader
  LiveDashboard.hpp       ANSI-based live terminal UI
src/
  DatasetLoader.cpp
  TrafficEngine.cpp
  RequestSimulator.cpp
  SimConfig.cpp
  main.cpp                end-to-end demo with mock predictor + cache ctrl
data/
  sample_trace.csv
  sim_config.json         default (queue=2048, history=128)
  sim_config_tiny.json    constrained hardware (queue=32, history=16)
  sim_config_fat.json     generous hardware (queue=64k, history=2048)
CMakeLists.txt
```
