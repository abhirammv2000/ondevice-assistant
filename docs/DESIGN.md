# Design

This is the reasoning behind the choices that are not obvious from reading the code. Numbers come from `docs/results/`
and `docs/PERFORMANCE.md`.

## What problem this is

A voice assistant gets a short piece of text and has to decide, fast and on the device, one of three things: do it
(set the timer), ask one more question (for how long?), or hand it to a bigger model that runs somewhere else. The
interesting part is the third answer. Sending everything away costs time, money and privacy. Handling everything
locally means a small model gets requests it cannot do and answers them wrongly with confidence.

So the engine's job is to be right when it acts, and to know when it should not.

## The pipeline

```
text -> features -> int8 linear model -> intent + confidence
                                            |
              confidence < 0.6 ------------>|--> escalate (redacted text)
              no local handler ------------>|--> escalate (redacted text)
              handler for that intent ------+--> parse slots (time, date, number, contact)
                                                   |-> on_device (action + reply)
                                                   |-> clarify   (one question)
                                                   |-> escalate  (could not parse)
```

`Engine::handle` is the whole thing. `Engine::classify` is only the model, with no allocation.

## Features

The model reads hashed n-grams, not words, so there is no vocabulary file to ship and no out-of-vocabulary case.

- Bytes in, so any UTF-8 (or broken UTF-8) is safe. ASCII letters are lowered. Letters, digits, apostrophes and every
  byte at or above 0x80 make a token. Everything else separates tokens.
- A token that is only ASCII digits becomes `<num>`, so "set a timer for 10 minutes" and "...for 45 minutes" look alike.
- Unigrams `u:<token>` and bigrams `b:<a> <b>`, with `^` and `$` standing for the start and end.
- Each feature is FNV-1a (64 bit) of that string, folded to 32 bits (`h ^ (h >> 32)`). The row in the weight table is the
  hash masked by `n_features - 1`, which is why `n_features` is a power of two.
- At most 96 tokens and 512 bytes. A token that does not fit is dropped with everything after it. The output is sorted
  and has no duplicates, so the same words in the same order always give the same features.

`tools/features.py` is the reference. `tests/golden/features.tsv` is written from it, and both the Python tests and the
C++ tests check against that file, so the two implementations cannot drift.

## Model

A linear softmax classifier. Scoring one utterance is adding up one row of the weight table per feature, so its cost is
about (features in the utterance) x (classes), a few hundred integer additions.

- Weights are int8 with one scale per class (largest weight in the class maps to 127). The sum over features is an
  `int32`; then `bias + scale * sum / sqrt(n_features_in_utterance)`; then a stable softmax. The division by `sqrt(n)`
  keeps long and short utterances on a similar scale.
- Weights are stored feature-major so one feature's weights for all classes are one contiguous run.
- The file is mapped, not read. See `docs/PERFORMANCE.md` for what that does and does not buy.
- A CRC-32 sits at the end. Loading checks it by default. The loader also checks every offset, count and alignment
  against the file size before it touches the data, and the fuzzer feeds it arbitrary bytes.

### File format (version 1, little-endian)

The table is in `core/include/assist/model.hpp`. Sections: 64-byte header, class names, scales (int8 only), bias, weights
(64-byte aligned for int8), CRC-32.

### Choosing the size

I trained with 12 to 16 bits of hash space and measured test accuracy on CLINC150 (150 intents, 4,500 test utterances):

| bits | int8 size | test accuracy (int8) |
|---|---|---|
| 12 | 0.62 MB | 88.6% |
| 13 | 1.23 MB | 88.9% |
| 14 | 2.46 MB | 89.1% |
| 15 | 4.92 MB | 89.2% |

Past 13 bits the model gains 0.2 and then 0.1 points per doubling, so 13 bits (1.23 MB) is what ships. The
sweep is in `docs/results/size_sweep.json`.

### Calibration

A softmax trained to fit the training set is overconfident, which makes a confidence threshold useless: 0.9 does not mean
90% right. I fit one temperature on the validation set (T = 0.6) and folded it into the weights, so there is no extra
step at run time. Expected calibration error went from 0.142 to 0.015 with no change in accuracy.

### What the threshold buys

On the 4,500 in-scope and 1,000 out-of-scope test utterances (`docs/results/eval_clinc150.json`), with the threshold
at 0.6: 88.8% of in-scope requests are answered on the device, with 95.0% accuracy among those answered, and 76.4% of
out-of-scope requests are caught instead of answered wrongly. Raising the threshold trades coverage for accuracy, and
the full curve is in the file.

## Slots and conventions

Once the intent is known, a hand-written parser pulls out the values. Every choice below is one a user could argue with,
so each is fixed and tested.

- A bare weekday ("friday") is the next such day strictly after today. If today is Friday, "friday" means in seven days.
  "this friday" means the same. "next friday" is seven days after that.
- A month and day without a year is the next one on or after today. A date that does not exist (30 February) is not a
  match. 29 February is found up to eight years ahead.
- A time of day with no am or pm ("at 7") takes the earliest occurrence after now ("tonight" means evening). The reply
  always states the resolved time ("Alarm set for tomorrow at 7:00 AM"), so a wrong guess is visible to the user.
- A time equal to now counts as already passed.
- A duration must be positive and at most 366 days. Each unit may appear once.
- Spoken numbers are whole numbers below a million. "five thirty" is two numbers because that is what "at five thirty"
  needs.
- Contacts match by whole name, with a typo allowed (bounded edit distance) and a sound match as a fallback. Two close
  matches are a question ("Dana Whitfield or Dana Wu?") and not a guess.
- Calendar math uses the civil-days algorithms and was checked against Python's `datetime` on 3,000 random dates between 1600 and 2600 plus the century and leap-year edges
  (`tests/golden/calendar.tsv`).

## Privacy

When a request is sent on, what goes is `forward_text`: the utterance with emails, phone numbers, card numbers (Luhn
checked), social security numbers, spoken digit runs of seven or more and the words of contact names replaced by
placeholders. The original text stays on the device. `tests/py/test_hybrid.py` checks that the request body a server
receives contains only the redacted text.

This is pattern matching, not a guarantee. A name that is not in the contacts, or a street address, is not caught.

## Memory and threads

- The request path (tokenize, score) does not allocate. `tests/test_no_alloc.cpp` replaces `operator new` and counts.
  Building the reply strings does allocate, and a test says so.
- An `Engine` owns scratch space and a cache, so it is not shared between threads. `fork()` makes another one that
  shares the model and the contact list (both read-only) and has its own scratch.
- `BoundedQueue`: many producers and consumers, blocks, has `close()` for shutdown. `SpscRing`: one producer, one
  consumer, lock-free, acquire and release ordering. `AssistService` is a worker pool in front of engines with a bounded
  queue, deadlines, cancellation and a `Busy` answer when full. `StreamSession` handles speech that is still being
  recognised: only the newest text is processed, and a result that finishes after newer text has arrived is dropped.
- ThreadSanitizer runs on all of it. I also weakened one release store on purpose and confirmed TSan reports it, so a
  clean run means something.

## Interfaces

C++ core, then a C interface (`capi/include/assist.h`) with explicit error codes and no exceptions across the boundary,
then a Swift package on top (`AssistKit`: an `actor` for the engine, an `AsyncStream` of partial results, cancellation
through task cancellation). The C interface is the part another language would bind to.

## What this is not

- It does not do speech recognition or speech synthesis.
- The model was trained on CLINC150, which is a general intent set. Only 13 of its 150 intents have a local handler.
  The rest are recognised and then sent on, which is the right behaviour but not a full assistant.
- A hashed linear model cannot represent word order beyond bigrams. A small neural model would do better on hard
  cases, and would cost more size and time. The measured gap is the thing to look at before choosing one.
- The Swift package builds and tests on Linux here. The CI file also runs it on macOS; I have not run it on Apple hardware.
