import Foundation

public struct HeartbeatSnapshot: Codable, Equatable, Sendable {
    public var page: String?
    public var total: Int?
    public var running: Int?
    public var waiting: Int?
    public var msg: String?
    public var entries: [String]?
    public var tokens: Int?
    public var tokensToday: Int?
    public var completed: Bool?
    /// 最近一次编译结果（"success" / "failure"），任何页面都带，板子据此播放提示音。
    public var buildResult: String?
    /// 最近一次编译结束时间（epoch ms），同一次编译不变；板子按它变化边沿触发音效。
    public var buildId: Int?

    enum CodingKeys: String, CodingKey {
        case page, total, running, waiting, msg, entries, tokens, completed
        case buildResult = "bres"
        case buildId = "bid"
        case tokensToday = "tokens_today"
    }

    public init(
        page: String? = nil,
        total: Int? = nil,
        running: Int? = nil,
        waiting: Int? = nil,
        msg: String? = nil,
        entries: [String]? = nil,
        tokens: Int? = nil,
        tokensToday: Int? = nil,
        completed: Bool? = nil
    ) {
        self.page = page
        self.total = total
        self.running = running
        self.waiting = waiting
        self.msg = msg
        self.entries = entries
        self.tokens = tokens
        self.tokensToday = tokensToday
        self.completed = completed
    }
}

public struct NUSBatteryInfo: Equatable, Sendable {
    public var pct: Int?

    public init(pct: Int? = nil) {
        self.pct = pct
    }
}

public struct NUSAckData: Equatable, Sendable {
    public var sec: Bool?
    public var bat: NUSBatteryInfo?

    public init(sec: Bool? = nil, bat: NUSBatteryInfo? = nil) {
        self.sec = sec
        self.bat = bat
    }
}

public struct NUSAck: Equatable, Sendable {
    public var ack: String
    public var ok: Bool
    public var error: String?
    public var data: NUSAckData?

    public init(ack: String, ok: Bool, error: String? = nil, data: NUSAckData? = nil) {
        self.ack = ack
        self.ok = ok
        self.error = error
        self.data = data
    }
}

public enum NUSInboundMessage: Equatable, Sendable {
    case ack(NUSAck)
    case command(String, [String: JSONValue])
    case heartbeat(HeartbeatSnapshot)
    case unknown(String)
}

public enum NUSOutbound: Sendable {
    case rawJSON(Data)
}

public enum MacBoardBLEError: Error, LocalizedError {
    case notReady
    case encodeFailed
    case invalidJSON

    public var errorDescription: String? {
        switch self {
        case .notReady: return "NUS link not ready"
        case .encodeFailed: return "Failed to encode JSON line"
        case .invalidJSON: return "Invalid JSON"
        }
    }
}

public func encodeLine(_ object: [String: Any]) throws -> Data {
    guard JSONSerialization.isValidJSONObject(object) else {
        throw MacBoardBLEError.invalidJSON
    }
    var data = try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
    data.append(0x0A)
    return data
}

enum NDJSON {
    static let encoder: JSONEncoder = {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.withoutEscapingSlashes, .sortedKeys]
        return encoder
    }()

    static func encodeLine<T: Encodable>(_ value: T) throws -> Data {
        var data = try encoder.encode(value)
        data.append(0x0A)
        return data
    }
}

struct NDJSONLineFramer {
    private var buffer = Data()

    mutating func ingest(_ chunk: Data) -> [String] {
        buffer.append(chunk)
        var lines: [String] = []
        while let index = buffer.firstIndex(of: 0x0A) {
            let lineData = buffer[..<index]
            buffer.removeSubrange(...index)
            guard !lineData.isEmpty,
                  let line = String(data: lineData, encoding: .utf8) else { continue }
            let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
            if !trimmed.isEmpty {
                lines.append(trimmed)
            }
        }
        return lines
    }

    mutating func reset() {
        buffer.removeAll(keepingCapacity: true)
    }
}

enum NUSInboundDecoder {
    static func decode(_ line: String) -> NUSInboundMessage {
        let data = Data(line.utf8)
        guard let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return .unknown(line)
        }

        if let ack = object["ack"] as? String {
            let ok = (object["ok"] as? Bool) ?? false
            let error = object["error"] as? String
            var ackData: NUSAckData?
            if let raw = object["data"] as? [String: Any] {
                let sec = raw["sec"] as? Bool
                var bat: NUSBatteryInfo?
                if let batObj = raw["bat"] as? [String: Any] {
                    let pct: Int?
                    if let i = batObj["pct"] as? Int {
                        pct = i
                    } else if let d = batObj["pct"] as? Double {
                        pct = Int(d)
                    } else {
                        pct = nil
                    }
                    bat = NUSBatteryInfo(pct: pct)
                }
                ackData = NUSAckData(sec: sec, bat: bat)
            }
            return .ack(NUSAck(ack: ack, ok: ok, error: error, data: ackData))
        }

        if let cmd = object["cmd"] as? String {
            let payload = (try? JSONDecoder().decode([String: JSONValue].self, from: data)) ?? [:]
            return .command(cmd, payload)
        }

        if let heartbeat = try? JSONDecoder().decode(HeartbeatSnapshot.self, from: data),
           heartbeat.page != nil || heartbeat.total != nil || heartbeat.msg != nil || heartbeat.running != nil {
            return .heartbeat(heartbeat)
        }

        return .unknown(line)
    }
}
