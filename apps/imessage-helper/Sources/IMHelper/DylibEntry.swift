import Foundation

/// Dylib entry point, registered via the linker's `-init __dylib_init` flag
/// (see build.sh). Fires when DYLD_INSERT_LIBRARIES loads us into a process.
/// We act only inside Messages.app and bootstrap on the main queue (IMCore
/// APIs require the main thread).
@_cdecl("_dylib_init")
public func _dylibInit() {
    let bundleId = Bundle.main.bundleIdentifier ?? "unknown"
    Log.info("loaded into \(bundleId)")

    let isMessages = bundleId == "com.apple.MobileSMS" || bundleId == "com.apple.Messages"
    guard isMessages else {
        Log.info("not Messages.app, skipping")
        return
    }

    DispatchQueue.main.async {
        Log.info("bootstrapping…")
        IMHelper.bootstrap()
    }
}
