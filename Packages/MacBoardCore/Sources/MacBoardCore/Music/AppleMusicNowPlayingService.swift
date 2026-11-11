import Foundation

public struct MusicPlaylistInfo: Equatable, Sendable {
    public var name: String
    public var trackCount: Int

    public init(name: String, trackCount: Int) {
        self.name = name
        self.trackCount = trackCount
    }
}

public struct MusicPlaylistSnapshot: Equatable, Sendable {
    public var name: String
    public var tracks: [String]
    /// 0-based index of the current track in `tracks`.
    public var currentIndex: Int
}

public struct NowPlayingSnapshot: Equatable, Sendable {
    public var isPlaying: Bool
    public var title: String
    public var artist: String
    public var album: String?
    public var stopped: Bool
    /// Playback position in whole seconds (display / Equatable).
    public var positionSeconds: Int
    /// Playback position in milliseconds (maps to heartbeat `waiting` on music page).
    public var positionMilliseconds: Int
    /// Track duration in whole seconds (maps to heartbeat `total`).
    public var durationSeconds: Int

    public static let idle = NowPlayingSnapshot(
        isPlaying: false,
        title: "未在播放",
        artist: "",
        album: nil,
        stopped: true,
        positionSeconds: 0,
        positionMilliseconds: 0,
        durationSeconds: 0
    )
}

public enum AppleMusicError: Error, LocalizedError {
    case scriptFailed(Int32, String?)
    case musicUnavailable

    public var errorDescription: String? {
        switch self {
        case .scriptFailed(let code, let msg):
            if let msg, !msg.isEmpty { return "Music 脚本失败 (\(code)): \(msg)" }
            return "Music 脚本失败 (\(code))"
        case .musicUnavailable:
            return "无法与「音乐」通信；请在系统设置 → 隐私与安全性 → 自动化中允许 MacBoard 控制音乐。"
        }
    }
}

public final class AppleMusicNowPlayingService: @unchecked Sendable {
    private let queue = DispatchQueue(label: "com.easyulife.MacBoard.music", qos: .utility)

    public init() {}

    public func refresh() async throws -> NowPlayingSnapshot {
        try await run(script: Self.readScript)
    }

    public func toggle() async throws {
        _ = try await runRaw(script: """
            tell application "Music"
                playpause
            end tell
            """)
    }

    public func play() async throws {
        _ = try await runRaw(script: """
            tell application "Music"
                if player state is not playing then
                    play
                end if
            end tell
            """)
    }

    public func pause() async throws {
        _ = try await runRaw(script: """
            tell application "Music"
                if player state is playing then
                    pause
                end if
            end tell
            """)
    }

    public func next() async throws {
        _ = try await runRaw(script: #"tell application "Music" to next track"#)
    }

    public func previous() async throws {
        _ = try await runRaw(script: #"tell application "Music" to previous track"#)
    }

    public func loadCurrentPlaylistTracks() async throws -> MusicPlaylistSnapshot? {
        let result = try await runRaw(script: """
            tell application "Music"
                if player state is stopped then
                    return "stopped"
                end if
                try
                    set pl to current playlist
                    set n to count of tracks of pl
                    set nm to name of pl
                    set cur to index of current track
                    set out to nm & linefeed & (n as string) & linefeed & (cur as string) & linefeed
                    set cap to n
                    if cap > 120 then set cap to 120
                    repeat with i from 1 to cap
                        set out to out & name of track i of pl & tab
                    end repeat
                    return out
                on error
                    return "unknown" & linefeed & "0" & linefeed & "0" & linefeed
                end try
            end tell
            """)
        let trimmed = result.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed == "stopped" || trimmed.isEmpty {
            return nil
        }
        let lines = trimmed.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        guard lines.count >= 3 else { return nil }
        let name = lines[0]
        let count = Int(lines[1]) ?? 0
        let current = max(0, (Int(lines[2]) ?? 1) - 1)
        let body = lines.count > 3 ? lines[3] : ""
        var tracks = body.split(separator: "\t", omittingEmptySubsequences: true).map(String.init)
        if count > 0, tracks.count > count {
            tracks = Array(tracks.prefix(count))
        }
        if tracks.isEmpty, count == 0 {
            return nil
        }
        let clampedCurrent = min(max(0, current), max(0, tracks.count - 1))
        return MusicPlaylistSnapshot(name: name, tracks: tracks, currentIndex: clampedCurrent)
    }

    public func playPlaylistTrack(at index: Int) async throws {
        let oneBased = index + 1
        _ = try await runRaw(script: """
            tell application "Music"
                play track \(oneBased) of current playlist
            end tell
            """)
    }

    public func currentPlaylist() async throws -> MusicPlaylistInfo? {
        let result = try await runRaw(script: """
            tell application "Music"
                if player state is stopped then
                    return "stopped"
                end if
                try
                    set pl to current playlist
                    set n to count of tracks of pl
                    set nm to name of pl
                    return nm & tab & (n as string)
                on error
                    return "unknown" & tab & "0"
                end try
            end tell
            """)
        let trimmed = result.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed == "stopped" || trimmed.isEmpty {
            return nil
        }
        let parts = trimmed.split(separator: "\t", maxSplits: 1).map(String.init)
        guard let name = parts.first else { return nil }
        let count = parts.count > 1 ? Int(parts[1]) ?? 0 : 0
        return MusicPlaylistInfo(name: name, trackCount: max(0, count))
    }

    private func run(script: String) async throws -> NowPlayingSnapshot {
        let result = try await runRaw(script: script)
        return Self.parseReadResult(result)
    }

    private func runRaw(script: String) async throws -> String {
        try await withCheckedThrowingContinuation { continuation in
            queue.async {
                var error: NSDictionary?
                let appleScript = NSAppleScript(source: script)
                guard let appleScript else {
                    continuation.resume(throwing: AppleMusicError.musicUnavailable)
                    return
                }
                let output = appleScript.executeAndReturnError(&error)
                if let error {
                    let code = (error[NSAppleScript.errorNumber] as? Int32) ?? -1
                    let msg = error[NSAppleScript.errorMessage] as? String
                    continuation.resume(throwing: AppleMusicError.scriptFailed(code, msg))
                    return
                }
                continuation.resume(returning: output.stringValue ?? "")
            }
        }
    }

    private static let readScript = """
    tell application "Music"
        set p to player state as string
        if p is "playing" or p is "paused" then
            set t to current track
            set trackName to name of t
            set trackArtist to artist of t
            try
                set trackAlbum to album of t
            on error
                set trackAlbum to ""
            end try
            set trackPosMs to 0
            try
                set trackPosMs to (round ((player position as real) * 1000)) as integer
            on error
                try
                    set trackPosMs to (player position as integer) * 1000
                on error
                    set trackPosMs to 0
                end try
            end try
            set trackDur to 0
            try
                -- Cloud / shared tracks: avoid `round duration` (coerces to real and fails on -1700).
                set trackDur to duration of t as integer
            on error
                try
                    tell t to set trackDur to duration as integer
                on error
                    set trackDur to 0
                end try
            end try
            return p & tab & trackName & tab & trackArtist & tab & trackAlbum & tab & trackPosMs & tab & trackDur
        end if
        return "stopped"
    end tell
    """

    private static func parseReadResult(_ raw: String) -> NowPlayingSnapshot {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed == "stopped" || trimmed.isEmpty {
            return .idle
        }
        let parts = trimmed.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
        guard parts.count >= 3 else { return .idle }
        let state = parts[0]
        let title = parts[1]
        let artist = parts[2]
        let album = parts.count > 3 && !parts[3].isEmpty ? parts[3] : nil
        let positionMs = parts.count > 4 ? Int(parts[4]) ?? 0 : 0
        let duration = parts.count > 5 ? Int(parts[5]) ?? 0 : 0
        let clampedMs = max(0, positionMs)
        return NowPlayingSnapshot(
            isPlaying: state == "playing",
            title: title,
            artist: artist,
            album: album,
            stopped: false,
            positionSeconds: clampedMs / 1000,
            positionMilliseconds: clampedMs,
            durationSeconds: max(0, duration)
        )
    }
}
