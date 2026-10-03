// hu-vision-ocr — on-device text + scene labels for one image (Apple Vision),
// for h-uman's local photo understanding (include/human/context/local_vision.h).
//
// Usage:
//   hu-vision-ocr <image-path>
//
// Prints ONE JSON object on stdout and nothing else:
//   {"labels":["birthday_cake"],"lines":["HAPPY 40th"],"ms":212}
//   {"error":"unreadable"}                       (exit 1)
// lines: recognized text, top-to-bottom then left-to-right, one string per
// observation — the AUTHORITATIVE text of the photo (the caption model is never
// trusted to quote it). labels: up to 5 classifier identifiers above 0.3.
// Nothing is written anywhere and nothing leaves the process.

import Foundation
import Vision

func emit(_ obj: [String: Any], code: Int32) -> Never {
    if let data = try? JSONSerialization.data(withJSONObject: obj, options: [.sortedKeys]) {
        FileHandle.standardOutput.write(data)
        FileHandle.standardOutput.write(Data("\n".utf8))
    } else {
        print("{\"error\":\"encode\"}")
    }
    exit(code)
}

let args = CommandLine.arguments
guard args.count == 2 else { emit(["error": "usage: hu-vision-ocr <image-path>"], code: 2) }
let started = Date()
let handler = VNImageRequestHandler(url: URL(fileURLWithPath: args[1]), options: [:])
let ocr = VNRecognizeTextRequest()
ocr.recognitionLevel = .accurate
ocr.usesLanguageCorrection = true
let classify = VNClassifyImageRequest()
do {
    try handler.perform([ocr, classify])
} catch {
    emit(["error": "unreadable"], code: 1)
}

let observations = (ocr.results ?? []).sorted { a, b in
    let dy = a.boundingBox.midY - b.boundingBox.midY
    if abs(dy) > 0.02 { return dy > 0 }  // Vision's origin is bottom-left
    return a.boundingBox.minX < b.boundingBox.minX
}
let lines = observations.compactMap { $0.topCandidates(1).first?.string }
    .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
    .filter { !$0.isEmpty }
let labels = (classify.results ?? [])
    .filter { $0.confidence > 0.3 }
    .prefix(5)
    .map { $0.identifier }
let ms = Int(Date().timeIntervalSince(started) * 1000)
emit(["lines": lines, "labels": Array(labels), "ms": ms], code: 0)
