// The first-run tour, and the one-line description of every feature.
//
// Both languages live here as pairs, like HelpTopics and for the same reason:
// these are sentences, and a catalog keyed by a sentence orphans its
// translation on the first edited comma. The Help topics are the long form;
// these are what fits on a tour card and beside a switch in Settings.
import Foundation

/// What one tab is for, in a sentence, and the first thing to do there.
struct FeatureGuide {
    let tab: DevXTab
    let english: String
    let vietnamese: String
    let tryEnglish: String
    let tryVietnamese: String

    var text: String { Strings.active == .vi ? vietnamese : english }
    var tryIt: String { Strings.active == .vi ? tryVietnamese : tryEnglish }
}

/// One card of the tour. A card about a tab opens that tab behind it and
/// highlights it in the sidebar.
struct TourStep: Identifiable {
    enum Kind: Equatable { case welcome, feature(DevXTab), finish }
    let kind: Kind

    var id: String {
        switch kind {
        case .welcome: return "welcome"
        case .feature(let tab): return tab.rawValue
        case .finish: return "finish"
        }
    }
    var tab: DevXTab? {
        switch kind {
        case .feature(let tab): return tab
        // The last card opens Settings, where the choice it describes is made.
        case .finish: return .settings
        case .welcome: return nil
        }
    }
}

enum Onboarding {
    /// Bumped when the tour changes enough to be worth showing again.
    static let version = 1

    /// Welcome, one card per tab in sidebar order, then where to choose.
    static let steps: [TourStep] =
        [TourStep(kind: .welcome)]
        + DevXTab.allCases.map { TourStep(kind: .feature($0)) }
        + [TourStep(kind: .finish)]

    static func guide(_ tab: DevXTab) -> FeatureGuide {
        guides.first { $0.tab == tab }!
    }

    static var welcomeTitle: String {
        Strings.active == .vi ? "Chào mừng đến với DevX" : "Welcome to DevX"
    }
    static var welcomeText: String {
        Strings.active == .vi
            ? "DevX đo hiệu năng app Android và iOS ngay trên máy Mac này: chọn thiết bị và app, thu một phiên, rồi đọc những gì đã đo được. Không có gì rời khỏi máy trừ khi bạn yêu cầu. Chuyến tham quan này đi qua từng tính năng ở thanh bên; bạn có thể chọn ngôn ngữ ngay bên dưới."
            : "DevX measures Android and iOS app performance on this Mac: pick a device and an app, capture a session, then read what was measured. Nothing leaves the machine unless you ask. This tour walks through every feature in the sidebar; pick your language just below."
    }
    static var finishTitle: String {
        Strings.active == .vi ? "Chọn tính năng bạn cần" : "Choose the features you need"
    }
    static var finishText: String {
        Strings.active == .vi
            ? "Thanh bên mặc định chỉ hiện các tính năng chính. Bật hoặc tắt từng tính năng trong Cài đặt › Tính năng hiển thị; lựa chọn được lưu lại, và lần sau DevX mở lại đúng tab bạn đang dùng. Mở lại chuyến tham quan này bất cứ lúc nào từ Trợ giúp hoặc Cài đặt."
            : "The sidebar shows the main features by default. Turn each one on or off in Settings › Features; the choice is saved, and next time DevX reopens on the tab you were using. Replay this tour any time from Help or Settings."
    }

    static let guides: [FeatureGuide] = [
        FeatureGuide(
            tab: .devices,
            english: "Every Android and iOS device and simulator this Mac can see, with a reason for each one that cannot be captured from.",
            vietnamese: "Mọi thiết bị và máy ảo Android, iOS mà máy Mac này nhìn thấy, kèm lý do cho từng thiết bị chưa thu được.",
            tryEnglish: "Plug in a phone or start a simulator, then select it. A dimmed row explains what is missing: a cable, an unlock, a trust prompt.",
            tryVietnamese: "Cắm điện thoại hoặc mở một máy ảo, rồi chọn nó. Dòng bị mờ sẽ giải thích còn thiếu gì: cáp, mở khoá, hay lời nhắc tin cậy."),
        FeatureGuide(
            tab: .apps,
            english: "The apps installed on the selected device, chosen by package name or bundle id. No PID needed.",
            vietnamese: "Các app đã cài trên thiết bị đang chọn, chọn theo package name hoặc bundle id. Không cần PID.",
            tryEnglish: "Pick the app you want to profile; it is remembered for next time.",
            tryVietnamese: "Chọn app bạn muốn đo; lựa chọn được nhớ cho lần sau."),
        FeatureGuide(
            tab: .preflight,
            english: "What can and cannot be measured for this device and app, probed rather than assumed, with how to fix each gap.",
            vietnamese: "Những gì đo được và không đo được với thiết bị và app này, được kiểm tra thực tế chứ không đoán, kèm cách khắc phục từng chỗ thiếu.",
            tryEnglish: "Run it before a capture that matters, or when one came back empty.",
            tryVietnamese: "Chạy trước một lần thu quan trọng, hoặc khi một lần thu không có dữ liệu."),
        FeatureGuide(
            tab: .live,
            english: "A live capture: frames, CPU and memory as they are measured, with findings recomputed on every tick.",
            vietnamese: "Thu trực tiếp: khung hình, CPU và bộ nhớ ngay khi được đo, kèm các phát hiện được tính lại sau mỗi nhịp.",
            tryEnglish: "Press start, use the app on the device, then stop to keep the session.",
            tryVietnamese: "Bấm bắt đầu, dùng app trên thiết bị, rồi dừng để lưu phiên."),
        FeatureGuide(
            tab: .record,
            english: "A fixed-length capture, analysed when it ends and saved as a session package you can re-read later.",
            vietnamese: "Một lần thu có thời lượng cố định, được phân tích khi kết thúc và lưu thành gói phiên để đọc lại sau.",
            tryEnglish: "Choose a duration and record; the result opens in Issues.",
            tryVietnamese: "Chọn thời lượng rồi ghi; kết quả mở trong Vấn đề."),
        FeatureGuide(
            tab: .inspect,
            english: "Network calls, console output and the Redux store of a React Native debug build, with nothing installed in the app.",
            vietnamese: "Các lệnh gọi mạng, console và Redux store của một bản build debug React Native, không cần cài thêm gì vào app.",
            tryEnglish: "Start Metro and the app, then click a request to read its headers and body.",
            tryVietnamese: "Chạy Metro và app, rồi bấm vào một request để xem header và body."),
        FeatureGuide(
            tab: .layout,
            english: "How the current screen is built: view counts, nesting depth, hidden views and navigation stacks.",
            vietnamese: "Màn hình hiện tại được dựng thế nào: số view, độ sâu lồng nhau, view bị ẩn và các navigation stack.",
            tryEnglish: "Open the screen on the device, then take a snapshot.",
            tryVietnamese: "Mở màn hình đó trên thiết bị, rồi chụp một snapshot."),
        FeatureGuide(
            tab: .emulator,
            english: "Android emulators inside DevX without Android Studio, plus real devices on BrowserStack.",
            vietnamese: "Máy ảo Android ngay trong DevX mà không cần Android Studio, cùng thiết bị thật trên BrowserStack.",
            tryEnglish: "Install an image, create a device and start it; click to tap, drag to swipe, type to type.",
            tryVietnamese: "Cài một image, tạo thiết bị rồi khởi động; bấm để chạm, kéo để vuốt, gõ phím để nhập."),
        FeatureGuide(
            tab: .sessions,
            english: "Every capture stored on this Mac. Opening one loads it into Issues, Threads, Timeline and Compare.",
            vietnamese: "Mọi phiên thu đã lưu trên máy Mac này. Mở một phiên sẽ nạp nó vào Vấn đề, Luồng, Dòng thời gian và So sánh.",
            tryEnglish: "Select a session to open it.",
            tryVietnamese: "Chọn một phiên để mở."),
        FeatureGuide(
            tab: .issues,
            english: "What the detectors found in the open session, with the evidence and the threshold behind each finding.",
            vietnamese: "Những gì detector tìm thấy trong phiên đang mở, kèm bằng chứng và ngưỡng đứng sau từng phát hiện.",
            tryEnglish: "Click a finding to read its evidence; \"cause: unknown\" means the cause was not established.",
            tryVietnamese: "Bấm vào một phát hiện để đọc bằng chứng; \"cause: unknown\" nghĩa là chưa xác định được nguyên nhân."),
        FeatureGuide(
            tab: .threads,
            english: "Where the sampled work went, split between the JavaScript thread and the native ones.",
            vietnamese: "Công việc đã lấy mẫu dồn vào đâu, chia giữa luồng JavaScript và các luồng native.",
            tryEnglish: "Open a session with CPU samples and compare the thread shares.",
            tryVietnamese: "Mở một phiên có mẫu CPU và so sánh tỉ lệ giữa các luồng."),
        FeatureGuide(
            tab: .timeline,
            english: "The capture's tracks over time. A blank column means not measured, never zero.",
            vietnamese: "Các track của phiên thu theo thời gian. Cột trống nghĩa là không đo, không bao giờ là 0.",
            tryEnglish: "Look for the bins where frames or CPU spike, then read the matching finding.",
            tryVietnamese: "Tìm các bin mà khung hình hoặc CPU tăng vọt, rồi đọc phát hiện tương ứng."),
        FeatureGuide(
            tab: .compare,
            english: "Two run sets against each other, saying inconclusive when the noise could explain the difference.",
            vietnamese: "So hai nhóm lần chạy với nhau, và báo chưa kết luận được khi độ nhiễu có thể giải thích sự khác biệt.",
            tryEnglish: "Choose a baseline and a candidate, then compare.",
            tryVietnamese: "Chọn một baseline và một candidate, rồi so sánh."),
        FeatureGuide(
            tab: .intelligence,
            english: "Production and CI/CD evidence from Sentry, GitLab CI, Firebase and JSONL imports, stored locally next to your measurements.",
            vietnamese: "Bằng chứng từ production và CI/CD qua Sentry, GitLab CI, Firebase và file JSONL, lưu cục bộ bên cạnh số đo của bạn.",
            tryEnglish: "Create a workspace, add a connector in Integrations, then sync.",
            tryVietnamese: "Tạo một workspace, thêm connector ở mục Integrations, rồi sync."),
        FeatureGuide(
            tab: .detectors,
            english: "Every detector, its thresholds and where each threshold came from, and which detectors could not run.",
            vietnamese: "Toàn bộ detector, các ngưỡng và nguồn gốc của từng ngưỡng, cùng những detector không chạy được.",
            tryEnglish: "Read it before arguing about whether a finding is real.",
            tryVietnamese: "Đọc trước khi tranh luận một phát hiện có thật hay không."),
        FeatureGuide(
            tab: .settings,
            english: "Which features the sidebar shows, the interface language, exporting a report, where sessions are stored and what leaves this Mac.",
            vietnamese: "Thanh bên hiện những tính năng nào, ngôn ngữ giao diện, xuất report, nơi lưu phiên và những gì rời khỏi máy Mac này.",
            tryEnglish: "Turn on the features you use in Features; the choice is saved.",
            tryVietnamese: "Bật các tính năng bạn dùng trong mục Tính năng hiển thị; lựa chọn được lưu lại."),
        FeatureGuide(
            tab: .help,
            english: "What each screen and field means, and how to connect an AI tool such as Claude, Cursor or Codex.",
            vietnamese: "Ý nghĩa của từng màn hình và từng trường, và cách kết nối công cụ AI như Claude, Cursor hay Codex.",
            tryEnglish: "Use Connect an AI tool to let an assistant read your captures.",
            tryVietnamese: "Dùng mục Kết nối một công cụ AI để trợ lý đọc được các phiên thu của bạn."),
    ]
}
