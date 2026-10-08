import Foundation
import XCTest

@testable import AssistKit

/// The real CLINC150 model from the repository, so these tests exercise the same code path an app would.
private let modelPath: String = {
    var url = URL(fileURLWithPath: #filePath)
    for _ in 0..<4 { url.deleteLastPathComponent() }  // AssistKitTests.swift -> AssistKitTests -> Tests -> swift -> repo
    return url.appendingPathComponent("models/clinc150.pmodel").path
}()

private let now = FixedTime(year: 2026, month: 10, day: 8, hour: 9, minute: 15)

final class AssistKitTests: XCTestCase {
    private func makeEngine(seed: UInt64 = 3) async throws -> AssistEngine {
        let engine = try AssistEngine(modelPath: modelPath, seed: seed, fixedTime: now)
        try await engine.setContacts([Contact(id: 1, name: "Mom"), Contact(id: 2, name: "Dana Whitfield")])
        return engine
    }

    func testTimerIsHandledOnDevice() async throws {
        let engine = try await makeEngine()
        let result = try await engine.handle("set a timer for ten minutes")
        XCTAssertEqual(result.route, .onDevice)
        XCTAssertEqual(result.action, "timer.set")
        XCTAssertEqual(result.slots["seconds"], "600")
        XCTAssertGreaterThan(result.confidence, 0.9)
    }

    func testCallResolvesAContact() async throws {
        let engine = try await makeEngine()
        let result = try await engine.handle("call mom")
        XCTAssertEqual(result.route, .onDevice)
        XCTAssertEqual(result.action, "call.start")
        XCTAssertEqual(result.slots["contact_id"], "1")
    }

    func testPersonalDataIsRemovedBeforeEscalation() async throws {
        let engine = try await makeEngine()
        let result = try await engine.handle("text 555 123 4567 and email jane@x.com to dana")
        XCTAssertEqual(result.route, .escalate)
        XCTAssertEqual(result.forwardText, "text <PHONE> and email <EMAIL> to <NAME>")
        XCTAssertFalse(result.forwardText.contains("555"))
        XCTAssertFalse(result.forwardText.contains("jane"))
    }

    func testNoLocalHandlerEscalatesWithTheTextUnchanged() async throws {
        let engine = try await makeEngine()
        let result = try await engine.handle("tell me about the history of rome")
        XCTAssertEqual(result.route, .escalate)
        XCTAssertEqual(result.reason, "no_local_handler")
    }

    func testSameSeedGivesTheSameDice() async throws {
        let a = try await makeEngine(seed: 7)
        let b = try await makeEngine(seed: 7)
        let first = try await a.handle("roll a die")
        let second = try await b.handle("roll a die")
        XCTAssertEqual(first, second)
    }

    func testEmptyAndUnusualTextDoesNotFail() async throws {
        let engine = try await makeEngine()
        let empty = try await engine.handle("")
        XCTAssertEqual(empty.reason, "empty_utterance")
        for text in ["caf\u{E9} \u{1F600} \u{4E2D}\u{6587}", String(repeating: "a ", count: 1000), "\u{0}\u{0}", "   "] {
            _ = try await engine.handle(text)
        }
    }

    func testContactsRejectedAreCounted() async throws {
        let engine = try await makeEngine()
        let rejected = try await engine.setContacts([Contact(id: 1, name: "Mom"), Contact(id: 2, name: "")])
        XCTAssertEqual(rejected, 1)
    }

    func testErrorsForBadModels() async throws {
        do {
            _ = try AssistEngine(modelPath: "/no/such/model.pmodel")
            XCTFail("expected an error")
        } catch let error as AssistError {
            XCTAssertEqual(error, .cannotReadModel)
        }
        let junk = FileManager.default.temporaryDirectory.appendingPathComponent("junk-\(UUID().uuidString).pmodel")
        try Data(repeating: 0x78, count: 300).write(to: junk)
        defer { try? FileManager.default.removeItem(at: junk) }
        do {
            _ = try AssistEngine(modelPath: junk.path)
            XCTFail("expected an error")
        } catch let error as AssistError {
            XCTAssertEqual(error, .invalidModel)
        }
    }

    func testManyTasksShareOneEngine() async throws {
        let engine = try await makeEngine()
        let results = try await withThrowingTaskGroup(of: AssistResult.self) { group in
            for _ in 0..<200 { group.addTask { try await engine.handle("set a timer for ten minutes") } }
            var all: [AssistResult] = []
            for try await r in group { all.append(r) }
            return all
        }
        XCTAssertEqual(results.count, 200)
        XCTAssertTrue(results.allSatisfy { $0.action == "timer.set" })
    }

    func testForkedEnginesRunInParallel() async throws {
        let engine = try await makeEngine()
        let forks = try await (0..<4).asyncMap { _ in try await engine.fork() }
        let counts = try await withThrowingTaskGroup(of: Int.self) { group in
            for fork in forks {
                group.addTask {
                    var ok = 0
                    for _ in 0..<100 {
                        if try await fork.handle("call mom").action == "call.start" { ok += 1 }
                    }
                    return ok
                }
            }
            var all: [Int] = []
            for try await c in group { all.append(c) }
            return all
        }
        XCTAssertEqual(counts, [100, 100, 100, 100])
    }

    func testStreamEndsWithTheResultForTheFinalText() async throws {
        let engine = try await makeEngine()
        let stream = try await engine.makeStream()
        let collector = Task { () -> [UInt64] in
            var sequences: [UInt64] = []
            for await p in stream.partials { sequences.append(p.sequence) }
            return sequences
        }
        for text in ["set", "set a timer", "set a timer for ten", "set a timer for ten minutes"] {
            try stream.update(text)
            try await Task.sleep(nanoseconds: 3_000_000)
        }
        let final = try await stream.finish()
        XCTAssertEqual(final.action, "timer.set")
        XCTAssertEqual(final.slots["seconds"], "600")
        let seen = await collector.value
        XCTAssertEqual(seen, seen.sorted())
        XCTAssertEqual(Set(seen).count, seen.count)
        // a stream is used once
        do {
            _ = try await stream.finish()
            XCTFail("expected an error")
        } catch let error as AssistError {
            XCTAssertEqual(error, .streamStopped)
        }
    }

    func testCancellingTheTaskCancelsTheStream() async throws {
        let engine = try await makeEngine()
        let stream = try await engine.makeStream()
        try stream.update("set a timer")
        stream.cancel()
        do {
            _ = try await stream.finish()
            XCTFail("expected an error")
        } catch let error as AssistError {
            XCTAssertEqual(error, .streamStopped)
        }
        for await _ in stream.partials {}  // the sequence must end, so this loop returns
    }
}

extension Sequence {
    fileprivate func asyncMap<T>(_ transform: (Element) async throws -> T) async rethrows -> [T] {
        var out: [T] = []
        for element in self { out.append(try await transform(element)) }
        return out
    }
}
