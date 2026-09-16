// What each screen is, in the language the reader chose.
//
// Both translations are stored here rather than in Strings.swift, which is
// keyed by the English text. A catalog key that is a two-hundred-word
// paragraph is unworkable -- one comma edited on the English side silently
// orphans the translation -- so a topic carries its own pair and `text`
// picks by the active language, the same language the picker sets.
//
// The text says what a screen *shows* and, where it matters, what it does not
// claim. That second half is the part worth writing down: most of this tool's
// surface exists to keep "we did not measure it" apart from "it was zero",
// and a help screen that omitted that would be describing a different tool.
import Foundation

struct HelpTopic: Identifiable {
    /// The tab this describes, or a section id for the non-tab topics.
    let id: String
    /// The screen's name as the sidebar shows it -- always English, because
    /// that is what is on screen.
    let title: String
    let english: String
    let vietnamese: String

    /// The explanation in the language currently in use.
    ///
    /// Falls back to English for any language this app has no help text in,
    /// which is the same degradation `tr()` gives: correct English rather
    /// than a blank paragraph.
    var text: String {
        Strings.active == .vi ? vietnamese : english
    }
}

enum HelpTopics {
    /// One topic per sidebar tab, in sidebar order.
    static let screens: [HelpTopic] = [
        HelpTopic(
            id: "devices",
            title: "Devices",
            english: "Every Android and iOS device and simulator this machine can see right now, in two columns so you can tell at a glance what is on each platform. A dimmed row cannot be captured from; tap it and one probe says why — a cable, an unlock, a trust prompt, Developer Mode. An empty list is a claim about the machine, and a failed enumeration is reported differently from \"nothing is connected\".",
            vietnamese: "Toàn bộ thiết bị và máy ảo Android, iOS mà máy này thấy được ngay lúc này, chia hai cột để nhìn ra ngay mỗi nền tảng có gì. Dòng bị làm mờ là không đo được; bấm vào thì một lần probe sẽ nói rõ vì sao — thiếu cáp, chưa mở khoá, chưa trust, hay chưa bật Developer Mode. Danh sách rỗng là một khẳng định về máy, và việc liệt kê thất bại được báo khác với \"không có thiết bị nào\"."),
        HelpTopic(
            id: "apps",
            title: "Apps",
            english: "The apps installed on the selected device, chosen by package name (Android) or bundle id (iOS). A PID is never required, and selecting an app does not bypass any platform restriction — trust prompts, Developer Mode and the debuggable flag still apply.",
            vietnamese: "Các app đã cài trên thiết bị đang chọn, chọn theo package name (Android) hoặc bundle id (iOS). Không bao giờ cần PID, và việc chọn app không bỏ qua được bất kỳ giới hạn nào của hệ điều hành — trust prompt, Developer Mode và cờ debuggable vẫn có hiệu lực."),
        HelpTopic(
            id: "preflight",
            title: "Preflight",
            english: "What can and cannot be measured for this device and app, probed rather than assumed. Each capability reports available, limited, unsupported or permission_denied, with the evidence behind that verdict, the prerequisite, and how to fix it. Run this before blaming a capture for coming back empty.",
            vietnamese: "Những gì đo được và không đo được với thiết bị và app này, bằng cách probe thật chứ không phỏng đoán. Mỗi capability báo available, limited, unsupported hoặc permission_denied, kèm bằng chứng cho kết luận đó, điều kiện cần, và cách xử lý. Nên xem màn này trước khi kết luận là phiên thu bị lỗi vì không có dữ liệu."),
        HelpTopic(
            id: "live",
            title: "Live",
            english: "A capture with the window left open: frames, CPU samples and memory counters arriving as they are measured, plus the per-source status and the findings recomputed on every tick. Everything here is marked preliminary until you stop, because a detector that has found nothing yet may still fire. The memory panel keeps each family separate and never sums two of them — rss_total and pss_total measure overlapping things in different ways.",
            vietnamese: "Một phiên thu với cửa sổ vẫn đang mở: frame, CPU sample và counter bộ nhớ hiện lên ngay khi đo được, kèm trạng thái từng nguồn và các phát hiện được tính lại mỗi tick. Mọi thứ ở đây đều được đánh dấu là sơ bộ cho tới khi bạn dừng, vì một detector chưa thấy gì vẫn có thể phát hiện sau đó. Panel bộ nhớ để riêng từng họ và không bao giờ cộng hai họ lại — rss_total và pss_total đo những phần chồng nhau theo hai cách khác nhau."),
        HelpTopic(
            id: "record",
            title: "Record",
            english: "A fixed-length capture, analysed when the window closes, written to disk as a session package. The package carries the capture and its provenance together, so a report can be re-read later and still say which tool produced each number.",
            vietnamese: "Một phiên thu có độ dài định trước, phân tích khi cửa sổ đóng lại, ghi xuống đĩa thành một session package. Package mang theo cả dữ liệu thu và nguồn gốc của nó, nên sau này đọc lại vẫn biết được mỗi con số do công cụ nào sinh ra."),
        HelpTopic(
            id: "inspect",
            title: "Inspect",
            english: "A running React Native debug build read through the inspector it already runs — its network calls, console output and Redux store — with nothing installed in the app. Click a request to read its headers and body beside the list. This is a debugger session, not a measurement: attaching changes what the runtime does, so no timing here is a performance number.",
            vietnamese: "Đọc một bản debug React Native đang chạy thông qua inspector mà chính nó đã bật — các lời gọi network, console output và Redux store — không cần cài gì vào app. Bấm vào một request để xem header và body ngay bên cạnh danh sách. Đây là một phiên debugger, không phải phép đo: việc attach làm thay đổi cách runtime chạy, nên không con số thời gian nào ở đây là số đo hiệu năng."),
        HelpTopic(
            id: "sessions",
            title: "Sessions",
            english: "Every capture stored on this machine. Opening one loads its report into Issues, Threads, Timeline and Compare.",
            vietnamese: "Toàn bộ các phiên thu đang lưu trên máy này. Mở một phiên sẽ nạp report của nó vào Issues, Threads, Timeline và Compare."),
        HelpTopic(
            id: "issues",
            title: "Issues",
            english: "What the detectors found, with the evidence behind each finding. Detection and cause are separate columns on purpose: \"observed\" says the thing was measured, and \"cause: unknown\" says nothing was established about why. The severity rationale, the threshold that fired and where that threshold came from are all the model's own fields, so this screen cannot present a softer story than the analysis.",
            vietnamese: "Những gì các detector tìm được, kèm bằng chứng cho từng phát hiện. Phát hiện và nguyên nhân là hai cột riêng có chủ đích: \"observed\" nghĩa là đã đo được thật, còn \"cause: unknown\" nghĩa là chưa xác định được vì sao. Lý do mức độ nghiêm trọng, ngưỡng đã bị vượt và nguồn gốc của ngưỡng đó đều là field của chính model, nên màn này không thể kể một câu chuyện nhẹ hơn so với phần phân tích."),
        HelpTopic(
            id: "threads",
            title: "Threads",
            english: "Where the sampled work went, split between the JavaScript thread and the native ones. The thread names are the runtime's own, so the split is a platform signal rather than this tool's guess. A share is a share of the samples taken on that thread, not of a core and not of wall time.",
            vietnamese: "Phần việc đã được sample rơi vào đâu, tách giữa thread JavaScript và các thread native. Tên thread là tên do chính runtime đặt, nên việc tách này là tín hiệu từ nền tảng chứ không phải phỏng đoán của công cụ. Một tỉ lệ là tỉ lệ trên số sample lấy được của thread đó, không phải trên một core và cũng không phải trên thời gian thực."),
        HelpTopic(
            id: "timeline",
            title: "Timeline",
            english: "The capture's tracks over time, binned. A bin carries a state as well as a number, and a blank column means NOT MEASURED — never zero. That distinction is the whole reason this view exists: an idle app and an interval nothing looked at produce the same number and are not the same fact.",
            vietnamese: "Các track của phiên thu theo thời gian, chia thành từng bin. Mỗi bin mang cả trạng thái chứ không chỉ một con số, và cột trống nghĩa là KHÔNG ĐO ĐƯỢC — không phải bằng không. Chính sự phân biệt đó là lý do màn này tồn tại: một app đang rảnh và một khoảng thời gian không ai đo cho ra cùng một con số nhưng là hai sự thật khác nhau."),
        HelpTopic(
            id: "compare",
            title: "Compare",
            english: "Two run sets against each other, with the gate status stated before any verdict. Too few runs, or too much variance between them, is reported as inconclusive rather than as a regression — a difference that the noise could explain is not a finding.",
            vietnamese: "So sánh hai bộ run với nhau, và trạng thái gate được nêu trước mọi kết luận. Quá ít lần chạy, hoặc độ biến thiên giữa các lần quá lớn, sẽ được báo là không kết luận được chứ không báo là hồi quy — một khác biệt mà nhiễu có thể giải thích thì không phải là một phát hiện."),
        HelpTopic(
            id: "detectors",
            title: "Detectors",
            english: "Every detector, its thresholds, and where each threshold came from — a measured platform constant or a configurable heuristic. Read this before arguing about whether a finding is real. It also shows which detectors could not run, and why, which is a different answer from finding nothing.",
            vietnamese: "Toàn bộ detector, các ngưỡng của chúng, và nguồn gốc từng ngưỡng — là hằng số đo được của nền tảng hay là heuristic có thể cấu hình. Nên đọc màn này trước khi tranh luận một phát hiện có thật hay không. Nó cũng cho biết detector nào không chạy được và vì sao, đó là câu trả lời khác với việc chạy mà không tìm thấy gì."),
        HelpTopic(
            id: "settings",
            title: "Export",
            english: "Export a report as JSON or Markdown, choose the interface language, see where sessions are stored, and read what this tool does and does not send anywhere. Nothing leaves this machine: the only network this app opens is loopback.",
            vietnamese: "Xuất report ra JSON hoặc Markdown, chọn ngôn ngữ giao diện, xem nơi lưu các phiên thu, và đọc phần công cụ này có gửi gì đi đâu hay không. Không có gì rời khỏi máy này: kết nối mạng duy nhất app mở là loopback."),
    ]

    /// Ideas that are not a screen but are the reason several screens look the
    /// way they do. Worth stating once, where someone is already reading.
    static let concepts: [HelpTopic] = [
        HelpTopic(
            id: "not-measured",
            title: "\"Not measured\" is not zero",
            english: "The single rule the whole tool is built on. A value nobody measured is absent or marked, never defaulted to zero, because a zero is a measurement and reads as one. A blank timeline bin, a dash instead of a status, an empty list with a banner over it — each is this rule showing through.",
            vietnamese: "Quy tắc duy nhất mà cả công cụ này được xây trên đó. Một giá trị không ai đo thì để trống hoặc đánh dấu rõ, không bao giờ mặc định về 0, vì 0 là một phép đo và người đọc sẽ hiểu nó như vậy. Một bin trống trên timeline, một dấu gạch thay cho trạng thái, một danh sách rỗng kèm banner giải thích — tất cả đều là quy tắc này thể hiện ra."),
        HelpTopic(
            id: "simulators",
            title: "A simulator is not a device",
            english: "Simulators and emulators are listed separately and their timings are never mixed into the same baseline as a physical device's. They run on your Mac's CPU, so their numbers answer a different question — useful for finding a hotspot, useless for saying how fast the app is on a phone.",
            vietnamese: "Máy ảo được liệt kê riêng và số đo của chúng không bao giờ bị trộn vào cùng một baseline với thiết bị thật. Chúng chạy trên CPU của máy Mac, nên các con số trả lời một câu hỏi khác — hữu ích để tìm điểm nóng, vô dụng để nói app chạy nhanh chậm thế nào trên điện thoại."),
        HelpTopic(
            id: "debugger",
            title: "Attaching a debugger changes the app",
            english: "The Inspect tab reads a live app through a debugger connection. That changes what the runtime does — Hermes may deoptimise — so everything it reports is an observation of behaviour and nothing it reports is a timing you can quote.",
            vietnamese: "Tab Inspect đọc app đang chạy qua một kết nối debugger. Việc đó làm thay đổi cách runtime hoạt động — Hermes có thể bỏ tối ưu — nên mọi thứ nó báo về là quan sát hành vi, và không con số thời gian nào ở đó dùng để dẫn chứng hiệu năng được."),
    ]

    /// Every topic, for the checks that say the catalogue is complete.
    static var all: [HelpTopic] { screens + concepts }
}
