import Combine
import CoreBluetooth
import Darwin
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
    /// 编译过程时间线（"mm:ss 文案"，按时间顺序，最多 `FieldLimits.entryCount` 条），推给板子上半部。
    @Published public private(set) var buildProcessLines: [String] = []
    @Published public private(set) var buildStartedAt: Date?
    @Published public private(set) var buildFinishedAt: Date?
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
    /// 编译中的兜底：30s 没读到新进度就一次性核对 birkinStatus
    private var buildWatchdogTimer: Timer?
    private var progressFileSource: DispatchSourceFileSystemObject?
    private var progressPollTimer: Timer?
    private var lastProgressFileError: String?
    private var lastBuildProgress: String?
    private var lastBuildProgressShape: String?
    private var lastBuildProgressAt: Date?
    /// 已处理的 Birkin 推送 seq（去重 / 连接时补齐用）
    private var birkinSeq = 0
    private var didLogSecureWait = false
    private var autoReconnectEnabled = true
    /// 断开后的自动重连：指数退避（2s → 4s → … 最多 30s）反复尝试，链路就绪后清零；用户在菜单里点「断开」则停止。
    private var reconnectTimer: Timer?
    private var reconnectAttempts = 0
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
        startProgressFileWatcher()
    }

    public func bootstrap() {
        attemptAutoReconnectIfNeeded(reason: "launch")
        scheduleReconnect(reason: "launch")
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
        cancelReconnect()
        central.stopScanning()
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

    /// 板子上报当前页（{"cmd":"page"}）后跟随：板子是页面的唯一来源，Mac 从不主动切页。
    private func followBoardPage(_ page: MacBoardPage) {
        activePage = page
        pushCurrentHeartbeat()
    }

    public func refreshMusicNow() {
        Task {
            await refreshMusicAndPushIfNeeded(force: true)
        }
    }

    /// 让板子试听编译音效（下行 {"cmd":"sound","name":"build_success"|"build_failure"}，不受板子 SOUND 开关影响）。
    public func previewBuildSound(success: Bool) {
        guard phase == .ready else {
            appendLog("试听音效：板子未连接")
            return
        }
        let name = success ? "build_success" : "build_failure"
        guard let data = try? JSONSerialization.data(
            withJSONObject: ["cmd": "sound", "name": name], options: [.sortedKeys]
        ) else { return }
        sendRaw(data, note: "sound \(name)")
        appendLog("试听 \(name)")
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

    /// 安排下一次自动重连（退避：2, 4, 8, 16, 30, 30… 秒）。
    private func scheduleReconnect(reason: String) {
        guard autoReconnectEnabled, lastKnownDevice != nil else { return }
        reconnectTimer?.invalidate()
        let delay = min(30.0, 2.0 * pow(2.0, Double(min(reconnectAttempts, 4))))
        reconnectAttempts += 1
        appendLog("\(Int(delay))s 后自动重连（\(reason)，第 \(reconnectAttempts) 次）")
        reconnectTimer = Timer.scheduledTimer(withTimeInterval: delay, repeats: false) { [weak self] _ in
            Task { @MainActor in self?.reconnectTimerFired(reason: reason) }
        }
    }

    private func reconnectTimerFired(reason: String) {
        reconnectTimer = nil
        guard autoReconnectEnabled else { return }
        switch phase {
        case .ready, .bluetoothOff:
            return
        case .connected:
            // 正在协商加密：再等一轮，不打断
            scheduleReconnect(reason: reason)
            return
        case .scanning, .connecting:
            // 上一轮没连上：重新发起（停扫 → 按 UUID 直连 + 扫描兜底）
            central.stopScanning()
            phase = .idle
        case .idle:
            break
        }
        attemptAutoReconnectIfNeeded(reason: reason)
        // 本轮若仍未就绪，下一轮继续（退避递增）
        scheduleReconnect(reason: reason)
    }

    private func cancelReconnect() {
        reconnectTimer?.invalidate()
        reconnectTimer = nil
        reconnectAttempts = 0
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
        cancelReconnect()
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

    // MARK: - Birkin 进度文件

    private func startProgressFileWatcher() {
        HammerspoonBuildService.ensureProgressFileExists()
        let dir = birkinProgressFileURL.deletingLastPathComponent()
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let fd = open(dir.path, O_EVTONLY)
        guard fd >= 0 else { return }
        let src = DispatchSource.makeFileSystemObjectSource(
            fileDescriptor: fd, eventMask: [.write, .extend, .attrib, .rename, .link, .delete],
            queue: .main
        )
        src.setEventHandler { [weak self] in
            self?.ingestProgressFile(live: true)
        }
        src.setCancelHandler { close(fd) }
        src.resume()
        progressFileSource = src
        appendLog("监听进度文件 \(birkinProgressFileURL.path)")
        startProgressPolling()
        ingestProgressFile(live: false)
    }

    /// 读 Birkin 写下的进度快照，把新行推进时间线，并写进菜单日志。
    private func ingestProgressFile(live: Bool) {
        let result = HammerspoonBuildService.readProgressFile(since: birkinSeq)
        if let error = result.error, live {
            // 避免每 0.5s 刷同一句
            if lastProgressFileError != error {
                lastProgressFileError = error
                appendLog("进度文件读失败: \(error)")
            }
            return
        }
        lastProgressFileError = nil
        guard let catchUp = result.value else { return }
        for line in catchUp.lines {
            handleBirkinLine(line, live: live)
        }
        // 只有没有新行时才处理 hidden，避免上一轮结束后的 hidden 把刚开始的编译取消掉
        if catchUp.hidden, catchUp.lines.isEmpty {
            handleBirkinLine(BirkinProgressLine(seq: 0, state: "hidden", text: ""), live: live)
        }
    }

    private func startProgressPolling() {
        guard progressPollTimer == nil else { return }
        // common modes：菜单栏跟踪 / 滚动时 default mode 定时器可能停，过程行就会被跳过。
        let timer = Timer(timeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.ingestProgressFile(live: true) }
        }
        RunLoop.main.add(timer, forMode: .common)
        progressPollTimer = timer
    }

    private func stopProgressPolling() {
        progressPollTimer?.invalidate()
        progressPollTimer = nil
    }

    private var isTrackingBuild: Bool {
        buildStartedAt != nil && buildFinishedAt == nil
    }

    /// 板子按了 STOP：忽略随后迟到的 busy 文案，等到 hidden/idle 或 watchdog 再收尾。
    private var stopRequested = false

    /// 处理一条 Birkin 文案：按 state 推进编译状态，文案追加到时间线，并推给板子。
    private func handleBirkinLine(_ line: BirkinProgressLine, live: Bool) {
        if line.seq > 0 {
            // 实时推送按到达顺序处理（Hammerspoon 重载后 seq 会从 1 重新开始）；只有补齐时去重
            if !live && line.seq <= birkinSeq { return }
            if line.state != "hidden" { birkinSeq = line.seq }
        }
        switch line.state {
        case "busy":
            if stopRequested, isTrackingBuild {
                break
            }
            if stopRequested {
                // 已经开了新一轮（例如 STOP 后立刻 PLAY），不要丢掉过程行
                stopRequested = false
            }
            if !isTrackingBuild {
                // 外部触发（Xcode ⌘B / ⌥R）：开新时间线
                buildErrorLines = []
                beginBuildTimeline(note: nil)
            }
            buildStatus = .busy
            appendBirkinText(line.text, throttle: true)
            armBuildWatchdog()
        case "success":
            appendBirkinText(line.text, throttle: false)
            finishBuild(.success, note: line.text.isEmpty)
        case "failure":
            appendBirkinText(line.text, throttle: false)
            if buildStatus != .failure || isTrackingBuild {
                finishBuild(.failure, note: line.text.isEmpty)
            }
        case "idle":
            appendBirkinText(line.text, throttle: false)
            if buildStatus.isBuilding || stopRequested {
                finishBuild(.idle, note: line.text.isEmpty)
            }
            stopRequested = false
        case "hidden":
            // 菜单栏被收起：编译中（非刚按下 PLAY 的 starting）即视为取消。
            // 若板子刚发了 STOP，starting 阶段的 hidden 也算取消。
            if stopRequested || buildStatus == .busy {
                if isTrackingBuild || buildStatus.isBuilding {
                    appendBuildProcess("canceled")
                    finishBuild(.idle, note: false)
                }
                stopRequested = false
            }
        default:
            appendBirkinText(line.text, throttle: false)
        }
        // 编译进度始终以 page=build 下发：板子只在编译页显示，不会被切页。
        // 结束态带上 build_result，任意页都能播提示音（不等 Mac 先收到切页上报）。
        if live, phase == .ready {
            pushBuildHeartbeat()
        }
    }

    /// 追加 Birkin 文案。busy 进度（如 "Planning 7533/7610"）每 0.35s 变一次：
    /// 去掉数字后形状相同且 3s 内的不重复追加，避免刷屏。
    private func appendBirkinText(_ text: String, throttle: Bool) {
        guard !text.isEmpty, text != lastBuildProgress else { return }
        let shape = text.filter { !$0.isNumber }
        let now = Date()
        if throttle, shape == lastBuildProgressShape,
           let at = lastBuildProgressAt, now.timeIntervalSince(at) < 3 {
            return
        }
        lastBuildProgress = text
        lastBuildProgressShape = shape
        lastBuildProgressAt = now
        appendBuildProcess(text)
        appendLog("编译 \(text)")
        if phase == .ready, activePage != .build {
            appendLog("编译进度已下发；Mac 尚未收到切页上报，仍显示\(activePage.displayName)页并会推歌名")
        }
    }

    private func finishBuild(_ status: BirkinBuildStatus, note: Bool) {
        buildStatus = status
        stopRequested = false
        stopBuildWatchdog()
        if isTrackingBuild {
            buildFinishedAt = Date()
            if note { appendBuildProcess(status.rawValue) }
        }
        if status == .failure {
            // 失败后一次性取错误摘要
            Task { @MainActor in
                buildErrorLines = await buildService.errorLines()
                if phase == .ready { pushBuildHeartbeat() }
            }
        } else if status == .success {
            buildErrorLines = []
        }
    }

    private func armBuildWatchdog() {
        stopBuildWatchdog()
        buildWatchdogTimer = Timer.scheduledTimer(withTimeInterval: 30, repeats: false) { [weak self] _ in
            Task { @MainActor in await self?.buildWatchdogFired() }
        }
    }

    private func stopBuildWatchdog() {
        buildWatchdogTimer?.invalidate()
        buildWatchdogTimer = nil
    }

    /// 30s 没有推送（Birkin 未加载 / Hammerspoon 重载 / 推送丢失）：一次性核对状态。
    private func buildWatchdogFired() async {
        buildWatchdogTimer = nil
        guard buildStatus.isBuilding else { return }
        ingestProgressFile(live: true)
        if !buildStatus.isBuilding { return }
        let status = await buildService.status()
        if status.isBuilding {
            armBuildWatchdog()
            return
        }
        finishBuild(status, note: true)
        if phase == .ready { pushBuildHeartbeat() }
    }

    private func stopTimers() {
        stopKeepalive()
        stopSecureProbe()
        stopMusicPolling()
    }

    private func pushCurrentHeartbeat() {
        guard phase == .ready else { return }
        sendHeartbeatSnapshot(currentHeartbeatSnapshot())
    }

    /// 只推编译页数据。板子不在编译页会忽略并上报当前页，Mac 不会因此改板子的页。
    private func pushBuildHeartbeat() {
        guard phase == .ready else { return }
        var snapshot = buildHeartbeat()
        if let finished = buildFinishedAt, buildStatus == .success || buildStatus == .failure {
            snapshot.buildResult = buildStatus.rawValue
            snapshot.buildId = Int(finished.timeIntervalSince1970 * 1000)
        }
        sendHeartbeatSnapshot(snapshot)
    }

    private func sendHeartbeatSnapshot(_ snapshot: HeartbeatSnapshot) {
        central.sendHeartbeat(snapshot) { [weak self] error in
            Task { @MainActor in
                if let error {
                    self?.appendLog("heartbeat 失败: \(error.localizedDescription)")
                }
            }
        }
    }

    private func currentHeartbeatSnapshot() -> HeartbeatSnapshot {
        var snapshot: HeartbeatSnapshot
        if playlistPickerActive, activePage == .music {
            snapshot = playlistHeartbeat()
        } else {
            switch activePage {
            case .music:
                snapshot = musicHeartbeat()
            case .build:
                snapshot = buildHeartbeat()
            case .volume:
                snapshot = HeartbeatSnapshot(page: MacBoardPage.volume.rawValue, tokens: 0, tokensToday: 0)
            }
        }
        // 编译结果随每个页面的 heartbeat 下发：板子在音乐页也能播放成功 / 失败音效
        if let finished = buildFinishedAt, buildStatus == .success || buildStatus == .failure {
            snapshot.buildResult = buildStatus.rawValue
            snapshot.buildId = Int(finished.timeIntervalSince1970 * 1000)
        }
        return snapshot
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

    /// page=build：msg 状态；running 编译中；waiting = 本次编译耗时 ms（结束后冻结）；
    /// entries = 失败时错误行，否则过程时间线（最新在末尾）。
    private func buildHeartbeat() -> HeartbeatSnapshot {
        let source = (buildStatus == .failure && !buildErrorLines.isEmpty)
            ? buildErrorLines
            : Array(buildProcessLines.suffix(FieldLimits.entryCount))
        let lines = FieldLimits.truncateEntries(source.map {
            FieldLimits.truncate($0, max: FieldLimits.entryMax) ?? $0
        })
        return HeartbeatSnapshot(
            page: MacBoardPage.build.rawValue,
            total: lines.count,
            running: buildStatus.isBuilding ? 1 : 0,
            waiting: buildElapsedMilliseconds(),
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

    /// 连接就绪时同步编译状态。以进度文件为准，不要用可能过期的 birkinStatus()
    /// 把 birkinSeq 往前跳、丢掉还没入库的过程行，也不要覆盖正在跟踪的时间线。
    private func refreshBuildSnapshot() async {
        ingestProgressFile(live: false)
        if !isTrackingBuild {
            let status = await buildService.status()
            if status == .failure {
                buildErrorLines = await buildService.errorLines()
            } else if status == .success {
                buildErrorLines = []
            }
            if status.isBuilding {
                beginBuildTimeline(note: status.rawValue)
                ingestProgressFile(live: false)
                armBuildWatchdog()
            }
        }
        if phase == .ready {
            if activePage == .build {
                pushCurrentHeartbeat()
            } else if buildStatus.isBuilding || isTrackingBuild || !(buildProcessLines.isEmpty) {
                pushBuildHeartbeat()
            }
        }
    }

    private func beginBuildTimeline(note: String?) {
        stopRequested = false
        buildStartedAt = Date()
        buildFinishedAt = nil
        buildProcessLines = []
        lastBuildProgress = nil
        lastBuildProgressShape = nil
        lastBuildProgressAt = nil
        startProgressPolling()
        if let note {
            appendBuildProcess(note)
        }
    }

    private func appendBuildProcess(_ text: String) {
        let seconds = Int(buildElapsedMilliseconds() / 1000)
        let line = String(format: "%02d:%02d %@", (seconds / 60) % 100, seconds % 60, text)
        buildProcessLines.append(FieldLimits.truncate(line, max: FieldLimits.entryMax) ?? line)
        if buildProcessLines.count > FieldLimits.entryCount {
            buildProcessLines.removeFirst(buildProcessLines.count - FieldLimits.entryCount)
        }
    }

    private func buildElapsedMilliseconds() -> Int {
        guard let start = buildStartedAt else { return 0 }
        let end = buildFinishedAt ?? Date()
        return max(0, Int(end.timeIntervalSince(start) * 1000))
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
        if buildStatus.isBuilding, isTrackingBuild {
            // 正在编译：不重复 ⌘B（板子侧也会在编译中屏蔽 PLAY）
            appendLog("birkinBuild 跳过：编译进行中")
            sendAck(.build, ok: false, error: "busy")
            pushBuildHeartbeat()
            return
        }
        // 先开时间线再等 birkinBuild()：hs 调用期间文件里可能已经写入过程行，
        // 结束后再 begin 会清空这些行，且 birkinSeq 已前进，过程就只剩开始和成功。
        beginBuildTimeline(note: fromBoard ? "PLAY: birkinBuild()" : "birkinBuild()")
        buildStatus = .starting
        buildErrorLines = []
        armBuildWatchdog()
        let result = await buildService.triggerBuild()
        appendLog("birkinBuild → \(result)")
        let lowered = result.lowercased()
        let ok = !lowered.contains("no-xcode")
        if !ok {
            sendAck(.build, ok: false, error: "no-xcode")
            buildStatus = .noXcode
            buildStartedAt = nil
            buildFinishedAt = nil
            buildProcessLines = ["00:00 Xcode not running"]
            pushBuildHeartbeat()
            return
        }
        // 不切板子页面：编译结果 bres/bid 随任何页的 heartbeat 下发，提示音照样响
        sendAck(.build, ok: true, error: nil)
        pushBuildHeartbeat()
        ingestProgressFile(live: true)
    }

    private func handleBuildStop() async {
        if !buildStatus.isBuilding {
            appendLog("birkinStop 跳过：当前没有在编译")
            sendAck(.build, ok: false, error: "idle")
            pushBuildHeartbeat()
            return
        }
        stopRequested = true
        appendBuildProcess("STOP")
        let result = await buildService.stopBuild()
        appendLog("birkinStop → \(result)")
        let lowered = result.lowercased()
        if lowered.contains("no-xcode") || lowered.contains("no-stop") {
            stopRequested = false
            sendAck(.build, ok: false, error: lowered.contains("no-stop") ? "no-stop" : "no-xcode")
            pushBuildHeartbeat()
            return
        }
        sendAck(.build, ok: true, error: nil)
        pushBuildHeartbeat()
        armBuildWatchdog()
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
                } else if action == "stop" {
                    Task { await handleBuildStop() }
                }
            case "page":
                // 板子长按 DOWN 切页 / 收到别的页的数据时上报当前页：Mac 跟随并推这一页的数据
                if let page = MacBoardPage(rawValue: jsonString(payload["action"]) ?? "") {
                    if page != activePage { playlistPickerActive = false }
                    followBoardPage(page)
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
            // 板子复位 / 重刷 / 链路掉线：自动重连（用户主动断开时 autoReconnectEnabled=false）
            if autoReconnectEnabled {
                reconnectAttempts = 0
                attemptAutoReconnectIfNeeded(reason: "disconnect")
                scheduleReconnect(reason: "disconnect")
            }
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
            if phase == .connecting, autoReconnectEnabled, reconnectTimer == nil {
                scheduleReconnect(reason: "connect-failed")
            }
        }
    }
}
