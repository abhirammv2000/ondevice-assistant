# Performance

Everything here comes from `bench/bench.cpp` (`assist_bench`), built Release with GCC in the `docker/dev.Dockerfile`
image on a Windows laptop (Docker Desktop, 2 vCPUs given to the VM). Raw numbers are in `results/bench_raw.json`.
Each figure is the median of many timed batches (200 for most rows, fewer for the slow ones), with the 99th percentile next to it. A shared 2-vCPU VM is noisy, so treat
differences under about 10% as noise. Rerun on your own machine before quoting any of this.

## What the request path costs

| Step | Median | p99 |
|---|---|---|
| Text to features (fixed buffers) | 155 ns | 222 ns |
| Score 150 classes (int8) | 1.11 us | 1.55 us |
| Tokenize and score (`classify`) | 1.29 us | 1.82 us |
| Whole `handle()` (timer, alarm, call, calculator) | 1.6 to 2.4 us | 2.3 to 4.1 us |

So the whole on-device decision, including building the reply, takes a couple of microseconds. The model is 1.23 MB.
Nothing about this is close to a frame budget (16 ms), which leaves room for the parts the numbers do not cover: audio,
speech recognition and the UI.

## Choices that were measured

**Fixed buffers in the tokenizer.** The version with `std::string`, `std::vector` and a `std::set` for de-duplication
does the same work in 1.03 us. The fixed-buffer version is 6.7x faster and, more important for a phone, never touches
the heap on this path (checked by `tests/test_no_alloc.cpp`, which counts calls to `operator new`).

**int8 weights.** Scoring is about the same speed as float32 (1.11 us against 1.18 us, inside the noise). The win is
size: 1.23 MB against 4.92 MB, with the same test accuracy (88.93% for both, `results/eval_clinc150.json`). On a device,
size decides how many pages must be resident, which is why it was worth doing, but it is not a speed trick.

**Contact lookup.** A trie finds candidate words and a bounded edit distance (stops once the distance passes the limit)
scores them. Against computing a full edit distance to every name: 5.5x faster at 1,000 names (29 us against 161 us),
and 6.2x at 10,000 (271 us against 1.69 ms). The gap grows with the list, as it should.

**LRU cache.** The preallocated open-addressing cache takes 13.5 ns per operation against 53.6 ns for `std::list` plus
`std::unordered_map`, about 4x, and it never allocates once full.

**Mutex queue against lock-free ring.** One thread handing 5 million integers to another: the lock-free `SpscRing`
moves 217 million per second, the `BoundedQueue` (mutex and condition variables) 6.7 million, about 32x. The mutex
queue is still the right choice for the worker pool, because many threads share it and it needs blocking and
`close()`. The ring is for the one-producer, one-consumer stream path.

**Model loading.** With a warm page cache, mapping the file and skipping the checksum takes 0.40 ms. Verifying the
CRC-32 reads every page and takes 3.2 ms. Copying the bytes and parsing them takes 0.06 ms because the bytes are
already in memory, but that copy needs 1.2 MB of private memory, where a mapping can be shared and paged in on demand.
The checksum is on by default and can be turned off for a model that was verified at install time.

## What did not show up

Two things I expected and did not see. They are listed here instead of repeating the textbook claim.

- **Cache-line padding and cached indices in the ring buffer.** The version with both indices side by side and no
  caching ran at 209 million per second against 217 million. That is within noise. False sharing needs two cores
  actually running at once on separate caches, and a 2-vCPU VM may schedule them in a way that hides it. The padded
  design is still the standard one, and the thread-sanitizer run checks that it is correct, but this machine did not
  prove it faster.
- **Service scaling with workers.** Requests per second were 580k, 442k, 633k and 554k for 1, 2, 4 and 8 workers: no
  scaling. Each request is about 1.6 us of work, so the cost is mostly the queue lock and the callback, and the VM
  has two cores. The service is built for isolation (deadlines, cancellation, backpressure, one engine per worker), not
  for throughput. A heavier request, such as a real network call to a larger model, is where more workers would help.

## Reproduce

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target assist_bench
build/assist_bench --model models/clinc150.pmodel --json docs/results/bench_raw.json
```
