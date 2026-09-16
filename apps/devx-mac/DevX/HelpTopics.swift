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

/// One named thing on a screen -- a column, a counter, a status word.
///
/// "dường như tôi ko hiểu từng field trong mỗi màn hình". A screen-level
/// paragraph says what a screen is for and still leaves `pss_total`,
/// `cause: unknown` and `limited` unexplained, and those are the words
/// someone is actually stuck on. The name is spelled exactly as the screen
/// spells it, so it can be matched by eye.
struct HelpField: Identifiable {
    /// As it appears on screen, verbatim.
    let name: String
    let english: String
    let vietnamese: String

    var id: String { name }

    var text: String { Strings.active == .vi ? vietnamese : english }
}

struct HelpTopic: Identifiable {
    /// The tab this describes, or a section id for the non-tab topics.
    let id: String
    /// The screen's name as the sidebar shows it -- always English, because
    /// that is what is on screen.
    let title: String
    let english: String
    let vietnamese: String
    /// The named things on this screen. Empty for a topic that is an idea
    /// rather than a screen.
    var fields: [HelpField] = []

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
            vietnamese: "Toàn bộ thiết bị và máy ảo Android, iOS mà máy này thấy được ngay lúc này, chia hai cột để nhìn ra ngay mỗi nền tảng có gì. Dòng bị làm mờ là không đo được; bấm vào thì một lần probe sẽ nói rõ vì sao — thiếu cáp, chưa mở khoá, chưa trust, hay chưa bật Developer Mode. Danh sách rỗng là một khẳng định về máy, và việc liệt kê thất bại được báo khác với \"không có thiết bị nào\".",
            fields: [
                HelpField(
                    name: "authorized / offline / unknown",
                    english: "The trust state. authorized means it can be captured from. offline means it is paired but unreachable -- it is not absent and it is not usable. unknown means the probe could not settle the question, which is a third answer and not a synonym for offline.",
                    vietnamese: "Trạng thái tin cậy. authorized là đo được. offline là đã ghép nối nhưng không với tới được -- không phải là không có, cũng không phải dùng được. unknown là probe không kết luận được, đây là câu trả lời thứ ba chứ không đồng nghĩa với offline."),
                HelpField(
                    name: "physical / simulator / emulator",
                    english: "The form. A simulator or emulator runs on your Mac CPU, so its timings answer a different question and are never mixed into the same baseline as a physical device.",
                    vietnamese: "Dạng thiết bị. Simulator hay emulator chạy trên CPU của máy Mac, nên số đo của chúng trả lời một câu hỏi khác và không bao giờ bị trộn vào cùng baseline với thiết bị thật."),
                HelpField(
                    name: "device id",
                    english: "The stable identifier capture tools accept: an adb serial on Android, a UDID on iOS. An AVD name is not one -- the id only exists once the device is running.",
                    vietnamese: "Định danh ổn định mà các công cụ thu nhận: adb serial trên Android, UDID trên iOS. Tên AVD không phải là id -- id chỉ tồn tại khi thiết bị đã chạy."),
            ]),
        HelpTopic(
            id: "apps",
            title: "Apps",
            english: "The apps installed on the selected device, chosen by package name (Android) or bundle id (iOS). A PID is never required, and selecting an app does not bypass any platform restriction — trust prompts, Developer Mode and the debuggable flag still apply.",
            vietnamese: "Các app đã cài trên thiết bị đang chọn, chọn theo package name (Android) hoặc bundle id (iOS). Không bao giờ cần PID, và việc chọn app không bỏ qua được bất kỳ giới hạn nào của hệ điều hành — trust prompt, Developer Mode và cờ debuggable vẫn có hiệu lực."),
        HelpTopic(
            id: "preflight",
            title: "Preflight",
            english: "What can and cannot be measured for this device and app, probed rather than assumed. Each capability reports available, limited, unsupported or permission_denied, with the evidence behind that verdict, the prerequisite, and how to fix it. Run this before blaming a capture for coming back empty.",
            vietnamese: "Những gì đo được và không đo được với thiết bị và app này, bằng cách probe thật chứ không phỏng đoán. Mỗi capability báo available, limited, unsupported hoặc permission_denied, kèm bằng chứng cho kết luận đó, điều kiện cần, và cách xử lý. Nên xem màn này trước khi kết luận là phiên thu bị lỗi vì không có dữ liệu.",
            fields: [
                HelpField(
                    name: "available / limited / unsupported / permission_denied",
                    english: "What the probe found. limited means it works in a narrower way than the name suggests, and the limitation says how. permission_denied is a host or device setting you can change, and the recovery line says which. unsupported means it was tried and genuinely cannot work.",
                    vietnamese: "Kết quả probe. limited nghĩa là chạy được nhưng hẹp hơn cái tên gợi ra, và phần limitation nói rõ hẹp thế nào. permission_denied là một thiết lập của máy hoặc thiết bị mà bạn đổi được, và dòng recovery nói rõ cái nào. unsupported là đã thử và thật sự không chạy được."),
                HelpField(
                    name: "tested: verified_on_physical_device / verified_on_simulator_or_emulator / not_tested",
                    english: "How far the claim has been proven. Verified on a simulator is not verified on hardware, and the row says which one it is rather than letting both read as confirmed.",
                    vietnamese: "Khẳng định đó đã được chứng minh tới đâu. Xác nhận trên máy ảo không phải là xác nhận trên thiết bị thật, và dòng này nói rõ là loại nào chứ không để cả hai cùng đọc thành đã xác nhận."),
                HelpField(
                    name: "evidence / limitation / prerequisite / recovery",
                    english: "Why the verdict is what it is, what it does not cover, what would be needed, and what to do about it. All four come from the probe rather than from a table written by hand.",
                    vietnamese: "Vì sao kết luận là như vậy, nó chưa bao gồm những gì, cần gì thêm, và phải làm gì. Cả bốn đều đến từ probe chứ không phải từ một bảng viết tay."),
            ]),
        HelpTopic(
            id: "live",
            title: "Live",
            english: "A capture with the window left open: frames, CPU samples and memory counters arriving as they are measured, plus the per-source status and the findings recomputed on every tick. Everything here is marked preliminary until you stop, because a detector that has found nothing yet may still fire. The memory panel keeps each family separate and never sums two of them — rss_total and pss_total measure overlapping things in different ways.",
            vietnamese: "Một phiên thu với cửa sổ vẫn đang mở: frame, CPU sample và counter bộ nhớ hiện lên ngay khi đo được, kèm trạng thái từng nguồn và các phát hiện được tính lại mỗi tick. Mọi thứ ở đây đều được đánh dấu là sơ bộ cho tới khi bạn dừng, vì một detector chưa thấy gì vẫn có thể phát hiện sau đó. Panel bộ nhớ để riêng từng họ và không bao giờ cộng hai họ lại — rss_total và pss_total đo những phần chồng nhau theo hai cách khác nhau.",
            fields: [
                HelpField(
                    name: "frames",
                    english: "How many frames the device reported presenting during the capture. Counted, not sampled -- this is the real number of frames, from framestats.",
                    vietnamese: "Số frame mà thiết bị báo đã hiển thị trong phiên thu. Đây là đếm thật chứ không phải sample -- con số frame thực, lấy từ framestats."),
                HelpField(
                    name: "cpu samples",
                    english: "How many stack samples the profiler took. A sample is one look at what a thread was doing, not a measurement of how long anything ran, so a share of samples estimates where time went and never states a duration.",
                    vietnamese: "Số stack sample mà profiler đã lấy. Một sample là một lần nhìn xem thread đang làm gì, không phải phép đo thời gian chạy, nên tỉ lệ sample chỉ ước lượng thời gian rơi vào đâu chứ không khẳng định một thời lượng."),
                HelpField(
                    name: "counter points",
                    english: "How many individual counter readings arrived across every family -- memory, CPU time and the rest added together. A high number means the capture has been running a while, not that anything is wrong.",
                    vietnamese: "Tổng số điểm đo counter đã về, cộng hết mọi họ -- bộ nhớ, thời gian CPU và các thứ còn lại. Số lớn chỉ nghĩa là phiên thu đã chạy lâu, không phải có gì sai."),
                HelpField(
                    name: "threads",
                    english: "How many distinct threads appeared in the samples. Each keeps the name the runtime gave it, which is how the JavaScript-versus-native split on the Threads tab is a platform signal rather than a guess.",
                    vietnamese: "Số thread khác nhau đã xuất hiện trong các sample. Mỗi thread giữ đúng tên do runtime đặt, nhờ đó việc tách JavaScript và native ở tab Threads là tín hiệu từ nền tảng chứ không phải phỏng đoán."),
                HelpField(
                    name: "elapsed",
                    english: "Wall-clock time since the capture started. Not the same as CPU time: a process using one core fully for ten seconds shows ten seconds elapsed and about ten seconds of CPU time, while an idle one shows ten elapsed and almost none.",
                    vietnamese: "Thời gian thực tính từ lúc bắt đầu thu. Khác với thời gian CPU: một process dùng hết một core trong mười giây sẽ hiện elapsed mười giây và CPU khoảng mười giây, còn process rảnh thì elapsed mười giây nhưng CPU gần như bằng không."),
                HelpField(
                    name: "ticks",
                    english: "How many collection rounds have run. The tick is how often this tool asks the device for numbers; it does not change how the app behaves, but a shorter tick costs more overhead.",
                    vietnamese: "Số vòng thu thập đã chạy. Tick là nhịp mà công cụ này hỏi thiết bị lấy số; nó không làm app chạy khác đi, nhưng tick càng ngắn thì chi phí overhead càng cao."),
                HelpField(
                    name: "last tick",
                    english: "How long the most recent round took. This is the cost of the tool, not of the app. If it climbs toward the tick interval, the collector is struggling to keep up and the numbers start lagging.",
                    vietnamese: "Vòng thu gần nhất mất bao lâu. Đây là chi phí của chính công cụ, không phải của app. Nếu nó tăng dần tới gần bằng khoảng tick thì collector đang không theo kịp và các con số bắt đầu bị trễ."),
                HelpField(
                    name: "rss_total",
                    english: "Resident set size: every page of physical memory the process has mapped, including pages shared with other processes. The largest of the memory families and the one that most overstates what the app alone costs.",
                    vietnamese: "Resident set size: toàn bộ trang bộ nhớ vật lý mà process đang map, kể cả các trang dùng chung với process khác. Đây là họ bộ nhớ lớn nhất và cũng là con số phóng đại nhất so với chi phí riêng của app."),
                HelpField(
                    name: "pss_total",
                    english: "Proportional set size: like rss, but a shared page is divided by how many processes share it. The fairest single number for what this app costs, and the one to use when comparing two apps.",
                    vietnamese: "Proportional set size: giống rss, nhưng một trang dùng chung được chia cho số process đang dùng nó. Đây là con số đơn lẻ công bằng nhất cho câu hỏi app này tốn bao nhiêu, và là con số nên dùng khi so sánh hai app."),
                HelpField(
                    name: "private_dirty",
                    english: "Memory this process alone has modified, so it cannot be shared or dropped -- the kernel must keep it or swap it. The number that grows when the app leaks, and the one worth watching over a long capture.",
                    vietnamese: "Bộ nhớ mà chỉ process này đã ghi vào, nên không thể dùng chung hay bỏ đi -- kernel buộc phải giữ hoặc swap. Đây là con số tăng lên khi app bị leak, và là con số nên theo dõi trong một phiên thu dài."),
                HelpField(
                    name: "native_heap_rss",
                    english: "The C and C++ side of the heap: anything the runtime, a native module, or the image and graphics libraries allocated. On React Native this is where bitmaps and JSI allocations land.",
                    vietnamese: "Phần heap phía C và C++: mọi thứ do runtime, native module, hay thư viện ảnh và đồ hoạ cấp phát ra. Trên React Native thì bitmap và các cấp phát JSI nằm ở đây."),
                HelpField(
                    name: "dalvik_heap_rss",
                    english: "The managed Java and Kotlin heap on Android, the part the garbage collector owns. Saw-tooth movement here is normal -- it is collection happening -- while a rising floor between collections is not.",
                    vietnamese: "Heap Java và Kotlin có quản lý trên Android, phần do garbage collector nắm. Dao động hình răng cưa ở đây là bình thường -- đó là lúc GC chạy -- nhưng nếu mức đáy giữa các lần GC cứ cao dần thì không bình thường."),
                HelpField(
                    name: "process_time",
                    english: "`cpu.process_time_ns` in a report or an export. Total CPU time the process has consumed, in nanoseconds -- user plus system. It is a duration and not a size, and it can exceed elapsed time on more than one core. It is also cumulative since the process started, not since this capture began, so a capture of an app that has been running for hours opens at hours rather than at zero.",
                    vietnamese: "Trong report hay bản export thì nó là `cpu.process_time_ns`. Tổng thời gian CPU mà process đã dùng, tính bằng nanosecond -- user cộng system. Đây là một khoảng thời gian chứ không phải dung lượng, và nó có thể vượt quá elapsed nếu chạy trên nhiều core. Nó cũng được tính dồn từ lúc process bắt đầu chạy, không phải từ lúc bắt đầu phiên đo này, nên đo một app đã chạy nhiều giờ thì con số mở đầu đã là nhiều giờ chứ không phải số không."),
                HelpField(
                    name: "process_user_time",
                    english: "`cpu.process_user_time_ns` in a report. The part of that CPU time spent running the code of the app itself, as opposed to running kernel code on its behalf.",
                    vietnamese: "Trong report là `cpu.process_user_time_ns`. Phần thời gian CPU dùng để chạy code của chính app, khác với phần chạy code kernel thay cho nó."),
                HelpField(
                    name: "process_system_time",
                    english: "`cpu.process_system_time_ns` in a report. The part spent inside the kernel on behalf of this process -- file and network I/O, memory mapping, locks. A large share here points at syscalls rather than at computation.",
                    vietnamese: "Trong report là `cpu.process_system_time_ns`. Phần thời gian chạy trong kernel thay cho process này -- I/O file và mạng, map bộ nhớ, lock. Tỉ lệ ở đây lớn thì vấn đề nằm ở syscall chứ không phải ở tính toán."),
                HelpField(
                    name: "available / limited / unsupported",
                    english: "The state of a source. available means it is measuring. limited means it is measuring something narrower than the name suggests, and the note beside it says what. unsupported means it was tried and genuinely cannot work here -- a different answer from not tried.",
                    vietnamese: "Trạng thái của một nguồn. available là đang đo được. limited là đang đo một phần hẹp hơn so với cái tên gợi ra, và ghi chú bên cạnh nói rõ phần nào. unsupported là đã thử và thật sự không chạy được ở đây -- khác với chưa thử."),
                HelpField(
                    name: "PRELIMINARY",
                    english: "The capture window is still open. A detector that has found nothing yet may still fire, and a finding already shown may change or disappear as more evidence arrives. Nothing under this banner is a final result.",
                    vietnamese: "Cửa sổ thu vẫn đang mở. Một detector chưa thấy gì vẫn có thể phát hiện sau, và một phát hiện đang hiện có thể đổi hoặc biến mất khi có thêm bằng chứng. Không có gì dưới banner này là kết quả cuối cùng."),
            ]),
        HelpTopic(
            id: "record",
            title: "Record",
            english: "A fixed-length capture, analysed when the window closes, written to disk as a session package. The package carries the capture and its provenance together, so a report can be re-read later and still say which tool produced each number.",
            vietnamese: "Một phiên thu có độ dài định trước, phân tích khi cửa sổ đóng lại, ghi xuống đĩa thành một session package. Package mang theo cả dữ liệu thu và nguồn gốc của nó, nên sau này đọc lại vẫn biết được mỗi con số do công cụ nào sinh ra."),
        HelpTopic(
            id: "inspect",
            title: "Inspect",
            english: "A running React Native debug build read through the inspector it already runs — its network calls, console output and Redux store — with nothing installed in the app. Click a request to read its headers and body beside the list. This is a debugger session, not a measurement: attaching changes what the runtime does, so no timing here is a performance number.",
            vietnamese: "Đọc một bản debug React Native đang chạy thông qua inspector mà chính nó đã bật — các lời gọi network, console output và Redux store — không cần cài gì vào app. Bấm vào một request để xem header và body ngay bên cạnh danh sách. Đây là một phiên debugger, không phải phép đo: việc attach làm thay đổi cách runtime chạy, nên không con số thời gian nào ở đây là số đo hiệu năng.",
            fields: [
                HelpField(
                    name: "method and status",
                    english: "The HTTP method, and the response status when one arrived. A dash means no status was ever reported -- the request may still have been in flight when the window closed -- and is never shown as a zero.",
                    vietnamese: "Method HTTP, và status của response nếu đã có. Dấu gạch nghĩa là chưa từng có status nào được báo về -- request có thể vẫn đang chạy khi cửa sổ đóng lại -- và không bao giờ hiện thành số 0."),
                HelpField(
                    name: "duration",
                    english: "From the request being sent to the response finishing, as the runtime reported it. A debugger is attached while this is measured, so treat it as a rough shape rather than a timing you can quote.",
                    vietnamese: "Từ lúc request được gửi đến lúc response xong, theo báo cáo của runtime. Lúc đo có debugger đang attach, nên hãy xem đây là hình dáng tương đối chứ không phải con số thời gian dùng để dẫn chứng."),
                HelpField(
                    name: "bytes on the wire",
                    english: "The encoded size of the response body as the runtime counted it -- after compression, not the decoded length. A dash means the runtime never reported one.",
                    vietnamese: "Dung lượng đã mã hoá của body response theo cách runtime đếm -- sau khi nén, không phải độ dài sau giải nén. Dấu gạch nghĩa là runtime chưa từng báo con số nào."),
                HelpField(
                    name: "tree / text",
                    english: "Two views of the same body. tree folds and searches, and is parsed -- parsing turns 1.0 into 1 and drops a duplicate key. text is the bytes that arrived with only whitespace changed, so text is the one to trust when comparing against a server log.",
                    vietnamese: "Hai cách xem cùng một body. tree thu gọn và tìm được, nhưng đã qua parse -- parse làm 1.0 thành 1 và bỏ mất key trùng. text là đúng byte đã nhận và chỉ thay đổi khoảng trắng, nên khi đối chiếu với log của server thì hãy tin text."),
                HelpField(
                    name: "no action named",
                    english: "A Redux state change arrived with no action attached. Under read-only watching that is normal: Redux passes subscribers no action. While wrapping dispatch it means the change came through a reference the wrapper does not sit on, such as the dispatch a thunk is handed.",
                    vietnamese: "Một thay đổi state Redux về mà không kèm action. Ở chế độ chỉ theo dõi thì đó là bình thường: Redux không truyền action cho subscriber. Khi đang bọc dispatch thì nghĩa là thay đổi đi qua một tham chiếu mà wrapper không nằm trên đó, ví dụ dispatch mà thunk được đưa."),
                HelpField(
                    name: "no-op",
                    english: "A slice was replaced with a value equal to the old one. Subscribers were woken and nothing changed, which is the classic source of wasted re-renders in a Redux app and is invisible in a list of action names.",
                    vietnamese: "Một slice bị thay bằng giá trị y như cũ. Subscriber bị đánh thức mà không có gì đổi, đây là nguyên nhân kinh điển gây render lại vô ích trong app Redux và không thể thấy được nếu chỉ xem danh sách tên action."),
            ]),
        HelpTopic(
            id: "sessions",
            title: "Sessions",
            english: "Every capture stored on this machine. Opening one loads its report into Issues, Threads, Timeline and Compare.",
            vietnamese: "Toàn bộ các phiên thu đang lưu trên máy này. Mở một phiên sẽ nạp report của nó vào Issues, Threads, Timeline và Compare."),
        HelpTopic(
            id: "issues",
            title: "Issues",
            english: "What the detectors found, with the evidence behind each finding. Detection and cause are separate columns on purpose: \"observed\" says the thing was measured, and \"cause: unknown\" says nothing was established about why. The severity rationale, the threshold that fired and where that threshold came from are all the model's own fields, so this screen cannot present a softer story than the analysis.",
            vietnamese: "Những gì các detector tìm được, kèm bằng chứng cho từng phát hiện. Phát hiện và nguyên nhân là hai cột riêng có chủ đích: \"observed\" nghĩa là đã đo được thật, còn \"cause: unknown\" nghĩa là chưa xác định được vì sao. Lý do mức độ nghiêm trọng, ngưỡng đã bị vượt và nguồn gốc của ngưỡng đó đều là field của chính model, nên màn này không thể kể một câu chuyện nhẹ hơn so với phần phân tích.",
            fields: [
                HelpField(
                    name: "severity: high / medium / low",
                    english: "How much the measured evidence supports acting, not how annoying it feels. It comes from the measured value against the threshold and, for frames, from the longest consecutive run -- the rationale beside each finding says which.",
                    vietnamese: "Mức độ mà bằng chứng đo được ủng hộ việc phải xử lý, không phải mức độ gây khó chịu. Nó được tính từ giá trị đo so với ngưỡng, và với frame thì còn từ chuỗi liên tiếp dài nhất -- phần rationale cạnh mỗi phát hiện nói rõ cái nào."),
                HelpField(
                    name: "observed / suspected",
                    english: "The detection status. observed means the thing itself was measured. suspected means the evidence is indirect or came from a proxy source, so the finding may be real and the measurement is not direct.",
                    vietnamese: "Trạng thái phát hiện. observed nghĩa là chính hiện tượng đó đã được đo. suspected nghĩa là bằng chứng chỉ gián tiếp hoặc đến từ một nguồn thay thế, nên phát hiện có thể đúng nhưng phép đo thì không trực tiếp."),
                HelpField(
                    name: "cause: unknown / supported",
                    english: "Deliberately separate from detection. unknown means nothing was established about why -- the thing happened and this tool is not guessing at a reason. supported means evidence for the cause was found too, and it is listed.",
                    vietnamese: "Cố ý tách khỏi phần phát hiện. unknown nghĩa là chưa xác định được vì sao -- hiện tượng có xảy ra và công cụ này không đoán lý do. supported nghĩa là đã tìm được cả bằng chứng cho nguyên nhân, và nó được liệt kê ra."),
                HelpField(
                    name: "DET-01 … DET-12",
                    english: "Which detector fired. The Detectors tab lists every one with its thresholds and where each threshold came from -- a measured platform constant or a configurable heuristic.",
                    vietnamese: "Detector nào đã phát hiện. Tab Detectors liệt kê tất cả kèm ngưỡng và nguồn gốc từng ngưỡng -- là hằng số đo được của nền tảng hay heuristic có thể cấu hình."),
                HelpField(
                    name: "occurrences",
                    english: "How many times the thing was measured happening. The evidence list below it is capped at ten, so these two numbers are allowed to differ -- the list is a sample and the count is the total.",
                    vietnamese: "Số lần hiện tượng được đo là đã xảy ra. Danh sách bằng chứng bên dưới bị giới hạn ở mười, nên hai con số này có thể khác nhau -- danh sách là mẫu, còn con số là tổng."),
                HelpField(
                    name: "symbols: unavailable",
                    english: "The stack was captured but the function names could not be resolved, so a frame shows as an address or as unresolved. The finding is still real; only the name is missing.",
                    vietnamese: "Stack đã thu được nhưng không giải được tên hàm, nên một frame hiện ra dưới dạng địa chỉ hoặc unresolved. Phát hiện vẫn là thật; chỉ thiếu cái tên."),
                HelpField(
                    name: "fingerprint",
                    english: "A content-derived id for this finding. It stays the same across recomputations of the same finding, which is how a selection survives a list being rebuilt, and how the same issue can be recognised between two captures.",
                    vietnamese: "Một id sinh ra từ nội dung của phát hiện. Nó giữ nguyên qua các lần tính lại cùng một phát hiện, nhờ đó lựa chọn không bị nhảy khi danh sách được dựng lại, và cùng một vấn đề có thể nhận ra được giữa hai phiên thu."),
                HelpField(
                    name: "missing evidence",
                    english: "What would be needed to say more, named explicitly. This is the honest half of a finding: it says what was not measured rather than leaving the reader to assume the picture is complete.",
                    vietnamese: "Những gì còn cần để nói được nhiều hơn, được nêu rõ ràng. Đây là nửa trung thực của một phát hiện: nó nói phần nào chưa đo được, thay vì để người đọc tưởng rằng bức tranh đã đầy đủ."),
            ]),
        HelpTopic(
            id: "threads",
            title: "Threads",
            english: "Where the sampled work went, split between the JavaScript thread and the native ones. The thread names are the runtime's own, so the split is a platform signal rather than this tool's guess. A share is a share of the samples taken on that thread, not of a core and not of wall time.",
            vietnamese: "Phần việc đã được sample rơi vào đâu, tách giữa thread JavaScript và các thread native. Tên thread là tên do chính runtime đặt, nên việc tách này là tín hiệu từ nền tảng chứ không phải phỏng đoán của công cụ. Một tỉ lệ là tỉ lệ trên số sample lấy được của thread đó, không phải trên một core và cũng không phải trên thời gian thực.",
            fields: [
                HelpField(
                    name: "JS / native",
                    english: "Which side of the bridge the sampled work was on. The thread names come from the runtime itself, so this split is a platform signal rather than a guess by this tool.",
                    vietnamese: "Phần việc được sample nằm ở phía nào của bridge. Tên thread do chính runtime đặt, nên việc tách này là tín hiệu từ nền tảng chứ không phải phỏng đoán của công cụ."),
                HelpField(
                    name: "share",
                    english: "A share of the samples taken on that thread. Not a share of a CPU core and not a share of wall time: a thread that ran briefly and was sampled twice can show a high share of very little work.",
                    vietnamese: "Tỉ lệ trên số sample lấy được của thread đó. Không phải tỉ lệ của một core CPU và cũng không phải của thời gian thực: một thread chạy ngắn mà được sample hai lần vẫn có thể hiện tỉ lệ cao trên một lượng việc rất nhỏ."),
                HelpField(
                    name: "self time",
                    english: "Time attributed to a frame itself, with its children excluded. It answers where the work is, not which call path led there -- an inclusive share would double-count a stack.",
                    vietnamese: "Thời gian quy cho chính frame đó, không tính các frame con. Nó trả lời việc nằm ở đâu, không trả lời đường gọi nào dẫn tới đó -- nếu tính gộp cả con thì một stack sẽ bị đếm hai lần."),
            ]),
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
