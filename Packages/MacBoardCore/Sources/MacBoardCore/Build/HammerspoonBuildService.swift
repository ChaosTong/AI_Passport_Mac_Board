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
