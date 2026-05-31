import Foundation

/// IMCore thread-identifier factory: given a message-part chat item, returns
/// the thread id to stamp on a threaded reply.
typealias IMCreateThreadIDFunc = @convention(c) (AnyObject) -> Unmanaged<NSString>

/// Runtime-resolved function pointer (set by resolvePrivateSymbols).
var resolved_IMCreateThreadIdentifier: IMCreateThreadIDFunc?

/// RTLD_DEFAULT on macOS is ((void*)-2).
private let RTLD_DEFAULT_PTR = UnsafeMutableRawPointer(bitPattern: -2)

/// Resolve private IMCore symbols at runtime. No link-time framework
/// dependency — that is what keeps this dylib loadable across macOS versions.
/// Call once during bootstrap.
func resolvePrivateSymbols() {
    if let ptr = dlsym(RTLD_DEFAULT_PTR, "IMCreateThreadIdentifierForMessagePartChatItem") {
        resolved_IMCreateThreadIdentifier = unsafeBitCast(ptr, to: IMCreateThreadIDFunc.self)
    } else {
        Log.info("IMCreateThreadIdentifierForMessagePartChatItem not found")
    }
    Log.info("private symbols resolved")
}
