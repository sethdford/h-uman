import Foundation

// MARK: - Objective-C runtime helpers
// Faithful port of imessage-rs's PrivateAPI.swift helpers — every IMCore call
// goes through `responds(to:)`-guarded performSelector or an IMP cast.

func getSharedInstance(_ className: String) -> NSObject? {
    guard let cls = NSClassFromString(className) as? NSObject.Type else { return nil }
    let sel = NSSelectorFromString("sharedInstance")
    guard cls.responds(to: sel) else { return nil }
    return cls.perform(sel)?.takeUnretainedValue() as? NSObject
}

func safePerform(_ obj: NSObject, selector: String, with arg: Any? = nil) {
    let sel = NSSelectorFromString(selector)
    guard obj.responds(to: sel) else {
        Log.error("safePerform: \(type(of: obj)) does not respond to \(selector)")
        return
    }
    if let arg = arg {
        obj.perform(sel, with: arg)
    } else {
        obj.perform(sel)
    }
}

func safePerformReturning(_ obj: NSObject, selector: String, with arg: Any? = nil) -> NSObject? {
    let sel = NSSelectorFromString(selector)
    guard obj.responds(to: sel) else { return nil }
    let result: Unmanaged<AnyObject>?
    if let arg = arg {
        result = obj.perform(sel, with: arg)
    } else {
        result = obj.perform(sel)
    }
    return result?.takeUnretainedValue() as? NSObject
}

func callInt(_ obj: NSObject, selector: String) -> Int {
    let sel = NSSelectorFromString(selector)
    guard obj.responds(to: sel) else { return 0 }
    typealias IntMethod = @convention(c) (NSObject, Selector) -> Int
    let imp = obj.method(for: sel)
    let fn = unsafeBitCast(imp, to: IntMethod.self)
    return fn(obj, sel)
}

/// Allocate without calling init — for IMCore classes that crash on no-args init.
func runtimeAlloc(_ cls: NSObject.Type) -> NSObject? {
    cls.perform(NSSelectorFromString("alloc"))?.takeUnretainedValue() as? NSObject
}

// MARK: - Chat / message resolution

/// Look up an IMChat by GUID, with the Tahoe "any;-;" prefix fallback.
func getChat(guid: String?, transaction: String?) -> NSObject? {
    guard let guid = guid else {
        IMHelper.respondError(transaction: transaction, error: "Provide a chat GUID!")
        return nil
    }
    guard let registry = getSharedInstance("IMChatRegistry") else {
        IMHelper.respondError(transaction: transaction, error: "IMChatRegistry not available!")
        return nil
    }
    if let chat = safePerformReturning(registry, selector: "existingChatWithGUID:", with: guid) {
        return chat
    }
    // Tahoe: services collapse to "any;-;"
    var tahoeGuid: String?
    if guid.hasPrefix("iMessage;-;") {
        tahoeGuid = guid.replacingOccurrences(of: "iMessage;-;", with: "any;-;")
    } else if guid.hasPrefix("SMS;-;") {
        tahoeGuid = guid.replacingOccurrences(of: "SMS;-;", with: "any;-;")
    }
    if let tahoeGuid = tahoeGuid,
       let chat = safePerformReturning(registry, selector: "existingChatWithGUID:", with: tahoeGuid) {
        return chat
    }
    IMHelper.respondError(transaction: transaction, error: "Chat does not exist!")
    return nil
}

/// Load a message by GUID from IMChatHistoryController (async completion).
func getMessageItem(guid: String, completion: @escaping (NSObject?) -> Void) {
    guard let historyController = getSharedInstance("IMChatHistoryController") else {
        completion(nil)
        return
    }
    let sel = NSSelectorFromString("loadMessageWithGUID:completionBlock:")
    guard historyController.responds(to: sel) else {
        completion(nil)
        return
    }
    typealias LoadMethod = @convention(c) (NSObject, Selector, NSString, @escaping @convention(block) (AnyObject?) -> Void) -> Void
    let imp = historyController.method(for: sel)
    let fn = unsafeBitCast(imp, to: LoadMethod.self)
    fn(historyController, sel, guid as NSString) { message in
        completion(message as? NSObject)
    }
}

/// Find the chat item for a given message part index.
func findPartChatItem(items: Any, partIndex: Int) -> NSObject? {
    if let itemArray = items as? [NSObject] {
        let aggregateClass: AnyClass? = NSClassFromString("IMAggregateAttachmentMessagePartChatItem")
        for item in itemArray {
            if let aggCls = aggregateClass, item.isKind(of: aggCls) {
                if let parts = safePerformReturning(item, selector: "aggregateAttachmentParts") as? [NSObject] {
                    for subItem in parts where callInt(subItem, selector: "index") == partIndex {
                        return subItem
                    }
                }
            } else if callInt(item, selector: "index") == partIndex {
                return item
            }
        }
        return nil
    } else if let single = items as? NSObject {
        return single
    }
    return nil
}

// MARK: - Reaction maps

/// Reaction string → IMCore associated-message-type code.
func parseReactionType(_ type: String) -> Int64 {
    switch type.lowercased() {
    case "love": return 2000
    case "like": return 2001
    case "dislike": return 2002
    case "laugh": return 2003
    case "emphasize": return 2004
    case "question": return 2005
    case "-love": return 3000
    case "-like": return 3001
    case "-dislike": return 3002
    case "-laugh": return 3003
    case "-emphasize": return 3004
    case "-question": return 3005
    default: return 0
    }
}

/// Reaction string → human-readable verb for the tapback summary text.
func reactionToVerb(_ type: String) -> String {
    switch type.lowercased() {
    case "love": return "Loved "
    case "like": return "Liked "
    case "dislike": return "Disliked "
    case "laugh": return "Laughed at "
    case "emphasize": return "Emphasized "
    case "question": return "Questioned "
    case "-love": return "Removed a heart from "
    case "-like": return "Removed a like from "
    case "-dislike": return "Removed a dislike from "
    case "-laugh": return "Removed a laugh from "
    case "-emphasize": return "Removed an exclamation from "
    case "-question": return "Removed a question mark from "
    default: return ""
    }
}
