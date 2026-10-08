/// Where a request should be handled.
public enum Route: String, Codable, Sendable {
    /// The device answered. Run `action` with `slots` and show `reply`.
    case onDevice = "on_device"
    /// The device needs one more piece of information. Show `reply` as the question.
    case clarify
    /// The device cannot answer. Send `forwardText` (personal data already removed) to a larger model.
    case escalate
}

/// What the on-device engine decided for one utterance.
public struct AssistResult: Codable, Sendable, Equatable {
    public let route: Route
    /// The model's best guess at the intent, even when it was not confident enough to act on it.
    public let intent: String
    public let confidence: Double
    /// Best probability minus the second best.
    public let margin: Double
    /// What to do, such as `timer.set`. Empty when there is nothing to do.
    public let action: String
    public let reply: String
    /// For `.escalate`: the utterance with emails, phone numbers, card numbers, SSNs and contact names replaced.
    public let forwardText: String
    /// Why this route was chosen, for logs.
    public let reason: String
    public let slots: [String: String]

    enum CodingKeys: String, CodingKey {
        case route, intent, confidence, margin, action, reply, reason, slots
        case forwardText = "forward_text"
    }
}

/// A result for text that may still change, with the position of the update that produced it.
public struct PartialResult: Sendable, Equatable {
    public let sequence: UInt64
    public let result: AssistResult
}

/// A contact the engine can resolve names against ("call mom").
public struct Contact: Sendable, Equatable {
    public let id: UInt32
    public let name: String

    public init(id: UInt32, name: String) {
        self.id = id
        self.name = name
    }
}

public enum AssistError: Error, Sendable, Equatable, CustomStringConvertible {
    case invalidArgument
    case cannotReadModel
    case invalidModel
    case damagedModel
    case streamStopped
    case outOfMemory
    case internalError
    case badResponse

    init(status: Int32) {
        switch status {
        case 1: self = .invalidArgument
        case 2: self = .cannotReadModel
        case 3: self = .invalidModel
        case 4: self = .damagedModel
        case 5: self = .streamStopped
        case 6: self = .outOfMemory
        default: self = .internalError
        }
    }

    public var description: String {
        switch self {
        case .invalidArgument: "invalid argument"
        case .cannotReadModel: "the model file could not be read"
        case .invalidModel: "the file is not a valid model"
        case .damagedModel: "the model file is damaged"
        case .streamStopped: "the stream has already finished or was cancelled"
        case .outOfMemory: "out of memory"
        case .internalError: "internal error"
        case .badResponse: "the engine returned something that is not a valid result"
        }
    }
}

/// A fixed clock for tests and demos. Leave it out to use the system clock.
public struct FixedTime: Sendable {
    public var year: Int32, month: Int32, day: Int32, hour: Int32, minute: Int32

    public init(year: Int32, month: Int32, day: Int32, hour: Int32, minute: Int32) {
        self.year = year
        self.month = month
        self.day = day
        self.hour = hour
        self.minute = minute
    }
}
