import Foundation
import MacBoardBLE

public enum WireCommand: String, Sendable {
    case name
    case owner
    case status
    case unpair
}

public enum BoardWireAck: String, Sendable {
    case media
    case build
}

public enum RawWireError: Error, LocalizedError {
    case invalidJSON
    case empty
    case forbiddenCommand(String)
    case unsupportedShape

    public var errorDescription: String? {
        switch self {
        case .invalidJSON: return "Invalid JSON"
        case .empty: return "Empty payload"
        case .forbiddenCommand(let cmd): return "Forbidden command: \(cmd)"
        case .unsupportedShape: return "Unsupported raw wire shape"
        }
    }
}

public enum RawWireValidator {
    private static let forbidden: Set<String> = [
        "char_begin", "file", "chunk", "file_end", "char_end", "permission"
    ]

    public static func validateAndEncode(_ text: String) throws -> Data {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { throw RawWireError.empty }
        guard let data = trimmed.data(using: .utf8),
              let object = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw RawWireError.invalidJSON
        }

        if let ack = object["ack"] as? String {
            guard BoardWireAck(rawValue: ack) != nil else {
                throw RawWireError.unsupportedShape
            }
            return try encodeLine(object)
        }

        if let cmd = object["cmd"] as? String {
            if forbidden.contains(cmd) || cmd.hasPrefix("folder_") {
                throw RawWireError.forbiddenCommand(cmd)
            }
            switch cmd {
            case WireCommand.name.rawValue,
                 WireCommand.owner.rawValue,
                 WireCommand.status.rawValue,
                 WireCommand.unpair.rawValue:
                return try encodeLine(object)
            default:
                throw RawWireError.forbiddenCommand(cmd)
            }
        }

        if object["time"] != nil {
            return try encodeLine(object)
        }

        if object["page"] != nil || object["total"] != nil || object["running"] != nil || object["msg"] != nil {
            return try encodeLine(object)
        }

        throw RawWireError.unsupportedShape
    }

    public static func timeSyncJSON() throws -> Data {
        let epoch = Int(Date().timeIntervalSince1970)
        let offset = TimeZone.current.secondsFromGMT()
        return try encodeLine(["time": [epoch, offset]])
    }

    public static func commandJSON(_ command: WireCommand, name: String? = nil) throws -> Data {
        switch command {
        case .name, .owner:
            let value = FieldLimits.truncate(name ?? "", max: FieldLimits.nameMax) ?? ""
            return try encodeLine(["cmd": command.rawValue, "name": value])
        case .status, .unpair:
            return try encodeLine(["cmd": command.rawValue])
        }
    }

    public static func ackJSON(_ ack: BoardWireAck, ok: Bool, error: String? = nil) throws -> Data {
        var object: [String: Any] = ["ack": ack.rawValue, "ok": ok]
        if let error, !error.isEmpty {
            object["error"] = FieldLimits.truncate(error, max: FieldLimits.entryMax)
        }
        return try encodeLine(object)
    }
}
