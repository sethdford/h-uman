import Foundation

/// Request frame: `{"type":"req","id":"...","method":"...","params":{...}}`.
///
/// Encoded and sent by `HumanConnection.request(method:params:)`.
@available(macOS 14.0, iOS 17.0, *)
public struct ControlRequest: Codable, Sendable {
    /// Frame type discriminator. Always `"req"` for `ControlRequest`.
    public let type: String
    /// Caller-supplied correlation id; the matching response carries the
    /// same `id`.
    public let id: String
    /// RPC method name (see `Methods` for the canonical list).
    public let method: String
    /// Method-specific parameters, or `nil` for parameter-less methods.
    public let params: [String: AnyCodable]?

    /// Create a request frame. `type` is fixed to `"req"`.
    public init(id: String, method: String, params: [String: AnyCodable]? = nil) {
        self.type = "req"
        self.id = id
        self.method = method
        self.params = params
    }

    enum CodingKeys: String, CodingKey {
        case type, id, method, params
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        type = try c.decode(String.self, forKey: .type)
        id = try c.decode(String.self, forKey: .id)
        method = try c.decode(String.self, forKey: .method)
        params = try c.decodeIfPresent([String: AnyCodable].self, forKey: .params)
    }

    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode("req", forKey: .type)
        try c.encode(id, forKey: .id)
        try c.encode(method, forKey: .method)
        try c.encodeIfPresent(params, forKey: .params)
    }
}

/// Response frame: `{"type":"res","id":"...","ok":true|false,"payload":{...}}`.
@available(macOS 14.0, iOS 17.0, *)
public struct ControlResponse: Codable, Sendable {
    /// Frame type discriminator. Always `"res"` for `ControlResponse`.
    public let type: String
    /// Correlation id echoing the originating request.
    public let id: String
    /// Whether the RPC succeeded at the gateway level.
    public let ok: Bool
    /// Method-specific payload, or `nil` for empty responses.
    public let payload: [String: AnyCodable]?

    enum CodingKeys: String, CodingKey {
        case type, id, ok, payload
    }
}

/// Event frame: `{"type":"event","event":"...","payload":{...},"seq":N}`.
@available(macOS 14.0, iOS 17.0, *)
public struct ControlEvent: Codable, Sendable {
    /// Frame type discriminator. Always `"event"`.
    public let type: String
    /// Event name (e.g. `"chat.delta"`).
    public let event: String
    /// Event-specific payload, or `nil`.
    public let payload: [String: AnyCodable]?
    /// Monotonic sequence number set by the gateway, or `nil` if absent.
    public let seq: UInt64?

    enum CodingKeys: String, CodingKey {
        case type, event, payload, seq
    }
}

/// Hello-ok (connect response): `{"type":"hello-ok","server":{...},"protocol":1,"features":{...}}`.
@available(macOS 14.0, iOS 17.0, *)
public struct HelloOk: Codable, Sendable {
    /// Frame type discriminator. Always `"hello-ok"`.
    public let type: String
    /// Server information (version, etc.).
    public let server: ServerInfo?
    /// Protocol revision the server speaks.
    public let protocolVersion: Int?
    /// Server-advertised features (RPC methods, etc.).
    public let features: Features?

    enum CodingKeys: String, CodingKey {
        case type, server, features
        case protocolVersion = "protocol"
    }
}

/// Server identity returned in `HelloOk.server`.
@available(macOS 14.0, iOS 17.0, *)
public struct ServerInfo: Codable, Sendable {
    /// Server build version string, if reported.
    public let version: String?
}

/// Server-advertised feature set returned in `HelloOk.features`.
@available(macOS 14.0, iOS 17.0, *)
public struct Features: Codable, Sendable {
    /// Names of supported RPC methods.
    public let methods: [String]?
}

/// Type-erased Codable wrapper for dynamic JSON payloads.
///
/// The wrapped value is one of `null`, `bool`, `int`, `double`, `string`,
/// `array`, or `object` — the closed set of JSON-shaped values. The
/// payload is stored in a `Sendable` enum (`JSONValue`); the `value`
/// accessor returns the corresponding Foundation type for legacy call
/// sites that switch on `is`/`as`. This replaces an earlier
/// `Any`-backed implementation that could not conform to `Sendable`.
@available(macOS 14.0, iOS 17.0, *)
public struct AnyCodable: Codable, Sendable {
    /// Closed-set tagged union of JSON-shaped values. `indirect` enables
    /// recursive `.array` / `.object` cases without boxing each leaf.
    public indirect enum JSONValue: Sendable, Equatable {
        /// JSON `null`.
        case null
        /// JSON boolean.
        case bool(Bool)
        /// JSON integer (any value `Int` can represent).
        case int(Int)
        /// JSON floating-point value.
        case double(Double)
        /// JSON string.
        case string(String)
        /// JSON array.
        case array([JSONValue])
        /// JSON object.
        case object([String: JSONValue])
    }

    /// The strongly-typed JSON storage.
    public let storage: JSONValue

    /// Foundation-typed projection of `storage`. Returns `NSNull` for
    /// `.null`, `Bool` / `Int` / `Double` / `String` for the primitive
    /// cases, `[Any]` for `.array`, and `[String: Any]` for `.object`.
    public var value: Any {
        Self.toFoundation(storage)
    }

    /// Wrap a `JSONValue` directly.
    public init(_ json: JSONValue) {
        self.storage = json
    }

    /// Wrap a Foundation value. Recognized types: `NSNull`, `Bool`,
    /// `Int`, `Double`, `Float`, `String`, `[Any]`, `[String: Any]`,
    /// and `JSONValue`. Unrecognized inputs collapse to `.null`.
    public init(_ value: Any) {
        self.storage = Self.fromFoundation(value)
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() {
            storage = .null
        } else if let b = try? container.decode(Bool.self) {
            storage = .bool(b)
        } else if let i = try? container.decode(Int.self) {
            storage = .int(i)
        } else if let d = try? container.decode(Double.self) {
            storage = .double(d)
        } else if let s = try? container.decode(String.self) {
            storage = .string(s)
        } else if let a = try? container.decode([AnyCodable].self) {
            storage = .array(a.map { $0.storage })
        } else if let o = try? container.decode([String: AnyCodable].self) {
            storage = .object(o.mapValues { $0.storage })
        } else {
            storage = .null
        }
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try Self.encode(storage, into: &container)
    }

    private static func encode(_ json: JSONValue, into container: inout SingleValueEncodingContainer) throws {
        switch json {
        case .null:
            try container.encodeNil()
        case .bool(let b):
            try container.encode(b)
        case .int(let i):
            try container.encode(i)
        case .double(let d):
            try container.encode(d)
        case .string(let s):
            try container.encode(s)
        case .array(let a):
            try container.encode(a.map { AnyCodable($0) })
        case .object(let o):
            try container.encode(o.mapValues { AnyCodable($0) })
        }
    }

    private static func fromFoundation(_ value: Any) -> JSONValue {
        if let j = value as? JSONValue { return j }
        if value is NSNull { return .null }
        if let b = value as? Bool { return .bool(b) }
        if let i = value as? Int { return .int(i) }
        if let d = value as? Double { return .double(d) }
        if let f = value as? Float { return .double(Double(f)) }
        if let s = value as? String { return .string(s) }
        if let a = value as? [Any] { return .array(a.map(fromFoundation)) }
        if let o = value as? [String: Any] { return .object(o.mapValues(fromFoundation)) }
        return .null
    }

    private static func toFoundation(_ json: JSONValue) -> Any {
        switch json {
        case .null: return NSNull()
        case .bool(let b): return b
        case .int(let i): return i
        case .double(let d): return d
        case .string(let s): return s
        case .array(let a): return a.map(toFoundation)
        case .object(let o): return o.mapValues(toFoundation)
        }
    }
}
