# ondevice-assistant

A small assistant engine that runs on the device. It reads a piece of text and decides: do it now (set the timer),
ask one more question, or send it to a bigger model with the personal details removed.

The core is C++20. There is a C interface, a Swift package on top of it, and Python tools for training and evaluation.

```
$ assist_cli --model models/clinc150.pmodel --contacts examples/contacts.tsv "set a timer for ten minutes"
{"route":"on_device","intent":"timer","confidence":1.0000,"action":"timer.set","reply":"Timer set for 10 minutes.", ...}

$ assist_cli --model models/clinc150.pmodel --contacts examples/contacts.tsv "text 555 123 4567 and email jane@x.com to dana"
{"route":"escalate", ..., "forward_text":"text <PHONE> and email <EMAIL> to <NAME>", ...}
```

## What it does

- Intent model: hashed n-gram features, a linear model, int8 weights, 1.23 MB, memory-mapped. 88.9% on CLINC150's
  150 intents. Confidence is calibrated, so a threshold means something.
- At a 0.6 threshold it answers 89% of in-scope requests on the device (95% correct among those) and catches 76% of
  out-of-scope ones.
- 13 intents have local handlers: timers, alarms, time, date, calculator, coin, dice, calls, texts, reminders and three
  small-talk ones. It parses spoken times, dates, durations and numbers, and finds contacts despite typos.
- Everything else goes on, as redacted text.
- A request takes about 2 microseconds on a laptop. The model scoring path allocates nothing.
- It handles many requests at once (worker pool with backpressure and deadlines) and speech that is still being
  recognised (only the newest text is processed).

## Build and test

```
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure      # 177 C++ cases
python -m pytest tests/py                       # Python tools
swift test                                      # Swift package
build/assist_bench --model models/clinc150.pmodel
```

Sanitizer builds: `-DASSIST_SANITIZE=address,undefined` or `-DASSIST_SANITIZE=thread`. Fuzzers need clang:
`-DASSIST_BUILD_FUZZ=ON`. `docker/dev.Dockerfile` has a Linux toolchain with all of this.

## Use it from Swift

```swift
let engine = try AssistEngine(modelPath: path)
try await engine.setContacts([Contact(id: 1, name: "Mom")])
let result = try await engine.handle("call mom")        // route, action, slots, reply

let stream = try await engine.makeStream()
Task { for await p in stream.partials { show(p.result) } }
try stream.update("set a timer"); try stream.update("set a timer for ten minutes")
let final = try await stream.finish()
```

## Send the rest to a bigger model

```
python tools/hybrid.py --cli build/assist_cli --model models/clinc150.pmodel "tell me about the history of rome"
```

Talks to a local Ollama server. Only the redacted text is sent, and a test checks that.

## Read next

- `docs/DESIGN.md`: how it works and why, and what it is not
- `docs/PERFORMANCE.md`: measurements, including two things that did not speed up
- `docs/TOPICS.md`: where each systems topic (memory mapping, concurrency, testing, ...) shows up in the code, with the limits

## Layout

```
core/      C++ library (tokenizer, model, slots, calculator, contacts, redaction, engine, service, stream)
capi/      C interface
swift/     Swift API and tests (Package.swift is at the root)
cli/       command-line tool
tools/     training, evaluation, hybrid demo (Python)
tests/     C++ tests, Python tests, golden files
fuzz/      libFuzzer targets
bench/     benchmarks
models/    the trained model
```

## Data and licence

Trained and evaluated on CLINC150 (Larson et al., 2019), released under CC BY 3.0. The code is MIT.
