// hu-calendar-free-busy — owner free/busy from macOS Calendar (EventKit), for
// h-uman's commitment guard (include/human/daemon/calendar_free_busy.h).
//
// Usage:
//   hu-calendar-free-busy --start <unix> --end <unix> [--prompt]
//   hu-calendar-free-busy --status
//   hu-calendar-free-busy --request-access
//
// Prints ONE JSON object on stdout and nothing else:
//   {"access":"granted","busy":[{"end":1759518000,"start":1759514400}]}
//   {"access":"denied"} | {"access":"undetermined"}
// Busy intervals only. Titles, notes, locations, URLs and attendees never leave
// EventKit. Events marked "free", cancelled events and events the owner
// declined are not busy.
//
// --prompt: when access is undetermined, ask macOS once (the owner's one-time
// permission prompt) without waiting for the answer; this call still reports
// "undetermined", so the caller treats the calendar as unknown.

import EventKit
import Foundation

func emit(_ obj: [String: Any]) {
    guard let data = try? JSONSerialization.data(withJSONObject: obj, options: [.sortedKeys]) else {
        print("{\"access\":\"error\"}")
        return
    }
    FileHandle.standardOutput.write(data)
    FileHandle.standardOutput.write(Data("\n".utf8))
}

func accessName() -> String {
    let status = EKEventStore.authorizationStatus(for: .event)
    switch status {
    case .fullAccess:
        return "granted"
    case .notDetermined:
        return "undetermined"
    case .writeOnly, .denied, .restricted:
        return "denied"
    default:
        return "denied"
    }
}

func requestAccess(store: EKEventStore, wait: TimeInterval) -> String {
    let done = DispatchSemaphore(value: 0)
    var granted = false
    store.requestFullAccessToEvents { ok, _ in
        granted = ok
        done.signal()
    }
    if done.wait(timeout: .now() + wait) == .timedOut {
        return "undetermined"
    }
    return granted ? "granted" : "denied"
}

func intArg(_ name: String, _ args: [String]) -> Int64? {
    guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
    return Int64(args[i + 1])
}

func isBusy(_ ev: EKEvent) -> Bool {
    if ev.availability == .free || ev.status == .canceled {
        return false
    }
    if let me = ev.attendees?.first(where: { $0.isCurrentUser }),
       me.participantStatus == .declined {
        return false
    }
    return true
}

let args = Array(CommandLine.arguments.dropFirst())
let store = EKEventStore()

if args.contains("--status") {
    emit(["access": accessName()])
    exit(0)
}
if args.contains("--request-access") {
    let current = accessName()
    emit(["access": current == "undetermined" ? requestAccess(store: store, wait: 120) : current])
    exit(0)
}
guard let start = intArg("--start", args), let end = intArg("--end", args), end > start else {
    FileHandle.standardError.write(Data("usage: hu-calendar-free-busy --start <unix> --end <unix> [--prompt] | --status | --request-access\n".utf8))
    exit(2)
}
let access = accessName()
if access == "undetermined" && args.contains("--prompt") {
    _ = requestAccess(store: store, wait: 0.5)
}
if access != "granted" {
    emit(["access": access])
    exit(0)
}
let pred = store.predicateForEvents(withStart: Date(timeIntervalSince1970: TimeInterval(start)),
                                    end: Date(timeIntervalSince1970: TimeInterval(end)),
                                    calendars: nil)
var busy: [[String: Int64]] = []
for ev in store.events(matching: pred) where isBusy(ev) {
    busy.append(["start": Int64(ev.startDate.timeIntervalSince1970),
                 "end": Int64(ev.endDate.timeIntervalSince1970)])
}
emit(["access": "granted", "busy": busy])
