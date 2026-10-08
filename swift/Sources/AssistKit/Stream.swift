import CAssist
import Dispatch
import Foundation

/// Carries partial results from the C stream thread into an `AsyncStream`.
private final class PartialSink: @unchecked Sendable {
    let continuation: AsyncStream<PartialResult>.Continuation

    init(_ continuation: AsyncStream<PartialResult>.Continuation) { self.continuation = continuation }
}

private let partialCallback: assist_partial_fn = { json, length, sequence, userData in
    guard let json, let userData else { return }
    let sink = Unmanaged<PartialSink>.fromOpaque(userData).takeUnretainedValue()
    let data = Data(bytes: json, count: length)
    // A partial that cannot be decoded is dropped. The final result still reports the error.
    if let result = try? JSONDecoder().decode(AssistResult.self, from: data) {
        sink.continuation.yield(PartialResult(sequence: sequence, result: result))
    }
}

/// Owns the C stream and the object its callback points at, and destroys them in the right order: the stream first
/// (which waits for a running callback to return), then the sink.
private final class StreamHandle: @unchecked Sendable {
    let pointer: OpaquePointer
    let sink: PartialSink
    private let retainedSink: Unmanaged<PartialSink>

    init(engine: EngineHandle, continuation: AsyncStream<PartialResult>.Continuation) throws {
        let sink = PartialSink(continuation)
        let retained = Unmanaged.passRetained(sink)
        var created: OpaquePointer?
        let status = assist_stream_create(engine.raw, partialCallback, retained.toOpaque(), &created)
        guard status == ASSIST_OK, let created else {
            retained.release()
            throw AssistError(status: Int32(status.rawValue))
        }
        self.sink = sink
        self.retainedSink = retained
        self.pointer = created
    }

    deinit {
        assist_stream_destroy(pointer)
        retainedSink.release()
    }
}

/// Understanding speech while it is still being recognised.
///
/// Call `update` with each new version of the transcript. Only the newest text is worked on, so a slow engine never
/// builds up a queue. `partials` yields what the engine made of the text so far, and `finish` returns the answer for
/// the final text. Cancelling the task that is awaiting `finish` cancels the stream.
public final class AssistStream: Sendable {
    private let raw: StreamHandle
    /// Partial results in order. Ends when the stream is finished or cancelled.
    public let partials: AsyncStream<PartialResult>

    init(engine: EngineHandle) throws {
        let (stream, continuation) = AsyncStream<PartialResult>.makeStream(bufferingPolicy: .bufferingNewest(8))
        partials = stream
        raw = try StreamHandle(engine: engine, continuation: continuation)
    }

    /// Sends the newest version of the transcript. Returns immediately.
    public func update(_ text: String) throws {
        let status = withUTF8(text) { bytes, count in
            assist_stream_update(raw.pointer, bytes, count)
        }
        guard status == ASSIST_OK else { throw AssistError(status: Int32(status.rawValue)) }
    }

    /// Waits for the newest text to be processed and returns that result. A stream can be finished once.
    public func finish() async throws -> AssistResult {
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (done: CheckedContinuation<AssistResult, Error>) in
                // The C call blocks until the worker is done, so it runs on a Dispatch queue and not on the
                // cooperative thread pool, which has only a few threads and must not be blocked.
                let raw = self.raw
                DispatchQueue.global().async {
                    var json: UnsafeMutablePointer<CChar>?
                    var length = 0
                    let status = assist_stream_finish(raw.pointer, &json, &length)
                    raw.sink.continuation.finish()
                    guard status == ASSIST_OK, let json else {
                        done.resume(throwing: AssistError(status: Int32(status.rawValue)))
                        return
                    }
                    do {
                        done.resume(returning: try decode(json, length: length))
                    } catch {
                        done.resume(throwing: error)
                    }
                }
            }
        } onCancel: {
            cancel()
        }
    }

    /// Abandons the stream. Nothing more is delivered after this returns.
    public func cancel() {
        assist_stream_cancel(raw.pointer)
        raw.sink.continuation.finish()
    }
}
