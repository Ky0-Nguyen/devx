// The layout snapshot: both parsers, the analysis, and the probe's socket
// client.
//
// The two fixtures under fixtures/layout are real: the probe's answer from
// Expo Go on an iOS 26 simulator, and `dumpsys activity top` from Settings on
// an API 33 emulator. The React Native tree below is synthetic and says so:
// no React Native app was available to capture, and the rules it exercises
// (screens mounted against showing, wrappers, depth) are about class names
// and shape, which a constructed tree pins as well as a recorded one.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include "adapters/ios/view_probe_client.hpp"
#include "core/observe/layout.hpp"
#include "tests/unit/test_framework.hpp"

namespace {

using mpi::observe::LayoutReport;
using mpi::observe::LayoutSnapshot;

std::string fixture(const std::string& name) {
  const char* dir = std::getenv("MPI_FIXTURE_DIR");
  std::ifstream in((dir != nullptr ? std::string(dir) : "fixtures") + "/layout/" + name);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::optional<LayoutSnapshot> probe_doc(const std::string& text) {
  mpi::json::ParseError err;
  const auto doc = mpi::json::parse(text, mpi::observe::probe_json_limits(), &err);
  if (!doc) return std::nullopt;
  std::string e;
  return mpi::observe::parse_ios_probe(*doc, &e);
}

bool has_observation(const LayoutReport& r, const std::string& code) {
  for (const auto& o : r.observations) {
    if (o.code == code) return true;
  }
  return false;
}

// A view node for the synthetic tree.
std::string view(const std::string& cls, double x, double y, double w, double h,
                 const std::string& extra, const std::string& children) {
  std::ostringstream o;
  o << "{\"class\":\"" << cls << "\",\"frame\":[" << x << "," << y << "," << w
    << "," << h << "]" << extra;
  if (!children.empty()) o << ",\"children\":[" << children << "]";
  o << "}";
  return o.str();
}

std::string chain(int levels) {
  std::string inner = view("RCTText", 0, 0, 100, 20, "", "");
  for (int i = 0; i < levels; i++) inner = view("RCTView", 0, 0, 100, 20, "", inner);
  return inner;
}

/// A React Native app on the old architecture: one root controller hosting an
/// RCTRootView, a react-native-screens stack holding eight screens of which
/// the top one shows, and on it a 35-deep chain of single-child Views.
std::string synthetic_rn_probe() {
  std::string screens;
  for (int i = 0; i < 8; i++) {
    if (i > 0) screens += ",";
    const bool top = i == 7;
    screens += view("RNSScreenView", 0, 0, 400, 800, top ? "" : ",\"hidden\":true",
                    top ? chain(35) : view("RCTView", 0, 0, 400, 800, "", ""));
  }
  const std::string root_view = view(
      "RCTRootView", 0, 0, 400, 800,
      ",\"controller\":\"UIViewController\",\"controller_id\":\"0x1\"",
      view("RNSScreenStackView", 0, 0, 400, 800, "", screens));
  return std::string("{\"probe_version\":1,\"bundle_id\":\"com.example.rn\",") +
         "\"pid\":42,\"screen\":{\"width\":400,\"height\":800,\"scale\":3}," +
         "\"truncated\":false,\"max_views\":60000,\"windows\":[{" +
         "\"class\":\"UIWindow\",\"key\":true,\"hidden\":false,\"frame\":[0,0,400,800]," +
         "\"root_controller\":{\"id\":\"0x1\",\"class\":\"UIViewController\"," +
         "\"kind\":\"content\",\"view_loaded\":true,\"visible\":true}," +
         "\"root\":" + view("UIWindow", 0, 0, 400, 800, "", root_view) + "}]}";
}

}  // namespace

MPI_TEST(a_real_probe_answer_parses_into_windows_views_and_controllers, {}) {
  const auto s = probe_doc(fixture("ios-probe-expo-go-home.real.json"));
  MPI_CHECK_MSG(s.has_value(), "the recorded probe answer must parse");
  MPI_CHECK_EQ(s->app_identifier, std::string("host.exp.Exponent"));
  MPI_CHECK_EQ(s->windows.size(), static_cast<std::size_t>(3));
  MPI_CHECK_EQ(s->views.size(), static_cast<std::size_t>(87));
  MPI_CHECK(!s->truncated);
  MPI_CHECK(!s->controllers.empty());
  // Preorder: every view's parent comes before it, so subtrees are runs.
  for (std::size_t i = 0; i < s->views.size(); i++) {
    MPI_CHECK(s->views[i].parent < static_cast<int>(i));
  }
}

MPI_TEST(the_screen_is_the_innermost_visible_controller_not_a_container, {}) {
  auto s = probe_doc(fixture("ios-probe-expo-go-home.real.json"));
  MPI_CHECK(s.has_value());
  const auto r = mpi::observe::analyze_layout(std::move(*s));
  // Expo Go's home: a SwiftUI hosting controller at the top of a navigation
  // controller inside a tab bar inside two content controllers. Only the
  // innermost is the screen; the keyboard's text-effects window is UIKit's,
  // not the app's, and contributes none.
  MPI_CHECK_EQ(r.screens.size(), static_cast<std::size_t>(1));
  MPI_CHECK_EQ(r.screens[0].basis, std::string("view_controller"));
  MPI_CHECK_MSG(r.screens[0].name.find("UIHostingController") != std::string::npos,
                "the SwiftUI hosting controller is the screen: " + r.screens[0].name);
  MPI_CHECK_EQ(r.screens[0].views, static_cast<std::int64_t>(24));
  MPI_CHECK_EQ(r.navigation.size(), static_cast<std::size_t>(1));
  MPI_CHECK_EQ(r.navigation[0].screens.size(), static_cast<std::size_t>(1));
  MPI_CHECK(!r.react_native.detected);
  MPI_CHECK(r.observations.empty());
}

MPI_TEST(swift_names_are_offered_for_demangling_and_applied, {}) {
  auto s = probe_doc(fixture("ios-probe-expo-go-home.real.json"));
  MPI_CHECK(s.has_value());
  const auto mangled = mpi::observe::mangled_class_names(*s);
  MPI_CHECK(!mangled.empty());
  for (const auto& m : mangled) MPI_CHECK(m.rfind("_Tt", 0) == 0 || m.rfind("$s", 0) == 0);
  std::map<std::string, std::string> names;
  for (const auto& m : mangled) names[m] = "Demangled";
  mpi::observe::apply_display_names(*s, names);
  bool applied = false;
  for (const auto& v : s->views) {
    if (v.cls.rfind("_Tt", 0) == 0) applied = v.display == "Demangled";
    if (v.cls == "UIView") MPI_CHECK_EQ(v.display, std::string("UIView"));
  }
  MPI_CHECK(applied);
}

MPI_TEST(a_react_native_stack_counts_screens_mounted_against_showing, {}) {
  auto s = probe_doc(synthetic_rn_probe());
  MPI_CHECK_MSG(s.has_value(), "the synthetic probe document must parse");
  const auto r = mpi::observe::analyze_layout(std::move(*s));
  MPI_CHECK(r.react_native.detected);
  MPI_CHECK_EQ(r.react_native.architecture, std::string("paper"));
  MPI_CHECK_EQ(r.react_native.screens_mounted, static_cast<std::int64_t>(8));
  MPI_CHECK_EQ(r.react_native.screens_on_screen, static_cast<std::int64_t>(1));
  MPI_CHECK_MSG(has_observation(r, "many_mounted_screens"),
                "seven hidden screens is past the threshold of six");
  // The controller screen, then one screen per RNSScreenView, each saying
  // which screen it sits inside.
  MPI_CHECK_EQ(r.screens.size(), static_cast<std::size_t>(9));
  MPI_CHECK_EQ(r.screens[0].basis, std::string("view_controller"));
  int showing = -1;
  for (std::size_t i = 1; i < r.screens.size(); i++) {
    MPI_CHECK_EQ(r.screens[i].basis, std::string("react_native_screen"));
    MPI_CHECK_EQ(r.screens[i].inside, 0);
    if (r.screens[i].on_screen) showing = static_cast<int>(i);
  }
  MPI_CHECK_EQ(showing, 8);
  // The showing screen: its own view, 35 wrappers and the text at the end.
  const auto& top = r.screens[8];
  MPI_CHECK_EQ(top.views, static_cast<std::int64_t>(37));
  MPI_CHECK_EQ(top.depth, 36);
  MPI_CHECK_MSG(top.single_child_wrappers >= 35,
                "a View whose only child covers it exactly is a wrapper");
  MPI_CHECK_MSG(has_observation(r, "deep_screen"), "36 levels is past 30");
  // A hidden screen's views are hidden too, through their parent.
  MPI_CHECK_EQ(r.screens[1].hidden_views, r.screens[1].views);
}

MPI_TEST(hidden_offscreen_and_zero_size_are_derived_not_trusted, {}) {
  LayoutSnapshot s;
  s.platform = mpi::model::Platform::kIos;
  s.screen_w = 400;
  s.screen_h = 800;
  mpi::observe::LayoutWindow w;
  w.cls = "UIWindow";
  w.w = 400;
  w.h = 800;
  w.root_view = 0;
  s.windows.push_back(w);
  auto add = [&](int parent, double x, double y, double ww, double hh, bool hidden,
                 double alpha) {
    mpi::observe::LayoutView v;
    v.cls = "UIView";
    v.display = "UIView";
    v.parent = parent;
    v.depth = parent < 0 ? 0 : s.views[static_cast<std::size_t>(parent)].depth + 1;
    v.x = x; v.y = y; v.w = ww; v.h = hh;
    v.hidden = hidden;
    v.alpha = alpha;
    s.views.push_back(v);
  };
  add(-1, 0, 0, 400, 800, false, 1);   // 0 window root
  add(0, 0, 0, 100, 100, true, 1);     // 1 hidden
  add(1, 0, 0, 50, 50, false, 1);      // 2 inside a hidden parent
  add(0, 500, 0, 100, 100, false, 1);  // 3 right of the window
  add(0, 0, 0, 0, 100, false, 1);      // 4 zero width
  add(0, 0, 0, 100, 100, false, 0);    // 5 fully transparent
  mpi::observe::finalize(s);
  MPI_CHECK(s.views[1].effectively_hidden);
  MPI_CHECK_MSG(s.views[2].effectively_hidden, "a hidden parent hides its children");
  MPI_CHECK(s.views[3].offscreen && !s.views[3].effectively_hidden);
  MPI_CHECK(s.views[4].zero_size && !s.views[4].offscreen);
  MPI_CHECK(s.views[5].effectively_hidden);
  MPI_CHECK_EQ(s.views[0].children, 4);
  const auto r = mpi::observe::analyze_layout(s);
  MPI_CHECK_EQ(r.visible_views, static_cast<std::int64_t>(1));
  MPI_CHECK_EQ(r.hidden_views, static_cast<std::int64_t>(3));
  MPI_CHECK_EQ(r.offscreen_views, static_cast<std::int64_t>(1));
}

MPI_TEST(a_cut_off_tree_is_reported_as_a_lower_bound, {}) {
  std::string doc = synthetic_rn_probe();
  const auto at = doc.find("\"truncated\":false");
  doc.replace(at, 17, "\"truncated\":true");
  auto s = probe_doc(doc);
  MPI_CHECK(s.has_value());
  const auto r = mpi::observe::analyze_layout(std::move(*s));
  MPI_CHECK(has_observation(r, "tree_truncated"));
  MPI_CHECK(r.to_json(false).find("totals")->find("truncated")->as_bool());
}

MPI_TEST(a_document_without_windows_is_refused, {}) {
  mpi::json::ParseError perr;
  const auto doc = mpi::json::parse("{\"probe_version\":1}", &perr);
  std::string err;
  MPI_CHECK(!mpi::observe::parse_ios_probe(*doc, &err).has_value());
  MPI_CHECK(!err.empty());
}

MPI_TEST(real_dumpsys_output_yields_the_packages_activity_only, {}) {
  const std::string text = fixture("android-dumpsys-activity-top-settings.real.txt");
  std::string err;
  auto s = mpi::observe::parse_android_dumpsys(text, "com.android.settings", &err);
  MPI_CHECK_MSG(s.has_value(), "Settings is on screen in the recording: " + err);
  MPI_CHECK_EQ(s->activity, std::string("com.android.settings.Settings"));
  MPI_CHECK_EQ(s->pid, static_cast<std::int64_t>(705));
  MPI_CHECK_EQ(s->screen_w, 1080.0);
  MPI_CHECK_EQ(s->screen_h, 2400.0);
  MPI_CHECK_EQ(s->views.size(), static_cast<std::size_t>(174));
  MPI_CHECK_EQ(s->views[0].cls, std::string("DecorView"));
  bool top_level = false, report = false;
  for (const auto& f : s->fragments) {
    if (f == "TopLevelSettings") top_level = true;
    if (f == "ReportFragment") report = true;
  }
  MPI_CHECK(top_level);
  MPI_CHECK_MSG(!report, "androidx's headless lifecycle fragment is no screen");
  const auto r = mpi::observe::analyze_layout(std::move(*s));
  MPI_CHECK_EQ(r.screens.size(), static_cast<std::size_t>(1));
  MPI_CHECK_EQ(r.screens[0].basis, std::string("activity"));
  MPI_CHECK_EQ(r.max_depth, 12);
  // The launcher's activity is in the same output and is a different block.
  auto launcher = mpi::observe::parse_android_dumpsys(
      text, "com.google.android.apps.nexuslauncher", &err);
  MPI_CHECK(launcher.has_value());
  MPI_CHECK(launcher->views.size() != static_cast<std::size_t>(174));
}

MPI_TEST(an_app_not_on_screen_is_named_as_that, {}) {
  std::string err;
  const auto s = mpi::observe::parse_android_dumpsys(
      fixture("android-dumpsys-activity-top-settings.real.txt"), "com.example.absent",
      &err);
  MPI_CHECK(!s.has_value());
  MPI_CHECK_MSG(err.find("not on screen") != std::string::npos, err);
}

MPI_TEST(dumpsys_bounds_are_relative_and_may_be_negative, {}) {
  const std::string text =
      "TASK 1:com.example id=1 userId=0\n"
      "  ACTIVITY com.example/.Main abc pid=9\n"
      "      mCurrentConfig={winConfig={ mBounds=Rect(0, 0 - 1080, 2400) }}\n"
      "    View Hierarchy:\n"
      "      DecorView@1[Main]\n"
      "        android.widget.FrameLayout{1 V.E...... ........ 0,100-1080,2400 #1020002 android:id/content}\n"
      "          com.swmansion.rnscreens.Screen{2 V.E...... ........ -1080,0-0,2300}\n"
      "          com.swmansion.rnscreens.Screen{3 V.E...... ........ 0,0-1080,2300}\n"
      "            com.facebook.react.views.view.ReactViewGroup{4 G.E...... ........ 10,10-20,20}\n"
      "    Looper (main, tid 1) {1}\n";
  std::string err;
  auto s = mpi::observe::parse_android_dumpsys(text, "com.example", &err);
  MPI_CHECK_MSG(s.has_value(), err);
  MPI_CHECK_EQ(s->activity, std::string("com.example.Main"));
  MPI_CHECK_EQ(s->views.size(), static_cast<std::size_t>(5));
  MPI_CHECK_EQ(s->views[1].identifier, std::string("android:id/content"));
  MPI_CHECK_EQ(s->views[2].x, -1080.0);
  MPI_CHECK_EQ(s->views[2].y, 100.0);
  MPI_CHECK_EQ(s->views[4].x, 10.0);
  MPI_CHECK_EQ(s->views[4].y, 110.0);
  MPI_CHECK(s->views[4].hidden);
  const auto r = mpi::observe::analyze_layout(std::move(*s));
  MPI_CHECK(r.react_native.detected);
  MPI_CHECK_EQ(r.react_native.screens_mounted, static_cast<std::int64_t>(2));
  MPI_CHECK_MSG(r.react_native.screens_on_screen == 1,
                "the screen left of the window is off screen");
}

MPI_TEST(without_the_screen_size_nothing_is_called_off_screen, {}) {
  // No mBounds line: the window's size is unknown, so a view cannot be said
  // to lie outside it, and none is.
  const std::string text =
      "  ACTIVITY com.example/.Main abc pid=9\n"
      "    View Hierarchy:\n"
      "      DecorView@1[Main]\n"
      "        android.widget.FrameLayout{1 V.E...... ........ -5000,0-0,10}\n";
  std::string err;
  auto s = mpi::observe::parse_android_dumpsys(text, "com.example", &err);
  MPI_CHECK_MSG(s.has_value(), err);
  for (const auto& v : s->views) MPI_CHECK(!v.offscreen);
}

MPI_TEST(the_probe_socket_is_per_app_per_simulator_and_fits_a_unix_path, {}) {
  const auto a = mpi::ios::probe_socket_path("UDID-1", "com.a");
  MPI_CHECK_EQ(a, mpi::ios::probe_socket_path("UDID-1", "com.a"));
  MPI_CHECK(a != mpi::ios::probe_socket_path("UDID-1", "com.b"));
  MPI_CHECK(a != mpi::ios::probe_socket_path("UDID-2", "com.a"));
  MPI_CHECK(a.size() < 104);
}

namespace {

/// A one-shot server standing in for the probe: accepts once, then either
/// answers with `body` or says nothing until `hold_ms` has passed.
std::string serve_once(const std::string& path, std::string body, int hold_ms,
                       std::thread& t) {
  ::unlink(path.c_str());
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
  ::listen(fd, 1);
  t = std::thread([fd, body = std::move(body), hold_ms] {
    const int c = ::accept(fd, nullptr, nullptr);
    if (hold_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    if (!body.empty()) {
      const ssize_t n = ::write(c, body.data(), body.size());
      static_cast<void>(n);
    }
    ::close(c);
    ::close(fd);
  });
  return path;
}

std::string temp_socket(const char* tag) {
  return "/tmp/mpi-test-" + std::string(tag) + "-" + std::to_string(::getpid()) + ".sock";
}

}  // namespace

MPI_TEST(the_client_reads_a_whole_answer, {}) {
  std::thread t;
  const std::string body(200000, 'x');
  const auto path = serve_once(temp_socket("ok"), body, 0, t);
  mpi::CancellationSource cancel;
  const auto f = mpi::ios::fetch_probe_snapshot(path, std::chrono::milliseconds(3000),
                                                false, cancel.token());
  t.join();
  ::unlink(path.c_str());
  MPI_CHECK_EQ(std::string(mpi::ios::to_string(f.answer)), std::string("answered"));
  MPI_CHECK_EQ(f.body.size(), body.size());
}

MPI_TEST(nothing_listening_is_not_loaded_not_an_empty_screen, {}) {
  mpi::CancellationSource cancel;
  const auto f = mpi::ios::fetch_probe_snapshot(temp_socket("absent"),
                                                std::chrono::milliseconds(500), false,
                                                cancel.token());
  MPI_CHECK_EQ(std::string(mpi::ios::to_string(f.answer)), std::string("not_loaded"));
  MPI_CHECK(f.body.empty());
}

MPI_TEST(a_probe_that_never_finishes_answering_times_out_with_no_partial_tree, {}) {
  std::thread t;
  const auto path = serve_once(temp_socket("slow"), "", 1500, t);
  mpi::CancellationSource cancel;
  const auto began = std::chrono::steady_clock::now();
  const auto f = mpi::ios::fetch_probe_snapshot(path, std::chrono::milliseconds(400),
                                                false, cancel.token());
  const auto took = std::chrono::steady_clock::now() - began;
  t.join();
  ::unlink(path.c_str());
  MPI_CHECK_EQ(std::string(mpi::ios::to_string(f.answer)), std::string("timed_out"));
  MPI_CHECK(f.body.empty());
  MPI_CHECK(took < std::chrono::milliseconds(1200));
}

// ---- observations on disk ---------------------------------------------------

#include <sys/stat.h>

#include "core/observe/observation_store.hpp"

namespace {

std::string temp_sessions_dir(const char* tag) {
  const std::string d = "/tmp/mpi-test-obs-" + std::string(tag) + "-" +
                        std::to_string(::getpid());
  ::mkdir(d.c_str(), 0700);
  return d;
}

void remove_tree(const std::string& d) {
  const std::string cmd = "rm -rf '" + d + "'";
  const int rc = std::system(cmd.c_str());
  static_cast<void>(rc);
}

}  // namespace

MPI_TEST(an_observation_is_saved_listed_and_read_back_whole, {}) {
  const auto dir = temp_sessions_dir("rt");
  auto s = probe_doc(fixture("ios-probe-expo-go-home.real.json"));
  MPI_CHECK(s.has_value());
  const auto report = mpi::observe::analyze_layout(std::move(*s));
  mpi::json::Value summary = mpi::json::Value::object();
  summary.set("views", mpi::json::Value::integer(report.views));
  const auto saved = mpi::observe::save_observation(
      dir, "layout", "host.exp.Exponent", "UDID", summary, report.to_json(true));
  MPI_CHECK_MSG(saved.ok, saved.error);
  MPI_CHECK(saved.id.find("-layout-") != std::string::npos);

  struct stat st {};
  MPI_CHECK(::stat(saved.path.c_str(), &st) == 0);
  MPI_CHECK_MSG((st.st_mode & 0077) == 0, "owner-only: an inspect observation holds tokens");
  MPI_CHECK(::stat(mpi::observe::observations_dir(dir).c_str(), &st) == 0);
  MPI_CHECK((st.st_mode & 0077) == 0);

  const auto list = mpi::observe::list_observations(dir, "", 0);
  MPI_CHECK_EQ(list.find("total")->as_int(), static_cast<std::int64_t>(1));
  const auto& entry = list.find("observations")->items()[0];
  MPI_CHECK_EQ(entry.find("kind")->as_string(), std::string("layout"));
  MPI_CHECK_EQ(entry.find("summary")->find("views")->as_int(), static_cast<std::int64_t>(87));
  MPI_CHECK_MSG(entry.find("document") == nullptr, "a listing carries no documents");

  std::string err;
  const auto back = mpi::observe::read_observation(dir, saved.id, &err);
  MPI_CHECK_MSG(back.has_value(), err);
  const auto* tree = back->find("document")->find("tree");
  MPI_CHECK_MSG(tree != nullptr && tree->items().size() == 87,
                "the saved snapshot carries every view, for a model to work from");
  remove_tree(dir);
}

MPI_TEST(a_listing_can_be_narrowed_to_one_kind, {}) {
  const auto dir = temp_sessions_dir("kind");
  const auto empty = mpi::json::Value::object();
  MPI_CHECK(mpi::observe::save_observation(dir, "layout", "a", "d", empty, empty).ok);
  MPI_CHECK(mpi::observe::save_observation(dir, "inspect", "a", "d", empty, empty).ok);
  MPI_CHECK(mpi::observe::save_observation(dir, "inspect", "a", "d", empty, empty).ok);
  MPI_CHECK_EQ(mpi::observe::list_observations(dir, "inspect", 0).find("total")->as_int(),
               static_cast<std::int64_t>(2));
  MPI_CHECK_EQ(mpi::observe::list_observations(dir, "", 1)
                   .find("observations")->items().size(),
               static_cast<std::size_t>(1));
  remove_tree(dir);
}

MPI_TEST(an_id_that_could_leave_the_directory_is_refused, {}) {
  std::string err;
  MPI_CHECK(!mpi::observe::read_observation("/tmp", "../../etc/passwd", &err).has_value());
  MPI_CHECK(!err.empty());
  MPI_CHECK(!mpi::observe::observation_id_is_safe("a/b"));
  MPI_CHECK(!mpi::observe::observation_id_is_safe(""));
  MPI_CHECK(mpi::observe::observation_id_is_safe("20261003125754-layout-f2dfe0dc"));
  const auto bad = mpi::observe::save_observation("/tmp", "../x", "a", "d",
                                                  mpi::json::Value::object(),
                                                  mpi::json::Value::object());
  MPI_CHECK_MSG(!bad.ok, "a kind becomes part of a file name, so it is a plain word");
}

MPI_TEST(an_inspect_summary_says_when_the_file_holds_headers, {}) {
  mpi::json::ParseError perr;
  const auto plain = mpi::json::parse(
      R"({"debugger_attached":true,"network":[{"url":"https://a"}],"console":[1,2]})", &perr);
  const auto s1 = mpi::observe::summarize_inspect(*plain);
  MPI_CHECK_EQ(s1.find("network")->as_int(), static_cast<std::int64_t>(1));
  MPI_CHECK_EQ(s1.find("console")->as_int(), static_cast<std::int64_t>(2));
  MPI_CHECK(!s1.find("contains_headers_or_bodies")->as_bool());
  const auto detail = mpi::json::parse(
      R"({"debugger_attached":true,"network":[{"request_headers":{"authorization":"x"}}]})",
      &perr);
  MPI_CHECK(mpi::observe::summarize_inspect(*detail)
                .find("contains_headers_or_bodies")->as_bool());
}
