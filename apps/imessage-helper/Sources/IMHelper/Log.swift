import Foundation
import os.log

private let logger = OSLog(subsystem: "com.human.imessage-helper", category: "main")

/// Minimal os_log wrapper. The dylib runs inside Messages.app, so stdout is not
/// ours — everything goes through the unified log (view with `log stream
/// --predicate 'subsystem == "com.human.imessage-helper"'`).
enum Log {
    static func info(_ message: String) {
        os_log(.default, log: logger, "%{public}@", message)
    }
    static func error(_ message: String) {
        os_log(.error, log: logger, "ERROR: %{public}@", message)
    }
}
