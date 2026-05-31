import Foundation

/// Message actions via IMCore. Faithful (trimmed) port of imessage-rs's
/// MessageActions.swift — see docs/investigations/imessage-private-api-mechanism.md.
/// Implemented: send text, threaded reply, classic tapback (2000-2005 /
/// 3000-3005), edit (Tahoe 5-arg + Sequoia 4-arg), unsend, local delete.
/// Deferred: emoji/sticker tapback, attachments, multipart (see README).
enum MessageActions {

    // MARK: - Send / reply / classic tapback

    static func sendMessage(data: [String: Any], transaction: String?) {
        guard let chat = getChat(guid: data["chatGuid"] as? String, transaction: transaction) else { return }

        let messageText = data["message"] as? String ?? ""
        let attrStr = NSMutableAttributedString(string: messageText)

        var subjectStr: NSMutableAttributedString?
        if let subject = data["subject"] as? String, !subject.isEmpty {
            subjectStr = NSMutableAttributedString(string: subject)
        }
        var effectId: String?
        if let eff = data["effectId"] as? String, !eff.isEmpty { effectId = eff }

        // Build + send. `reaction == nil` → regular/threaded message; else tapback.
        let createAndSend: (NSAttributedString?, NSAttributedString?, String?, String?, String?, Int64?, NSRange, [String: Any]?) -> Void = {
            message, subject, effectId, threadId, assocGuid, reaction, range, summaryInfo in

            guard let messageClass = NSClassFromString("IMMessage") as? NSObject.Type else { return }
            var messageToSend = messageClass.init()

            if reaction == nil {
                let flags: Int64 = (subject != nil) ? 0x10000d : 0x100005
                let sel = NSSelectorFromString("initWithSender:time:text:messageSubject:fileTransferGUIDs:flags:error:guid:subject:balloonBundleID:payloadData:expressiveSendStyleID:")
                if messageToSend.responds(to: sel) {
                    typealias InitMethod = @convention(c) (NSObject, Selector, Any?, Any?, NSAttributedString?, NSAttributedString?, NSArray?, Int64, Any?, Any?, Any?, Any?, Any?, NSString?) -> NSObject
                    let imp = messageToSend.method(for: sel)
                    let fn = unsafeBitCast(imp, to: InitMethod.self)
                    messageToSend = fn(messageToSend, sel, nil, nil, message, subject, nil, flags, nil, nil, nil, nil, nil, effectId as NSString?)
                }
                if let threadId = threadId {
                    messageToSend.setValue(threadId, forKey: "threadIdentifier")
                }
            } else {
                let sel = NSSelectorFromString("initWithSender:time:text:messageSubject:fileTransferGUIDs:flags:error:guid:subject:associatedMessageGUID:associatedMessageType:associatedMessageRange:messageSummaryInfo:")
                if messageToSend.responds(to: sel) {
                    typealias InitMethod = @convention(c) (NSObject, Selector, Any?, Any?, NSAttributedString?, NSAttributedString?, Any?, Int64, Any?, Any?, Any?, NSString?, Int64, NSRange, NSDictionary?) -> NSObject
                    let imp = messageToSend.method(for: sel)
                    let fn = unsafeBitCast(imp, to: InitMethod.self)
                    messageToSend = fn(messageToSend, sel, nil, nil, message, subject, nil, 0x5, nil, nil, nil, assocGuid as NSString?, reaction!, range, summaryInfo as NSDictionary?)
                }
            }

            safePerform(chat, selector: "sendMessage:", with: messageToSend)
            if let transaction = transaction {
                let lastGuid = safePerformReturning(chat, selector: "lastSentMessage")?.value(forKey: "guid") as? String ?? ""
                IMHelper.respond(transaction: transaction, extra: ["identifier": lastGuid])
            }
        }

        // Reply / reaction both reference a parent via selectedMessageGuid.
        if let selectedGuid = data["selectedMessageGuid"] as? String, !selectedGuid.isEmpty {
            getMessageItem(guid: selectedGuid) { message in
                guard let message = message else {
                    IMHelper.respondError(transaction: transaction, error: "Message not found: \(selectedGuid)")
                    return
                }
                let messageItem = safePerformReturning(message, selector: "_imMessageItem")
                let items = messageItem?.perform(NSSelectorFromString("_newChatItems"))?.takeRetainedValue()
                let partIndex = (data["partIndex"] as? Int) ?? (data["partIndex"] as? NSNumber)?.intValue ?? 0
                let item = items.flatMap { findPartChatItem(items: $0, partIndex: partIndex) }

                if let reactionType = data["reactionType"] as? String, !reactionType.isEmpty {
                    // Classic tapback only (emoji/sticker deferred — see README).
                    let reactionLong = parseReactionType(reactionType)
                    guard reactionLong != 0 else {
                        IMHelper.respondError(transaction: transaction, error: "Invalid reaction type: \(reactionType)")
                        return
                    }
                    let verb = reactionToVerb(reactionType)

                    var textString: String?
                    if let item = item, let text = safePerformReturning(item, selector: "text") {
                        textString = (text as? NSAttributedString)?.string
                    }
                    if textString == nil {
                        textString = (message.value(forKey: "text") as? NSAttributedString)?.string
                    }
                    let isAttachment = (textString == nil)
                    let messageGuid = message.value(forKey: "guid") as? String ?? ""
                    let summaryText = isAttachment ? "an attachment" : "\u{201C}\(textString ?? "")\u{201D}"
                    let newAttrStr = NSMutableAttributedString(string: verb + summaryText)

                    var assocGuid: String
                    var messageSummary: [String: Any]
                    var range: NSRange
                    if let item = item {
                        let itemText = safePerformReturning(item, selector: "text") as? NSAttributedString
                        messageSummary = isAttachment ? [:] : ["amc": 1, "ams": itemText?.string ?? textString ?? ""]
                        let partRangeSel = NSSelectorFromString("messagePartRange")
                        if item.responds(to: partRangeSel) {
                            typealias RangeMethod = @convention(c) (NSObject, Selector) -> NSRange
                            let imp = item.method(for: partRangeSel)
                            let fn = unsafeBitCast(imp, to: RangeMethod.self)
                            range = fn(item, partRangeSel)
                        } else {
                            range = NSRange(location: 0, length: 0)
                        }
                        assocGuid = isAttachment ? "p:\(partIndex)/\(messageGuid)"
                                  : (itemText == nil ? "bp:\(messageGuid)" : "p:\(partIndex)/\(messageGuid)")
                    } else {
                        messageSummary = isAttachment ? [:] : ["amc": 1, "ams": textString ?? ""]
                        range = NSRange(location: 0, length: textString?.count ?? 0)
                        assocGuid = messageGuid
                    }
                    createAndSend(newAttrStr, subjectStr, effectId, nil, assocGuid, reactionLong, range, messageSummary)
                } else {
                    // Threaded reply.
                    var threadId = message.value(forKey: "threadIdentifier") as? String ?? ""
                    if threadId.isEmpty, let item = item, let fn = resolved_IMCreateThreadIdentifier {
                        threadId = fn(item).takeRetainedValue() as String
                    }
                    guard !threadId.isEmpty else {
                        IMHelper.respondError(transaction: transaction, error: "Cannot create thread identifier")
                        return
                    }
                    createAndSend(attrStr, subjectStr, effectId, threadId, nil, nil, NSRange(location: 0, length: 0), nil)
                }
            }
        } else {
            createAndSend(attrStr, subjectStr, effectId, nil, nil, nil, NSRange(location: 0, length: 0), nil)
        }
    }

    // MARK: - Edit

    static func editMessage(data: [String: Any], transaction: String?) {
        guard let chat = getChat(guid: data["chatGuid"] as? String, transaction: transaction) else { return }
        guard let messageGuid = data["messageGuid"] as? String else { return }

        getMessageItem(guid: messageGuid) { message in
            guard let message = message else {
                IMHelper.respondError(transaction: transaction, error: "Message not found for edit!")
                return
            }
            let editedText = data["editedMessage"] as? String ?? ""
            let bcText = data["backwardsCompatibilityMessage"] as? String ?? ""
            let partIndex = (data["partIndex"] as? Int) ?? (data["partIndex"] as? NSNumber)?.intValue ?? 0
            let editedString = NSMutableAttributedString(string: editedText)
            let bcString = NSMutableAttributedString(string: bcText)

            guard let messageItem = safePerformReturning(message, selector: "_imMessageItem") else {
                IMHelper.respondError(transaction: transaction, error: "Failed to get message item for edit!")
                return
            }

            let tahoeSel = NSSelectorFromString("editMessageItem:atPartIndex:withNewPartText:newPartTranslation:backwardCompatabilityText:")
            let sequoiaSel = NSSelectorFromString("editMessageItem:atPartIndex:withNewPartText:backwardCompatabilityText:")

            if chat.responds(to: tahoeSel) {
                typealias EditMethod = @convention(c) (NSObject, Selector, NSObject, Int, NSMutableAttributedString, Any?, NSMutableAttributedString) -> Void
                let imp = chat.method(for: tahoeSel)
                let fn = unsafeBitCast(imp, to: EditMethod.self)
                fn(chat, tahoeSel, messageItem, partIndex, editedString, nil, bcString)
            } else if chat.responds(to: sequoiaSel) {
                typealias EditMethod = @convention(c) (NSObject, Selector, NSObject, Int, NSMutableAttributedString, NSMutableAttributedString) -> Void
                let imp = chat.method(for: sequoiaSel)
                let fn = unsafeBitCast(imp, to: EditMethod.self)
                fn(chat, sequoiaSel, messageItem, partIndex, editedString, bcString)
            } else {
                IMHelper.respondError(transaction: transaction, error: "No edit selector found!")
                return
            }
            IMHelper.respond(transaction: transaction)
        }
    }

    // MARK: - Unsend

    static func unsendMessage(data: [String: Any], transaction: String?) {
        guard let chat = getChat(guid: data["chatGuid"] as? String, transaction: transaction) else { return }
        guard let messageGuid = data["messageGuid"] as? String else { return }
        let partIndex = (data["partIndex"] as? Int) ?? (data["partIndex"] as? NSNumber)?.intValue ?? 0

        getMessageItem(guid: messageGuid) { message in
            guard let message = message,
                  let messageItem = safePerformReturning(message, selector: "_imMessageItem"),
                  let items = messageItem.perform(NSSelectorFromString("_newChatItems"))?.takeRetainedValue() else {
                IMHelper.respondError(transaction: transaction, error: "Message not found for unsend!")
                return
            }
            if let item = findPartChatItem(items: items, partIndex: partIndex) {
                safePerform(chat, selector: "retractMessagePart:", with: item)
            }
            IMHelper.respond(transaction: transaction)
        }
    }

    // MARK: - Delete (local)

    static func deleteMessage(data: [String: Any], transaction: String?) {
        guard let chat = getChat(guid: data["chatGuid"] as? String, transaction: transaction) else { return }
        guard let messageGuid = data["messageGuid"] as? String else { return }

        getMessageItem(guid: messageGuid) { message in
            guard let message = message,
                  let messageItem = safePerformReturning(message, selector: "_imMessageItem"),
                  let items = messageItem.perform(NSSelectorFromString("_newChatItems"))?.takeRetainedValue() else {
                IMHelper.respondError(transaction: transaction, error: "Message not found for delete!")
                return
            }
            if let arr = items as? [Any] {
                safePerform(chat, selector: "deleteChatItems:", with: arr as NSArray)
            } else {
                safePerform(chat, selector: "deleteChatItems:", with: [items] as NSArray)
            }
            IMHelper.respond(transaction: transaction)
        }
    }
}
