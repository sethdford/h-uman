import Foundation

/// Minimal HTTP request parser. Handles what we need for OpenAI-compatible endpoints.
@available(macOS 14.0, iOS 17.0, *)
public struct HTTPRequest: Sendable {
    /// HTTP method (GET, POST, OPTIONS, etc.).
    public let method: String
    /// Request path including any query string.
    public let path: String
    /// Header field name → value, lowercased keys.
    public let headers: [String: String]
    /// Request body, or `nil` if no body was sent.
    public let body: Data?

    /// Parse an HTTP request from raw data. Returns nil if incomplete.
    static func parse(_ data: Data) -> HTTPRequest? {
        let separator = Data([0x0D, 0x0A, 0x0D, 0x0A]) // \r\n\r\n
        guard let sepRange = data.range(of: separator) else { return nil }
        let headerData = data[data.startIndex..<sepRange.lowerBound]
        guard let headerStr = String(data: headerData, encoding: .utf8) else { return nil }

        let lines = headerStr.components(separatedBy: "\r\n")
        guard let requestLine = lines.first, !requestLine.isEmpty else { return nil }

        let parts = requestLine.split(separator: " ", maxSplits: 2)
        guard parts.count >= 2 else { return nil }
        let method = String(parts[0])
        let path = String(parts[1])

        var headers: [String: String] = [:]
        for line in lines.dropFirst() where !line.isEmpty {
            if let colonIdx = line.firstIndex(of: ":") {
                let key = String(line[line.startIndex..<colonIdx]).trimmingCharacters(in: .whitespaces).lowercased()
                let val = String(line[line.index(after: colonIdx)...]).trimmingCharacters(in: .whitespaces)
                headers[key] = val
            }
        }

        let bodyStart = sepRange.upperBound
        let remaining = data.count - bodyStart

        if let contentLength = headers["content-length"], let expected = Int(contentLength) {
            guard expected >= 0, expected <= HTTPServer.maxRequestSize else { return nil }
            guard remaining >= expected else { return nil }
            let body = data.subdata(in: bodyStart..<(bodyStart + expected))
            return HTTPRequest(method: method, path: path, headers: headers, body: body)
        }

        return HTTPRequest(method: method, path: path, headers: headers, body: remaining > 0 ? data.subdata(in: bodyStart..<data.endIndex) : nil)
    }

    /// Decode the body as JSON, returning the top-level object or `nil`.
    public var json: [String: Any]? {
        guard let body = body, !body.isEmpty else { return nil }
        return try? JSONSerialization.jsonObject(with: body) as? [String: Any]
    }
}
