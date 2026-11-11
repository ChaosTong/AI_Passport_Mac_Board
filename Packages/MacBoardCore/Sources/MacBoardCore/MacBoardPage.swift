import Foundation

public enum MacBoardPage: String, Sendable, CaseIterable, Identifiable {
    case music
    case build

    public var id: String { rawValue }

    public var displayName: String {
        switch self {
        case .music: return "音乐"
        case .build: return "编译"
        }
    }
}
