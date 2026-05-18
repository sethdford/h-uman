import Foundation

#if canImport(FoundationModels)
import FoundationModels

/// On-device inference using Apple's FoundationModels framework (macOS 26+, iOS 26+).
/// Wraps SystemLanguageModel for free, private, zero-latency LLM access.
@available(macOS 26.0, iOS 26.0, *)
public final class OnDeviceProvider: Sendable {
    public static let modelName = "apple-foundationmodel"
    public static let contextWindow = 4096

    public enum OnDeviceError: Error, Sendable {
        case modelUnavailable
        case guardrailBlocked
        case contextOverflow
        case generationFailed(String)
    }

    public init() {}

    /// Check whether Apple Intelligence is available on this device.
    public var isAvailable: Bool {
        SystemLanguageModel.default.isAvailable
    }

    /// Generate a response for a simple prompt.
    public func generate(prompt: String, systemPrompt: String? = nil) async throws -> String {
        let model = SystemLanguageModel.default
        guard model.isAvailable else {
            throw OnDeviceError.modelUnavailable
        }

        var session: LanguageModelSession
        if let systemPrompt {
            session = LanguageModelSession(model: model, instructions: systemPrompt)
        } else {
            session = LanguageModelSession(model: model)
        }

        do {
            let response = try await session.respond(to: prompt)
            return String(response.content)
        } catch {
            let desc = String(describing: error)
            if desc.contains("guardrail") || desc.contains("safety") {
                throw OnDeviceError.guardrailBlocked
            }
            if desc.contains("context") || desc.contains("overflow") || desc.contains("token") {
                throw OnDeviceError.contextOverflow
            }
            throw OnDeviceError.generationFailed(desc)
        }
    }

    /// Generate a streaming response, delivering chunks as they arrive.
    public func stream(
        prompt: String,
        systemPrompt: String? = nil,
        onChunk: @Sendable @escaping (String) -> Void
    ) async throws -> String {
        let model = SystemLanguageModel.default
        guard model.isAvailable else {
            throw OnDeviceError.modelUnavailable
        }

        var session: LanguageModelSession
        if let systemPrompt {
            session = LanguageModelSession(model: model, instructions: systemPrompt)
        } else {
            session = LanguageModelSession(model: model)
        }

        var fullResponse = ""
        let stream = session.streamResponse(to: prompt)
        for try await chunk in stream {
            let text = String(chunk.content)
            fullResponse = text
            onChunk(text)
        }
        return fullResponse
    }
}

#else

/// Stub when FoundationModels is not available (pre-macOS 26, non-Apple platforms).
@available(macOS 14.0, iOS 17.0, *)
public final class OnDeviceProvider: Sendable {
    public static let modelName = "apple-foundationmodel"
    public static let contextWindow = 4096

    public enum OnDeviceError: Error, Sendable {
        case modelUnavailable
        case guardrailBlocked
        case contextOverflow
        case generationFailed(String)
    }

    public init() {}

    public var isAvailable: Bool { false }

    public func generate(prompt: String, systemPrompt: String? = nil) async throws -> String {
        throw OnDeviceError.modelUnavailable
    }

    public func stream(
        prompt: String,
        systemPrompt: String? = nil,
        onChunk: @Sendable @escaping (String) -> Void
    ) async throws -> String {
        throw OnDeviceError.modelUnavailable
    }
}

#endif
