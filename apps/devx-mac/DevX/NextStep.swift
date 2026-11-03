import Foundation

/// What a click just did, and where the reader probably goes next.
///
/// Choosing a device or an app changes one line at the foot of the sidebar
/// and nothing else, so a "use" button read as broken: "bấm use xong cứ bị
/// đơ, không biết làm gì tiếp theo". A confirmation bar says what changed and
/// offers the screens that use it, without navigating on the reader's behalf
/// -- someone choosing an app from Inspect is not asking to leave Inspect.
struct NextStep: Equatable {
    /// Already translated.
    let message: String
    let tabs: [DevXTab]
    /// What to do on this screen instead, when the next step is here.
    var hint: String? = nil
    /// Distinguishes two confirmations with the same text, so the timer of
    /// the first does not dismiss the second.
    let serial: Int
}

enum NextSteps {
    /// After choosing a device: pick the app, or check what can be measured.
    static let afterDevice: [DevXTab] = [.apps, .preflight, .emulator]

    /// After choosing an app: everything that measures or watches one.
    static let afterApp: [DevXTab] = [.preflight, .live, .record, .inspect, .layout]

    /// The suggestions minus the screen the reader is already on.
    static func offered(_ tabs: [DevXTab], from current: DevXTab) -> [DevXTab] {
        tabs.filter { $0 != current }
    }

    /// How long a confirmation stays up before it clears itself.
    static let lifetime: TimeInterval = 20
}
