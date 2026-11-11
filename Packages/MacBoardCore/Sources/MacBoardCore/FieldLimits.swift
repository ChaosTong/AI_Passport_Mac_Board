import Foundation

public enum FieldLimits {
    public static let messageMax = 160
    public static let entryMax = 96
    public static let entryCount = 8
    public static let nameMax = 32

    public static func truncate(_ text: String?, max: Int) -> String? {
        guard let text else { return nil }
        guard text.count > max else { return text }
        return String(text.prefix(max))
    }

    public static func truncateEntries(_ entries: [String]) -> [String] {
        Array(entries.prefix(entryCount)).map { truncate($0, max: entryMax) ?? "" }
    }
}
