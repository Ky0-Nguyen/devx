#include "core/heap/heap_graph.hpp"

#include <algorithm>
#include <deque>
#include <unordered_set>

namespace mpi::heap {

const char* to_string(RootKind k) {
  switch (k) {
    case RootKind::kUnknown: return "unknown";
    case RootKind::kJniGlobal: return "jni_global";
    case RootKind::kJniLocal: return "jni_local";
    case RootKind::kJavaFrame: return "java_frame";
    case RootKind::kNativeStack: return "native_stack";
    case RootKind::kStickyClass: return "sticky_class";
    case RootKind::kThreadBlock: return "thread_block";
    case RootKind::kMonitorUsed: return "monitor_used";
    case RootKind::kThreadObject: return "thread_object";
    case RootKind::kInternedString: return "interned_string";
    case RootKind::kFinalizing: return "finalizing";
    case RootKind::kDebugger: return "debugger";
    case RootKind::kReferenceCleanup: return "reference_cleanup";
    case RootKind::kVmInternal: return "vm_internal";
    case RootKind::kJniMonitor: return "jni_monitor";
  }
  return "unknown";
}

bool root_is_app_meaningful(RootKind k) {
  switch (k) {
    // These say something about the app: its threads hold the object, its
    // native code holds a global reference, a class it loaded is sticky.
    case RootKind::kJniGlobal:
    case RootKind::kJniLocal:
    case RootKind::kJavaFrame:
    case RootKind::kNativeStack:
    case RootKind::kThreadObject:
    case RootKind::kThreadBlock:
    case RootKind::kStickyClass:
    case RootKind::kMonitorUsed:
    case RootKind::kJniMonitor:
      return true;
    // These are the runtime's own bookkeeping. An object rooted only here is
    // held by the VM, and saying "the app retains it" would be wrong.
    case RootKind::kInternedString:
    case RootKind::kVmInternal:
    case RootKind::kFinalizing:
    case RootKind::kDebugger:
    case RootKind::kReferenceCleanup:
    case RootKind::kUnknown:
      return false;
  }
  return false;
}

const char* to_string(ObjectKind k) {
  switch (k) {
    case ObjectKind::kInstance: return "instance";
    case ObjectKind::kObjectArray: return "object_array";
    case ObjectKind::kPrimitiveArray: return "primitive_array";
    case ObjectKind::kClass: return "class";
  }
  return "instance";
}

const char* to_string(HeapSpace s) {
  switch (s) {
    case HeapSpace::kUnknown: return "unknown";
    case HeapSpace::kApp: return "app";
    case HeapSpace::kZygote: return "zygote";
    case HeapSpace::kImage: return "image";
  }
  return "unknown";
}

json::Value GraphLimits::to_json() const {
  json::Value o = json::Value::object();
  o.set("truncated_by_object_cap",
        json::Value::boolean(truncated_by_object_cap));
  o.set("truncated_by_byte_cap", json::Value::boolean(truncated_by_byte_cap));
  o.set("objects_dropped", json::Value::integer(objects_dropped));
  o.set("unresolved_class_names", json::Value::integer(unresolved_class_names));
  o.set("unrecognised_records", json::Value::integer(unrecognised_records));
  o.set("dangling_references", json::Value::integer(dangling_references));
  json::Value n = json::Value::array();
  for (const auto& note : notes) n.push_back(json::Value::string(note));
  o.set("notes", std::move(n));
  return o;
}

json::Value ReferencePath::to_json() const {
  json::Value o = json::Value::object();
  o.set("found", json::Value::boolean(found));
  if (!not_found_reason.empty()) {
    o.set("not_found_reason", json::Value::string(not_found_reason));
  }
  o.set("truncated", json::Value::boolean(truncated));
  o.set("root_kind", json::Value::string(to_string(root.kind)));
  o.set("root_is_app_meaningful",
        json::Value::boolean(root_is_app_meaningful(root.kind)));
  o.set("root_class", json::Value::string(root_class));
  if (root.thread_serial.has_value()) {
    o.set("root_thread_serial", json::Value::integer(*root.thread_serial));
  }
  o.set("target_class", json::Value::string(target_class));
  o.set("hops", json::Value::integer(static_cast<std::int64_t>(steps.size())));
  json::Value s = json::Value::array();
  for (const auto& step : steps) {
    json::Value e = json::Value::object();
    e.set("holder_class", json::Value::string(step.holder_class));
    if (step.via_index.has_value()) {
      e.set("via_index", json::Value::integer(*step.via_index));
    } else {
      e.set("via_field", json::Value::string(step.via_field));
    }
    s.push_back(std::move(e));
  }
  o.set("steps", std::move(s));
  return o;
}

std::optional<bool> Object::flag(const std::string& name) const {
  for (const auto& [key, value] : flags) {
    if (key == name) return value;
  }
  return std::nullopt;
}

void HeapGraph::add_class(ClassInfo c) {
  const ObjectId id = c.id;
  classes_[id] = std::move(c);
}

void HeapGraph::add_object(Object o) {
  const ObjectId id = o.id;
  objects_[id] = std::move(o);
  reverse_built_ = false;
}

void HeapGraph::add_root(Root r) {
  roots_.push_back(r);
  reverse_built_ = false;
}

const ClassInfo* HeapGraph::find_class(ObjectId id) const {
  const auto it = classes_.find(id);
  return it == classes_.end() ? nullptr : &it->second;
}

const Object* HeapGraph::find_object(ObjectId id) const {
  const auto it = objects_.find(id);
  return it == objects_.end() ? nullptr : &it->second;
}

std::string HeapGraph::class_name_of(ObjectId object_id) const {
  const Object* o = find_object(object_id);
  if (o == nullptr) return {};
  // A class object's own "class" is java.lang.Class; its name is its own.
  if (o->kind == ObjectKind::kClass) {
    if (const ClassInfo* self = find_class(object_id)) return self->name;
  }
  if (const ClassInfo* c = find_class(o->class_id)) return c->name;
  return {};
}

std::map<std::string, std::int64_t> HeapGraph::instance_histogram() const {
  std::map<std::string, std::int64_t> out;
  for (const auto& [id, o] : objects_) {
    if (o.kind == ObjectKind::kClass) continue;
    std::string name = class_name_of(id);
    // An object whose class name could not be resolved is counted under a
    // name that says so, never folded into a neighbouring class.
    if (name.empty()) name = "(class name unresolved)";
    ++out[name];
  }
  return out;
}

std::vector<ObjectId> HeapGraph::instances_of(const std::string& class_name) const {
  std::vector<ObjectId> out;
  for (const auto& [id, o] : objects_) {
    if (o.kind == ObjectKind::kClass) continue;
    if (class_name_of(id) == class_name) out.push_back(id);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<ObjectId> HeapGraph::instances_assignable_to(
    const std::string& class_name,
    std::vector<std::string>* matched_subclasses) const {
  // Which class ids satisfy the name, following `super_id` upwards. A chain
  // is walked with a guard rather than trusted: a corrupt dump can describe a
  // cycle, and this must not hang on one.
  std::unordered_map<ObjectId, bool> accepts;
  const auto satisfies = [&](ObjectId id) {
    const auto cached = accepts.find(id);
    if (cached != accepts.end()) return cached->second;
    std::vector<ObjectId> chain;
    bool answer = false;
    ObjectId cur = id;
    int guard = 0;
    while (cur != 0 && guard++ < 256) {
      const auto seen = accepts.find(cur);
      if (seen != accepts.end()) { answer = seen->second; break; }
      const ClassInfo* c = find_class(cur);
      if (c == nullptr) break;
      chain.push_back(cur);
      if (c->name == class_name) { answer = true; break; }
      cur = c->super_id;
    }
    for (const ObjectId step : chain) accepts[step] = answer;
    accepts[id] = answer;
    return answer;
  };

  std::vector<ObjectId> out;
  std::vector<std::string> matched;
  for (const auto& [id, o] : objects_) {
    if (o.kind == ObjectKind::kClass) continue;
    if (!satisfies(o.class_id)) continue;
    out.push_back(id);
    if (matched_subclasses != nullptr) {
      if (const ClassInfo* c = find_class(o.class_id)) {
        if (std::find(matched.begin(), matched.end(), c->name) == matched.end()) {
          matched.push_back(c->name);
        }
      }
    }
  }
  std::sort(out.begin(), out.end());
  if (matched_subclasses != nullptr) {
    std::sort(matched.begin(), matched.end());
    *matched_subclasses = std::move(matched);
  }
  return out;
}

void HeapGraph::build_reverse_index() const {
  if (reverse_built_) return;
  holders_.clear();
  root_kinds_.clear();
  for (const auto& [id, o] : objects_) {
    for (const auto& ref : o.references) {
      if (ref.target == 0) continue;
      holders_[ref.target].push_back(id);
    }
  }
  for (const auto& r : roots_) {
    // A root recorded twice keeps the more informative kind: the app-
    // meaningful ones are what a path is worth anchoring on.
    const auto it = root_kinds_.find(r.object);
    if (it == root_kinds_.end() ||
        (!root_is_app_meaningful(it->second) && root_is_app_meaningful(r.kind))) {
      root_kinds_[r.object] = r.kind;
    }
  }
  reverse_built_ = true;
}

ReferencePath HeapGraph::path_from_root(ObjectId target, std::size_t max_visited,
                                        bool prefer_app_meaningful) const {
  ReferencePath out;
  out.target = target;
  out.target_class = class_name_of(target);
  if (find_object(target) == nullptr) {
    out.not_found_reason =
        "the object is not in this dump, so nothing can be said about what "
        "kept it alive";
    return out;
  }
  build_reverse_index();

  // Breadth-first backwards from the target. The first root reached is the
  // shortest chain; with `prefer_app_meaningful` the search keeps going to
  // the end of that same distance, so a VM-internal root does not win over a
  // thread frame at the same depth.
  // Discovered-from, keyed by the node discovered: for each holder the search
  // reaches, which object it was holding. Keying it the other way -- child to
  // holder -- looks natural and is wrong, because a child has many holders and
  // the last one written is not the one on the path to the root that won. That
  // produced paths whose first step did not match their own root.
  std::unordered_map<ObjectId, ObjectId> toward_target;
  std::unordered_set<ObjectId> seen{target};
  std::deque<ObjectId> frontier{target};
  std::size_t visited = 0;

  ObjectId best_root = 0;
  bool best_is_app = false;
  std::size_t best_depth = 0;
  std::size_t depth = 0;

  while (!frontier.empty()) {
    if (best_root != 0 && depth > best_depth) break;
    std::deque<ObjectId> next;
    for (const ObjectId id : frontier) {
      if (++visited > max_visited) {
        out.truncated = true;
        break;
      }
      const auto rk = root_kinds_.find(id);
      if (rk != root_kinds_.end()) {
        const bool app = root_is_app_meaningful(rk->second);
        if (best_root == 0 || (prefer_app_meaningful && app && !best_is_app)) {
          best_root = id;
          best_is_app = app;
          best_depth = depth;
          out.root.object = id;
          out.root.kind = rk->second;
          for (const auto& r : roots_) {
            if (r.object == id && r.thread_serial.has_value()) {
              out.root.thread_serial = r.thread_serial;
              break;
            }
          }
          // An app-meaningful root at this depth is the best answer there is;
          // nothing deeper can beat it and nothing at this depth can either.
          if (app) break;
        }
      }
      const auto it = holders_.find(id);
      if (it == holders_.end()) continue;
      for (const ObjectId holder : it->second) {
        if (!seen.insert(holder).second) continue;
        toward_target[holder] = id;
        next.push_back(holder);
      }
    }
    if (out.truncated) break;
    if (best_is_app) break;
    frontier = std::move(next);
    ++depth;
  }

  if (best_root == 0) {
    out.not_found_reason =
        out.truncated
            ? "no root was found within the search bound, so this says "
              "nothing about whether one exists further out"
            : "no root reaches this object: it was unreachable when the dump "
              "was taken, which makes it garbage the collector had not yet "
              "swept rather than something being retained";
    return out;
  }

  // Walk forward from the root to the target, which is the direction a reader
  // follows: this holds that, which holds that.
  out.found = true;
  out.root_class = class_name_of(best_root);
  // Walk down from the root, which is the direction the map records.
  std::vector<ObjectId> chain{best_root};
  {
    ObjectId cur = best_root;
    std::size_t guard = 0;
    while (cur != target && guard++ <= seen.size()) {
      const auto it = toward_target.find(cur);
      if (it == toward_target.end()) break;
      cur = it->second;
      chain.push_back(cur);
    }
    if (chain.back() != target) {
      // The chain did not close. Reporting a partial chain as a path would
      // describe references that do not connect what it claims to connect.
      out.found = false;
      out.steps.clear();
      out.not_found_reason =
          "a root was reached but the chain back to the object could not be "
          "reconstructed, so no path is reported rather than a partial one";
      return out;
    }
  }

  for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
    const Object* holder = find_object(chain[i]);
    if (holder == nullptr) continue;
    PathStep step;
    step.holder = chain[i];
    step.holder_class = class_name_of(chain[i]);
    for (const auto& ref : holder->references) {
      if (ref.target != chain[i + 1]) continue;
      step.via_field = ref.field_name;
      step.via_index = ref.index;
      break;
    }
    if (step.via_field.empty() && !step.via_index.has_value()) {
      // The edge exists -- it is how we got here -- but the field could not
      // be named. Said plainly rather than left blank.
      step.via_field = "(field not recorded in the dump)";
    }
    out.steps.push_back(std::move(step));
  }
  return out;
}

json::Value HeapGraph::summary_json() const {
  json::Value o = json::Value::object();
  o.set("provider", json::Value::string(provider));
  o.set("source_path", json::Value::string(source_path));
  o.set("dump_time_ms", json::Value::integer(dump_time_ms));
  // Whether a collection was requested first decides whether unreachable
  // objects in this dump mean anything.
  o.set("gc_requested_before_dump",
        json::Value::boolean(gc_requested_before_dump));
  o.set("classes", json::Value::integer(static_cast<std::int64_t>(classes_.size())));
  o.set("objects", json::Value::integer(static_cast<std::int64_t>(objects_.size())));
  o.set("roots", json::Value::integer(static_cast<std::int64_t>(roots_.size())));
  std::int64_t app_meaningful = 0;
  for (const auto& r : roots_) {
    if (root_is_app_meaningful(r.kind)) ++app_meaningful;
  }
  o.set("app_meaningful_roots", json::Value::integer(app_meaningful));
  // Objects per heap. An object on the boot image heap is shared with every
  // process on the device and is not the app's to release, so a total across
  // heaps would not be the app's memory.
  std::map<std::string, std::int64_t> by_heap;
  for (const auto& [id, obj] : objects_) {
    static_cast<void>(id);
    ++by_heap[to_string(obj.heap)];
  }
  json::Value heaps = json::Value::object();
  for (const auto& [name, n] : by_heap) {
    heaps.set(name, json::Value::integer(n));
  }
  o.set("objects_by_heap", std::move(heaps));
  o.set("limits", limits.to_json());
  return o;
}

}  // namespace mpi::heap
