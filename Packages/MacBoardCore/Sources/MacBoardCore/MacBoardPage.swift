import Foundation

public enum MacBoardPage: String, Sendable, CaseIterable, Identifiable {
    case music
    case build
    /// 板子本地音量页（Mac 只负责切到这一页，音量在板子上用按键调）
    case volume

    public var id: String { rawValue }

    public var displayName: String {
        switch self {
        case .music: return "音乐"
        case .build: return "编译"
        case .volume: return "音量"
        }
    }
}
