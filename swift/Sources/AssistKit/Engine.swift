import CAssist
import Foundation

/// Owns the C engine. The C side serializes calls itself, so sharing this between threads is safe.
final class EngineHandle: @unchecked Sendable {
    let raw: OpaquePointer

    init(raw: OpaquePointer) { self.raw = raw }

    deinit { assist_engine_destroy(raw) }
}

/// Turns the engine's JSON into a result and frees the C buffer.
func decode(_ json: UnsafeMutablePointer<CChar>, length: Int) throws -> AssistResult {
    defer { assist_string_free(json) }
    let data = Data(bytes: json, count: length)
    do {
        return try JSONDecoder().decode(AssistResult.self, from: data)
    } catch {
        throw AssistError.badResponse
    }
}

/// Calls a C function that takes UTF-8 text and a byte count. A NULL pointer is passed for empty text.
func withUTF8<R>(_ text: String, _ body: (UnsafePointer<CChar>?, Int) -> R) -> R {
    var copy = text
    return copy.withUTF8 { buffer in
        guard let base = buffer.baseAddress, buffer.count > 0 else { return body(nil, 0) }
        return base.withMemoryRebound(to: CChar.self, capacity: buffer.count) { body($0, buffer.count) }
    }
}

/// One synchronous call into the engine. It is a free function so the output variables are plain locals and not
/// state captured from the actor.
func callHandle(_ engine: OpaquePointer, _ utterance: String) -> (assist_status, UnsafeMutablePointer<CChar>?, Int) {
    var json: UnsafeMutablePointer<CChar>?
    var length = 0
    let status = withUTF8(utterance) { text, count in
        assist_engine_handle(engine, text, count, &json, &length)
    }
    return (status, json, length)
}

/// The on-device assistant. Calls run one at a time on the actor; use `fork()` for a second engine that can run in
/// parallel with this one and shares the model and the contact list.
public actor AssistEngine {
    private let core: EngineHandle

    /// - Parameters:
    ///   - modelPath: a `.pmodel` file.
    ///   - seed: 0 seeds dice and coins from the system; any other value makes them repeatable.
    ///   - verifyChecksum: read the whole file once at load to detect damage.
    ///   - fixedTime: use this instead of the system clock (tests and demos).
    public init(modelPath: String, seed: UInt64 = 0, verifyChecksum: Bool = true, fixedTime: FixedTime? = nil) throws {
        var created: OpaquePointer?
        var status = ASSIST_ERR_INTERNAL
        modelPath.withCString { path in
            var options = assist_options()
            options.struct_size = UInt32(MemoryLayout<assist_options>.size)
            options.model_path = path
            options.random_seed = seed
            options.verify_checksum = verifyChecksum ? 1 : 0
            if let t = fixedTime {
                options.use_fixed_time = 1
                options.year = t.year
                options.month = t.month
                options.day = t.day
                options.hour = t.hour
                options.minute = t.minute
            }
            status = assist_engine_create(&options, &created)
        }
        guard status == ASSIST_OK, let created else { throw AssistError(status: Int32(status.rawValue)) }
        core = EngineHandle(raw: created)
    }

    private init(core: EngineHandle) { self.core = core }

    /// Replaces the contact list. Returns how many entries were rejected (for example an empty name).
    @discardableResult
    public func setContacts(_ contacts: [Contact]) throws -> Int {
        // The C strings must stay valid for the whole call, so they are made first and freed after it.
        let names = contacts.map { strdup($0.name) }
        defer { names.forEach { free($0) } }
        var items = zip(contacts, names).map { assist_contact(id: $0.id, name: UnsafePointer($1)) }
        var rejected = 0
        let status = assist_engine_set_contacts(core.raw, &items, items.count, &rejected)
        guard status == ASSIST_OK else { throw AssistError(status: Int32(status.rawValue)) }
        return rejected
    }

    /// Understands one utterance.
    public func handle(_ utterance: String) throws -> AssistResult {
        let (status, json, length) = callHandle(core.raw, utterance)
        guard status == ASSIST_OK, let json else { throw AssistError(status: Int32(status.rawValue)) }
        return try decode(json, length: length)
    }

    /// A second engine with its own working memory, sharing the model and contacts. Safe to use in parallel.
    public func fork() throws -> AssistEngine {
        var forked: OpaquePointer?
        let status = assist_engine_fork(core.raw, &forked)
        guard status == ASSIST_OK, let forked else { throw AssistError(status: Int32(status.rawValue)) }
        return AssistEngine(core: EngineHandle(raw: forked))
    }

    /// Starts a stream for speech that is still being recognised.
    public func makeStream() throws -> AssistStream {
        try AssistStream(engine: core)
    }
}
