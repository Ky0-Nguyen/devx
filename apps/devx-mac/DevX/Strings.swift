// The app's interface in more than one language.
//
// Three decisions shape this file.
//
// **The English text is the key.** There is no `"live.panel.title"`
// indirection: `tr("Capture configuration")` looks up a translation and returns
// its argument unchanged when there is none. In a codebase whose prose is the
// product -- the difference between "no issue found" and "the detector did not
// run" is carried in sentences, not in labels -- keeping the English readable
// at the call site matters more than a tidy key namespace. It also means a
// missing translation degrades to correct English rather than to `live.panel.
// title` on screen.
//
// **It is compiled in, not a resource.** ADR-0002 keeps this tree free of
// binary assets and the bundle carries only its icon; `.lproj` directories
// would be the first localisation payload and would need Info.plist entries to
// match. A Swift dictionary needs neither.
//
// **Only the app's own chrome is translated.** Detector findings, coverage
// notes, threshold origins and refusal messages come from the C++ core and are
// written into session packages, which are then exported, diffed and compared
// across runs. That text is *evidence*, not interface: translating it would
// change recorded measurements and make two sessions of the same app
// incomparable because they were captured on differently configured machines.
// So it stays in one language, and the Settings tab says so rather than
// leaving a Vietnamese reader to wonder why half the Issues tab is English.
import Foundation

enum DevXLanguage: String, CaseIterable, Identifiable {
    case system, en, vi
    var id: String { rawValue }
    /// Each language's name in itself, which is what a picker should show: a
    /// reader who cannot read the current language still recognises their own.
    var label: String {
        switch self {
        case .system: return "System"
        case .en: return "English"
        case .vi: return "Tiếng Việt"
        }
    }
}

enum Strings {
    /// The language lookups currently use. Set from AppState, which owns the
    /// preference and republishes so the view tree re-renders.
    static var active: DevXLanguage = .en

    /// Resolves `.system` against the user's preferred languages.
    ///
    /// Pure and separate so it can be tested: the interesting cases are a
    /// regional tag (`vi-VN`), a language this app does not have, and an empty
    /// list, and none of them should be reached by reading `Locale.current` in
    /// a view.
    static func resolve(_ preference: DevXLanguage,
                        preferredLanguages: [String]) -> DevXLanguage {
        guard preference == .system else { return preference }
        for tag in preferredLanguages {
            // Match on the language subtag only: `vi-VN`, `vi-Hani-VN` and
            // plain `vi` are all Vietnamese, and a region this app does not
            // distinguish must not fall through to English.
            let lang = tag.split(separator: "-").first.map(String.init)?.lowercased() ?? ""
            if lang == "vi" { return .vi }
            if lang == "en" { return .en }
        }
        return .en
    }

    /// Looks up `text`, or returns it unchanged.
    static func translate(_ text: String, into language: DevXLanguage) -> String {
        switch language {
        case .vi: return vietnamese[text] ?? text
        case .en, .system: return text
        }
    }

    /// Words that are not words.
    ///
    /// `unknown`, `running`, `not_tested`, `unsupported` and `offline` are
    /// *values* the core emits, and the sentences below explain what they mean.
    /// Translating them inside the prose would leave the explanation pointing
    /// at a token that never appears on screen. They are left verbatim on
    /// purpose, and that is not an oversight.
    static let vietnamese: [String: String] = [
        // ---- Tabs ----
        "Devices": "Thiết bị",
        "Apps": "Ứng dụng",
        "Preflight": "Kiểm tra trước",
        "Live": "Trực tiếp",
        "Record": "Ghi",
        "Sessions": "Phiên",
        "Issues": "Vấn đề",
        "Threads": "Luồng",
        "Timeline": "Dòng thời gian",
        "Compare": "So sánh",
        "Detectors": "Bộ phát hiện",
        "Export": "Xuất",

        // ---- Common controls ----
        "Refresh": "Làm mới",
        "Refresh Devices": "Làm mới thiết bị",
        "Reload": "Tải lại",
        "Rebuild": "Dựng lại",
        "Hide": "Ẩn",
        "Show": "Hiện",
        "Start": "Khởi động",
        "Filter": "Lọc",
        "Choose…": "Chọn…",
        "Cancel Running Operation": "Hủy tác vụ đang chạy",
        "Notes": "Ghi chú",
        "Sources": "Nguồn",
        "Paths": "Đường dẫn",
        "Metrics": "Chỉ số",
        "Memory": "Bộ nhớ",
        "Overall": "Tổng thể",
        "Classification": "Phân loại",
        "Conditions": "Điều kiện",
        "Prerequisites": "Điều kiện tiên quyết",
        "Limitations": "Giới hạn",

        // ---- Devices ----
        "Start a simulator or emulator": "Khởi động simulator hoặc emulator",
        "not devices yet: a device id exists only once one is running":
            "chưa phải thiết bị: device id chỉ tồn tại khi đã chạy",
        "Already running": "Đang chạy",
        "Arriving now": "Đang xuất hiện",
        "Started, NOT confirmed ready": "Đã khởi động, CHƯA xác nhận sẵn sàng",
        "Could not start it": "Không khởi động được",
        "Ready": "Sẵn sàng",
        "No device discovered": "Không phát hiện thiết bị nào",
        "Device enumeration failed": "Liệt kê thiết bị thất bại",
        "Provider notes": "Ghi chú từ provider",
        "Recently profiled": "Đã profile gần đây",
        "  (the name when it was last profiled)":
            "  (tên ở lần profile gần nhất)",
        "Android and iOS are discovered together. A paired-but-unreachable "
        + "device reads as offline — it is not absent, and it is not usable. "
        + "Simulators and emulators are listed separately on purpose: their "
        + "timings are never comparable to a physical device.":
            "Android và iOS được phát hiện cùng lúc. Thiết bị đã ghép nối "
            + "nhưng không liên lạc được sẽ hiện là offline — không phải là "
            + "không có, và cũng không dùng được. Simulator và emulator được "
            + "liệt kê riêng có chủ đích: thời gian đo của chúng không bao giờ "
            + "so sánh được với thiết bị thật.",

        // ---- Apps ----
        "App enumeration failed for this device":
            "Liệt kê ứng dụng thất bại trên thiết bị này",
        "Ambiguous target": "Mục tiêu không rõ ràng",
        "resolving the target…": "đang xác định mục tiêu…",
        "**running** does not mean foreground. **unknown** means the provider "
        + "could not observe the state — it does not mean not running. "
        + "Profiling availability is independent of runtime state, and entries "
        + "that cannot be profiled are kept and marked rather than hidden.":
            "**running** không có nghĩa là đang ở foreground. **unknown** "
            + "nghĩa là provider không quan sát được trạng thái — không có "
            + "nghĩa là không chạy. Khả năng profile độc lập với trạng thái "
            + "chạy, và những mục không profile được vẫn được giữ lại và đánh "
            + "dấu chứ không bị ẩn đi.",

        // ---- Preflight ----
        "Capabilities": "Khả năng",
        "Every row is a probe result, not a plan. **unknown** and "
        + "**not_tested** are distinct answers from **unsupported**, and none "
        + "of them means \"false\".":
            "Mỗi dòng là kết quả của một phép thử, không phải một dự định. "
            + "**unknown** và **not_tested** là những câu trả lời khác với "
            + "**unsupported**, và không cái nào trong số đó có nghĩa là "
            + "\"false\".",

        // ---- Live ----
        "Live capture": "Ghi trực tiếp",
        "Capture configuration": "Cấu hình ghi",
        "Capture sources": "Nguồn dữ liệu",
        "Heavier collectors": "Collector nặng hơn",
        "start live capture": "bắt đầu ghi trực tiếp",
        "stop and save": "dừng và lưu",
        "stopping and saving…": "đang dừng và lưu…",
        "Session written": "Đã ghi phiên",
        "No session was written": "Không có phiên nào được ghi",
        "Partial capture": "Ghi không đầy đủ",
        "Live capture not implemented for this platform":
            "Ghi trực tiếp chưa hỗ trợ nền tảng này",
        "framestats is a ring buffer of about the last 120 frames and does not "
        + "drain when read, so the history is cleared at capture start to keep "
        + "the window to this capture's frames. Turn the reset off to analyse "
        + "frames the app produced before you pressed Record.":
            "framestats là một ring buffer chứa khoảng 120 frame gần nhất và "
            + "không bị xóa khi đọc, nên lịch sử được dọn lúc bắt đầu ghi để "
            + "cửa sổ dữ liệu chỉ gồm frame của lần ghi này. Tắt reset nếu bạn "
            + "muốn phân tích những frame ứng dụng tạo ra trước khi bấm Record.",

        // ---- Sessions ----
        "Captures and imports on this machine. A session built from an import "
        + "is labelled as one, and so is a session built from synthetic "
        + "fixture data.":
            "Các lần ghi và nhập trên máy này. Phiên được tạo từ dữ liệu nhập "
            + "sẽ được ghi nhãn rõ, và phiên tạo từ dữ liệu fixture tổng hợp "
            + "cũng vậy.",
        "Showing a re-analysis": "Đang xem kết quả phân tích lại",
        "Re-analysed": "Đã phân tích lại",
        "Checksum mismatch": "Checksum không khớp",

        // ---- Issues ----
        "Open issues": "Vấn đề đang mở",
        "Preliminary": "Sơ bộ",
        "Evidence": "Bằng chứng",
        "Missing evidence": "Thiếu bằng chứng",
        "Coverage gaps": "Khoảng trống dữ liệu",
        "Alternative explanations": "Các cách giải thích khác",
        "Proposed remediation": "Hướng khắc phục đề xuất",
        "Suggested verification": "Cách kiểm chứng đề xuất",
        "Known false positives": "Các trường hợp báo sai đã biết",
        "Measurement context": "Bối cảnh đo",
        "Data quality": "Chất lượng dữ liệu",
        "Detector execution": "Việc chạy bộ phát hiện",
        "Focus on timeline": "Xem trên dòng thời gian",
        "Open in Issues": "Mở trong Vấn đề",
        "Every reason, not just the first": "Mọi lý do, không chỉ lý do đầu tiên",
        "No detector that ran produced a finding":
            "Không bộ phát hiện nào đã chạy đưa ra phát hiện",
        "No detector that can run has found anything yet. That is not the same "
        + "as nothing being wrong: several detectors cannot run until more "
        + "evidence arrives.":
            "Chưa bộ phát hiện nào có thể chạy tìm ra điều gì. Điều đó không "
            + "đồng nghĩa với việc không có vấn đề: một số bộ phát hiện chưa "
            + "thể chạy cho tới khi có thêm bằng chứng.",
        "Already suppressed: ": "Đã được bỏ qua: ",
        "Nothing is suppressed in this project.":
            "Chưa có gì bị bỏ qua trong dự án này.",
        "A suppressed finding stays in the exported document, with its reason: "
        + "that is what makes a suppression auditable rather than a deletion.":
            "Một phát hiện bị bỏ qua vẫn nằm trong tài liệu xuất ra, kèm lý "
            + "do: đó là điều khiến việc bỏ qua có thể kiểm tra lại được, chứ "
            + "không phải là một lần xóa.",
        "An expiry is honoured: once it passes the finding comes back and the "
        + "report says which suppression lapsed. Leave it empty to accept the "
        + "finding until someone removes the entry.":
            "Ngày hết hạn được tôn trọng: khi đã qua, phát hiện sẽ quay lại và "
            + "báo cáo nói rõ mục bỏ qua nào đã hết hiệu lực. Để trống nếu bạn "
            + "muốn chấp nhận phát hiện đó cho đến khi có người xóa mục này.",
        "Written to the project's suppressions.json, which `mpi analyze "
        + "--suppressions` reads too. The session on disk is not modified.":
            "Được ghi vào suppressions.json của dự án, cũng là tệp mà `mpi "
            + "analyze --suppressions` đọc. Phiên trên đĩa không bị thay đổi.",

        // ---- Threads ----
        // React Native's own thread names. "UI", "JS" and "native" stay in
        // English inside them: those are the words a Vietnamese React Native
        // developer uses, and translating them would make the labels harder
        // to match against the documentation, not easier.
        "UI thread (main)": "luồng UI (main)",
        "JS thread": "luồng JS",
        "native modules thread": "luồng native modules",
        "render thread": "luồng render",
        "other": "khác",
        "draws and handles input; work here is what a user feels first":
            "vẽ và xử lý thao tác nhập; công việc ở đây là thứ người dùng "
            + "cảm nhận đầu tiên",
        "the app's own JavaScript; a long task here does not block drawing "
        + "unless the UI thread waits on it":
            "JavaScript của chính ứng dụng; một tác vụ dài ở đây không chặn "
            + "việc vẽ, trừ khi luồng UI phải chờ nó",
        "bridged native work on behalf of JS":
            "công việc native được gọi qua bridge thay cho JS",
        "the platform's own render thread, not the app's code":
            "luồng render của chính nền tảng, không phải mã của ứng dụng",
        "no role could be established from what this capture records":
            "không xác định được vai trò từ những gì lần ghi này lưu lại",
        "platform signal": "tín hiệu từ nền tảng",
        "from the thread's name": "suy ra từ tên luồng",
        "not established": "không xác định được",
        "No threads recorded": "Không có luồng nào được ghi",
        "Roles below come from thread names":
            "Vai trò bên dưới suy ra từ tên luồng",
        "(unnamed)": "(không tên)",
        "no samples attributed": "không có sample nào được quy cho luồng này",
        "no share available": "không có tỷ lệ",

        // ---- Timeline ----
        "No timeline": "Không có dòng thời gian",
        "Issue intervals": "Khoảng thời gian của vấn đề",
        "Reading these tracks": "Cách đọc các track này",
        "no measured bin": "không có bin nào được đo",
        "Not placed": "Không xác định được vị trí",
        "none recorded": "không ghi nhận được gì",
        "not stated": "không nêu",
        "peak ": "đỉnh ",
        "spread ": "độ trải ",

        // ---- Compare ----
        "Cannot compare": "Không so sánh được",
        "Cross-platform pair": "Cặp khác nền tảng",
        "Conditions do not match": "Điều kiện không khớp",
        "Run sets": "Tập các lần chạy",
        "Candidate stacks": "Stack của bản ứng viên",
        "No metric appears in both run sets, so there is nothing to compare. "
        + "That is not a result of zero change.":
            "Không có chỉ số nào xuất hiện ở cả hai tập chạy, nên không có gì "
            + "để so sánh. Đó không phải là kết quả \"không thay đổi\".",

        // ---- Detectors ----
        "All twelve detectors from the specification catalog are registered, "
        + "including the ones not implemented yet — a detector the engine has "
        + "never heard of could not be reported as skipped, and then \"no "
        + "findings\" would be indistinguishable from \"no analysis\".":
            "Cả mười hai bộ phát hiện trong danh mục đặc tả đều được đăng ký, "
            + "kể cả những bộ chưa hiện thực — một bộ phát hiện mà engine chưa "
            + "từng biết đến thì không thể báo là đã bỏ qua, và khi đó \"không "
            + "có phát hiện\" sẽ không phân biệt được với \"không có phân "
            + "tích\".",

        // ---- Export / Settings ----
        "Export the open session": "Xuất phiên đang mở",
        "Export JSON…": "Xuất JSON…",
        "Export Markdown…": "Xuất Markdown…",
        "No session is open. Open one from Sessions first.":
            "Chưa có phiên nào được mở. Hãy mở một phiên ở tab Phiên trước.",
        "Use Choose… rather than typing a path into Documents, Desktop or "
        + "Downloads: macOS gates those and an ad-hoc signed build cannot "
        + "raise the prompt, so a typed path there cannot be read at all.":
            "Hãy dùng Chọn… thay vì tự gõ đường dẫn vào Documents, Desktop "
            + "hay Downloads: macOS chặn các thư mục đó và một bản build ký "
            + "ad-hoc không thể hiện hộp thoại xin quyền, nên đường dẫn gõ tay "
            + "vào đó sẽ không đọc được.",
        "Spec section 13 asks for no source or trace upload without configured "
        + "consent. There is no consent control here because there is nothing "
        + "to consent to: this build has no upload path. Exporting writes a "
        + "local file and that is the whole of it.":
            "Mục 13 của đặc tả yêu cầu không tải lên mã nguồn hay trace mà "
            + "chưa có sự đồng ý được cấu hình. Ở đây không có tùy chọn đồng ý "
            + "vì không có gì để đồng ý: bản build này không có đường tải lên "
            + "nào. Xuất dữ liệu chỉ ghi ra một tệp cục bộ, và chỉ có vậy.",
        "The SDK transport is the only network listener, it binds 127.0.0.1 "
        + "only, it requires a token, and it receives markers rather than "
        + "sending anything. There is no wireless path, on purpose.":
            "Kênh truyền SDK là listener mạng duy nhất, chỉ bind 127.0.0.1, "
            + "bắt buộc có token, và chỉ nhận marker chứ không gửi gì đi. "
            + "Không có đường không dây nào, và đó là chủ đích.",

        // ---- Device freshness ----
        "re-scanning every 5s · last looked {when}":
            "đang quét lại mỗi 5s · lần xem gần nhất {when}",
        "last looked {when}": "lần xem gần nhất {when}",
        "last looked {when} — a device connected or started since then is not "
        + "in this list":
            "lần xem gần nhất {when} — thiết bị được kết nối hoặc khởi động "
            + "sau thời điểm đó không có trong danh sách này",
        "{n}s ago": "{n}s trước",
        "{n}m ago": "{n} phút trước",
        "{n}h ago": "{n} giờ trước",
        "discovery has not run — this list is not yet a claim about what is "
        + "connected":
            "chưa quét thiết bị — danh sách này chưa khẳng định điều gì về "
            + "những gì đang kết nối",
        "a moment ago": "vừa xong",
        "at an unknown time": "vào một thời điểm không rõ",

        // ---- Added after measuring coverage: these were falling
        // back to English mid-panel. ----
        "Android and iOS measure different things with different "
        + "providers on different hardware. These two can be displayed "
        + "side by side, but the pair cannot be used as a regression "
        + "gate.":
            "Android và iOS đo những thứ khác nhau bằng những provider khác "
            + "nhau trên phần cứng khác nhau. Hai bên có thể được trình bày "
            + "cạnh nhau, nhưng cặp này không dùng được làm cổng chặn hồi "
            + "quy.",
        "Android: enable USB debugging and accept the authorization "
        + "prompt. iOS: unlock the device, trust this computer, and "
        + "enable Developer Mode.":
            "Android: bật USB debugging và chấp nhận hộp thoại xin cấp "
            + "quyền. iOS: mở khóa thiết bị, tin cậy máy tính này, và bật "
            + "Developer Mode.",
        "Binning re-reads and re-analyzes the trace, so it is done on "
        + "request.":
            "Việc chia bin sẽ đọc lại và phân tích lại trace, nên chỉ chạy "
            + "khi có yêu cầu.",
        "Each family is a separate measurement and is never summed with "
        + "another.":
            "Mỗi họ chỉ số là một phép đo riêng và không bao giờ được cộng "
            + "dồn với họ khác.",
        "Each source is independent: one failing does not stop the "
        + "others, and a source that did not run becomes a coverage gap "
        + "rather than an absence of events.":
            "Mỗi nguồn hoạt động độc lập: một nguồn lỗi không làm dừng các "
            + "nguồn khác, và nguồn không chạy sẽ trở thành khoảng trống dữ "
            + "liệu chứ không phải là không có sự kiện nào.",
        "Frames and memory stream at the tick cadence. CPU sampling "
        + "runs on its own thread in longer windows, because simpleperf "
        + "costs about 5.6 s per record-and-symbolise cycle — so CPU "
        + "numbers lag, and the intervals between windows are recorded as "
        + "coverage gaps rather than as measured idle time.":
            "Frame và bộ nhớ được truyền theo nhịp tick. Việc lấy mẫu CPU "
            + "chạy trên luồng riêng với cửa sổ dài hơn, vì simpleperf tốn "
            + "khoảng 5,6 s cho mỗi vòng ghi-và-giải-ký-hiệu — nên số liệu "
            + "CPU bị trễ, và các quãng giữa những cửa sổ đó được ghi nhận là "
            + "khoảng trống dữ liệu chứ không phải thời gian nhàn rỗi đã đo "
            + "được.",
        "No platform signal says which thread runs JavaScript, so the "
        + "JS and native-module roles are matched on the thread's name -- "
        + "`mqt_v_js`, `mqt_js`, anything containing `hermes`. That is a "
        + "convention React Native has changed before, and any thread "
        + "could carry such a name. The UI main thread is the exception: "
        + "the collector establishes it from the process, not the name.":
            "Không có tín hiệu nào từ nền tảng cho biết luồng nào chạy "
            + "JavaScript, nên vai trò JS và native-module được suy ra từ tên "
            + "luồng -- `mqt_v_js`, `mqt_js`, hay bất cứ tên nào chứa "
            + "`hermes`. Đó là một quy ước mà React Native đã từng thay đổi, "
            + "và luồng nào cũng có thể mang tên như vậy. Luồng UI chính là "
            + "ngoại lệ: collector xác định nó từ tiến trình, không phải từ "
            + "tên.",
        "Nothing was started. Device id ":
            "Không có gì được khởi động. Device id ",
        "Open one from Sessions to see how its work divides between the "
        + "UI, JS and native threads.":
            "Hãy mở một phiên ở tab Phiên để xem công việc được chia ra sao "
            + "giữa luồng UI, JS và native.",
        "Open one from Sessions to see its timeline.":
            "Hãy mở một phiên ở tab Phiên để xem dòng thời gian của nó.",
        "Open one from Sessions, or record a new capture.":
            "Hãy mở một phiên ở tab Phiên, hoặc ghi một lần mới.",
        "Pick a baseline and a candidate run-set file. A run set is "
        + "several runs of the same scenario under stated conditions -- "
        + "one capture is not a benchmark.":
            "Hãy chọn tệp run-set cho bản nền và bản ứng viên. Một run set "
            + "gồm nhiều lần chạy cùng một tình huống dưới các điều kiện đã "
            + "nêu rõ -- một lần ghi thì chưa phải là một phép benchmark.",
        "Pick a usable device first.":
            "Hãy chọn một thiết bị dùng được trước.",
        "Record one, or import a trace with the CLI.":
            "Hãy ghi một phiên, hoặc nhập một trace bằng CLI.",
        "Recorded with the session, because two runs are only "
        + "comparable when they used the same collector settings.":
            "Được ghi kèm phiên, vì hai lần chạy chỉ so sánh được với nhau "
            + "khi dùng cùng một cấu hình collector.",
        "Scheduling and I/O  (atrace)":
            "Lập lịch và I/O  (atrace)",
        "See the detector table below for which detectors could not "
        + "run, and why. A skipped detector found nothing because it did "
        + "not run — that is not the same statement as \"no issue "
        + "exists\".":
            "Xem bảng bộ phát hiện bên dưới để biết bộ nào không chạy được "
            + "và vì sao. Một bộ phát hiện bị bỏ qua thì không tìm thấy gì vì "
            + "nó không chạy — điều đó không giống với câu \\\"không có vấn "
            + "đề nào\\\".",
        "Suppress this finding":
            "Bỏ qua phát hiện này",
        "Suppressed":
            "Đã bỏ qua",
        "Suppressions": "Các mục bỏ qua",
        "{a} ran, {b} could not.": "{a} đã chạy, {b} không chạy được.",
        "Synthetic data":
            "Dữ liệu tổng hợp",
        "Target":
            "Mục tiêu",
        "Target not found":
            "Không tìm thấy mục tiêu",
        "That is a different answer from \"no devices are connected\".":
            "Đó là một câu trả lời khác với \\\"không có thiết bị nào đang "
            + "kết nối\\\".",
        "That is not the same as the device having no apps.":
            "Điều đó không giống với việc thiết bị không có ứng dụng nào.",
        "The capture window is still open. A detector that has found "
        + "nothing yet may still fire, and a finding may change as more "
        + "evidence arrives. Nothing below is a final result.":
            "Cửa sổ ghi vẫn đang mở. Một bộ phát hiện chưa tìm thấy gì vẫn "
            + "có thể báo sau, và một phát hiện có thể thay đổi khi có thêm "
            + "bằng chứng. Không có gì bên dưới là kết quả cuối cùng.",
        "The findings on screen were re-run with the current "
        + "suppression list. This export copies the report as it was "
        + "recorded, which is not the same document. Use `mpi analyze "
        + "--suppressions` to write a report with them applied.":
            "Các phát hiện trên màn hình đã được chạy lại với danh sách bỏ "
            + "qua hiện tại. Bản xuất này sao chép báo cáo đúng như lúc được "
            + "ghi, và đó không phải cùng một tài liệu. Hãy dùng `mpi analyze "
            + "--suppressions` để tạo báo cáo đã áp dụng chúng.",
        "The recording ended abnormally, so absence of a finding is not "
        + "evidence of absence. ":
            "Lần ghi kết thúc bất thường, nên việc không có phát hiện không "
            + "phải là bằng chứng rằng không có vấn đề.",
        "The recording ended abnormally, so an empty stretch may be the "
        + "recording stopping rather than the app going quiet.":
            "Lần ghi kết thúc bất thường, nên một quãng trống có thể là do "
            + "việc ghi bị dừng chứ không phải do ứng dụng im lặng.",
        "These findings come from re-running the detectors over this "
        + "session's stored trace with the current suppression list. The "
        + "session on disk is unchanged -- the raw trace is immutable and "
        + "this is a view of it.":
            "Các phát hiện này đến từ việc chạy lại các bộ phát hiện trên "
            + "trace đã lưu của phiên này với danh sách bỏ qua hiện tại. "
            + "Phiên trên đĩa không thay đổi -- trace thô là bất biến và đây "
            + "chỉ là một cách xem nó.",
        "These tracks came from a labelled fixture. Nothing here "
        + "describes a real application.":
            "Các track này đến từ dữ liệu fixture đã được ghi nhãn. Không "
            + "có gì ở đây mô tả một ứng dụng thật.",
        "This capture names no threads, so there is no split to show. "
        + "That is a missing provider, not an app with one thread: thread "
        + "identity comes from the sampler, and a capture without CPU "
        + "sampling records none.":
            "Lần ghi này không đặt tên luồng nào, nên không có phần chia "
            + "nào để hiển thị. Đó là thiếu provider, không phải một ứng dụng "
            + "chỉ có một luồng: danh tính luồng đến từ bộ lấy mẫu, và một "
            + "lần ghi không lấy mẫu CPU thì không ghi nhận luồng nào.",
        "This session came from a labelled fixture, not a real device "
        + "capture. Nothing here describes a real application. ":
            "Phiên này đến từ dữ liệu fixture đã ghi nhãn, không phải từ "
            + "một lần ghi trên thiết bị thật. Không có gì ở đây mô tả một "
            + "ứng dụng thật.",
        "This session produced no issue to inspect. That is not the "
        + "same as nothing being wrong: the detector table says which "
        + "detectors ran.":
            "Phiên này không tạo ra vấn đề nào để xem xét. Điều đó không "
            + "đồng nghĩa với việc không có gì sai: bảng bộ phát hiện cho "
            + "biết những bộ nào đã chạy.",
        "Threshold":
            "Ngưỡng",
        "Thresholds applied":
            "Các ngưỡng đã áp dụng",
        "Timeline unavailable":
            "Không có dòng thời gian",
        "Tracks":
            "Các track",
        "What is still unknown":
            "Những gì vẫn chưa biết",
        "What leaves this machine":
            "Những gì rời khỏi máy này",
        "What the evidence supports":
            "Những gì bằng chứng chứng minh được",
        "What these tracks do not say":
            "Những gì các track này không nói",
        "What this split does not say":
            "Những gì cách chia này không nói",
        "Where the sampled work was":
            "Công việc được lấy mẫu nằm ở đâu",
        "Window":
            "Cửa sổ",
        "a verdict is only as meaningful as the bar it cleared":
            "một kết luận chỉ có ý nghĩa tương ứng với mức ngưỡng mà nó "
            + "vượt qua",
        "busiest first; a thread with no samples is not a thread that "
        + "did nothing":
            "bận nhất lên trước; một luồng không có sample không phải là "
            + "luồng không làm gì",
        "each side is a set of runs, not a single capture":
            "mỗi bên là một tập nhiều lần chạy, không phải một lần ghi đơn lẻ",
        "from the collectors' own records, not inferred from quiet bins":
            "từ chính ghi nhận của các collector, không phải suy ra từ các "
            + "bin im lặng",
        "no comparison yet":
            "chưa có so sánh nào",
        "no device selected":
            "chưa chọn thiết bị",
        "no session open":
            "chưa mở phiên nào",
        "no sessions yet":
            "chưa có phiên nào",
        "nothing selected":
            "chưa chọn gì",
        "off by default; each one costs something on the device and "
        + "says what":
            "mặc định tắt; mỗi cái đều tốn tài nguyên trên thiết bị và có "
            + "nêu rõ là gì",
        "one row per source; families are never stacked, because a "
        + "total would double-count":
            "mỗi nguồn một dòng; các họ chỉ số không bao giờ xếp chồng, vì "
            + "một tổng số sẽ đếm trùng",
        "press Build":
            "bấm Dựng lại",
        "shared with `mpi analyze --suppressions`; a suppressed finding "
        + "stays in the export":
            "dùng chung với `mpi analyze --suppressions`; một phát hiện bị "
            + "bỏ qua vẫn nằm trong bản xuất",
        "the app's own interface only":
            "chỉ giao diện của chính ứng dụng",
        "the evidence's own interval; clicking focuses it":
            "khoảng thời gian của chính bằng chứng; bấm vào để tập trung vào nó",
        "the package's own report, copied unchanged":
            "báo cáo của chính gói phiên, sao chép không thay đổi",
        "the pair is only comparable where these agree":
            "cặp này chỉ so sánh được ở những chỗ các điều kiện này trùng nhau",
        "timeline not built":
            "chưa dựng dòng thời gian",
        "what this app reads, and what the CLI reads with it":
            "những gì ứng dụng này đọc, và những gì CLI đọc cùng với nó",
        "what was profiled from this machine — never evidence that an "
        + "app is running now":
            "những gì đã được profile từ máy này — không bao giờ là bằng "
            + "chứng rằng một ứng dụng đang chạy",

        // ---- Inspect ----
        "A React Native debug build already runs an inspector and "
        + "already connects itself to Metro. This reads that connection, "
        + "so nothing is added to the app: no dependency, no import, no "
        + "rebuild. A release build runs no inspector, and an empty "
        + "capture there means there was nothing to attach to.":
            "Một bản build debug của React Native đã tự chạy inspector và "
            + "đã tự kết nối tới Metro. Tính năng này đọc kết nối đó, nên "
            + "không cần thêm gì vào ứng dụng: không thư viện, không import, "
            + "không build lại. Bản release không chạy inspector, và một lần "
            + "quan sát trống ở đó nghĩa là không có gì để kết nối tới.",
        "A screenshot was not taken":
            "Không chụp được ảnh màn hình",
        "Attachable now":
            "Có thể kết nối lúc này",
        "Inspect":
            "Quan sát",
        "Metro is not answering":
            "Metro không trả lời",
        "Metro is running and nothing is attached to its inspector. A "
        + "debug build connects itself; a release build has no inspector "
        + "to connect.":
            "Metro đang chạy và chưa có gì kết nối tới inspector của nó. "
            + "Bản build debug sẽ tự kết nối; bản release không có inspector "
            + "để kết nối.",
        "Network":
            "Mạng",
        "No app is attached":
            "Chưa có ứng dụng nào kết nối",
        "Nothing below is a statement about the app.":
            "Không có gì bên dưới nói lên điều gì về ứng dụng.",
        "Nothing was attached":
            "Không kết nối được gì",
        "Nothing was reported. The domain was enabled, so this is the "
        + "app making no JavaScript HTTP calls in the window — a native "
        + "module's own HTTP would not appear here either way.":
            "Không có gì được báo về. Domain đã được bật, nên đây là việc "
            + "ứng dụng không gọi HTTP nào từ JavaScript trong khoảng thời "
            + "gian này — HTTP do một native module tự gọi thì dù sao cũng "
            + "không xuất hiện ở đây.",
        "Observe":
            "Quan sát",
        "Redux state":
            "Trạng thái Redux",
        "Screenshots":
            "Ảnh màn hình",
        "The app logged nothing in this window.":
            "Ứng dụng không ghi log gì trong khoảng thời gian này.",
        "This says nothing about the app.":
            "Điều này không nói lên gì về ứng dụng.",
        "What this does not show":
            "Những gì phần này không cho thấy",
        "a debugger session, not a measurement":
            "một phiên debugger, không phải một phép đo",
        "a store holds tokens and personal data; the values are left "
        + "out unless asked for":
            "store chứa token và dữ liệu cá nhân; các giá trị sẽ bị loại ra "
            + "trừ khi bạn yêu cầu",
        "each shows the moment it was taken, and nothing else":
            "mỗi ảnh chỉ cho thấy đúng thời điểm nó được chụp, không gì khác",
        "include its values":
            "kèm theo các giá trị",
        "inferred Redux action:":
            "action Redux suy ra được:",
        "listed by Metro; a debug build appears here by itself":
            "do Metro liệt kê; bản build debug sẽ tự xuất hiện ở đây",
        "needs a device: Metro knows the app but not which device it is on":
            "cần một thiết bị: Metro biết ứng dụng nhưng không biết nó đang "
            + "chạy trên thiết bị nào",
        "not looked yet":
            "chưa quét",
        "observe":
            "quan sát",
        "observing…":
            "đang quan sát…",
        "read the Redux store":
            "đọc store Redux",
        "screenshot the screen before and after":
            "chụp màn hình trước và sau",
        "size unknown":
            "không rõ kích thước",
        "slice(s)":
            "slice",
        "state, not actions":
            "trạng thái, không phải action",
        "still in flight when the window closed: evidence of the "
        + "request, none of its outcome":
            "vẫn đang chạy khi cửa sổ quan sát đóng lại: là bằng chứng có "
            + "request, không nói gì về kết quả của nó",
        "the JavaScript side only":
            "chỉ phía JavaScript",
        "use":
            "dùng",
        "whatever the app chose to log":
            "bất cứ thứ gì ứng dụng chọn ghi log",

        // ---- Field labels ----
        // Short labels down the left of every panel. Technical
        // tokens a Vietnamese developer says in English -- cpu,
        // frame, tick -- stay inside them.
        "app": "ứng dụng",
        "baseline": "bản nền",
        "candidate": "bản ứng viên",
        "cause": "nguyên nhân",
        "clock": "đồng hồ",
        "collected": "đã thu",
        "counter points": "điểm counter",
        "cpu samples": "mẫu CPU",
        "cpu window": "cửa sổ CPU",
        "detection": "phát hiện",
        "detector": "bộ phát hiện",
        "device": "thiết bị",
        "duration": "thời lượng",
        "elapsed": "đã trôi qua",
        "eligibility": "điều kiện hợp lệ",
        "evidence": "bằng chứng",
        "expiry": "ngày hết hạn",
        "fingerprint": "dấu vân dữ liệu",
        "fix": "cách khắc phục",
        "frame history": "lịch sử frame",
        "frames": "frame",
        "interval": "khoảng",
        "last tick": "tick gần nhất",
        "max relative spread": "độ trải tương đối tối đa",
        "min absolute delta": "chênh lệch tuyệt đối tối thiểu",
        "min relative delta": "chênh lệch tương đối tối thiểu",
        "min valid runs": "số lần chạy hợp lệ tối thiểu",
        "mode": "chế độ",
        "occurrences": "số lần xuất hiện",
        "platform": "nền tảng",
        "reason": "lý do",
        "recents": "gần đây",
        "reference": "tham chiếu",
        "resolution": "độ phân giải",
        "sampling": "lấy mẫu",
        "scope": "phạm vi",
        "screen": "màn hình",
        "seconds": "giây",
        "session": "phiên",
        "sessions": "các phiên",
        "severity": "mức độ",
        "sources": "nguồn",
        "suppressed": "đã bỏ qua",
        "suppressions": "các mục bỏ qua",
        "symbols": "ký hiệu",
        "target": "mục tiêu",
        "threads": "luồng",
        "window": "cửa sổ",

        // ---- Language ----
        "Language": "Ngôn ngữ",
        "Interface language": "Ngôn ngữ giao diện",
        "Findings, coverage notes and refusal messages come from the analysis "
        + "core and stay in English. They are written into the session package "
        + "and compared across runs, so they are evidence rather than "
        + "interface: translating them would make two captures of the same app "
        + "incomparable because the machines were configured differently.":
            "Các phát hiện, ghi chú về độ phủ dữ liệu và thông báo từ chối đến "
            + "từ lõi phân tích và được giữ nguyên tiếng Anh. Chúng được ghi "
            + "vào gói phiên và đem so sánh giữa các lần chạy, nên chúng là "
            + "bằng chứng chứ không phải giao diện: dịch chúng sẽ làm hai lần "
            + "ghi của cùng một ứng dụng không so sánh được với nhau chỉ vì "
            + "hai máy được cấu hình khác nhau.",
    ]
}

/// Translates a piece of the app's own interface.
///
/// Named `tr` rather than `t`, which this codebase already uses for loop
/// variables -- a shadowed `t` turned nine call sites into attempts to call a
/// JSON value, and the compiler's message for that says nothing about
/// translation.
func tr(_ text: String) -> String {
    Strings.translate(text, into: Strings.active)
}
