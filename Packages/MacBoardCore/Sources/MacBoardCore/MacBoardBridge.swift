import Combine
import CoreBluetooth
import Foundation
import MacBoardBLE

public enum MacBoardConnectionPhase: String, Sendable {
    case idle
    case bluetoothOff
    case scanning
    case connecting
    case connected
    case ready
}

public struct DiscoveredBoard: Identifiable, Sendable, Equatable {
    public let id: UUID
    public let name: String
    public let rssi: Int
}

public struct BridgeLogLine: Identifiable, Sendable, Equatable {
    public let id: UUID
    public let date: Date
    public let text: String

    public init(text: String, date: Date = Date()) {
        self.id = UUID()
        self.date = date
        self.text = text
    }
}

@MainActor
public final class MacBoardBridge: NSObject, ObservableObject {
    @Published public private(set) var phase: MacBoardConnectionPhase = .idle
    @Published public private(set) var discovered: [DiscoveredBoard] = []
    @Published public private(set) var logs: [BridgeLogLine] = []
    @Published public private(set) var connectedName: String?
    @Published public private(set) var lastKnownDevice: LastBoardDevice?
    @Published public private(set) var linkSecure = false
    @Published public private(set) var activePage: MacBoardPage = .music
    @Published public private(set) var nowPlaying: NowPlayingSnapshot = .idle
    @Published public private(set) var buildStatus: BirkinBuildStatus = .idle
    @Published public private(set) var buildErrorLines: [String] = []
    @Published public var passportDisplayName: String = "Passport" {
        didSet {
            let trimmed = passportDisplayName.trimmingCharacters(in: .whitespacesAndNewlines)
            let limited = FieldLimits.truncate(trimmed, max: FieldLimits.nameMax) ?? "Passport"
            if limited != passportDisplayName {
                passportDisplayName = limited
                return
            }
            UserDefaults.standard.set(limited, forKey: Self.displayNameKey)
            if phase == .ready {
                sendCommand(.name, name: limited)
            }
        }
    }

    private static let displayNameKey = "macboard.passportDisplayName"

    private let central: NUSCentralManager
    private let music = AppleMusicNowPlayingService()
    private let buildService = HammerspoonBuildService()
    private let ownerName = "MacBoard"
    private let namePrefix: String

    private var peripheralsByID: [UUID: CBPeripheral] = [:]
    private var keepaliveTimer: Timer?
    private var secureProbeTimer: Timer?
    private var musicPollTimer: Timer?
    private var buildPollTimer: Timer?
    private var didLogSecureWait = false
    private var autoReconnectEnabled = true
    private var lastMusicSnapshot: NowPlayingSnapshot?
    private var playlistPickerActive = false
    private var playlistName = ""
    private var playlistTracks: [String] = []
    private var playlistWindowStart = 0
    private let playlistPageSize = 8

    public init(namePrefix: String = "Claude") {
        self.namePrefix = namePrefix
        self.passportDisplayName = UserDefaults.standard.string(forKey: Self.displayNameKey) ?? "Passport"
        self.lastKnownDevice = LastBoardStore.load()
        self.central = NUSCentralManager(namePrefix: namePrefix)
        super.init()
        self.central.delegate = self
    }

    public func bootstrap() {
        attemptAutoReconnectIfNeeded(reason: "launch")
    }

    public func startScanning() {
        autoReconnectEnabled = true
        discovered = []
        peripheralsByID = [:]
        phase = .scanning
        central.startScanning()
        appendLog("扫描 \(namePrefix)* …")
    }

    public func stopScanning() {
        central.stopScanning()
        if phase == .scanning {
            phase = .idle
        }
    }

    public func connect(id: UUID) {
        guard let peripheral = peripheralsByID[id] else {
            appendLog("未知设备 \(id)")
            return
        }
        autoReconnectEnabled = true
        rememberDevice(uuid: id, name: peripheral.name)
        phase = .connecting
        connectedName = peripheral.name
        central.connect(peripheral)
        appendLog("连接 \(peripheral.name ?? id.uuidString)…")
    }

    public func disconnect() {
        autoReconnectEnabled = false
        stopTimers()
        linkSecure = false
        central.disconnect()
        phase = .idle
        connectedName = nil
        appendLog("已断开")
    }

    public func sendStatus() {
        sendCommand(.status)
    }

    public func sendUnpair() {
        sendCommand(.unpair)
    }

    public func setActivePage(_ page: MacBoardPage) {
        activePage = page
        pushCurrentHeartbeat()
    }

    public func refreshMusicNow() {
        Task {
            await refreshMusicAndPushIfNeeded(force: true)
        }
    }

    public func triggerTestBuild() {
        Task { await handleBuildStart(fromBoard: false) }
    }

    public func clearLogs() {
        logs.removeAll()
    }

    private func rememberDevice(uuid: UUID, name: String?) {
        let device = LastBoardDevice(uuid: uuid, name: name)
        lastKnownDevice = device
        LastBoardStore.save(device)
    }

    private func attemptAutoReconnectIfNeeded(reason: String) {
        guard autoReconnectEnabled else { return }
        guard phase == .idle || phase == .bluetoothOff else { return }
        guard let last = lastKnownDevice else {
            startScanning()
            return
        }
        appendLog("自动重连 (\(reason)): \(last.name ?? last.uuid.uuidString)")
        phase = .connecting
        connectedName = last.name
        if central.retrieveAndConnect(uuid: last.uuid) {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { [weak self] in
                guard let self else { return }
                if self.phase == .connecting || self.phase == .idle {
                    self.phase = .scanning
                    self.central.startScanning()
                }
            }
        } else {
            startScanning()
        }
    }

    private func maybeAutoConnectDiscovered(id: UUID, name: String?) {
        guard autoReconnectEnabled else { return }
        guard phase == .scanning || phase == .idle || phase == .connecting else { return }
        guard let last = lastKnownDevice, last.uuid == id else { return }
        if phase == .ready { return }
        connect(id: id)
    }

    private func onLinkReady() {
        linkSecure = false
        didLogSecureWait = false
        phase = .connected
        appendLog("NUS 就绪 — 探测加密")
        startSecureProbe()
    }

    private func markLinkSecure(source: String) {
        guard !linkSecure else { return }
        linkSecure = true
        stopSecureProbe()
        phase = .ready
        appendLog("链路加密就绪 (\(source))")
        sendRaw((try? RawWireValidator.timeSyncJSON()) ?? Data(), note: "time")
        sendCommand(.owner, name: ownerName)
        sendCommand(.name, name: passportDisplayName)
        pushCurrentHeartbeat()
        startKeepalive()
        startMusicPolling()
        Task { await refreshBuildSnapshot() }
    }

    private func startSecureProbe() {
        stopSecureProbe()
        stopKeepalive()
        probeSecureLink()
        secureProbeTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.probeSecureLink() }
        }
    }

    private func stopSecureProbe() {
        secureProbeTimer?.invalidate()
        secureProbeTimer = nil
    }

    private func probeSecureLink() {
        guard phase == .connected, !linkSecure else { return }
        sendRaw((try? RawWireValidator.timeSyncJSON()) ?? Data(), note: "time")
        sendCommand(.owner, name: ownerName)
        sendCommand(.name, name: passportDisplayName)
        let snapshot = currentHeartbeatSnapshot()
        central.sendHeartbeat(snapshot) { [weak self] error in
            Task { @MainActor in
                guard let self, !self.linkSecure else { return }
                if let error {
                    let lower = error.localizedDescription.lowercased()
                    if lower.contains("encrypt") || lower.contains("auth") || lower.contains("pair") {
                        if !self.didLogSecureWait {
                            self.didLogSecureWait = true
                            self.appendLog("等待配对加密: \(error.localizedDescription)")
                        }
                    }
                }
            }
        }
    }

    private func startKeepalive() {
        stopKeepalive()
        keepaliveTimer = Timer.scheduledTimer(withTimeInterval: 10, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.pushCurrentHeartbeat() }
        }
    }

    private func stopKeepalive() {
        keepaliveTimer?.invalidate()
        keepaliveTimer = nil
    }

    private func startMusicPolling() {
        stopMusicPolling()
        musicPollTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.refreshMusicAndPushIfNeeded(force: false) }
        }
    }

    private func stopMusicPolling() {
        musicPollTimer?.invalidate()
        musicPollTimer = nil
    }

    private func startBuildPolling() {
        stopBuildPolling()
        buildPollTimer = Timer.scheduledTimer(withTimeInterval: 2.5, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.pollBuildStatus() }
        }
    }

    private func stopBuildPolling() {
        buildPollTimer?.invalidate()
        buildPollTimer = nil
    }

    private func stopTimers() {
        stopKeepalive()
        stopSecureProbe()
        stopMusicPolling()
        stopBuildPolling()
    }

    private func pushCurrentHeartbeat() {
        guard phase == .ready else { return }
        let snapshot = currentHeartbeatSnapshot()
        central.sendHeartbeat(snapshot) { [weak self] error in
            Task { @MainActor in
                if let error {
                    self?.appendLog("heartbeat 失败: \(error.localizedDescription)")
                }
            }
        }
    }

    private func currentHeartbeatSnapshot() -> HeartbeatSnapshot {
        if playlistPickerActive, activePage == .music {
            return playlistHeartbeat()
        }
        switch activePage {
        case .music:
            return musicHeartbeat()
        case .build:
            return buildHeartbeat()
        }
    }

    private func playlistHeartbeat() -> HeartbeatSnapshot {
        let start = min(max(0, playlistWindowStart), max(0, playlistTracks.count))
        let end = min(start + playlistPageSize, playlistTracks.count)
        let slice = playlistTracks[start..<end].map {
            FieldLimits.truncate($0, max: FieldLimits.entryMax) ?? $0
        }
        return HeartbeatSnapshot(
            page: "playlist",
            total: playlistTracks.count,
            running: 0,
            waiting: start,
            msg: FieldLimits.truncate(playlistName, max: FieldLimits.messageMax),
            entries: FieldLimits.truncateEntries(Array(slice)),
            tokens: 0,
            tokensToday: 0
        )
    }

    private func musicHeartbeat() -> HeartbeatSnapshot {
        var entries: [String] = []
        if !nowPlaying.artist.isEmpty {
            entries.append(FieldLimits.truncate(nowPlaying.artist, max: FieldLimits.entryMax) ?? "")
        }
        if let album = nowPlaying.album {
            entries.append(FieldLimits.truncate(album, max: FieldLimits.entryMax) ?? "")
        }
        return HeartbeatSnapshot(
            page: MacBoardPage.music.rawValue,
            total: nowPlaying.durationSeconds,
            running: nowPlaying.isPlaying ? 1 : 0,
            waiting: nowPlaying.positionMilliseconds,
            msg: FieldLimits.truncate(nowPlaying.title, max: FieldLimits.messageMax),
            entries: FieldLimits.truncateEntries(entries),
            tokens: 0,
            tokensToday: 0
        )
    }

    private func buildHeartbeat() -> HeartbeatSnapshot {
        let lines = FieldLimits.truncateEntries(buildErrorLines.map {
            FieldLimits.truncate($0, max: FieldLimits.entryMax) ?? $0
        })
        return HeartbeatSnapshot(
            page: MacBoardPage.build.rawValue,
            total: lines.count,
            running: buildStatus.isBuilding ? 1 : 0,
            waiting: 0,
            msg: buildStatus.rawValue,
            entries: lines.isEmpty ? nil : lines,
            tokens: 0,
            tokensToday: 0
        )
    }

    private func refreshMusicAndPushIfNeeded(force: Bool) async {
        do {
            let snap = try await music.refresh()
            nowPlaying = snap
            let changed = force || snap != lastMusicSnapshot
            lastMusicSnapshot = snap
            let pushProgress = activePage == .music && !snap.stopped
            if phase == .ready, activePage == .music, !playlistPickerActive, (changed || pushProgress) {
                pushCurrentHeartbeat()
            }
        } catch {
            if force {
                appendLog("Music: \(error.localizedDescription)")
            }
        }
    }

    private func refreshBuildSnapshot() async {
        let status = await buildService.status()
        buildStatus = status
        if status == .failure {
            buildErrorLines = await buildService.errorLines()
        } else if status == .success {
            buildErrorLines = []
        }
        if phase == .ready, activePage == .build {
            pushCurrentHeartbeat()
        }
    }

    private func pollBuildStatus() async {
        let status = await buildService.status()
        buildStatus = status
        if status == .failure {
            buildErrorLines = await buildService.errorLines()
        }
        if phase == .ready, activePage == .build {
            pushCurrentHeartbeat()
        }
        if status.isTerminal {
            stopBuildPolling()
        }
    }

    private func handleMediaCommand(action: String) async {
        var ok = true
        var errorText: String?
        do {
            switch action {
            case "toggle": try await music.toggle()
            case "play": try await music.play()
            case "pause": try await music.pause()
            case "next": try await music.next()
            case "previous": try await music.previous()
            case "playlist_open":
                try await handlePlaylistOpen()
                return
            case "playlist_close":
                handlePlaylistClose()
                sendAck(.media, ok: true, error: nil)
                return
            default:
                if action.hasPrefix("window:") {
                    let raw = String(action.dropFirst("window:".count))
                    if let start = Int(raw) {
                        playlistWindowStart = max(0, start)
                        sendAck(.media, ok: true, error: nil)
                        if phase == .ready { pushCurrentHeartbeat() }
                        return
                    }
                }
                if action.hasPrefix("pick:") {
                    let raw = String(action.dropFirst("pick:".count))
                    if let index = Int(raw) {
                        try await handlePlaylistPick(index: index)
                        return
                    }
                }
                ok = false
                errorText = "unknown-action"
            }
        } catch {
            ok = false
            errorText = error.localizedDescription
            appendLog("media \(action) 失败: \(error.localizedDescription)")
        }
        sendAck(.media, ok: ok, error: errorText)
        await refreshMusicAndPushIfNeeded(force: true)
    }

    private func handlePlaylistOpen() async throws {
        guard let snapshot = try await music.loadCurrentPlaylistTracks() else {
            sendAck(.media, ok: false, error: "not-playing")
            appendLog("playlist: 无当前列表")
            return
        }
        playlistPickerActive = true
        playlistName = snapshot.name
        playlistTracks = snapshot.tracks
        let idx = snapshot.currentIndex
        playlistWindowStart = max(0, min(idx, max(0, snapshot.tracks.count - playlistPageSize)))
        appendLog("playlist: \(snapshot.name) (\(snapshot.tracks.count) 首)")
        sendAck(.media, ok: true, error: nil)
        if phase == .ready {
            var hb = playlistHeartbeat()
            hb.running = idx
            pushPlaylistHeartbeat(hb)
        }
    }

    private func handlePlaylistClose() {
        playlistPickerActive = false
        playlistTracks = []
        playlistName = ""
        playlistWindowStart = 0
        if phase == .ready, activePage == .music {
            pushCurrentHeartbeat()
        }
    }

    private func pushPlaylistHeartbeat(_ snapshot: HeartbeatSnapshot) {
        central.sendHeartbeat(snapshot) { [weak self] error in
            Task { @MainActor in
                if let error {
                    self?.appendLog("playlist heartbeat 失败: \(error.localizedDescription)")
                }
            }
        }
    }

    private func handlePlaylistPick(index: Int) async throws {
        guard playlistPickerActive, index >= 0, index < playlistTracks.count else {
            sendAck(.media, ok: false, error: "bad-index")
            return
        }
        try await music.playPlaylistTrack(at: index)
        playlistPickerActive = false
        playlistTracks = []
        sendAck(.media, ok: true, error: nil)
        await refreshMusicAndPushIfNeeded(force: true)
    }

    private func handleBuildStart(fromBoard: Bool) async {
        let result = await buildService.triggerBuild()
        appendLog("birkinBuild → \(result)")
        let lowered = result.lowercased()
        let ok = !lowered.contains("no-xcode")
        if !ok {
            sendAck(.build, ok: false, error: "no-xcode")
            buildStatus = .noXcode
            pushCurrentHeartbeat()
            return
        }
        buildStatus = .starting
        buildErrorLines = []
        activePage = .build
        sendAck(.build, ok: true, error: nil)
        pushCurrentHeartbeat()
        startBuildPolling()
        await pollBuildStatus()
    }

    private func sendAck(_ ack: BoardWireAck, ok: Bool, error: String?) {
        guard let data = try? RawWireValidator.ackJSON(ack, ok: ok, error: error) else { return }
        sendRaw(data, note: "ack:\(ack.rawValue)")
    }

    private func sendCommand(_ command: WireCommand, name: String? = nil) {
        do {
            let data = try RawWireValidator.commandJSON(command, name: name)
            sendRaw(data, note: command.rawValue)
        } catch {
            appendLog("\(command.rawValue) 编码失败")
        }
    }

    private func sendRaw(_ data: Data, note: String) {
        central.send(.rawJSON(data)) { [weak self] error in
            Task { @MainActor in
                if let error {
                    self?.appendLog("发送 \(note) 失败: \(error.localizedDescription)")
                }
            }
        }
    }

    private func appendLog(_ text: String) {
        logs.insert(BridgeLogLine(text: text), at: 0)
        if logs.count > 200 {
            logs = Array(logs.prefix(200))
        }
    }

    private func handleInbound(_ message: NUSInboundMessage) {
        if !linkSecure {
            markLinkSecure(source: "inbound")
        }
        switch message {
        case .ack(let ack):
            appendLog("← ack \(ack.ack) ok=\(ack.ok)")
        case .command(let name, let payload):
            appendLog("← cmd \(name)")
            switch name {
            case "media":
                let action = jsonString(payload["action"]) ?? ""
                Task { await handleMediaCommand(action: action) }
            case "build":
                let action = jsonString(payload["action"]) ?? ""
                if action == "start" {
                    Task { await handleBuildStart(fromBoard: true) }
                }
            default:
                break
            }
        case .unknown(let raw):
            appendLog("← \(raw.prefix(120))")
        case .heartbeat:
            break
        }
    }

    private func jsonString(_ value: JSONValue?) -> String? {
        guard case .string(let s) = value else { return nil }
        return s
    }
}

extension MacBoardBridge: NUSCentralDelegate {
    nonisolated public func nusCentral(_ central: NUSCentralManager, didUpdateState state: CBManagerState) {
        Task { @MainActor in
            switch state {
            case .poweredOn:
                if phase == .idle || phase == .bluetoothOff {
                    attemptAutoReconnectIfNeeded(reason: "bt-on")
                }
            case .poweredOff:
                phase = .bluetoothOff
                stopTimers()
                appendLog("蓝牙已关闭")
            case .unauthorized:
                phase = .bluetoothOff
                appendLog("蓝牙未授权")
            default:
                break
            }
        }
    }

    nonisolated public func nusCentral(_ central: NUSCentralManager, didDiscover peripheral: CBPeripheral, rssi: NSNumber) {
        Task { @MainActor in
            peripheralsByID[peripheral.identifier] = peripheral
            let name = peripheral.name ?? "Claude"
            let board = DiscoveredBoard(id: peripheral.identifier, name: name, rssi: rssi.intValue)
            if let idx = discovered.firstIndex(where: { $0.id == board.id }) {
                discovered[idx] = board
            } else {
                discovered.append(board)
                appendLog("发现 \(name) rssi=\(rssi)")
            }
            maybeAutoConnectDiscovered(id: peripheral.identifier, name: peripheral.name)
        }
    }

    nonisolated public func nusCentral(_ central: NUSCentralManager, didConnect peripheral: CBPeripheral) {
        Task { @MainActor in
            peripheralsByID[peripheral.identifier] = peripheral
            rememberDevice(uuid: peripheral.identifier, name: peripheral.name)
            phase = .connected
            connectedName = peripheral.name
            appendLog("已连接 \(peripheral.name ?? peripheral.identifier.uuidString)")
        }
    }

    nonisolated public func nusCentralDidBecomeReady(_ central: NUSCentralManager) {
        Task { @MainActor in onLinkReady() }
    }

    nonisolated public func nusCentral(
        _ central: NUSCentralManager,
        didDisconnect peripheral: CBPeripheral,
        error: Error?
    ) {
        Task { @MainActor in
            stopTimers()
            linkSecure = false
            didLogSecureWait = false
            phase = .idle
            connectedName = nil
            appendLog("断开: \(error?.localizedDescription ?? "ok")")
        }
    }

    nonisolated public func nusCentral(
        _ central: NUSCentralManager,
        didReceive message: NUSInboundMessage,
        from peripheral: CBPeripheral
    ) {
        Task { @MainActor in handleInbound(message) }
    }

    nonisolated public func nusCentral(_ central: NUSCentralManager, didFailWith error: Error) {
        Task { @MainActor in
            appendLog("BLE 错误: \(error.localizedDescription)")
        }
    }
}
