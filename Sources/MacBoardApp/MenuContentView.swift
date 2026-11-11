import MacBoardCore
import SwiftUI

struct MenuContentView: View {
    @EnvironmentObject private var bridge: MacBoardBridge

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            header
            Divider()
            connectionSection
            Divider()
            pageSection
            if bridge.activePage == .music {
                musicSection
            } else {
                buildSection
            }
            Divider()
            logSection
        }
        .padding(12)
        .onAppear { bridge.bootstrap() }
    }

    private var header: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text("AI Passport Mac 伴侣")
                    .font(.headline)
                Text(statusCaption)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            phaseBadge
        }
    }

    private var statusCaption: String {
        if let name = bridge.connectedName, bridge.phase == .ready {
            return "\(name) · BLE+"
        }
        if let name = bridge.connectedName {
            return name
        }
        return bridge.phase.rawValue
    }

    private var phaseBadge: some View {
        Text(bridge.linkSecure ? "就绪" : bridge.phase.rawValue)
            .font(.caption.bold())
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(bridge.linkSecure ? Color.green.opacity(0.2) : Color.orange.opacity(0.2))
            .clipShape(Capsule())
    }

    private var connectionSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("蓝牙")
                .font(.subheadline.bold())
            HStack {
                Button("扫描") { bridge.startScanning() }
                Button("断开") { bridge.disconnect() }
                    .disabled(bridge.phase == .idle)
                Button("状态探测") { bridge.sendStatus() }
                    .disabled(bridge.phase != .ready)
            }
            if !bridge.discovered.isEmpty {
                Text("附近设备")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                ForEach(bridge.discovered.prefix(5)) { board in
                    Button {
                        bridge.connect(id: board.id)
                    } label: {
                        HStack {
                            Text(board.name)
                            Spacer()
                            Text("\(board.rssi) dBm")
                                .foregroundStyle(.secondary)
                        }
                    }
                    .buttonStyle(.plain)
                }
            }
        }
    }

    private var pageSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("板子页面")
                .font(.subheadline.bold())
            // 页面只由板子决定（长按 DOWN 切换），Mac 只跟随、推送该页数据
            Text("当前：\(bridge.activePage.displayName)（在板子上长按 DOWN 切换）")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    private var musicSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Apple Music")
                .font(.subheadline.bold())
            Text(bridge.nowPlaying.title)
                .lineLimit(2)
            if !bridge.nowPlaying.artist.isEmpty {
                Text(bridge.nowPlaying.artist)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            HStack {
                Image(systemName: bridge.nowPlaying.isPlaying ? "play.fill" : "pause.fill")
                Text(bridge.nowPlaying.isPlaying ? "播放中" : "未播放 / 已暂停")
                    .font(.caption)
            }
            Button("刷新 Now Playing") { bridge.refreshMusicNow() }
        }
    }

    private var buildSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Xcode 编译")
                .font(.subheadline.bold())
            Text("状态：\(bridge.buildStatus.rawValue)")
                .font(.caption.monospaced())
            Button("触发 birkinBuild()") { bridge.triggerTestBuild() }
            HStack {
                Button("试听成功音效") { bridge.previewBuildSound(success: true) }
                Button("试听失败音效") { bridge.previewBuildSound(success: false) }
            }
            .disabled(bridge.phase != .ready)
            if !bridge.buildProcessLines.isEmpty {
                Text("编译过程")
                    .font(.caption.bold())
                ForEach(Array(bridge.buildProcessLines.suffix(4).enumerated()), id: \.offset) { _, line in
                    Text(line)
                        .font(.caption2.monospaced())
                        .lineLimit(1)
                }
            }
            if !bridge.buildErrorLines.isEmpty {
                Text("错误摘要")
                    .font(.caption.bold())
                ForEach(bridge.buildErrorLines.prefix(4), id: \.self) { line in
                    Text(line)
                        .font(.caption2)
                        .lineLimit(2)
                }
            }
        }
    }

    private var logSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("日志")
                    .font(.subheadline.bold())
                Spacer()
                Button("清空") { bridge.clearLogs() }
                    .font(.caption)
            }
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 4) {
                    ForEach(bridge.logs.prefix(30)) { line in
                        Text(line.text)
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                            .textSelection(.enabled)
                    }
                }
            }
            .frame(height: 100)
        }
    }
}

struct SettingsView: View {
    @EnvironmentObject private var bridge: MacBoardBridge

    var body: some View {
        Form {
            Section("设备显示名") {
                TextField("发到板子的昵称（≤32）", text: $bridge.passportDisplayName)
            }
            Section("维护") {
                Button("发送 unpair") { bridge.sendUnpair() }
                Button("清空日志") { bridge.clearLogs() }
            }
            Section("说明") {
                Text("音乐控制需在「隐私与安全性 → 自动化」中允许 MacBoard 控制「音乐」。编译依赖本机 Hammerspoon 的 birkinBuild / birkinStatus。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .padding()
    }
}
