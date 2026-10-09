# Siri User Experiences JD: where each requirement shows up

Which project, which file, what was measured, and what is missing. Everything marked "branch" is
committed locally and not pushed.

| Project | Where | State |
|---|---|---|
| `ondevice-assistant` | `C:\Users\abhir\Projects\ondevice-assistant` | `main`, local only, no GitHub repo yet |
| `ad-pulse` | `ad-server-svc/` | branch `serve-path-performance` |
| `LodgeZilla` | `backend/app/routes/bookings.py` | branch `fix/double-booking` |
| `twitterClone` | `app/utils/serialization.py` | branch `performance-and-privacy` (the folder had no git history; I added a baseline commit first) |
| `multi-tenant-platform` | `control_plane/app/k8s_resources.py` | branch `isolation-tests` |

## Languages

| JD | Evidence |
|---|---|
| C++ | `ondevice-assistant/core/` (C++20, about 3,900 lines in the library, about 3,200 in the tests), 177 test cases |
| Swift | `ondevice-assistant/swift/` (actor, async/await, `AsyncStream`, task cancellation), 12 XCTest cases on Linux |
| Python | training and evaluation tools, plus the backends in the other four projects |
| Objective-C, Java | **not used.** The C interface (`capi/include/assist.h`) is what either would bind to. |

## Computer science fundamentals

| JD | Evidence |
|---|---|
| Data structures and algorithms | trie, bounded edit distance, Soundex, LRU with open addressing, ring buffer, bounded queue, shunting yard, calendar arithmetic (`ondevice-assistant/docs/INTERVIEW.md` has the table). Measured against the simple versions in `docs/PERFORMANCE.md`. |
| Object-oriented design | injected `Clock`, `Engine::fork()` with copy-on-write shared state, RAII throughout, opaque C handles. |
| Debugging, testing | golden files shared by C++ and Python, differential testing, property tests, libFuzzer targets, ASan/UBSan/TSan, allocation-counting tests. LodgeZilla and twitterClone show the same habit on web backends: tests that count queries or race real MongoDB requests, and a control that proves the test can fail. |
| Version control | small commits, one branch per change, CI on each repository. |

## Preferred qualifications

| JD | Evidence | Limit |
|---|---|---|
| Operating system concepts | `mmap` and `CreateFileMapping` (`mapped_file.cpp`); alignment; page cache behaviour. `multi-tenant-platform`: namespaces, Pod Security `restricted`, ResourceQuota, default-deny NetworkPolicy, dropped capabilities, seccomp, no mounted service account token, tested in `tests/test_isolation_manifests.py`. | Warm-cache load time did not improve with `mmap` (measured, and written down). No scheduling or priority work. |
| Concurrency | `SpscRing` (acquire/release), `BoundedQueue` (mutex and condition variables, `close()`), `AssistService` (backpressure, deadlines, cancellation), `StreamSession` (latest wins), Swift actor. `ad-pulse`: a parse cache under 8 racing goroutines, `go test -race` in CI. `LodgeZilla`: 24 threads reserving the same dates against a real MongoDB, exactly one wins. | The lock-free ring was not measurably faster than a simpler ring on a 2-vCPU VM. |
| Performance optimisation | `docs/PERFORMANCE.md`. `ad-pulse`: 50 campaigns x 20 ads, 14.2 ms to 2.1 ms CPU, 27,304 to 134 allocations, 1,051 to 52 Redis calls per request. `twitterClone`: 152 to 6 queries per feed page. | Measured on a laptop (Docker, 2 cores). Numbers are comparisons, not capacity claims. |
| ML, GenAI, LLMs | linear intent model, int8 quantization, temperature calibration (ECE 0.142 to 0.015), out-of-scope detection (AUROC 0.92), confidence-gated escalation to a larger model with redaction (`tools/hybrid.py`, tested against a fake server and run once with a local 7B model). | No on-device generative model, no embeddings. |
| Mobile, systems, platform software | C ABI with explicit error codes and no exceptions across the boundary, Swift package consuming it, memory-mapped model, fixed memory on the request path. | The Swift package has run on Linux only. CI includes macOS, not yet run on Apple hardware. |
| High-performance, resource-constrained software | 1.23 MB model, about 2 microseconds per request, zero allocation on the scoring path (tested), bounded queues everywhere. | |
| Interfacing with AI-powered on-device systems | the engine decides between acting locally, asking a question, and sending redacted text to a larger model; streaming partial hypotheses. | No audio or speech recognition. |
| AI-assisted development tools | the checks around drafted code: golden files, fuzzing, sanitizers, query counters and race tests, with a control test where one is possible. See the last section of `INTERVIEW.md`. | |

## Bugs the tests found in existing projects

These are the strongest interview stories, because each one was found by a measurement or a test and
then fixed with a regression test that fails on the old code.

| Project | Found | Fix |
|---|---|---|
| `LodgeZilla` | The same dates could be reserved twice, even one request after the other. Reserving any listing created through the API returned 500 (`$push` onto `null`). A reservation for an unknown user left a booking behind. | One atomic `find_one_and_update` with an overlap filter; null history normalised; user checked first; dates validated. A control test shows the old behaviour double-booking in 15 of 15 rounds on MongoDB 7. |
| `twitterClone` | Following a private account was approved at once. `/api/explore` and `/api/search` showed private accounts' tweets to anyone. Every feed page cost 6 queries per tweet. The test suite did not run at all. | `follow()` writes `pending`; public queries exclude private authors; batched authors and counts; test config fixed. |
| `ad-pulse` | One request read a creative from Redis for every ranked ad, even after every impression was filled. Most CPU was re-decoding the same JSON. | Read each creative once and stop early; content-addressed parse cache. |
| `multi-tenant-platform` | Tenant pods had the service account token mounted. The API allowed 10 replicas but the quota fits 8. | `automountServiceAccountToken: false`; cap of 8 with a test that ties it to the quota. |

## Not done, said plainly

- Nothing has been pushed. Merging and creating the `ondevice-assistant` GitHub repository are your call.
- The CI files for `ondevice-assistant` (including macOS) and the new `LodgeZilla` MongoDB job have not run on GitHub yet.
- Objective-C and Java are not used anywhere.
- `multi-tenant-platform` has uncommitted work that was already there (`proxy/`, `k8s/loadtest/`, `k8s/proxy/`, `.gitignore`). I left it alone.
- `LodgeZilla` still has ten files with uncommitted edits from before; they are not in my commit.
