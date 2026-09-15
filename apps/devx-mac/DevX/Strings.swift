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
        // Two catalogs, checked in order. `vietnamese` is this app's own
        // chrome; `vietnameseCore` is text the C++ core emits.
        case .vi: return vietnamese[text] ?? vietnameseCore[text] ?? text
        case .en, .system: return text
        }
    }

    /// Text the analysis core emits, translated **for display only**.
    ///
    /// This started as a deliberate exclusion: findings, coverage notes and
    /// threshold origins are written into the session package, exported, and
    /// compared across runs, so translating them would leave two captures of
    /// the same app incomparable because the machines were configured
    /// differently.
    ///
    /// That reasoning was about the *stored* text, and it still holds -- so
    /// nothing here changes what is written to disk. The session package, the
    /// JSON and Markdown exports and everything `mpi compare` reads stay in
    /// one language. This is a lookup applied when a string is drawn on
    /// screen, and a reader who chose Vietnamese should not have to read the
    /// detector catalog in English to use the tool.
    ///
    /// Identifiers are **not** here and must not be: `frames_responsiveness`,
    /// `configurable_heuristic`, `DET-01`, `frame_records`. Those are values,
    /// not prose -- they appear in the exported document, in
    /// `--suppressions` files and in the specification, and a translated
    /// identifier would not match any of them.
    static let vietnameseCore: [String: String] = [
        "Expensive development-tooling or profiler activity":
            "Hoạt động tốn kém của công cụ phát triển hoặc profiler",
        "JS execution spans with measured durations (a sampling profile "
        + "alone does not contain task boundaries)":
            "các span thực thi JS có thời lượng đo được (một sampling "
            + "profile đơn thuần không chứa ranh giới tác vụ)",
        "Long JS execution":
            "Tác vụ JS chạy dài",
        "Memory growth across screen cycles":
            "Bộ nhớ tăng qua các vòng vào/ra màn hình",
        "Missed frame deadlines / UI responsiveness":
            "Frame trễ hạn / độ phản hồi của UI",
        "Network delay affecting an interaction":
            "Độ trễ mạng ảnh hưởng tới một tương tác",
        "Performance regression":
            "Hồi quy hiệu năng",
        "React commit markers from the app's own profiling data, "
        + "reported through the SDK; sampled stacks are not a substitute":
            "các marker React commit từ dữ liệu profiling của chính ứng "
            + "dụng, báo về qua SDK; stack lấy mẫu không thay thế được",
        "Repeated React renders":
            "React render lặp lại",
        "Retained-object investigation":
            "Điều tra đối tượng bị giữ lại",
        "Sampled CPU hotspot":
            "Điểm nóng CPU theo lấy mẫu",
        "Startup budget exceedance":
            "Vượt ngân sách thời gian khởi động",
        "Synchronous I/O on a user-visible thread":
            "I/O đồng bộ trên luồng người dùng thấy được",
        "Wait / lock contention":
            "Tranh chấp chờ / khoá",
        "a background surface's frames are not a foreground animation (E04)":
            "frame của một surface chạy nền không phải là animation ở tiền "
            + "cảnh (E04)",
        "a baseline and a candidate run set for the same scenario version":
            "một run set bản nền và một run set bản ứng viên cho cùng một "
            + "phiên bản tình huống",
        "a block this long is very likely to be user-visible; still an "
        + "impact ordering, not a standard":
            "một lần chặn dài như vậy rất có thể người dùng thấy được; vẫn "
            + "chỉ là xếp thứ tự ảnh hưởng, không phải tiêu chuẩn",
        "a bound on the search, not a claim: a longer chain is not "
        + "reported rather than being reported as absent":
            "một giới hạn cho việc tìm kiếm, không phải một khẳng định: một "
            + "chuỗi dài hơn sẽ không được báo, chứ không phải được báo là "
            + "không có",
        "a cache filling up on purpose produces exactly this shape, and "
        + "this rule cannot tell a cache from a retention":
            "một cache đang được làm đầy có chủ đích tạo ra đúng hình dạng "
            + "này, và quy tắc này không phân biệt được cache với việc bị giữ "
            + "lại",
        "a component that commits cheaply many times can cost less than "
        + "one that commits once expensively; a count is not a cost":
            "một component commit nhiều lần nhưng nhẹ có thể tốn ít hơn một "
            + "component chỉ commit một lần nhưng nặng; số lần không phải là "
            + "chi phí",
        "a debug build's startup includes work the shipped build does "
        + "not do, and this rule never subtracts an estimate for it":
            "khởi động của bản debug bao gồm công việc mà bản phát hành "
            + "không làm, và quy tắc này không bao giờ trừ đi một con số ước "
            + "lượng cho phần đó",
        "a debug build's tooling -- LeakCanary, an instrumentation "
        + "hook, a profiler -- holds a reference precisely to watch it":
            "công cụ trong bản debug -- LeakCanary, một instrumentation "
            + "hook, một profiler -- giữ tham chiếu chính là để theo dõi đối "
            + "tượng đó",
        "a declared clock domain for the JS spans":
            "một clock domain được khai báo cho các span JS",
        "a deliberate cache keyed on the object, where retention is the "
        + "feature and not the fault":
            "một cache có chủ đích lấy đối tượng làm khoá, nơi việc giữ lại "
            + "là tính năng chứ không phải lỗi",
        "a display-callback proxy can report a late callback while the "
        + "frame was still presented on time (E21)":
            "một display-callback proxy có thể báo callback muộn trong khi "
            + "frame vẫn được trình chiếu đúng hạn (E21)",
        "a heap dump with reference paths, from `mpi record --heap` on "
        + "Android; memory counters cannot substitute, because a counter "
        + "says how much is held and never by what":
            "một heap dump kèm đường dẫn tham chiếu, từ `mpi record --heap` "
            + "trên Android; counter bộ nhớ không thay thế được, vì counter "
            + "chỉ nói giữ bao nhiêu chứ không bao giờ nói bị giữ bởi cái gì",
        "a kernel-reported I/O wait (`sched_blocked_reason` with "
        + "iowait=1) attributed to one of the app's threads":
            "một lần chờ I/O do kernel báo (`sched_blocked_reason` với "
            + "iowait=1) được quy cho một trong các luồng của ứng dụng",
        "a list that streams rows, a progress indicator, or an animated "
        + "value commits often by design, and this rule cannot tell that "
        + "from a redundant re-render":
            "một danh sách đang stream từng dòng, một chỉ báo tiến trình, "
            + "hay một giá trị đang animate thì commit thường xuyên theo "
            + "thiết kế, và quy tắc này không phân biệt được điều đó với việc "
            + "render lại dư thừa",
        "a long task on a background JS runtime or worker may do no UI "
        + "harm (spec E07, G06)":
            "một tác vụ dài trên một JS runtime chạy nền hoặc worker có thể "
            + "không gây hại gì cho UI (spec E07, G06)",
        "a memory counter series with a reading inside each cycle":
            "một chuỗi counter bộ nhớ có ít nhất một số đọc trong mỗi vòng",
        "a page-cache read that would have been served from memory on a "
        + "warmer device: a cold first run blocks where a later one does "
        + "not":
            "một lần đọc page-cache mà trên máy đã 'ấm' hơn thì sẽ được "
            + "phục vụ từ bộ nhớ: lần chạy nguội đầu tiên bị chặn ở nơi mà "
            + "lần sau thì không",
        "a per-frame deadline, either provider-reported or derived from "
        + "an observed refresh rate":
            "một hạn chót cho mỗi frame, do provider báo về hoặc suy ra từ "
            + "tần số làm mới quan sát được",
        "a project startup budget: there is no platform standard to "
        + "default to, so one must be configured as DET-07.budget_ms":
            "một ngân sách thời gian khởi động của dự án: không có tiêu "
            + "chuẩn nền tảng nào để lấy làm mặc định, nên phải cấu hình một "
            + "giá trị ở DET-07.budget_ms",
        "a reading older than this relative to the cycle boundary is "
        + "not treated as that cycle's memory":
            "một số đọc cũ hơn khoảng này so với ranh giới vòng sẽ không "
            + "được coi là bộ nhớ của vòng đó",
        "a real difference between two configurations that says nothing "
        + "about release performance, when either side is not "
        + "benchmark-eligible (spec I16)":
            "một khác biệt thật giữa hai cấu hình nhưng không nói lên điều "
            + "gì về hiệu năng bản phát hành, khi một trong hai bên không đủ "
            + "điều kiện làm benchmark (spec I16)",
        "a regression at or above this relative size is ranked high; it "
        + "orders impact and is not a platform standard":
            "một hồi quy ở mức tương đối này trở lên được xếp mức cao; nó "
            + "xếp thứ tự ảnh hưởng và không phải tiêu chuẩn của nền tảng",
        "a request that was slow because the app queued it behind "
        + "others, which is the app's scheduling and not the network":
            "một request chậm vì ứng dụng xếp nó sau các request khác, đó "
            + "là việc lập lịch của ứng dụng chứ không phải của mạng",
        "a request the interaction did not actually wait on: "
        + "overlapping in time is not the same as blocking, and "
        + "prefetching looks identical from here":
            "một request mà tương tác thực ra không chờ: trùng nhau về thời "
            + "gian không đồng nghĩa với bị chặn, và prefetch nhìn từ đây thì "
            + "y như vậy",
        "a run of consecutive misses is more likely to be user-visible "
        + "than the same count scattered":
            "một chuỗi frame trễ liên tiếp dễ bị người dùng thấy hơn là "
            + "cùng số đó nhưng rải rác",
        "a sampler misses functions shorter than its interval, so an "
        + "absent leaf is not proof of absent work (E14)":
            "bộ lấy mẫu bỏ sót các hàm ngắn hơn chu kỳ lấy mẫu, nên một "
            + "leaf không xuất hiện không chứng minh là không có công việc "
            + "(E14)",
        "a scenario that changed meaning between the two versions while "
        + "keeping its id, which no amount of run repetition detects":
            "một tình huống đã đổi ý nghĩa giữa hai phiên bản nhưng vẫn giữ "
            + "nguyên id, điều mà lặp lại bao nhiêu lần chạy cũng không phát "
            + "hiện được",
        "a screen that legitimately keeps more state after being "
        + "visited, such as a list that has loaded more pages":
            "một màn hình giữ lại nhiều trạng thái hơn một cách chính đáng "
            + "sau khi được vào, ví dụ một danh sách đã tải thêm trang",
        "a shared allocator, GC, or scheduler effect caused by tooling "
        + "cannot be cleanly separated and stays unclassified (section 9 "
        + "rule 5)":
            "một ảnh hưởng dùng chung ở allocator, GC hoặc scheduler do "
            + "công cụ gây ra thì không tách bạch được và vẫn để không phân "
            + "loại (mục 9 quy tắc 5)",
        "a single launch is one sample. A cold start on a device that "
        + "was busy with something else looks identical to a slow app "
        + "(spec section 12 requires repeated runs before a startup "
        + "claim)":
            "một lần khởi động là một mẫu. Một lần khởi động nguội trên máy "
            + "đang bận việc khác nhìn y như một ứng dụng chậm (spec mục 12 "
            + "yêu cầu lặp lại nhiều lần chạy trước khi khẳng định về khởi "
            + "động)",
        "a single long task is reportable; unlike a rate, it needs no "
        + "sample population":
            "một tác vụ dài đơn lẻ vẫn báo được; khác với một tỷ lệ, nó "
            + "không cần một tập mẫu",
        "a startup interval from a launch this tool performed, with a "
        + "named endpoint":
            "một khoảng thời gian khởi động từ lần khởi chạy do chính công "
            + "cụ này thực hiện, với một điểm kết thúc được nêu rõ",
        "a stated expectation of when the object should have been "
        + "freed. Without one, a reachable object is just a reachable "
        + "object":
            "một kỳ vọng được nêu rõ về thời điểm đối tượng lẽ ra phải được "
            + "giải phóng. Không có kỳ vọng đó thì một đối tượng còn tham "
            + "chiếu chỉ là một đối tượng còn tham chiếu",
        "a task measured while a debugger was paused has an invalid "
        + "duration (spec C08) -- the eligibility model records the "
        + "pause, but a mid-capture attach can still contaminate one "
        + "interval":
            "một tác vụ được đo trong lúc debugger đang tạm dừng thì có "
            + "thời lượng không hợp lệ (spec C08) -- mô hình điều kiện hợp lệ "
            + "ghi nhận lần tạm dừng, nhưng việc attach giữa lúc ghi vẫn có "
            + "thể làm nhiễu một khoảng",
        "a thread sleeping because it has nothing to do is waiting, not "
        + "contending, and this rule cannot always tell them apart":
            "một luồng đang ngủ vì không có việc gì làm là đang chờ, không "
            + "phải đang tranh chấp, và quy tắc này không luôn phân biệt được "
            + "hai điều đó",
        "an I/O wait during startup, when the app is legitimately "
        + "reading its own code and assets":
            "một lần chờ I/O trong lúc khởi động, khi ứng dụng đang đọc mã "
            + "và tài nguyên của chính nó một cách chính đáng",
        "an allocator holding freed pages instead of returning them to "
        + "the OS, which raises RSS while the app's live set is unchanged":
            "allocator giữ lại các page đã giải phóng thay vì trả về cho "
            + "OS, làm RSS tăng trong khi live set của ứng dụng không đổi",
        "an emulator's I/O behaviour is not a phone's, and neither is a "
        + "device with a full disk":
            "hành vi I/O của emulator không phải của điện thoại, và một máy "
            + "sắp hết dung lượng cũng vậy",
        "an emulator's startup is not a phone's: the CPU, the storage "
        + "and the thermal behaviour all differ":
            "khởi động trên emulator không phải khởi động trên điện thoại: "
            + "CPU, bộ lưu trữ và hành vi nhiệt đều khác",
        "an environmental change the run conditions do not capture -- a "
        + "background update, a thermal state that drifted between the "
        + "two sets -- looks identical to a code regression at this level "
        + "(I04, I06)":
            "một thay đổi môi trường mà điều kiện chạy không ghi lại -- một "
            + "bản cập nhật chạy nền, trạng thái nhiệt thay đổi giữa hai tập "
            + "-- nhìn ở mức này thì y như một hồi quy do mã (I04, I06)",
        "an idle surface legitimately producing few frames can look "
        + "like a low frame rate without any jank (spec E03)":
            "một surface đang rảnh và tạo ra ít frame một cách chính đáng "
            + "có thể trông như tần số frame thấp mà chẳng có jank nào (spec "
            + "E03)",
        "an initial default for a dominant leaf":
            "giá trị mặc định ban đầu cho một leaf chiếm phần lớn",
        "an initial default for a leaf worth investigating; there is no "
        + "universal 'CPU > 80% is a bug' rule":
            "mặc định ban đầu cho một leaf đáng để điều tra; không có quy "
            + "tắc phổ quát nào kiểu 'CPU > 80% là lỗi'",
        "an initial heuristic for a task likely to be noticed "
        + "regardless of frame evidence":
            "một heuristic ban đầu cho một tác vụ có khả năng bị để ý bất "
            + "kể có bằng chứng về frame hay không",
        "an initial heuristic for a task long enough to be worth "
        + "looking at; it is not a platform deadline and not a mobile OS "
        + "standard":
            "một heuristic ban đầu cho một tác vụ dài đủ để đáng xem; đây "
            + "không phải hạn chót của nền tảng và không phải tiêu chuẩn của "
            + "hệ điều hành di động",
        "an interaction that was already complete for the user while a "
        + "request finished in the background":
            "một tương tác mà với người dùng thì đã xong, trong khi một "
            + "request vẫn kết thúc ở chạy nền",
        "an interaction with a begin and an end, so there is something "
        + "the request could have delayed":
            "một tương tác có điểm bắt đầu và điểm kết thúc, để có một thứ "
            + "mà request có thể đã làm chậm",
        "an optimized, inlined, or tail-called frame may be attributed "
        + "to its caller (E15)":
            "một frame đã được tối ưu, inline hoặc tail-call có thể bị quy "
            + "cho hàm gọi nó (E15)",
        "at least one conclusive ownership rule matched; a library name "
        + "alone is not sufficient":
            "có ít nhất một quy tắc quy chủ mang tính kết luận khớp; chỉ "
            + "một tên thư viện là chưa đủ",
        "at least one frame record for the target process":
            "ít nhất một frame record của tiến trình mục tiêu",
        "at least three completed mount/unmount cycles of the same "
        + "screen, from the app's own SDK markers":
            "ít nhất ba vòng mount/unmount hoàn tất của cùng một màn hình, "
            + "từ marker SDK của chính ứng dụng",
        "background computation that does not affect the UI can "
        + "dominate the sample population (E04)":
            "tính toán chạy nền không ảnh hưởng tới UI có thể chiếm phần "
            + "lớn tập mẫu (E04)",
        "below this many frames the miss rate is too noisy to report; "
        + "raise it for steadier results":
            "dưới số frame này thì tỷ lệ trễ quá nhiễu để báo; hãy tăng lên "
            + "để có kết quả ổn định hơn",
        "below this population a share is not distinguishable from "
        + "sampling noise":
            "dưới cỡ mẫu này thì một tỷ lệ không phân biệt được với nhiễu "
            + "lấy mẫu",
        "below three visits a rise is one difference, not a trend; "
        + "raise it for a stronger claim":
            "dưới ba lần vào màn hình thì một mức tăng chỉ là một khác "
            + "biệt, không phải một xu hướng; hãy tăng lên để có khẳng định "
            + "mạnh hơn",
        "caps how many hotspots one capture reports":
            "giới hạn số điểm nóng mà một lần ghi báo về",
        "commits driven by real incoming data are not the component's fault":
            "các commit do dữ liệu thật đi vào gây ra thì không phải lỗi "
            + "của component",
        "each commit must name the component it belongs to":
            "mỗi commit phải nêu rõ component mà nó thuộc về",
        "enough completed runs on both sides for a median and a spread":
            "đủ số lần chạy hoàn tất ở cả hai bên để tính trung vị và độ trải",
        "enough samples that a share is meaningful rather than noise":
            "đủ số mẫu để một tỷ lệ có ý nghĩa thay vì chỉ là nhiễu",
        "exceeding the budget by this much or more is ranked high; it "
        + "orders impact, it is not a standard":
            "vượt ngân sách từ mức này trở lên được xếp mức cao; nó xếp thứ "
            + "tự ảnh hưởng, không phải một tiêu chuẩn",
        "for any claim about who unblocked a thread, a `sched_waking` "
        + "event emitted by the waker itself":
            "để khẳng định bất cứ điều gì về việc ai đã bỏ chặn một luồng, "
            + "cần một sự kiện `sched_waking` do chính luồng đánh thức phát "
            + "ra",
        "garbage that exists but has not been collected yet: without a "
        + "forced collection before each reading, the rise may be "
        + "entirely collectable":
            "rác đã tồn tại nhưng chưa được thu hồi: nếu không buộc thu gom "
            + "trước mỗi lần đọc, mức tăng có thể hoàn toàn là phần thu hồi "
            + "được",
        "high CPU without any user-visible harm is not a defect (spec E05)":
            "CPU cao mà không gây hại gì người dùng thấy được thì không "
            + "phải một lỗi (spec E05)",
        "impact ordering, not a standard":
            "chỉ để xếp thứ tự mức ảnh hưởng, không phải một tiêu chuẩn",
        "in-process dev tooling is genuinely part of the app process "
        + "total in a debug build; reporting it is about measurement "
        + "validity, not a product defect (spec F12)":
            "công cụ phát triển chạy trong tiến trình thực sự là một phần "
            + "tổng của tiến trình ứng dụng trong bản debug; báo về nó là "
            + "chuyện tính hợp lệ của phép đo, không phải một lỗi sản phẩm "
            + "(spec F12)",
        "initial default only, near one frame at 60 Hz; no platform "
        + "publishes a wait budget":
            "chỉ là mặc định ban đầu, xấp xỉ một frame ở 60 Hz; không nền "
            + "tảng nào công bố ngân sách cho việc chờ",
        "initial default only. Chosen to be near a frame at 120 Hz, not "
        + "because any platform publishes an I/O budget":
            "chỉ là mặc định ban đầu. Chọn xấp xỉ một frame ở 120 Hz, không "
            + "phải vì có nền tảng nào công bố ngân sách I/O",
        "initial default only. There is no correct number of renders: a "
        + "streaming list legitimately commits far more than this":
            "chỉ là mặc định ban đầu. Không có con số render nào là đúng: "
            + "một danh sách đang stream commit nhiều hơn thế một cách chính "
            + "đáng",
        "initial default only. There is no universal memory ceiling and "
        + "this is not one: it is the smallest per-cycle rise worth "
        + "reporting for a typical app":
            "chỉ là mặc định ban đầu. Không có ngưỡng bộ nhớ phổ quát nào "
            + "và đây cũng không phải một ngưỡng như vậy: nó là mức tăng mỗi "
            + "vòng nhỏ nhất đáng báo với một ứng dụng thông thường",
        "initial default only; a request faster than this is rarely "
        + "what a user notices":
            "chỉ là mặc định ban đầu; một request nhanh hơn mức này thì "
            + "người dùng ít khi để ý",
        "initial default only; tune per project":
            "chỉ là mặc định ban đầu; hãy điều chỉnh theo từng dự án",
        "initial default only; tune per project, it is not a platform "
        + "standard":
            "chỉ là mặc định ban đầu; hãy điều chỉnh theo từng dự án, đây "
            + "không phải tiêu chuẩn của nền tảng",
        "matching device, OS, refresh policy, collector preset, launch "
        + "class and input state":
            "trùng thiết bị, hệ điều hành, chính sách làm mới, preset "
            + "collector, loại khởi động và trạng thái nhập liệu",
        "network request markers with durations, reported by the app":
            "các marker request mạng kèm thời lượng, do ứng dụng báo về",
        "one destroyed-but-held instance is already the thing the "
        + "platform says should not be there, so the default does not "
        + "wait for a pattern":
            "một instance đã bị destroy mà vẫn bị giữ thì bản thân đã là "
            + "điều nền tảng nói rằng không nên có, nên mặc định không chờ "
            + "thành một mẫu hình",
        "presentation timestamps, or a clearly labelled "
        + "display-callback proxy":
            "dấu thời gian trình chiếu, hoặc một display-callback proxy "
            + "được ghi nhãn rõ ràng",
        "resolved symbols before any function name is attributed; "
        + "without them the hotspot is reported by raw frame only":
            "các ký hiệu đã được giải trước khi quy bất cứ tên hàm nào; "
            + "không có chúng thì điểm nóng chỉ được báo theo frame thô",
        "runnable-but-not-running on a busy emulator reflects the "
        + "host's scheduler as much as the app's own contention":
            "trạng thái sẵn sàng-nhưng-chưa-chạy trên một emulator đang bận "
            + "phản ánh scheduler của máy host ngang với tranh chấp của chính "
            + "ứng dụng",
        "sampled stacks for the target process":
            "các stack đã lấy mẫu của tiến trình mục tiêu",
        "sampled stacks, or events a collector explicitly attributed to "
        + "tooling":
            "các stack đã lấy mẫu, hoặc các sự kiện mà collector quy rõ "
            + "ràng cho công cụ",
        "scheduling intervals with a state and a duration, so a wait is "
        + "measured rather than inferred":
            "các khoảng lập lịch có trạng thái và thời lượng, để một lần "
            + "chờ được đo chứ không phải suy ra",
        "share above which the capture likely misrepresents the app's "
        + "own behavior":
            "tỷ lệ mà vượt qua nó thì lần ghi có khả năng phản ánh sai hành "
            + "vi của chính ứng dụng",
        "share of a thread's samples above which the diagnostic "
        + "configuration is worth reporting as a measurement problem":
            "tỷ lệ mẫu của một luồng mà vượt qua nó thì cấu hình chẩn đoán "
            + "đáng được báo như một vấn đề của phép đo",
        "startup bundle evaluation is expected to be long and is not by "
        + "itself a defect":
            "việc đánh giá bundle lúc khởi động vốn được cho là dài và bản "
            + "thân điều đó không phải một lỗi",
        "the blocked thread must be the UI main thread or the JS "
        + "thread; a background thread blocking on I/O is usually its job":
            "luồng bị chặn phải là luồng UI chính hoặc luồng JS; một luồng "
            + "chạy nền bị chặn do I/O thường là đúng việc của nó",
        "the cycles must be of the same screen in the same process, so "
        + "the readings are comparable":
            "các vòng phải thuộc cùng một màn hình trong cùng một tiến "
            + "trình, để các số đọc so sánh được với nhau",
        "the dump was taken while the object was on a finalizer or "
        + "reference queue, where it is legitimately held for one more "
        + "collection cycle":
            "bản dump được lấy khi đối tượng đang nằm trên finalizer hoặc "
            + "reference queue, nơi nó bị giữ một cách chính đáng thêm một "
            + "vòng thu gom",
        "the framework itself keeps the last destroyed Activity for a "
        + "configuration change or for its recents entry, which is by "
        + "design":
            "bản thân framework giữ lại Activity bị destroy gần nhất cho "
            + "một lần đổi cấu hình hoặc cho mục recents, và đó là theo thiết "
            + "kế",
        "the project's own startup budget; no default exists because no "
        + "platform publishes one, so the rule does not run until this is "
        + "configured":
            "ngân sách khởi động của chính dự án; không có mặc định vì "
            + "không nền tảng nào công bố, nên quy tắc không chạy cho tới khi "
            + "giá trị này được cấu hình",
        "the rate matters more than the total, since a long session "
        + "accumulates commits honestly":
            "tốc độ quan trọng hơn tổng số, vì một phiên dài thì tích luỹ "
            + "commit một cách chính đáng",
        "the request must cover this much of the interaction before the "
        + "interaction is said to have waited on it":
            "request phải chiếm ít nhất phần này của tương tác trước khi "
            + "nói rằng tương tác đã chờ nó",
        "the request must overlap the interaction; happening nearby is "
        + "not the same as being waited on":
            "request phải chồng lấn với tương tác; xảy ra gần đó không đồng "
            + "nghĩa với bị chờ",
        "the rise must also be this large relative to the first "
        + "reading, so a big app is not flagged for noise":
            "mức tăng cũng phải lớn chừng này so với số đọc đầu tiên, để "
            + "một ứng dụng lớn không bị gắn cờ vì nhiễu",
        "the uninterruptible interval that wait belongs to, so the "
        + "block has a duration and not just an instant":
            "khoảng không thể ngắt mà lần chờ đó thuộc về, để lần chặn có "
            + "một thời lượng chứ không chỉ là một thời điểm",
        "the waker is not necessarily the holder: a thread can be woken "
        + "by a timer, by unrelated I/O completing, or by whichever "
        + "thread signalled a shared condition":
            "luồng đánh thức không nhất thiết là luồng đang giữ khoá: một "
            + "luồng có thể bị đánh thức bởi timer, bởi một I/O không liên "
            + "quan hoàn tất, hoặc bởi luồng nào đã signal một condition dùng "
            + "chung",
    ]

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
        "why can't I use this?": "vì sao không dùng được?",
        "last seen": "lần cuối nhìn thấy",
        "no specific reason could be established":
            "không xác định được lý do cụ thể",
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
        "watch live": "xem trực tiếp",
        "stop watching": "dừng xem",
        "watching — act in the app and calls appear here":
            "đang xem — hãy thao tác trong ứng dụng và các lệnh gọi sẽ "
            + "hiện ra ở đây",
        "The observation ended early": "Phiên quan sát kết thúc sớm",
        "clear": "xoá lọc",
        "what to show, not what was captured":
            "hiển thị cái gì, không phải đã thu được cái gì",
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
