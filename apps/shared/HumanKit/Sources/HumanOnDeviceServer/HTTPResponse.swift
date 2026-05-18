import Foundation

/// Minimal HTTP response builder.
@available(macOS 14.0, iOS 17.0, *)
public struct HTTPResponse: Sendable {
    /// Numeric HTTP status code (200, 404, etc.).
    public let statusCode: Int
    /// Human-readable status phrase paired with `statusCode`.
    public let statusText: String
    /// `Content-Type` response header.
    public let contentType: String
    /// Response body bytes. Empty for streaming responses.
    public let body: Data
    /// Optional chunk stream for `text/event-stream` responses.
    public let streamChunks: AsyncStream<Data>?
    /// When set (e.g. CORS preflight), emitted after `Access-Control-Allow-Origin`.
    public let accessControlAllowMethods: String?
    /// When set, emitted as `Access-Control-Allow-Headers`.
    public let accessControlAllowHeaders: String?

    /// True when this response uses chunked streaming.
    public var isStreaming: Bool { streamChunks != nil }

    /// Construct a response with full control over each field.
    public init(
        statusCode: Int,
        statusText: String,
        contentType: String,
        body: Data,
        streamChunks: AsyncStream<Data>?,
        accessControlAllowMethods: String? = nil,
        accessControlAllowHeaders: String? = nil
    ) {
        self.statusCode = statusCode
        self.statusText = statusText
        self.contentType = contentType
        self.body = body
        self.streamChunks = streamChunks
        self.accessControlAllowMethods = accessControlAllowMethods
        self.accessControlAllowHeaders = accessControlAllowHeaders
    }

    /// Build a JSON response (`application/json`) from a Foundation object.
    public static func json(_ object: Any, status: Int = 200) -> HTTPResponse {
        let body = (try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])) ?? Data()
        return HTTPResponse(
            statusCode: status,
            statusText: statusText(for: status),
            contentType: "application/json",
            body: body,
            streamChunks: nil,
            accessControlAllowMethods: nil,
            accessControlAllowHeaders: nil
        )
    }

    /// Build a chunked Server-Sent Events response from a `Data` stream.
    public static func sseStream(status: Int = 200, chunks: AsyncStream<Data>) -> HTTPResponse {
        HTTPResponse(
            statusCode: status,
            statusText: "OK",
            contentType: "text/event-stream",
            body: Data(),
            streamChunks: chunks,
            accessControlAllowMethods: nil,
            accessControlAllowHeaders: nil
        )
    }

    /// Build an OpenAI-style error response.
    public static func error(_ message: String, status: Int = 500) -> HTTPResponse {
        json(["error": ["message": message, "type": "server_error"]], status: status)
    }

    /// Convenience for the 404 case.
    public static func notFound() -> HTTPResponse {
        error("Not found", status: 404)
    }

    /// Convenience for the 405 case.
    public static func methodNotAllowed() -> HTTPResponse {
        error("Method not allowed", status: 405)
    }

    func headerData(accessControlAllowOrigin: String? = nil) -> Data {
        var header = "HTTP/1.1 \(statusCode) \(statusText)\r\n"
        header += "Content-Type: \(contentType)\r\n"
        if let origin = accessControlAllowOrigin {
            header += "Access-Control-Allow-Origin: \(origin)\r\n"
        }
        if let methods = accessControlAllowMethods {
            header += "Access-Control-Allow-Methods: \(methods)\r\n"
        }
        if let allowHeaders = accessControlAllowHeaders {
            header += "Access-Control-Allow-Headers: \(allowHeaders)\r\n"
        }
        header += "Connection: close\r\n"
        if !isStreaming {
            header += "Content-Length: \(body.count)\r\n"
        } else {
            header += "Transfer-Encoding: chunked\r\n"
            header += "Cache-Control: no-cache\r\n"
        }
        header += "\r\n"
        return Data(header.utf8)
    }

    private static func statusText(for code: Int) -> String {
        switch code {
        case 200: return "OK"
        case 400: return "Bad Request"
        case 404: return "Not Found"
        case 401: return "Unauthorized"
        case 405: return "Method Not Allowed"
        case 500: return "Internal Server Error"
        case 503: return "Service Unavailable"
        default: return "Unknown"
        }
    }
}
