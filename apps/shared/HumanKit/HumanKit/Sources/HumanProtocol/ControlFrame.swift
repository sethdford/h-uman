import Foundation

/// Request frame: `{"type":"req","id":"...","method":"...","params":{...}}`
public struct ControlRequest: Codable {
    public let type: String
    public let id: String
    public let method: String
    public let params: [String: AnyCodable]?

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

/// Response frame: `{"type":"res","id":"...","ok":true|false,"payload":{...}}`
public struct ControlResponse: Codable {
    public let type: String
    public let id: String
    public let ok: Bool
    public let payload: [String: AnyCodable]?

    enum CodingKeys: String, CodingKey {
        case type, id, ok, payload
    }
}

/// Event frame: `{"type":"event","event":"...","payload":{...},"seq":N}`
public struct ControlEvent: Codable {
    public let type: String
    public let event: String
    public let payload: [String: AnyCodable]?
    public let seq: UInt64?

    enum CodingKeys: String, CodingKey {
        case type, event, payload, seq
    }
}

/// Hello-ok (connect response): `{"type":"hello-ok","server":{...},"protocol":1,"features":{...}}`
public struct HelloOk: Codable {
    public let type: String
    public let server: ServerInfo?
    public let protocolVersion: Int?
    public let features: Features?

    enum CodingKeys: String, CodingKey {
        case type, server, features
        case protocolVersion = "protocol"
    }
}

public struct ServerInfo: Codable {
    public let version: String?
}

public struct Features: Codable {
    public let methods: [String]?
}

/// Type-erased Codable for dynamic JSON payloads.
public struct AnyCodable: Codable {
    public let value: Any

    public init(_ value: Any) {
        self.value = value
    }

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
