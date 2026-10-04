#include "core/observe/layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>
#include <utility>

namespace mpi::observe {
namespace {

std::string str(const json::Value& v, std::string_view key) {
  const json::Value* f = v.find(key);
  return f != nullptr && f->is_string() ? f->as_string() : std::string();
}

double num(const json::Value& v, std::string_view key, double fallback) {
  const json::Value* f = v.find(key);
  return f != nullptr && f->is_number() ? f->as_double() : fallback;
}

bool flag(const json::Value& v, std::string_view key, bool fallback) {
  const json::Value* f = v.find(key);
  return f != nullptr && f->is_bool() ? f->as_bool() : fallback;
}

void read_frame(const json::Value& v, double* x, double* y, double* w,
                double* h) {
  const json::Value* f = v.find("frame");
  if (f == nullptr || !f->is_array() || f->items().size() != 4) return;
  *x = f->items()[0].as_double();
  *y = f->items()[1].as_double();
  *w = f->items()[2].as_double();
  *h = f->items()[3].as_double();
}

int as_index(std::size_t n) { return static_cast<int>(n); }

void add_controllers(const json::Value& node, int parent, int depth, int window,
                     bool presented, LayoutSnapshot& s) {
  LayoutController c;
  c.id = str(node, "id");
  c.cls = str(node, "class");
  c.display = c.cls;
  c.kind = str(node, "kind");
  c.view_loaded = flag(node, "view_loaded", false);
  c.visible = flag(node, "visible", false);
  c.presented = presented;
  c.parent = parent;
  c.depth = depth;
  c.window = window;
  if (const json::Value* st = node.find("stack"); st != nullptr && st->is_array()) {
    for (const auto& e : st->items()) {
      if (e.is_string()) c.stack.push_back(e.as_string());
    }
  }
  const int me = as_index(s.controllers.size());
  s.controllers.push_back(std::move(c));
  if (const json::Value* ch = node.find("children"); ch != nullptr && ch->is_array()) {
    for (const auto& k : ch->items()) {
      if (k.is_object()) add_controllers(k, me, depth + 1, window, false, s);
    }
  }
  if (const json::Value* p = node.find("presented"); p != nullptr && p->is_object()) {
    add_controllers(*p, me, depth + 1, window, true, s);
  }
}

// Preorder, iteratively: a pathological tree must not exhaust the stack, and
// preorder keeps every subtree contiguous, which the analysis relies on.
int add_views(const json::Value& root, int window, LayoutSnapshot& s) {
  struct Item {
    const json::Value* node;
    int parent;
    int depth;
  };
  std::vector<Item> stack{{&root, -1, 0}};
  const int first = as_index(s.views.size());
  while (!stack.empty()) {
    const Item it = stack.back();
    stack.pop_back();
    if (!it.node->is_object()) continue;
    LayoutView v;
    v.cls = str(*it.node, "class");
    v.display = v.cls;
    read_frame(*it.node, &v.x, &v.y, &v.w, &v.h);
    v.hidden = flag(*it.node, "hidden", false);
    v.alpha = num(*it.node, "alpha", 1.0);
    v.controller = str(*it.node, "controller");
    v.controller_id = str(*it.node, "controller_id");
    v.identifier = str(*it.node, "identifier");
    v.parent = it.parent;
    v.depth = it.depth;
    v.window = window;
    const int me = as_index(s.views.size());
    s.views.push_back(std::move(v));
    if (const json::Value* ch = it.node->find("children");
        ch != nullptr && ch->is_array()) {
      const auto& items = ch->items();
      for (auto k = items.rbegin(); k != items.rend(); ++k) {
        stack.push_back({&*k, me, it.depth + 1});
      }
    }
  }
  return first;
}

std::string trim(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
  return s.substr(a, b - a);
}

std::size_t indent_of(const std::string& s) {
  std::size_t n = 0;
  while (n < s.size() && s[n] == ' ') n++;
  return n;
}

bool starts_with(const std::string& s, std::string_view p) {
  return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

// "l,t-r,b", each possibly negative.
bool parse_bounds(const std::string& tok, long* l, long* t, long* r, long* b) {
  const char* p = tok.c_str();
  char* end = nullptr;
  *l = std::strtol(p, &end, 10);
  if (end == p || *end != ',') return false;
  p = end + 1;
  *t = std::strtol(p, &end, 10);
  if (end == p || *end != '-') return false;
  p = end + 1;
  *r = std::strtol(p, &end, 10);
  if (end == p || *end != ',') return false;
  p = end + 1;
  *b = std::strtol(p, &end, 10);
  return end != p;
}

std::vector<std::string> split_spaces(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string tok;
  while (in >> tok) out.push_back(tok);
  return out;
}

std::string fmt_num(double v) {
  char buf[32];
  if (std::fabs(v - std::round(v)) < 1e-6) {
    std::snprintf(buf, sizeof buf, "%.0f", v);
  } else {
    std::snprintf(buf, sizeof buf, "%.1f", v);
  }
  return buf;
}

std::string pad(const std::string& s, std::size_t w, bool right = false) {
  if (s.size() >= w) return s;
  return right ? std::string(w - s.size(), ' ') + s
               : s + std::string(w - s.size(), ' ');
}

bool is_rn_screen_view(const LayoutView& v, model::Platform p) {
  if (p == model::Platform::kAndroid) {
    return v.cls == "com.swmansion.rnscreens.Screen";
  }
  return v.display == "RNSScreenView";
}

bool is_rn_host_view(const LayoutView& v, model::Platform p) {
  if (p == model::Platform::kAndroid) {
    return starts_with(v.cls, "com.facebook.react.");
  }
  return starts_with(v.display, "RCT");
}

bool same_frame(const LayoutView& a, const LayoutView& b) {
  return std::fabs(a.x - b.x) < 0.5 && std::fabs(a.y - b.y) < 0.5 &&
         std::fabs(a.w - b.w) < 0.5 && std::fabs(a.h - b.h) < 0.5;
}

// UIKit's own windows (the keyboard's, text effects') are not the app's
// screens. An app's window is a UIWindow or its own subclass of one.
bool is_system_window(const LayoutWindow& w) {
  if (w.cls == "UIWindow") return false;
  return starts_with(w.cls, "UI") || starts_with(w.cls, "_UI");
}

bool view_on_screen(const LayoutView& v) {
  return !v.effectively_hidden && !v.offscreen && !v.zero_size;
}

}  // namespace

const char* to_string(LayoutSource s) {
  switch (s) {
    case LayoutSource::kIosInjectedProbe: return "ios_injected_probe";
    case LayoutSource::kAndroidDumpsys:   return "android_dumpsys_activity_top";
  }
  return "unknown";
}

json::Limits probe_json_limits() {
  json::Limits l;
  l.max_depth = 4096;
  l.max_bytes = 256ull * 1024ull * 1024ull;
  return l;
}

std::optional<LayoutSnapshot> parse_ios_probe(const json::Value& doc,
                                              std::string* error) {
  const json::Value* windows = doc.find("windows");
  if (!doc.is_object() || windows == nullptr || !windows->is_array()) {
    if (error != nullptr) *error = "not a layout probe document: no windows";
    return std::nullopt;
  }
  LayoutSnapshot s;
  s.source = LayoutSource::kIosInjectedProbe;
  s.platform = model::Platform::kIos;
  s.app_identifier = str(doc, "bundle_id");
  s.pid = static_cast<std::int64_t>(num(doc, "pid", 0));
  s.taken_at_unix_ms = static_cast<std::int64_t>(num(doc, "taken_at_ms", 0));
  s.truncated = flag(doc, "truncated", false);
  s.max_views = static_cast<std::int64_t>(num(doc, "max_views", 0));
  if (const json::Value* sc = doc.find("screen"); sc != nullptr) {
    s.screen_w = num(*sc, "width", 0);
    s.screen_h = num(*sc, "height", 0);
    s.scale = num(*sc, "scale", 0);
  }
  const auto version = static_cast<std::int64_t>(num(doc, "probe_version", 0));
  if (version != 1) {
    s.warnings.push_back("probe version " + std::to_string(version) +
                         " is not the one this build reads (1); fields it "
                         "does not know are ignored");
  }
  for (const auto& w : windows->items()) {
    if (!w.is_object()) continue;
    const int index = as_index(s.windows.size());
    LayoutWindow win;
    win.cls = str(w, "class");
    win.key = flag(w, "key", false);
    win.hidden = flag(w, "hidden", false);
    read_frame(w, &win.x, &win.y, &win.w, &win.h);
    if (const json::Value* rc = w.find("root_controller");
        rc != nullptr && rc->is_object()) {
      add_controllers(*rc, -1, 0, index, false, s);
    }
    if (const json::Value* root = w.find("root"); root != nullptr) {
      win.root_view = add_views(*root, index, s);
    }
    s.windows.push_back(std::move(win));
  }
  finalize(s);
  return s;
}

std::optional<LayoutSnapshot> parse_android_dumpsys(const std::string& text,
                                                    const std::string& package,
                                                    std::string* error) {
  std::vector<std::string> lines;
  {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
  }
  // The activity block: from its ACTIVITY line to the next ACTIVITY or TASK.
  std::size_t begin = lines.size(), end = lines.size();
  std::string component;
  std::int64_t pid = 0;
  int matches = 0;
  for (std::size_t i = 0; i < lines.size(); i++) {
    const std::string t = trim(lines[i]);
    if (!starts_with(t, "ACTIVITY ")) continue;
    const auto toks = split_spaces(t);
    if (toks.size() < 2) continue;
    const auto slash = toks[1].find('/');
    if (slash == std::string::npos || toks[1].substr(0, slash) != package) continue;
    matches++;
    if (matches > 1) continue;
    begin = i;
    component = toks[1];
    for (const auto& tok : toks) {
      if (starts_with(tok, "pid=")) pid = std::atoll(tok.c_str() + 4);
    }
    end = lines.size();
    for (std::size_t j = i + 1; j < lines.size(); j++) {
      const std::string tj = trim(lines[j]);
      if (starts_with(tj, "ACTIVITY ") || starts_with(lines[j], "TASK ")) {
        end = j;
        break;
      }
    }
  }
  if (begin == lines.size()) {
    if (error != nullptr) {
      *error = "'" + package + "' has no activity in `dumpsys activity top`, "
               "so it is not on screen. Bring it to the foreground and retry";
    }
    return std::nullopt;
  }

  LayoutSnapshot s;
  s.source = LayoutSource::kAndroidDumpsys;
  s.platform = model::Platform::kAndroid;
  s.app_identifier = package;
  s.pid = pid;
  {
    const auto slash = component.find('/');
    std::string cls = component.substr(slash + 1);
    if (starts_with(cls, ".")) cls = package + cls;
    s.activity = cls;
  }
  if (matches > 1) {
    s.warnings.push_back(std::to_string(matches) + " activities of " + package +
                         " are on screen; the first one listed is read");
  }

  std::set<std::string> seen_fragments;
  std::size_t hierarchy = end;
  for (std::size_t i = begin; i < end; i++) {
    const std::string t = trim(lines[i]);
    if (t == "View Hierarchy:" && hierarchy == end) hierarchy = i;
    // "mBounds=Rect(0, 0 - 1080, 2400)" in the activity's configuration.
    if (s.screen_w == 0) {
      const auto at = lines[i].find("mBounds=Rect(");
      if (at != std::string::npos) {
        long l = 0, tp = 0, r = 0, b = 0;
        if (std::sscanf(lines[i].c_str() + at, "mBounds=Rect(%ld, %ld - %ld, %ld)",
                        &l, &tp, &r, &b) == 4) {
          s.screen_w = static_cast<double>(r - l);
          s.screen_h = static_cast<double>(b - tp);
          s.scale = 1;
        }
      }
    }
    if (t == "Added Fragments:") {
      const std::size_t base = indent_of(lines[i]);
      for (std::size_t j = i + 1; j < end; j++) {
        if (indent_of(lines[j]) <= base || trim(lines[j]).empty()) break;
        const std::string f = trim(lines[j]);
        // "#0: TopLevelSettings{c1c3ca5} (...)"
        const auto colon = f.find(": ");
        if (colon == std::string::npos) continue;
        std::string name = f.substr(colon + 2);
        const auto brace = name.find('{');
        if (brace != std::string::npos) name = name.substr(0, brace);
        // androidx.lifecycle's headless fragment: present in every activity
        // that uses lifecycles, and no screen of anyone's.
        if (name == "ReportFragment") continue;
        if (seen_fragments.insert(name).second) s.fragments.push_back(name);
      }
    }
  }
  if (hierarchy == end) {
    if (error != nullptr) {
      *error = "the activity " + s.activity + " printed no view hierarchy";
    }
    return std::nullopt;
  }

  LayoutWindow win;
  win.cls = "DecorView";
  win.key = true;
  win.w = s.screen_w;
  win.h = s.screen_h;
  const std::size_t base = indent_of(lines[hierarchy]);
  std::vector<int> at_depth;  // the last view seen at each depth
  for (std::size_t i = hierarchy + 1; i < end; i++) {
    const std::size_t ind = indent_of(lines[i]);
    const std::string t = trim(lines[i]);
    if (t.empty() || ind <= base) break;
    const int depth = static_cast<int>((ind - base - 2) / 2);
    LayoutView v;
    v.depth = depth;
    v.window = 0;
    const auto brace = t.find('{');
    if (brace == std::string::npos) {
      // The root: "DecorView@2239d0c[Settings]". It has no bounds of its own
      // in this output; it is the window.
      const auto at = t.find('@');
      v.cls = at == std::string::npos ? t : t.substr(0, at);
      v.w = s.screen_w;
      v.h = s.screen_h;
    } else {
      v.cls = t.substr(0, brace);
      const auto close = t.rfind('}');
      const auto toks = split_spaces(t.substr(
          brace + 1, (close == std::string::npos ? t.size() : close) - brace - 1));
      // hash, view flags, private flags, bounds, then "#id name" when set.
      if (toks.size() >= 2 && !toks[1].empty()) {
        v.hidden = toks[1][0] == 'G' || toks[1][0] == 'I';
      }
      long l = 0, tp = 0, r = 0, b = 0;
      if (toks.size() >= 4 && parse_bounds(toks[3], &l, &tp, &r, &b)) {
        v.x = static_cast<double>(l);
        v.y = static_cast<double>(tp);
        v.w = static_cast<double>(r - l);
        v.h = static_cast<double>(b - tp);
      }
      if (toks.size() >= 6 && starts_with(toks[4], "#")) v.identifier = toks[5];
    }
    v.display = v.cls;
    if (depth <= 0 || static_cast<std::size_t>(depth) > at_depth.size()) {
      v.parent = depth <= 0 ? -1 : (at_depth.empty() ? -1 : at_depth.back());
    } else {
      v.parent = at_depth[static_cast<std::size_t>(depth - 1)];
    }
    // Bounds are relative to the parent; make them absolute.
    if (v.parent >= 0) {
      const auto& p = s.views[static_cast<std::size_t>(v.parent)];
      v.x += p.x;
      v.y += p.y;
    }
    const int me = as_index(s.views.size());
    if (win.root_view < 0) win.root_view = me;
    s.views.push_back(std::move(v));
    at_depth.resize(static_cast<std::size_t>(std::max(depth, 0)) + 1);
    at_depth[static_cast<std::size_t>(std::max(depth, 0))] = me;
  }
  s.windows.push_back(std::move(win));
  s.warnings.push_back(
      "dumpsys gives each view's bounds relative to its parent and omits "
      "scroll offsets, so a view inside a scrolled container is placed as if "
      "it were not scrolled; off-screen counts below a scroll view are "
      "approximate");
  finalize(s);
  return s;
}

std::vector<std::string> mangled_class_names(const LayoutSnapshot& s) {
  std::set<std::string> out;
  auto take = [&](const std::string& c) {
    if (starts_with(c, "_Tt") || starts_with(c, "$s") || starts_with(c, "_$s")) {
      out.insert(c);
    }
  };
  for (const auto& v : s.views) take(v.cls);
  for (const auto& c : s.controllers) {
    take(c.cls);
    for (const auto& st : c.stack) take(st);
  }
  return {out.begin(), out.end()};
}

void apply_display_names(LayoutSnapshot& s,
                         const std::map<std::string, std::string>& demangled) {
  auto name = [&](const std::string& c) {
    const auto it = demangled.find(c);
    return it == demangled.end() || it->second.empty() ? c : it->second;
  };
  for (auto& v : s.views) v.display = name(v.cls);
  for (auto& c : s.controllers) {
    c.display = name(c.cls);
    for (auto& st : c.stack) st = name(st);
  }
}

void finalize(LayoutSnapshot& s) {
  for (auto& v : s.views) v.children = 0;
  for (std::size_t i = 0; i < s.views.size(); i++) {
    auto& v = s.views[i];
    v.zero_size = v.w <= 0 || v.h <= 0;
    bool parent_hidden = false;
    if (v.parent >= 0) {
      auto& p = s.views[static_cast<std::size_t>(v.parent)];
      p.children++;
      parent_hidden = p.effectively_hidden;
    }
    v.effectively_hidden = v.hidden || v.alpha <= 0.01 || parent_hidden;
    double ww = s.screen_w, wh = s.screen_h;
    if (v.window >= 0 && static_cast<std::size_t>(v.window) < s.windows.size()) {
      const auto& w = s.windows[static_cast<std::size_t>(v.window)];
      if (w.w > 0 && w.h > 0) {
        ww = w.w;
        wh = w.h;
      }
    }
    v.offscreen = !v.zero_size && ww > 0 && wh > 0 &&
                  (v.x + v.w <= 0 || v.y + v.h <= 0 || v.x >= ww || v.y >= wh);
  }
}

LayoutReport analyze_layout(LayoutSnapshot snapshot,
                            const LayoutThresholds& thresholds) {
  LayoutReport r;
  r.thresholds = thresholds;
  r.snapshot = std::move(snapshot);
  const auto& s = r.snapshot;
  const auto& views = s.views;
  const std::size_t n = views.size();

  // Preorder means a subtree is a contiguous run starting at its root.
  std::vector<std::size_t> subtree(n, 1);
  for (std::size_t i = n; i-- > 0;) {
    if (views[i].parent >= 0) subtree[static_cast<std::size_t>(views[i].parent)] += subtree[i];
  }

  for (const auto& v : views) {
    r.views++;
    if (view_on_screen(v)) r.visible_views++;
    if (v.effectively_hidden) {
      r.hidden_views++;
    } else if (v.offscreen) {
      r.offscreen_views++;
    }
    r.max_depth = std::max(r.max_depth, v.depth);
  }

  auto stats_for = [&](LayoutScreen& sc) {
    const auto root = static_cast<std::size_t>(sc.root_view);
    const int root_depth = views[root].depth;
    std::map<std::string, std::int64_t> classes;
    for (std::size_t i = root; i < root + subtree[root] && i < n; i++) {
      const auto& v = views[i];
      sc.views++;
      if (view_on_screen(v)) sc.visible_views++;
      if (v.effectively_hidden) {
        sc.hidden_views++;
      } else if (v.offscreen) {
        sc.offscreen_views++;
      }
      if (v.depth - root_depth > sc.depth) {
        sc.depth = v.depth - root_depth;
        sc.absolute_depth = v.depth;
      }
      if (v.children == 1 && i + 1 < n && views[i + 1].parent == static_cast<int>(i) &&
          same_frame(v, views[i + 1]) && v.controller.empty() &&
          v.identifier.empty()) {
        sc.single_child_wrappers++;
      }
      classes[v.display]++;
    }
    for (const auto& [name, count] : classes) sc.top_classes.push_back({name, count});
    std::sort(sc.top_classes.begin(), sc.top_classes.end(),
              [](const ClassCount& a, const ClassCount& b) {
                return a.count != b.count ? a.count > b.count : a.name < b.name;
              });
    if (sc.top_classes.size() > 8) sc.top_classes.resize(8);
  };

  std::map<int, int> screen_by_root;
  auto add_screen = [&](LayoutScreen sc) {
    if (sc.root_view < 0 || static_cast<std::size_t>(sc.root_view) >= n) return;
    if (screen_by_root.count(sc.root_view) != 0) return;
    stats_for(sc);
    screen_by_root[sc.root_view] = as_index(r.screens.size());
    r.screens.push_back(std::move(sc));
  };

  if (s.platform == model::Platform::kAndroid) {
    for (const auto& w : s.windows) {
      LayoutScreen sc;
      sc.name = s.activity.empty() ? w.cls : s.activity;
      sc.basis = "activity";
      sc.on_screen = true;
      sc.root_view = w.root_view;
      add_screen(std::move(sc));
    }
  } else {
    // A screen is a visible content controller with no visible content
    // controller inside it: containers (navigation, tabs) are not screens,
    // and an outer content controller that only hosts another is not either.
    const auto& cs = s.controllers;
    std::vector<bool> has_visible_content_below(cs.size(), false);
    for (std::size_t i = cs.size(); i-- > 0;) {
      const bool counts = cs[i].kind == "content" && cs[i].visible;
      if ((counts || has_visible_content_below[i]) && cs[i].parent >= 0) {
        has_visible_content_below[static_cast<std::size_t>(cs[i].parent)] = true;
      }
    }
    std::map<std::string, int> view_by_controller;
    for (std::size_t i = 0; i < n; i++) {
      if (!views[i].controller_id.empty()) {
        view_by_controller.emplace(views[i].controller_id, as_index(i));
      }
    }
    for (std::size_t i = 0; i < cs.size(); i++) {
      if (cs[i].kind != "content" || !cs[i].visible || has_visible_content_below[i]) {
        continue;
      }
      if (cs[i].window >= 0 && static_cast<std::size_t>(cs[i].window) < s.windows.size() &&
          is_system_window(s.windows[static_cast<std::size_t>(cs[i].window)])) {
        continue;
      }
      const auto it = view_by_controller.find(cs[i].id);
      if (it == view_by_controller.end()) continue;
      LayoutScreen sc;
      sc.name = cs[i].display;
      sc.basis = "view_controller";
      sc.on_screen = view_on_screen(views[static_cast<std::size_t>(it->second)]);
      sc.root_view = it->second;
      add_screen(std::move(sc));
    }
    if (r.screens.empty()) {
      // No controller tree to go by: each shown window is the screen.
      for (const auto& w : s.windows) {
        if (w.hidden || w.root_view < 0 || is_system_window(w)) continue;
        LayoutScreen sc;
        sc.name = w.cls;
        sc.basis = "window";
        sc.on_screen = true;
        sc.root_view = w.root_view;
        add_screen(std::move(sc));
      }
    }
  }

  // React Native: host views, and the screens react-native-screens mounts.
  int rn_index = 0;
  for (std::size_t i = 0; i < n; i++) {
    const auto& v = views[i];
    if (is_rn_host_view(v, s.platform)) {
      r.react_native.host_views++;
      if (s.platform == model::Platform::kIos) {
        if (v.display == "RCTViewComponentView" ||
            v.display == "RCTSurfaceHostingProxyRootView" ||
            v.display == "RCTSurfaceView") {
          r.react_native.architecture = "fabric";
        } else if (r.react_native.architecture == "unknown" &&
                   (v.display == "RCTView" || v.display == "RCTRootView" ||
                    v.display == "RCTRootContentView")) {
          r.react_native.architecture = "paper";
        }
      }
    }
    if (!is_rn_screen_view(v, s.platform)) continue;
    r.react_native.screens_mounted++;
    if (view_on_screen(v)) r.react_native.screens_on_screen++;
    LayoutScreen sc;
    rn_index++;
    sc.name = !v.identifier.empty()
                  ? v.identifier
                  : "react-native screen #" + std::to_string(rn_index);
    sc.basis = "react_native_screen";
    sc.on_screen = view_on_screen(v);
    sc.root_view = as_index(i);
    // The nearest enclosing screen, so a reader can tell the counts nest.
    for (int p = v.parent; p >= 0; p = views[static_cast<std::size_t>(p)].parent) {
      const auto it = screen_by_root.find(p);
      if (it != screen_by_root.end()) {
        sc.inside = it->second;
        break;
      }
    }
    add_screen(std::move(sc));
  }
  r.react_native.detected = r.react_native.host_views > 0;
  if (s.platform == model::Platform::kAndroid && r.react_native.detected) {
    r.react_native.architecture = "unknown";
  }

  // Navigation.
  for (const auto& c : s.controllers) {
    if (c.kind != "navigation") continue;
    LayoutNavigation nav;
    nav.container = c.display;
    nav.kind = "navigation_controller";
    nav.on_screen = c.visible;
    nav.screens = c.stack;
    r.navigation.push_back(std::move(nav));
  }
  if (!s.fragments.empty()) {
    LayoutNavigation nav;
    nav.container = s.activity;
    nav.kind = "fragments";
    nav.on_screen = true;
    nav.screens = s.fragments;
    r.navigation.push_back(std::move(nav));
  }

  // Observations: heuristics, each with its threshold in the message.
  const auto& t = thresholds;
  if (s.truncated) {
    r.observations.push_back(
        {"tree_truncated",
         "the probe stopped at " + std::to_string(s.max_views) +
             " views; the screen holds more, and every count here is a "
             "lower bound"});
  }
  for (const auto& sc : r.screens) {
    if (!sc.on_screen) continue;
    if (sc.depth > t.depth) {
      r.observations.push_back(
          {"deep_screen",
           sc.name + ": views nest " + std::to_string(sc.depth) +
               " levels below the screen's root (threshold " +
               std::to_string(t.depth) + "). Each level is another layout "
               "pass; on React Native it is usually wrapper Views"});
    }
    if (sc.views > t.views_per_screen) {
      r.observations.push_back(
          {"heavy_screen",
           sc.name + ": " + std::to_string(sc.views) + " views (threshold " +
               std::to_string(t.views_per_screen) + ")"});
    }
  }
  for (const auto& nav : r.navigation) {
    if (nav.screens.size() > t.stack_screens) {
      r.observations.push_back(
          {"deep_stack",
           nav.container + " holds " + std::to_string(nav.screens.size()) +
               " screens (threshold " + std::to_string(t.stack_screens) +
               "); every screen below the top stays in memory"});
    }
  }
  const auto rn_hidden = r.react_native.screens_mounted - r.react_native.screens_on_screen;
  if (rn_hidden > static_cast<std::int64_t>(t.stack_screens)) {
    r.observations.push_back(
        {"many_mounted_screens",
         std::to_string(r.react_native.screens_mounted) +
             " react-native screens are mounted and " +
             std::to_string(r.react_native.screens_on_screen) +
             " are showing (threshold " + std::to_string(t.stack_screens) +
             " mounted but hidden): the rest keep their whole view trees"});
  }
  const auto not_shown = r.hidden_views + r.offscreen_views;
  if (r.views >= t.hidden_share_min_views &&
      static_cast<double>(not_shown) > t.hidden_share * static_cast<double>(r.views)) {
    r.observations.push_back(
        {"mostly_not_shown",
         std::to_string(not_shown) + " of " + std::to_string(r.views) +
             " views are hidden or off screen (threshold " +
             std::to_string(static_cast<int>(t.hidden_share * 100)) +
             "%): mounted and laid out, but not seen"});
  }
  return r;
}

namespace {

json::Value classes_json(const std::vector<ClassCount>& cs) {
  json::Value a = json::Value::array();
  for (const auto& c : cs) {
    json::Value o = json::Value::object();
    o.set("class", json::Value::string(c.name));
    o.set("count", json::Value::integer(c.count));
    a.push_back(std::move(o));
  }
  return a;
}

json::Value strings_json(const std::vector<std::string>& ss) {
  json::Value a = json::Value::array();
  for (const auto& s : ss) a.push_back(json::Value::string(s));
  return a;
}

const char* basis_sentence(LayoutSource s) {
  switch (s) {
    case LayoutSource::kIosInjectedProbe:
      return "read by the layout probe, a library injected into the app at "
             "launch on the simulator; the app's main thread paused while the "
             "tree was read";
    case LayoutSource::kAndroidDumpsys:
      return "read from `dumpsys activity top`, which prints the top "
             "activity's view hierarchy; nothing ran inside the app";
  }
  return "";
}

}  // namespace

json::Value LayoutReport::to_json(bool include_tree) const {
  const auto& s = snapshot;
  json::Value o = json::Value::object();
  o.set("kind", json::Value::string("layout_snapshot"));
  o.set("source", json::Value::string(to_string(s.source)));
  o.set("platform", json::Value::string(model::to_string(s.platform)));
  o.set("device_id", json::Value::string(s.device_id));
  o.set("app_identifier", json::Value::string(s.app_identifier));
  o.set("pid", json::Value::integer(s.pid));
  o.set("taken_at_unix_ms", json::Value::integer(s.taken_at_unix_ms));
  if (!s.activity.empty()) o.set("activity", json::Value::string(s.activity));
  o.set("basis", json::Value::string(basis_sentence(s.source)));
  o.set("not_a_measurement",
        json::Value::string("A layout snapshot is one instant of the view "
                            "tree; it says how the screen is built, not how "
                            "fast. Observations are heuristics."));
  json::Value screen = json::Value::object();
  screen.set("width", json::Value::number(s.screen_w));
  screen.set("height", json::Value::number(s.screen_h));
  screen.set("scale", json::Value::number(s.scale));
  o.set("screen", std::move(screen));

  json::Value totals = json::Value::object();
  totals.set("windows", json::Value::integer(static_cast<std::int64_t>(s.windows.size())));
  totals.set("views", json::Value::integer(views));
  totals.set("visible_views", json::Value::integer(visible_views));
  totals.set("hidden_views", json::Value::integer(hidden_views));
  totals.set("offscreen_views", json::Value::integer(offscreen_views));
  totals.set("max_depth", json::Value::integer(max_depth));
  totals.set("truncated", json::Value::boolean(s.truncated));
  if (s.max_views > 0) totals.set("max_views", json::Value::integer(s.max_views));
  o.set("totals", std::move(totals));

  json::Value scr = json::Value::array();
  for (const auto& sc : screens) {
    json::Value e = json::Value::object();
    e.set("name", json::Value::string(sc.name));
    e.set("basis", json::Value::string(sc.basis));
    e.set("on_screen", json::Value::boolean(sc.on_screen));
    e.set("inside", sc.inside >= 0 ? json::Value::integer(sc.inside) : json::Value::null());
    e.set("root_view", json::Value::integer(sc.root_view));
    e.set("views", json::Value::integer(sc.views));
    e.set("visible_views", json::Value::integer(sc.visible_views));
    e.set("hidden_views", json::Value::integer(sc.hidden_views));
    e.set("offscreen_views", json::Value::integer(sc.offscreen_views));
    e.set("depth", json::Value::integer(sc.depth));
    e.set("absolute_depth", json::Value::integer(sc.absolute_depth));
    e.set("single_child_wrappers", json::Value::integer(sc.single_child_wrappers));
    e.set("top_classes", classes_json(sc.top_classes));
    scr.push_back(std::move(e));
  }
  o.set("screens", std::move(scr));

  json::Value nav = json::Value::array();
  for (const auto& nv : navigation) {
    json::Value e = json::Value::object();
    e.set("container", json::Value::string(nv.container));
    e.set("kind", json::Value::string(nv.kind));
    e.set("on_screen", json::Value::boolean(nv.on_screen));
    e.set("screens", strings_json(nv.screens));
    nav.push_back(std::move(e));
  }
  o.set("navigation", std::move(nav));

  json::Value rn = json::Value::object();
  rn.set("detected", json::Value::boolean(react_native.detected));
  rn.set("architecture", json::Value::string(react_native.architecture));
  rn.set("host_views", json::Value::integer(react_native.host_views));
  rn.set("screens_mounted", json::Value::integer(react_native.screens_mounted));
  rn.set("screens_on_screen", json::Value::integer(react_native.screens_on_screen));
  rn.set("basis", json::Value::string(
                      "class names: React Native's host views and "
                      "react-native-screens' screen views. A navigator that "
                      "does not use react-native-screens mounts no such view"));
  o.set("react_native", std::move(rn));

  json::Value th = json::Value::object();
  th.set("depth", json::Value::integer(thresholds.depth));
  th.set("views_per_screen", json::Value::integer(thresholds.views_per_screen));
  th.set("stack_screens", json::Value::integer(static_cast<std::int64_t>(thresholds.stack_screens)));
  th.set("hidden_share", json::Value::number(thresholds.hidden_share));
  o.set("thresholds", std::move(th));

  json::Value obs = json::Value::array();
  for (const auto& ob : observations) {
    json::Value e = json::Value::object();
    e.set("code", json::Value::string(ob.code));
    e.set("message", json::Value::string(ob.message));
    obs.push_back(std::move(e));
  }
  o.set("observations", std::move(obs));
  if (!s.fragments.empty()) o.set("fragments", strings_json(s.fragments));
  o.set("warnings", strings_json(s.warnings));

  if (include_tree) {
    json::Value tree = json::Value::array();
    for (const auto& v : s.views) {
      json::Value e = json::Value::object();
      e.set("parent", json::Value::integer(v.parent));
      e.set("depth", json::Value::integer(v.depth));
      e.set("window", json::Value::integer(v.window));
      e.set("class", json::Value::string(v.cls));
      if (v.display != v.cls) e.set("display", json::Value::string(v.display));
      json::Value f = json::Value::array();
      f.push_back(json::Value::number(v.x));
      f.push_back(json::Value::number(v.y));
      f.push_back(json::Value::number(v.w));
      f.push_back(json::Value::number(v.h));
      e.set("frame", std::move(f));
      if (v.hidden) e.set("hidden", json::Value::boolean(true));
      if (v.alpha < 1.0) e.set("alpha", json::Value::number(v.alpha));
      if (v.effectively_hidden) e.set("effectively_hidden", json::Value::boolean(true));
      if (v.offscreen) e.set("offscreen", json::Value::boolean(true));
      if (!v.controller.empty()) e.set("controller", json::Value::string(v.controller));
      if (!v.identifier.empty()) e.set("identifier", json::Value::string(v.identifier));
      tree.push_back(std::move(e));
    }
    o.set("tree", std::move(tree));
  }
  return o;
}

std::string LayoutReport::to_text(bool include_tree) const {
  const auto& s = snapshot;
  std::ostringstream out;
  out << "Layout of " << s.app_identifier;
  if (!s.device_id.empty()) out << " on " << s.device_id;
  out << " (" << to_string(s.source) << ")\n";
  if (!s.activity.empty()) out << "activity " << s.activity << "\n";
  out << "pid " << s.pid << " · " << s.windows.size() << " window(s) · " << views
      << " views (" << visible_views << " visible, " << hidden_views
      << " hidden, " << offscreen_views << " off screen) · deepest " << max_depth
      << "\n";
  out << "note: " << basis_sentence(s.source)
      << ". A snapshot of how the screen is built, not a performance measurement.\n";

  out << "\nSCREENS\n";
  out << "  " << pad("SHOWN", 6) << pad("VIEWS", 7, true) << pad("VISIBLE", 9, true)
      << pad("HIDDEN", 8, true) << pad("OFFSCR", 8, true) << pad("DEPTH", 7, true)
      << pad("WRAPPERS", 10, true) << "  NAME\n";
  for (std::size_t i = 0; i < screens.size(); i++) {
    const auto& sc = screens[i];
    out << "  " << pad(sc.on_screen ? "yes" : "no", 6)
        << pad(std::to_string(sc.views), 7, true)
        << pad(std::to_string(sc.visible_views), 9, true)
        << pad(std::to_string(sc.hidden_views), 8, true)
        << pad(std::to_string(sc.offscreen_views), 8, true)
        << pad(std::to_string(sc.depth), 7, true)
        << pad(std::to_string(sc.single_child_wrappers), 10, true) << "  "
        << sc.name << "  [" << sc.basis;
    if (sc.inside >= 0) out << ", inside #" << sc.inside;
    out << "] #" << i << "\n";
    if (!sc.top_classes.empty()) {
      out << "        top: ";
      for (std::size_t k = 0; k < sc.top_classes.size() && k < 5; k++) {
        if (k > 0) out << " · ";
        out << sc.top_classes[k].name << " " << sc.top_classes[k].count;
      }
      out << "\n";
    }
  }
  if (screens.empty()) out << "  (no screen could be identified)\n";

  out << "\nNAVIGATION\n";
  if (navigation.empty()) out << "  (no navigation controller or fragment list)\n";
  for (const auto& nv : navigation) {
    out << "  " << nv.container << " (" << nv.kind
        << (nv.on_screen ? ", on screen" : ", not on screen") << "): "
        << nv.screens.size() << " screen(s)\n";
    // A navigation controller's stack has a top; a fragment manager's added
    // list is not ordered that way, so nothing in it is called the top.
    const bool stack = nv.kind == "navigation_controller";
    for (std::size_t k = 0; k < nv.screens.size(); k++) {
      out << "    " << (k + 1) << ". " << nv.screens[k]
          << (stack && k + 1 == nv.screens.size() ? "  <- top" : "") << "\n";
    }
  }

  out << "\nREACT NATIVE\n";
  if (!react_native.detected) {
    out << "  not detected (no React Native host views in the tree)\n";
  } else {
    out << "  detected, architecture " << react_native.architecture << ": "
        << react_native.host_views << " host views, "
        << react_native.screens_mounted << " react-native-screens screen(s) mounted, "
        << react_native.screens_on_screen << " showing\n";
  }

  out << "\nOBSERVATIONS (heuristics, thresholds stated)\n";
  if (observations.empty()) out << "  (none)\n";
  for (const auto& ob : observations) out << "  - " << ob.message << "\n";
  for (const auto& w : s.warnings) out << "warning: " << w << "\n";

  if (include_tree) {
    out << "\nTREE\n";
    for (const auto& v : s.views) {
      out << "  " << std::string(static_cast<std::size_t>(v.depth) * 2, ' ')
          << v.display << " [" << fmt_num(v.x) << "," << fmt_num(v.y) << " "
          << fmt_num(v.w) << "x" << fmt_num(v.h) << "]";
      if (!v.controller.empty()) out << " vc=" << v.controller;
      if (!v.identifier.empty()) out << " id=" << v.identifier;
      if (v.effectively_hidden) {
        out << " hidden";
      } else if (v.offscreen) {
        out << " offscreen";
      }
      out << "\n";
    }
  }
  return out.str();
}

}  // namespace mpi::observe
