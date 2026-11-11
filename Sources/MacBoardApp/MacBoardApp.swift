import MacBoardCore
import SwiftUI

@main
struct MacBoardApp: App {
    @StateObject private var bridge = MacBoardBridge()

    var body: some Scene {
        WindowGroup("MacBoard") {
            MenuContentView()
                .environmentObject(bridge)
                .frame(minWidth: 380, minHeight: 480)
        }
        .defaultSize(width: 400, height: 520)

        MenuBarExtra {
            MenuContentView()
                .environmentObject(bridge)
                .frame(width: 360)
        } label: {
            Label(menuBarTitle, systemImage: menuBarIcon)
        }
        .menuBarExtraStyle(.window)

        Settings {
            SettingsView()
                .environmentObject(bridge)
                .frame(minWidth: 420, minHeight: 360)
        }
    }

    private var menuBarTitle: String {
        switch bridge.phase {
        case .ready:
            return "MacBoard"
        case .scanning, .connecting, .connected:
            return "MacBoard…"
        case .bluetoothOff:
            return "MacBoard!"
        case .idle:
            return "MacBoard"
        }
    }

    private var menuBarIcon: String {
        bridge.linkSecure ? "antenna.radiowaves.left.and.right" : "antenna.radiowaves.left.and.right.slash"
    }
}
