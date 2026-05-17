import Foundation

/// Request frame for the Human gateway control protocol.
///
/// Wire form: `{"type":"req","id":"...","method":"...","params":{...}}`.
@available(macOS 14.0, iOS 17.0, *)
public struct ControlRequest: Codable {
    /// Frame discriminator. Always `"req"` for requests.
    public let type: String
    /// Client-generated correlation ID matched by the server in the response frame.
    public let id: String
    /// RPC method name (see `Methods`).
    public let method: String
    /// Method-specific parameters, JSON-shaped via `AnyCodable`.
    public let params: [String: AnyCodable]?

    /// Build a new request frame with `type` pinned to `"req"`.
    ///
    /// - Parameters:
    ///   - id: Client correlation ID.
    ///   - method: RPC method name.
    ///   - params: Method parameters (defaults to `nil`).
    public init(id: String, method: String, params: [String: AnyCodable]? = nil) {
        self.type = "req"
        self.id = id
        self.method = method
        self.params = params
    }

    enum CodingKeys: String, CodingKey {
        case type, id, method, params
    }

    /// Decode a request frame from JSON.
    ///
    /// - Parameter decoder: The decoder reading the frame.
    /// - Throws: A `DecodingError` if any required key is missing or mistyped.
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        type = try c.decode(String.self, forKey: .type)
        id = try c.decode(String.self, forKey: .id)
        method = try c.decode(String.self, forKey: .method)
        params = try c.decodeIfPresent([String: AnyCodable].self, forKey: .params)
    }

    /// Encode this request frame as JSON.
    ///
    /// - Parameter encoder: The encoder receiving the frame.
    /// - Throws: An `EncodingError` if a payload value cannot be represented.
    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode("req", forKey: .type)
        try c.encode(id, forKey: .id)
        try c.encode(method, forKey: .method)
        try c.encodeIfPresent(params, forKey: .params)
    }
}

/// Response frame for the Human gateway control protocol.
///
/// Wire form: `{"type":"res","id":"...","ok":true|false,"payload":{...}}`.
@available(macOS 14.0, iOS 17.0, *)
public struct ControlResponse: Codable {
    /// Frame discriminator. Always `"res"` for responses.
    public let type: String
    /// Correlation ID echoing the originating `ControlRequest.id`.
    public let id: String
    /// `true` on success; `false` if the server returned a structured error.
    public let ok: Bool
    /// Method-specific result data, JSON-shaped via `AnyCodable`.
    public let payload: [String: AnyCodable]?

    enum CodingKeys: String, CodingKey {
        case type, id, ok, payload
    }
}

/// Server-pushed event frame for the Human gateway control protocol.
///
/// Wire form: `{"type":"event","event":"...","payload":{...},"seq":N}`.
@available(macOS 14.0, iOS 17.0, *)
public struct ControlEvent: Codable {
    /// Frame discriminator. Always `"event"` for events.
    public let type: String
    /// Event name (e.g. `"tool.call.delta"`).
    public let event: String
    /// Event-specific payload, JSON-shaped via `AnyCodable`.
    public let payload: [String: AnyCodable]?
    /// Monotonic per-stream sequence number, when emitted by the server.
    public let seq: UInt64?

    enum CodingKeys: String, CodingKey {
        case type, event, payload, seq
    }
}

/// Hello-ok handshake frame returned by the server after a successful connect.
///
/// Wire form: `{"type":"hello-ok","server":{...},"protocol":1,"features":{...}}`.
@available(macOS 14.0, iOS 17.0, *)
public struct HelloOk: Codable {
    /// Frame discriminator. Always `"hello-ok"`.
    public let type: String
    /// Server identity and version info, when supplied.
    public let server: ServerInfo?
    /// Protocol version the server is speaking.
    public let protocolVersion: Int?
    /// Feature flags negotiated by the server.
    public let features: Features?

    enum CodingKeys: String, CodingKey {
        case type, server, features
        case protocolVersion = "protocol"
    }
}

/// Server identity advertised in the `HelloOk` frame.
@available(macOS 14.0, iOS 17.0, *)
public struct ServerInfo: Codable {
    /// Server version string (e.g. `"0.4.2"`), when reported.
    public let version: String?
}

/// Feature-flag set advertised in the `HelloOk` frame.
@available(macOS 14.0, iOS 17.0, *)
public struct Features: Codable {
    /// RPC method names the server will accept, when reported.
    public let methods: [String]?
}

/// Type-erased `Codable` wrapper for dynamic JSON values.
///
/// Use this when the schema of a payload is decided at runtime — `params` and
/// `payload` fields on control frames hold JSON-shaped values whose concrete
/// type isn't known to the protocol layer.
@available(macOS 14.0, iOS 17.0, *)
public struct AnyCodable: Codable {
    /// The wrapped value. Concrete type is one of `NSNull`, `Bool`, `Int`,
    /// `Double`, `String`, `[Any]`, or `[String: Any]`.
    public let value: Any

    /// Wrap an arbitrary JSON-shaped value.
    ///
    /// - Parameter value: Any value the encoder knows how to serialize.
    public init(_ value: Any) {
        self.value = value
    }

    /// Decode a single JSON value into the most specific Swift type that fits.
    ///
    /// - Parameter decoder: The decoder reading the value.
    /// - Throws: A `DecodingError` if the underlying container cannot be read.
    public init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() {
            value = NSNull()
        } else if let b = try? container.decode(Bool.self) {
            value = b
        } else if let i = try? container.decode(Int.self) {
            value = i
        } else if let d = try? container.decode(Double.self) {
            value = d
        } else if let s = try? container.decode(String.self) {
            value = s
        } else if let a = try? container.decode([AnyCodable].self) {
            value = a.map { $0.value }
        } else if let o = try? container.decode([String: AnyCodable].self) {
            value = o.mapValues { $0.value }
        } else {
            value = NSNull()
        }
    }

    /// Encode the wrapped value to JSON. Unrecognized types are written as null.
    ///
    /// - Parameter encoder: The encoder receiving the value.
    /// - Throws: An `EncodingError` if the underlying container rejects a value.
    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch value {
        case is NSNull:
            try container.encodeNil()
        case let b as Bool:
            try container.encode(b)
        case let i as Int:
            try container.encode(i)
        case let d as Double:
            try container.encode(d)
        case let s as String:
            try container.encode(s)
        case let a as [Any]:
            try container.encode(a.map { AnyCodable($0) })
        case let o as [String: Any]:
            try container.encode(o.mapValues { AnyCodable($0) })
        default:
            try container.encodeNil()
        }
    }
}
