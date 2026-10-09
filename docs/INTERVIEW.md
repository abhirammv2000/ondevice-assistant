# Where each topic lives in this project

For each topic: where it is in the code, what to say about it, and what the limit is. The limits matter. Saying
"here is what this does not show" is part of a good answer.

## Operating systems

**Memory-mapped files.** `core/src/mapped_file.cpp` uses `mmap` on POSIX and `CreateFileMapping` on Windows. The kernel
loads only touched pages, pages come from the page cache and are shared between processes, and clean pages can be
dropped under memory pressure because they can be re-read. A heap copy gets none of that. Measured in
`docs/PERFORMANCE.md`: with a warm cache, mapping is not faster than copying (0.40 ms against 0.06 ms for a 1.2 MB
file, because the system calls dominate). The benefit is memory behaviour, not load time. I did not measure resident
memory under pressure, so I do not claim a number for it.

**Alignment.** int8 weights start on a 64-byte boundary (`model.hpp`). `Model::from_bytes` copies into a 64-byte aligned
buffer, and the loader rejects an unaligned section with `BadLayout` instead of reading it.

**Cache lines and false sharing.** `SpscRing` keeps `head_` and `tail_` on separate cache lines (`spsc_ring.hpp`).
Limit: on my 2-vCPU test VM the padded ring was not measurably faster than one without padding (217 against 209 million
items per second). I keep the design because it is the standard one, and I say the measurement did not show it.

**Memory ordering.** `SpscRing` uses a release store to publish an element and an acquire load to see it. TSan accepts
it. As a one-off check I weakened one store to relaxed and TSan reported the race, so I know the tests would catch
the mistake. That check is not kept in the repository.

**Processes, threads, scheduling.** `AssistService` is a fixed worker pool. Each worker is a `std::thread` with its own
forked `Engine`. Limit: I did not use priorities, QoS classes or real-time scheduling, which a real audio path would.

**What I would say about Apple platforms specifically.** The code is portable C++20 with no platform calls beyond the
file mapping. I avoided `std::jthread` and `std::to_chars(double)` because they came late to Apple's libc++. The Swift
package builds on Linux here and the CI file runs it on macOS; I have not run it on Apple hardware.

## Data structures and algorithms

| Topic | Where | Notes |
|---|---|---|
| Hash function | `hash.hpp` | FNV-1a 64 bit, folded to 32. Known test vectors in `tests/py/test_features_parity.py`. |
| Hashing trick | `tokenizer.cpp` | Fixed-size feature space, collisions accepted. The sweep in `size_sweep.json` shows what the collisions cost. |
| Trie | `fuzzy.cpp` | Child/sibling arrays, no per-node allocation. Finds words by prefix. |
| Edit distance (DP) | `fuzzy.cpp` | bounded Damerau-Levenshtein (a swapped pair counts as one edit) over the distinct words, with a cutoff: it stops as soon as the distance must exceed the limit. 5 to 6 times faster than the full table (`PERFORMANCE.md`). |
| Phonetic matching | `fuzzy.cpp` | Soundex codes, so names that sound alike are candidates even when the spelling is far off. |
| LRU cache | `lru.hpp` | Open addressing with backward-shift deletion (Knuth 6.4 algorithm R), a doubly linked list by index. Preallocated, so a full cache never allocates. About 4x faster than list plus `unordered_map`. |
| Ring buffer | `spsc_ring.hpp` | Power-of-two capacity so an index wraps with a mask. |
| Bounded queue | `bounded_queue.hpp` | Mutex and two condition variables, with `close()` for shutdown. |
| Shunting-yard style evaluation | `calc.cpp` | Two stacks (values, operators) for spoken arithmetic with precedence. Both are fixed-size arrays sized from the lexer's token limit, so long input cannot overflow them. |
| Calendar arithmetic | `calendar.cpp` | Days-from-civil and civil-from-days (Howard Hinnant's algorithms) in integer math. |
| Softmax | `model.cpp` | Subtract the max first so `exp` cannot overflow. |
| Sort and unique | `tokenizer.cpp` | Features are sorted and de-duplicated so the same text always gives the same list. |

Complexity answers: scoring is O(f x c) with f features (at most 194) and c classes (150). Contact lookup compares the
query with the words that share a prefix or a sound, not with every name. Comparing with every name costs O(n x name
length squared) and measured 5 to 6 times slower at 1,000 and 10,000 names.

## Object-oriented design

- **One responsibility per class.** `Model` (loads and scores), `Engine` (decides), `FuzzyIndex` (finds names),
  `Redactor` (removes personal data), `AssistService` (runs many requests), `StreamSession` (handles growing text).
- **Dependency injection for time.** `Engine` takes a `Clock`. Tests pass a `FixedClock`, the app passes `SystemClock`.
  That is how "next friday" can be tested exactly.
- **Value semantics and sharing.** `Engine::fork()` copies the small mutable parts and shares the big immutable parts
  through a `shared_ptr<const Shared>`. `set_contacts` builds a new shared state, so engines forked earlier keep
  the list they had (copy on write).
- **RAII.** `MappedFile`, `ScopedAllocCount`, and the service destructor joining its threads. No owning raw pointers.
- **Interface boundary.** `assist.h` hides the C++ types behind opaque handles, so a Swift or Java caller never sees them.
- Limit: handlers are a table of function pointers (`kHandlers` in `engine.cpp`), not a class hierarchy. I chose that
  because there is no per-handler state, and a hierarchy would add virtual calls for no gain.

## Concurrency

- Four tools, each used where it fits: a lock-free ring for one producer and one consumer; a mutex queue for the worker
  pool; atomics for flags and counters; one mutex per C engine handle so a caller cannot corrupt it.
- **Backpressure.** `submit()` returns `Busy` on a full queue. The caller decides (send elsewhere, tell the user).
- **Deadlines and cancellation.** A job past its deadline is not run. A cancelled job is not run. The callback is told which.
- **Latest wins.** `StreamSession` keeps one pending hypothesis. A newer one replaces it, and a result that finishes
  after newer text has arrived is dropped (counted as `stale`).
- **Shutdown without leaks or hangs.** `close()` on the queue wakes every waiter. Workers drain and exit.
- **Swift concurrency.** `AssistEngine` is an `actor`. `AssistStream.partials` is an `AsyncStream` with a bounded buffer
  that keeps the newest. The blocking C call runs on a Dispatch queue, not on the cooperative thread pool, which has
  few threads. Cancelling the awaiting task cancels the stream.
- **Verified with:** ThreadSanitizer on the C++ tests and on the Swift tests, plus the one-off mutation check above.
- Limit: no lock-free multi-producer structure, no memory reclamation scheme. I would not hand-roll those without a
  strong reason.

## Performance on a constrained device

All numbers are in `docs/PERFORMANCE.md`. The ones to remember: the whole decision takes about 1.6 to 2.4 microseconds,
the model is 1.23 MB, tokenizing without allocation is 6.7x faster than the simple version, and the request path
allocates nothing (tested). int8 gave a 4x smaller file with the same accuracy and no speedup. Say all three parts.

Method: measure first, change one thing, measure again, keep the simple version next to the fast one so the gain is
visible. Two optimisations did not show a gain on my machine (ring padding, worker scaling) and the document says so.

## Machine learning and language models

- **Model:** a linear classifier on hashed n-grams. Why not a neural network: size and latency can be stated in advance,
  and the accuracy gap on this task is small. Limit: 88.9% on CLINC150's 150 classes. A fine-tuned small transformer
  would be higher. I did not train one.
- **Quantization:** per-class symmetric int8. The error per weight is at most half a step (property test in
  `tests/py/test_model_format.py`). Accuracy unchanged on this model.
- **Calibration:** temperature scaling, ECE 0.142 to 0.015. Why it matters: a threshold only works if confidence means something.
- **Out-of-scope detection:** AUROC 0.92 for separating in-scope from out-of-scope by confidence; at threshold 0.6 it
  catches 76% of out-of-scope requests while answering 89% of in-scope ones.
- **Hybrid on-device and large model:** `tools/hybrid.py` sends only the redacted text to a local Ollama model. I ran it
  with `qwen2.5-coder:7b`, a code model, so the answers show the plumbing works and say nothing about answer quality.
- **Evaluation discipline:** the C++ engine and the Python reference give the same intent on all 5,500 test utterances
  (largest confidence difference 2e-6). Routing is measured on the whole system, not only the model.
- **Where an LLM would sit in a real system:** behind the escalate route. The engine decides when to call it and
  what it may see.
- Limit: no on-device generative model, no speech, no embeddings.

## Testing and debugging

| Kind | Where |
|---|---|
| Unit tests | `tests/test_*.cpp` (doctest), 177 cases |
| Golden files shared by two languages | `tests/golden/`, written by `tools/make_golden.py`, read by C++ and Python tests |
| Differential testing | C++ engine against the Python reference, `tools/eval_system.py` |
| Property tests | sortedness and bounds of features, quantization error bound, AUROC against the pairwise definition |
| Fuzzing | `fuzz/` (libFuzzer with ASan and UBSan): model loader and the text parsers plus the engine, with invariants |
| Sanitizers | ASan, UBSan, TSan in CI |
| Allocation tests | `tests/test_no_alloc.cpp` replaces `operator new` and counts |
| Mutation check | one-off: weakened a release store and confirmed TSan fails (not kept in the repo) |
| Cross-language tests | C API tests in C++, Swift tests with XCTest |
| Failure injection | corrupted model bytes (checksum), bad UTF-8, embedded NULs, oversized input, empty input |

Debugging stories worth telling:
- A calculator miss on typed symbols (`%`, `54,788`) found by measuring the whole system, not by a unit test.
- `std::quoted` colliding with a test helper name through argument-dependent lookup.
- A test of mine that leaked an engine because a failed call overwrote the pointer before I freed it (found by ASan).

## Version control and process

Small commits with one-sentence messages, CI on every push and pull request. Docker image for a reproducible Linux
toolchain (`docker/dev.Dockerfile`).

## Using AI coding tools on this project

What I would say: I use an AI coding assistant for drafting, and I do not trust a draft until something independent
checks it. Here that was golden files shared with a second implementation, sanitizers, a fuzzer with stated
invariants, and measurements. Two examples where the checks mattered: a design comment said cache-line padding would
make the ring faster, the measurement on my machine did not show it, so the documents say so; and a pointer leaked by
one of my own tests, which ASan found.

## Languages

C++20 (core), C (stable interface), Swift 6 (package, actor, async/await), Python (training, evaluation, demo).
Not covered: Objective-C and Java. The C interface is the integration point either would use (an Objective-C wrapper is
a thin class around the same functions; Java would go through JNI).
