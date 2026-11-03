import Foundation

/// Apps already uploaded to BrowserStack, so a build is uploaded once and then
/// used again: after switching tabs, after relaunching DevX, or when CI
/// uploaded it.
///
/// App Automate and App Live keep separate uploads of the same file, each with
/// its own bs:// URL. DevX uploads to both at once, so an App Live upload is
/// paired with the App Automate upload of the same file name and version made
/// within a few minutes of it. File name and version alone cannot pair them:
/// every React Native release build is "app-release.apk 1.0".
enum BrowserStackApps {
    struct Upload: Equatable {
        var name: String
        var version: String
        var url: String
        var customID: String
        var uploadedAt: String
    }

    /// One uploaded build, with its URL in each product ("" where it was not
    /// uploaded).
    struct Entry: Equatable {
        var name: String
        var version: String
        var customID: String
        var uploadedAt: String
        var automate: String
        var live: String

        var label: String { version.isEmpty ? name : name + " " + version }
    }

    /// BrowserStack keeps an upload for 30 days.
    static let retention: TimeInterval = 30 * 24 * 3600
    /// How far apart the two uploads of one file can be and still pair.
    static let pairingWindow: TimeInterval = 15 * 60

    /// A `recent_apps` reply: an array of uploads, or {"message": "No results
    /// found"} when there are none. Anything without a bs:// URL is dropped.
    static func uploads(_ body: JSON) -> [Upload] {
        body.array.compactMap { a in
            let url = a["app_url"].text
            guard isAppURL(url) else { return nil }
            return Upload(name: a["app_name"].text, version: a["app_version"].text, url: url,
                          customID: a["custom_id"].text, uploadedAt: a["uploaded_at"].text)
        }
    }

    /// Every App Automate upload is its own entry; each App Live upload joins
    /// the closest unpaired App Automate upload of the same file, or stands
    /// alone. Newest first.
    static func entries(automate: [Upload], live: [Upload]) -> [Entry] {
        var out = automate.map {
            Entry(name: $0.name, version: $0.version, customID: $0.customID,
                  uploadedAt: $0.uploadedAt, automate: $0.url, live: "")
        }
        for l in live {
            let at = uploadedDate(l.uploadedAt)
            let match = out.indices
                .filter { out[$0].live.isEmpty && !out[$0].automate.isEmpty
                    && out[$0].name == l.name && out[$0].version == l.version }
                .compactMap { i -> (Int, TimeInterval)? in
                    guard let at, let other = uploadedDate(out[i].uploadedAt) else { return nil }
                    let gap = abs(other.timeIntervalSince(at))
                    return gap <= pairingWindow ? (i, gap) : nil
                }
                .min { $0.1 < $1.1 }
            if let (i, _) = match {
                out[i].live = l.url
            } else {
                out.append(Entry(name: l.name, version: l.version, customID: l.customID,
                                 uploadedAt: l.uploadedAt, automate: "", live: l.url))
            }
        }
        return out.sorted { (uploadedDate($0.uploadedAt) ?? .distantPast) > (uploadedDate($1.uploadedAt) ?? .distantPast) }
    }

    /// "bs://" and BrowserStack's app id.
    static func isAppURL(_ text: String) -> Bool {
        guard text.hasPrefix("bs://") else { return false }
        let id = text.dropFirst(5)
        return !id.isEmpty && id.count <= 128 && id.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber) }
    }

    /// What BrowserStack accepts as a custom_id: letters, digits, ".", "-",
    /// "_", at most 100. BrowserStack drops other characters itself; doing it
    /// here shows the reader the id that will be stored.
    static func customID(_ text: String) -> String {
        String(text.filter { $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "." || $0 == "-" || $0 == "_") }
            .prefix(100))
    }

    /// BrowserStack's "2026-10-07 16:36:49 UTC".
    static func uploadedDate(_ text: String) -> Date? {
        stampFormatter.date(from: text)
    }

    /// A time in that form, for an upload DevX just made.
    static func uploadedText(_ date: Date) -> String {
        stampFormatter.string(from: date)
    }

    private static var stampFormatter: DateFormatter {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.timeZone = TimeZone(identifier: "UTC")
        f.dateFormat = "yyyy-MM-dd HH:mm:ss 'UTC'"
        return f
    }

    /// Days before BrowserStack deletes the upload, a part day counting as
    /// one; 0 or less means it is gone. nil when the upload time is not known
    /// (a pasted URL).
    static func daysLeft(uploadedAt: String, now: Date) -> Int? {
        guard let at = uploadedDate(uploadedAt) else { return nil }
        return Int((at.addingTimeInterval(retention).timeIntervalSince(now) / 86400).rounded(.up))
    }
}
