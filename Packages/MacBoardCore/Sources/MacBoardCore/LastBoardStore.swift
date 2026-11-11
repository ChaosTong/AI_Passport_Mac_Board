import Foundation

public struct LastBoardDevice: Codable, Equatable, Sendable {
    public var uuid: UUID
    public var name: String?

    public init(uuid: UUID, name: String?) {
        self.uuid = uuid
        self.name = name
    }
}

public enum LastBoardStore {
    private static let key = "macboard.lastBoardDevice"

    public static func load(defaults: UserDefaults = .standard) -> LastBoardDevice? {
        guard let data = defaults.data(forKey: key) else { return nil }
        return try? JSONDecoder().decode(LastBoardDevice.self, from: data)
    }

    public static func save(_ device: LastBoardDevice, defaults: UserDefaults = .standard) {
        guard let data = try? JSONEncoder().encode(device) else { return }
        defaults.set(data, forKey: key)
    }

    public static func clear(defaults: UserDefaults = .standard) {
        defaults.removeObject(forKey: key)
    }
}
