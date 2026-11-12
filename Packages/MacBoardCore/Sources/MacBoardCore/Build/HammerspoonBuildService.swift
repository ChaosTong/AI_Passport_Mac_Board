import Foundation

public enum BirkinBuildStatus: String, Sendable, Equatable {
    case idle
    case starting
    case busy
    case success
    case failure
    case noXcode = "no-xcode"
    case unknown

    public init(wire: String) {
        self = BirkinBuildStatus(rawValue: wire.trimmingCharacters(in: .whitespacesAndNewlines)) ?? .unknown
    }

    public var isTerminal: Bool {
        switch self {
        case .success, .failure, .idle, .noXcode, .unknown:
            return true
        case .starting, .busy:
            return false
        }
    }

    public var isBuilding: Bool {
        self == .starting || self == .busy
    }
}

/// Birkin 推送 / 补齐的一条构建文案（Hammerspoon 菜单栏标题或 alert 显示过的原文，已清洗）。
public struct BirkinProgressLine: Sendable, Equatable {
    public let seq: Int
    /// busy / success / failure / idle / info / hidden
    public let state: String
    public let text: String
}

/// Birkin 写下的进度快照（与 tools/hammerspoon/init.lua 的 MACBOARD_PROGRESS_FILE 一致）。
public let birkinProgressFileURL: URL = FileManager.default.homeDirectoryForCurrentUser
    .appendingPathComponent("Library/Application Support/MacBoard/birkin-progress.json")

private struct BirkinProgressFile: Decodable {
    var seq: Int?
    var state: String?
    var text: String?
    var items: [Item]?

    struct Item: Decodable {
        var seq: Int
        var state: String
        var text: String
    }
}

public struct HammerspoonBuildService: Sendable {
    public var hsExecutable: String

    public init(hsExecutable: String? = nil) {
        if let hsExecutable {
            self.hsExecutable = hsExecutable
        } else if FileManager.default.isExecutableFile(atPath: "/opt/homebrew/bin/hs") {
            self.hsExecutable = "/opt/homebrew/bin/hs"
        } else if FileManager.default.isExecutableFile(atPath: "/usr/local/bin/hs") {
            self.hsExecutable = "/usr/local/bin/hs"
        } else {
            self.hsExecutable = "/opt/homebrew/bin/hs"
        }
    }

    public func triggerBuild() async -> String {
        await runLuaExpression("birkinBuild()")
    }

    public func stopBuild() async -> String {
        await runLuaExpression(
            "(type(birkinStop) == 'function' and birkinStop()) or 'no-stop'"
        )
    }

    public func status() async -> BirkinBuildStatus {
        BirkinBuildStatus(wire: await runLuaExpression("birkinStatus()"))
    }

    public func errorLines() async -> [String] {
        let raw = await runLuaExpression("birkinErrors()")
        return raw
            .split(whereSeparator: \.isNewline)
            .map { String($0).trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
    }

    /// 一次性补齐：Birkin 记录的 seq 之后的文案（仅连接时调用，不轮询）。
    /// Birkin 没有导出 `birkinProgressLines` 时返回 nil。
    public func progressLines(since seq: Int) async -> (latestSeq: Int, lines: [BirkinProgressLine])? {
        let raw = await runLuaExpression(
            "(type(birkinProgressLines) == 'function' and birkinProgressLines(\(max(0, seq)))) or ''"
        )
        let rows = raw.split(whereSeparator: \.isNewline).map(String.init)
        guard let head = rows.first, head.hasPrefix("#"), let latest = Int(head.dropFirst()) else {
            return nil
        }
        var lines: [BirkinProgressLine] = []
        for row in rows.dropFirst() {
            let cols = row.split(separator: "\t", maxSplits: 2, omittingEmptySubsequences: false)
            guard cols.count == 3, let seq = Int(cols[0]),
                  let text = Self.cleanProgress(String(cols[2])) else { continue }
            lines.append(BirkinProgressLine(seq: seq, state: String(cols[1]), text: text))
        }
        return (latest, lines)
    }

    public struct ProgressFileRead: Sendable {
        public var value: (latestSeq: Int, lines: [BirkinProgressLine], hidden: Bool)?
        public var error: String?
    }

    /// 读取 Birkin 进度文件里 seq 之后的新文案。文件还不存在时 value 和 error 都是 nil。
    public static func readProgressFile(since seq: Int) -> ProgressFileRead {
        let url = birkinProgressFileURL
        guard FileManager.default.fileExists(atPath: url.path) else {
            return ProgressFileRead(value: nil, error: nil)
        }
        do {
            let data = try Data(contentsOf: url)
            let file = try JSONDecoder().decode(BirkinProgressFile.self, from: data)
            let latest = file.seq ?? file.items?.last?.seq ?? 0
            var lines: [BirkinProgressLine] = []
            for item in file.items ?? [] where item.seq > seq {
                let text = cleanProgress(item.text) ?? ""
                lines.append(BirkinProgressLine(seq: item.seq, state: item.state, text: text))
            }
            return ProgressFileRead(value: (latest, lines, file.state == "hidden"), error: nil)
        } catch {
            if isMissingProgressFile(error) {
                return ProgressFileRead(value: nil, error: nil)
            }
            return ProgressFileRead(value: nil, error: error.localizedDescription)
        }
    }

    /// Hammerspoon 原子写进度时可能短暂不可读；不当成硬错误。
    private static func isMissingProgressFile(_ error: Error) -> Bool {
        let ns = error as NSError
        if ns.domain == NSCocoaErrorDomain, ns.code == NSFileReadNoSuchFileError { return true }
        if ns.domain == NSPOSIXErrorDomain, ns.code == ENOENT { return true }
        if let underlying = ns.userInfo[NSUnderlyingErrorKey] as? NSError {
            return isMissingProgressFile(underlying)
        }
        return false
    }

    /// 与 init.lua 空快照一致，避免 Mac 先启动时一直报「文件不存在」。
    public static func ensureProgressFileExists() {
        let url = birkinProgressFileURL
        let dir = url.deletingLastPathComponent()
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        guard !FileManager.default.fileExists(atPath: url.path) else { return }
        let stub = Data("{\"seq\":0,\"state\":\"idle\",\"text\":\"\",\"items\":[]}".utf8)
        try? stub.write(to: url, options: .atomic)
    }

    static func cleanProgress(_ text: String) -> String? {
        // Birkin 的状态 emoji 换成板子字体有的 ASCII，其余 emoji 去掉（板子字体没有这些字形）
        let mapped = text
            .replacingOccurrences(of: "✅", with: "OK ")
            .replacingOccurrences(of: "❌", with: "FAIL ")
            .replacingOccurrences(of: "⚠️", with: "! ")
            .replacingOccurrences(of: "⚠", with: "! ")
        let scalars = mapped.unicodeScalars.filter { scalar in
            let v = scalar.value
            if v == 0xFE0F || v == 0x200D { return false }
            if (0x2600...0x27BF).contains(v) || v >= 0x1F000 { return false }
            return !scalar.properties.isEmojiPresentation
        }
        var line = String(String.UnicodeScalarView(scalars)).trimmingCharacters(in: .whitespacesAndNewlines)
        if line == "nil" || line.isEmpty { return nil }
        // 板子字体只有 ASCII + 常用汉字：把 Xcode 的分隔符换成 ASCII。
        line = line
            .replacingOccurrences(of: "\u{2009}", with: " ")
            .replacingOccurrences(of: "\u{202F}", with: " ")
            .replacingOccurrences(of: "\u{00A0}", with: " ")
            .replacingOccurrences(of: "•", with: "-")
            .replacingOccurrences(of: "│", with: "|")
            .replacingOccurrences(of: "·", with: "-")
            .replacingOccurrences(of: "…", with: "...")
            .replacingOccurrences(of: "，", with: ", ")
        if line.hasPrefix("Building - ") {
            line = String(line.dropFirst("Building - ".count))
        }
        return line.isEmpty ? nil : line
    }

    private func runLuaExpression(_ expr: String) async -> String {
        await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .utility).async {
                let process = Process()
                let pipe = Pipe()
                process.executableURL = URL(fileURLWithPath: hsExecutable)
                process.arguments = ["-c", "return \(expr)"]
                process.standardOutput = pipe
                process.standardError = pipe
                do {
                    try process.run()
                    process.waitUntilExit()
                    let data = pipe.fileHandleForReading.readDataToEndOfFile()
                    let text = String(data: data, encoding: .utf8)?
                        .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
                    continuation.resume(returning: text)
                } catch {
                    continuation.resume(returning: "no-xcode")
                }
            }
        }
    }
}
