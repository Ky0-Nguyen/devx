// The Android emulator part of the C ABI (declared in mpi_capi.h).
//
// Management calls return JSON like the rest of the ABI. The display is the
// exception: frames arrive ~50 times a second, so their notification is three
// integers read under a lock, and input goes to a worker thread so a touch
// never waits on gRPC on the caller's (the UI's) thread.
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <functional>
#include <mutex>
#include <thread>

#include <unistd.h>

#include "adapters/android/emulator/avd.hpp"
#include "adapters/android/emulator/emulator_control.hpp"
#include "adapters/android/emulator/emulator_process.hpp"
#include "adapters/android/emulator/sdk_repository.hpp"
#include "core/capi/mpi_capi.h"
#include "core/observe/observation_store.hpp"

namespace mpi::capi {
CancellationToken current_cancel_token();
}  // namespace mpi::capi

namespace {

using namespace mpi;
using namespace mpi::android;

char* dup_json(const json::Value& v) {
  const std::string s = v.dump(2);
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (out != nullptr) std::memcpy(out, s.c_str(), s.size() + 1);
  return out;
}

template <typename Fn>
char* guard(Fn&& fn) {
  try {
    return dup_json(fn());
  } catch (const std::exception& e) {
    json::Value v = json::Value::object();
    v.set("error", json::Value::string(std::string("internal error: ") + e.what()));
    return dup_json(v);
  }
}

std::string safe(const char* s) { return s == nullptr ? std::string() : std::string(s); }

json::Value error_doc(const std::string& message) {
  json::Value v = json::Value::object();
  v.set("ok", json::Value::boolean(false));
  v.set("error", json::Value::string(message));
  return v;
}

json::Value ok_doc() {
  json::Value v = json::Value::object();
  v.set("ok", json::Value::boolean(true));
  return v;
}

// ---- catalog cache and install ---------------------------------------------

std::mutex g_catalog_mu;
SdkCatalog g_catalog;
bool g_have_catalog = false;

struct InstallState {
  std::mutex mu;
  bool running = false;
  bool finished = false;
  std::string path;
  InstallProgress progress;
  InstallResult result;
  CancellationSource cancel;
  std::thread worker;
};
InstallState g_install;

// ---- display sessions ---------------------------------------------------------
//
// One per device on screen, by handle, so several devices can be shown side by
// side. Each has its own stream, frame file, input queue and worker.

struct Display {
  std::mutex mu;  // guards ctl
  std::unique_ptr<EmulatorControl> ctl;
  std::string avd_id;
  std::string path;
  // Input worker and the latest frame.
  std::mutex qmu;
  std::condition_variable qcv;
  std::deque<std::function<void(EmulatorControl&)>> queue;
  FrameInfo frame;
  bool open = false;
  bool stopping = false;
  std::thread worker;
};

std::mutex g_displays_mu;  // guards the map, not the sessions in it
std::map<int, std::shared_ptr<Display>> g_displays;
int g_next_handle = 1;

std::shared_ptr<Display> display(int handle) {
  std::lock_guard<std::mutex> lock(g_displays_mu);
  const auto it = g_displays.find(handle);
  return it == g_displays.end() ? nullptr : it->second;
}

void shut(Display& d) {
  {
    std::lock_guard<std::mutex> lock(d.qmu);
    d.stopping = true;
    d.open = false;
  }
  d.qcv.notify_all();
  if (d.worker.joinable() && d.worker.get_id() != std::this_thread::get_id()) d.worker.join();
  std::lock_guard<std::mutex> lock(d.mu);
  if (d.ctl) d.ctl->stop_stream();
  d.ctl.reset();
  if (!d.path.empty()) {
    std::error_code ec;
    std::filesystem::remove(d.path, ec);
  }
}

void close_handle(int handle) {
  std::shared_ptr<Display> d;
  {
    std::lock_guard<std::mutex> lock(g_displays_mu);
    const auto it = g_displays.find(handle);
    if (it == g_displays.end()) return;
    d = it->second;
    g_displays.erase(it);
  }
  shut(*d);
}

void close_avd(const std::string& avd_id) {
  std::vector<int> handles;
  {
    std::lock_guard<std::mutex> lock(g_displays_mu);
    for (const auto& [h, d] : g_displays) {
      if (d->avd_id == avd_id) handles.push_back(h);
    }
  }
  for (int h : handles) close_handle(h);
}

void enqueue(int handle, std::function<void(EmulatorControl&)> fn) {
  auto d = display(handle);
  if (!d) return;
  {
    std::lock_guard<std::mutex> lock(d->qmu);
    if (!d->open) return;
    d->queue.push_back(std::move(fn));
  }
  d->qcv.notify_one();
}

// Runs `fn` on a session's control, serialised with its input worker.
template <typename Fn>
char* with_display(int handle, Fn&& fn) {
  return guard([&] {
    auto d = display(handle);
    if (!d) return error_doc("no display with that handle is open");
    std::lock_guard<std::mutex> lock(d->mu);
    if (!d->ctl) return error_doc("the display has closed");
    return fn(*d);
  });
}

json::Value avds_with_running() {
  json::Value arr = json::Value::array();
  const auto running = running_emulators();
  for (const auto& a : list_avds()) {
    json::Value o = a.to_json();
    for (const auto& r : running) {
      if (r.avd_id == a.id) o.set("running", r.to_json());
    }
    arr.push_back(std::move(o));
  }
  return arr;
}

}  // namespace

extern "C" {

char* mpi_android_sdk_json(void) {
  return guard([] {
    const auto loc = locate_sdk();
    json::Value o = json::Value::object();
    o.set("root", json::Value::string(loc.root));
    o.set("source", json::Value::string(loc.source));
    o.set("exists", json::Value::boolean(loc.exists));
    json::Value pk = json::Value::array();
    bool emulator = false;
    for (const auto& p : installed_packages(loc.root)) {
      json::Value e = json::Value::object();
      e.set("path", json::Value::string(p.path));
      e.set("revision", json::Value::string(p.revision));
      e.set("display_name", json::Value::string(p.display_name));
      pk.push_back(std::move(e));
      emulator = emulator || p.path == "emulator";
    }
    o.set("installed", std::move(pk));
    o.set("emulator_installed", json::Value::boolean(emulator));
    o.set("avds", avds_with_running());
    json::Value rn = json::Value::array();
    for (const auto& r : running_emulators()) rn.push_back(r.to_json());
    o.set("running", std::move(rn));
    o.set("host_abi", json::Value::string(this_host().abi));
    return o;
  });
}

char* mpi_android_catalog_json(int timeout_ms) {
  return guard([&] {
    auto cat = fetch_catalog(std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 60000),
                             capi::current_cancel_token());
    const auto loc = locate_sdk();
    json::Value o = cat.to_json(installed_packages(loc.root));
    json::Value accepted = json::Value::array();
    for (const auto& [id, text] : cat.licenses) {
      if (license_accepted(loc.root, id, text)) accepted.push_back(json::Value::string(id));
    }
    o.set("accepted_licenses", std::move(accepted));
    o.set("sdk_root", json::Value::string(loc.root));
    std::lock_guard<std::mutex> lock(g_catalog_mu);
    g_catalog = std::move(cat);
    g_have_catalog = true;
    return o;
  });
}

char* mpi_android_accept_license_json(const char* license_id) {
  return guard([&] {
    const std::string id = safe(license_id);
    std::lock_guard<std::mutex> lock(g_catalog_mu);
    if (!g_have_catalog || g_catalog.licenses.count(id) == 0) {
      return error_doc("load the catalog first; '" + id + "' is not a license in it");
    }
    std::string err;
    if (!accept_license(locate_sdk().root, id, g_catalog.licenses.at(id), &err)) {
      return error_doc(err);
    }
    return ok_doc();
  });
}

char* mpi_android_install_start_json(const char* package_path) {
  return guard([&] {
    const std::string path = safe(package_path);
    SdkPackage pkg;
    std::string text;
    {
      std::lock_guard<std::mutex> lock(g_catalog_mu);
      bool found = false;
      for (const auto& p : g_catalog.packages) {
        if (p.path == path) {
          pkg = p;
          found = true;
        }
      }
      if (!found) return error_doc("'" + path + "' is not in the loaded catalog");
      if (g_catalog.licenses.count(pkg.license_id) != 0) text = g_catalog.licenses.at(pkg.license_id);
    }
    std::lock_guard<std::mutex> lock(g_install.mu);
    if (g_install.running) return error_doc("an install is already running: " + g_install.path);
    if (g_install.worker.joinable()) g_install.worker.join();
    g_install.running = true;
    g_install.finished = false;
    g_install.path = path;
    g_install.progress = InstallProgress{"starting", 0, pkg.archive.size};
    g_install.result = InstallResult{};
    g_install.cancel = CancellationSource();
    const auto token = g_install.cancel.token();
    const std::string root = locate_sdk().root;
    g_install.worker = std::thread([pkg, text, root, token] {
      auto r = install_package(pkg, root, text,
                               [](const InstallProgress& p) {
                                 std::lock_guard<std::mutex> l(g_install.mu);
                                 g_install.progress = p;
                               },
                               token);
      std::lock_guard<std::mutex> l(g_install.mu);
      g_install.result = std::move(r);
      g_install.running = false;
      g_install.finished = true;
    });
    return ok_doc();
  });
}

char* mpi_android_install_poll_json(void) {
  return guard([] {
    std::lock_guard<std::mutex> lock(g_install.mu);
    json::Value o = json::Value::object();
    o.set("path", json::Value::string(g_install.path));
    o.set("running", json::Value::boolean(g_install.running));
    o.set("finished", json::Value::boolean(g_install.finished));
    o.set("phase", json::Value::string(g_install.progress.phase));
    o.set("done", json::Value::integer(static_cast<std::int64_t>(g_install.progress.done)));
    o.set("total", json::Value::integer(static_cast<std::int64_t>(g_install.progress.total)));
    if (g_install.finished) {
      o.set("ok", json::Value::boolean(g_install.result.ok));
      if (!g_install.result.error.empty()) {
        o.set("error", json::Value::string(g_install.result.error));
      }
    }
    return o;
  });
}

void mpi_android_install_cancel(void) {
  std::lock_guard<std::mutex> lock(g_install.mu);
  g_install.cancel.cancel();
}

char* mpi_android_presets_json(void) {
  return guard([] {
    json::Value a = json::Value::array();
    for (const auto& p : device_presets()) a.push_back(preset_json(p));
    return a;
  });
}

char* mpi_avd_create_json(const char* name, const char* system_image, const char* preset_id,
                          int width, int height, int density, int ram_mb) {
  return guard([&] {
    AvdSpec spec;
    spec.display_name = safe(name);
    spec.system_image = safe(system_image);
    spec.width = width;
    spec.height = height;
    spec.density = density;
    if (ram_mb > 0) spec.ram_mb = ram_mb;
    if (const auto* p = find_preset(safe(preset_id))) {
      if (spec.width <= 0) spec.width = p->width;
      if (spec.height <= 0) spec.height = p->height;
      if (spec.density <= 0) spec.density = p->density;
      spec.device_name = p->id;
    }
    AvdInfo out;
    std::string err;
    if (!create_avd(locate_sdk().root, spec, out, &err)) return error_doc(err);
    json::Value o = ok_doc();
    o.set("avd", out.to_json());
    return o;
  });
}

char* mpi_avd_delete_json(const char* avd_id) {
  return guard([&] {
    const std::string id = safe(avd_id);
    if (find_running(id)) return error_doc("'" + id + "' is running; stop it first");
    std::string err;
    if (!delete_avd(id, &err)) return error_doc(err);
    return ok_doc();
  });
}

char* mpi_avd_resize_json(const char* avd_id, int width, int height, int density,
                          int override_running) {
  return guard([&] {
    const std::string id = safe(avd_id);
    std::string err;
    if (override_running != 0) {
      const auto r = find_running(id);
      if (!r) return error_doc("'" + id + "' is not running");
      if (!override_screen(locate_sdk().root, r->serial, width, height, density, &err)) {
        return error_doc(err);
      }
      json::Value o = ok_doc();
      o.set("note", json::Value::string(
                        width > 0 ? "overridden through `wm`: no restart; the hardware size is unchanged"
                                  : "override reset to the device's own size"));
      return o;
    }
    if (!set_avd_screen(id, width, height, density, &err)) return error_doc(err);
    json::Value o = ok_doc();
    o.set("note", json::Value::string("takes effect at the next start, which will be a cold boot"));
    return o;
  });
}

char* mpi_emulator_start_json(const char* avd_id, int cold_boot, int wipe_data) {
  return guard([&] {
    LaunchOptions o;
    o.cold_boot = cold_boot != 0;
    o.wipe_data = wipe_data != 0;
    o.cancel = capi::current_cancel_token();
    const auto r = launch_emulator(locate_sdk().root, safe(avd_id), o);
    json::Value out = json::Value::object();
    out.set("ok", json::Value::boolean(r.ok));
    out.set("started", json::Value::boolean(r.started));
    if (!r.error.empty()) out.set("error", json::Value::string(r.error));
    if (r.started) out.set("emulator", r.emulator.to_json());
    json::Value notes = json::Value::array();
    for (const auto& n : r.notes) notes.push_back(json::Value::string(n));
    out.set("notes", std::move(notes));
    if (!r.log_path.empty()) out.set("log_path", json::Value::string(r.log_path));
    return out;
  });
}

char* mpi_emulator_stop_json(const char* avd_id) {
  return guard([&] {
    const std::string id = safe(avd_id);
    close_avd(id);
    const auto r = find_running(id);
    if (!r) return error_doc("'" + id + "' is not running");
    std::string err;
    if (!stop_emulator(locate_sdk().root, *r, &err)) return error_doc(err);
    return ok_doc();
  });
}

char* mpi_emulator_display_open_json(const char* avd_id, int box) {
  return guard([&] {
    const std::string id = safe(avd_id);
    // One display per device: showing it again replaces the old session.
    close_avd(id);
    const auto r = find_running(id);
    if (!r) return error_doc("'" + id + "' is not running");
    auto d = std::make_shared<Display>();
    d->ctl = std::make_unique<EmulatorControl>();
    std::string err;
    if (!d->ctl->connect(*r, &err)) return error_doc(err);
    int handle = 0;
    {
      std::lock_guard<std::mutex> lock(g_displays_mu);
      handle = g_next_handle++;
    }
    std::error_code ec;
    // A new name every time, so a view still holding an old session's file can
    // never mistake it for this one.
    const std::string path = (std::filesystem::temp_directory_path(ec) /
                              ("devx-display-" + std::to_string(::getpid()) + "-" +
                               std::to_string(handle) + ".rgba"))
                                 .string();
    const auto side = static_cast<std::uint32_t>(box > 0 ? box : 1280);
    std::weak_ptr<Display> weak = d;
    if (!d->ctl->start_stream(path, side,
                              [weak](const FrameInfo& f) {
                                if (auto dd = weak.lock()) {
                                  std::lock_guard<std::mutex> l(dd->qmu);
                                  dd->frame = f;
                                }
                              },
                              &err)) {
      return error_doc("could not start the screen stream: " + err);
    }
    d->avd_id = id;
    d->path = path;
    d->open = true;
    Display* raw = d.get();
    d->worker = std::thread([raw] {
      for (;;) {
        std::function<void(EmulatorControl&)> job;
        {
          std::unique_lock<std::mutex> l(raw->qmu);
          raw->qcv.wait(l, [raw] { return raw->stopping || !raw->queue.empty(); });
          if (raw->stopping) return;
          job = std::move(raw->queue.front());
          raw->queue.pop_front();
        }
        std::lock_guard<std::mutex> l(raw->mu);
        if (raw->ctl) job(*raw->ctl);
      }
    });
    json::Value o = ok_doc();
    o.set("handle", json::Value::integer(handle));
    o.set("path", json::Value::string(path));
    o.set("box", json::Value::integer(side));
    o.set("serial", json::Value::string(r->serial));
    o.set("avd_id", json::Value::string(id));
    json::Value st;
    if (d->ctl->status(&st, nullptr)) o.set("status", st);
    {
      std::lock_guard<std::mutex> lock(g_displays_mu);
      g_displays[handle] = std::move(d);
    }
    return o;
  });
}

int mpi_emulator_display_frame(int handle, unsigned int* seq, unsigned int* width,
                               unsigned int* height) {
  auto d = display(handle);
  if (!d) return 0;
  bool alive = false;
  {
    std::lock_guard<std::mutex> lock(d->mu);
    alive = d->ctl && d->ctl->stream_alive();
  }
  if (!alive) {
    // The stream ended -- the emulator stopped or restarted -- so the session
    // ends, and the window can say so instead of showing the last frame.
    close_handle(handle);
    return 0;
  }
  std::lock_guard<std::mutex> l(d->qmu);
  if (!d->open) return 0;
  if (seq != nullptr) *seq = d->frame.seq;
  if (width != nullptr) *width = d->frame.width;
  if (height != nullptr) *height = d->frame.height;
  return 1;
}

void mpi_emulator_display_close(int handle) {
  if (handle > 0) {
    close_handle(handle);
    return;
  }
  std::vector<int> all;
  {
    std::lock_guard<std::mutex> lock(g_displays_mu);
    for (const auto& [h, _] : g_displays) all.push_back(h);
  }
  for (int h : all) close_handle(h);
}

void mpi_emulator_touch(int handle, int x, int y, int pressure) {
  enqueue(handle, [x, y, pressure](EmulatorControl& c) { c.touch(x, y, pressure); });
}

void mpi_emulator_key(int handle, const char* key, int phase) {
  const std::string k = safe(key);
  enqueue(handle, [k, phase](EmulatorControl& c) { c.key(k, phase); });
}

void mpi_emulator_text(int handle, const char* utf8) {
  const std::string t = safe(utf8);
  enqueue(handle, [t](EmulatorControl& c) { c.text(t); });
}

char* mpi_emulator_rotate_json(int handle, int degrees) {
  return with_display(handle, [&](Display& d) {
    return d.ctl->rotate(degrees) ? ok_doc() : error_doc(d.ctl->last_error());
  });
}

char* mpi_emulator_extended_controls_json(int handle, int pane) {
  return with_display(handle, [&](Display& d) {
    return d.ctl->show_extended_controls(pane) ? ok_doc() : error_doc(d.ctl->last_error());
  });
}

char* mpi_emulator_status_json(int handle) {
  return with_display(handle, [&](Display& d) {
    json::Value st;
    std::string err;
    if (!d.ctl->status(&st, &err)) return error_doc(err);
    st.set("ok", json::Value::boolean(true));
    st.set("avd_id", json::Value::string(d.avd_id));
    return st;
  });
}

char* mpi_emulator_screenshot_json(int handle, const char* sessions_dir) {
  const std::string dir = safe(sessions_dir);
  return with_display(handle, [&](Display& d) {
    std::string png, err;
    if (!d.ctl->screenshot_png(&png, 0, 0, &err)) return error_doc(err);
    const std::string shots = dir + "/emulator-shots";
    std::error_code ec;
    std::filesystem::create_directories(shots, ec);
    const std::string file = shots + "/" + d.avd_id + "-" +
                             std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count()) +
                             ".png";
    std::ofstream(file, std::ios::binary) << png;
    json::Value doc = json::Value::object();
    doc.set("avd_id", json::Value::string(d.avd_id));
    doc.set("serial", json::Value::string(d.ctl->emulator().serial));
    doc.set("png_path", json::Value::string(file));
    doc.set("bytes", json::Value::integer(static_cast<std::int64_t>(png.size())));
    json::Value st;
    if (d.ctl->status(&st, nullptr)) doc.set("status", st);
    const auto saved = observe::save_observation(dir, "emulator_screenshot", d.avd_id,
                                                 d.ctl->emulator().serial, json::Value::object(), doc);
    json::Value o = ok_doc();
    o.set("path", json::Value::string(file));
    if (saved.ok) o.set("observation", json::Value::string(saved.id));
    return o;
  });
}

}  // extern "C"
