// An object graph from a heap dump, and the one question it can answer.
//
// A heap dump is a photograph of one instant. It can say, with certainty,
// *that* an object was reachable and *by what chain of references* -- and that
// chain is real evidence, read from the dump rather than inferred. What it
// cannot say is that the object leaked. Reachable is not leaked: a cache, a
// singleton, an object pool and a framework-retained instance are all
// reachable by design, and none of them is a defect.
//
// So this layer deliberately answers one question and refuses the neighbouring
// ones:
//
//   answers      "what chain of references keeps this object alive?"
//   refuses      "is this a leak?"        -- not a property of one dump
//   refuses      "what is its retained size?" -- needs a dominator tree, and
//                an approximation presented as a size would be a number
//                nobody could check. Shallow size is reported instead, named
//                as shallow.
//
// Platform-neutral on purpose: Android HPROF fills it in today, and nothing
// here knows that. The parser's job is the file format; this is the graph.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/util/json.hpp"

namespace mpi::heap {

using ObjectId = std::uint64_t;

// Why the collector considered an object a root. After Android's own dump
// these are specific; a dump converted by `hprof-conv` collapses most of them
// to `kUnknown`, which is worth knowing because it decides how informative a
// path's anchor is.
enum class RootKind {
  kUnknown,
  kJniGlobal,
  kJniLocal,
  kJavaFrame,
  kNativeStack,
  kStickyClass,
  kThreadBlock,
  kMonitorUsed,
  kThreadObject,
  kInternedString,
  kFinalizing,
  kDebugger,
  kReferenceCleanup,
  kVmInternal,
  kJniMonitor,
};
const char* to_string(RootKind k);
// Whether a root of this kind says anything about the app's own code. A
// thread's stack frame holding an object is a fact about the app; the VM
// interning a string is not.
bool root_is_app_meaningful(RootKind k);

enum class ObjectKind { kInstance, kObjectArray, kPrimitiveArray, kClass };
const char* to_string(ObjectKind k);

// Which heap an object lives on. Android dumps separate the app's heap from
// the zygote and boot image, and the difference matters: an object on the
// image heap is shared with every process on the device and is not the app's
// to release.
enum class HeapSpace { kUnknown, kApp, kZygote, kImage };
const char* to_string(HeapSpace s);

struct Reference {
  // Empty for an array element; the array index is in `index` instead.
  std::string field_name;
  std::optional<std::int64_t> index;
  ObjectId target = 0;
};

struct Object {
  ObjectId id = 0;
  ObjectId class_id = 0;
  ObjectKind kind = ObjectKind::kInstance;
  HeapSpace heap = HeapSpace::kUnknown;
  // Bytes this object itself occupies. Not what it retains: an object that
  // holds a 40 MB bitmap has a shallow size of a few dozen bytes.
  std::int64_t shallow_size = 0;
  std::vector<Reference> references;
  // Named boolean fields the parser was asked to keep.
  //
  // Only a requested few, because the point is narrow: the difference between
  // "an Activity is reachable" -- which is unremarkable -- and "an Activity
  // that reports itself destroyed is reachable", which is a lifecycle
  // expectation the platform itself documents. Keeping every primitive field
  // of 345,000 instances to answer one question would be a poor trade.
  std::vector<std::pair<std::string, bool>> flags;

  // The flag's value, or absent when this object does not carry it. Absent is
  // not false: a class without the field and an object whose field is false
  // are different facts, and the second is the only one that supports a
  // conclusion.
  std::optional<bool> flag(const std::string& name) const;
};

struct ClassInfo {
  ObjectId id = 0;
  std::string name;
  ObjectId super_id = 0;
  std::int64_t instance_size = 0;
};

struct Root {
  ObjectId object = 0;
  RootKind kind = RootKind::kUnknown;
  // Present for the root kinds that name one: a thread's frame, a monitor.
  std::optional<std::int64_t> thread_serial;
};

// One hop of a reference chain, in the direction a reader thinks in: from the
// root towards the object.
struct PathStep {
  ObjectId holder = 0;
  std::string holder_class;
  // How the holder points at the next object: a field name, or an index.
  std::string via_field;
  std::optional<std::int64_t> via_index;
};

struct ReferencePath {
  bool found = false;
  // Why not, when it was not. "Unreachable" is itself a finding -- an object
  // in the dump that no root reaches is garbage the collector had not yet
  // swept, not a retained object.
  std::string not_found_reason;
  Root root;
  std::string root_class;
  std::vector<PathStep> steps;
  ObjectId target = 0;
  std::string target_class;
  // True when the search stopped at its own bound rather than exhausting the
  // graph, in which case "no path" means "none within the bound".
  bool truncated = false;
  json::Value to_json() const;
};

// What the parse could not do. Every field here is a reason a conclusion
// drawn from this graph is narrower than it looks.
struct GraphLimits {
  bool truncated_by_object_cap = false;
  bool truncated_by_byte_cap = false;
  std::int64_t objects_dropped = 0;
  std::int64_t unresolved_class_names = 0;
  std::int64_t unrecognised_records = 0;
  // References whose target is not in the graph: a dangling id, usually
  // because the dump was truncated or an object was dropped by a cap.
  std::int64_t dangling_references = 0;
  std::vector<std::string> notes;
  json::Value to_json() const;
};

class HeapGraph {
 public:
  void add_class(ClassInfo c);
  void add_object(Object o);
  void add_root(Root r);

  const ClassInfo* find_class(ObjectId id) const;
  const Object* find_object(ObjectId id) const;
  std::string class_name_of(ObjectId object_id) const;

  const std::vector<Root>& roots() const { return roots_; }
  const std::unordered_map<ObjectId, Object>& objects() const { return objects_; }
  std::size_t class_count() const { return classes_.size(); }

  // Instances per class name, for finding the classes worth asking about.
  // Counts instances present in the dump: not an allocation count, and not a
  // live-object count if the collector had not run.
  std::map<std::string, std::int64_t> instance_histogram() const;
  std::vector<ObjectId> instances_of(const std::string& class_name) const;
  // Instances of `class_name` or of anything that extends it.
  //
  // The distinction matters for every question worth asking: nothing is ever
  // an instance of `android.app.Activity` itself, only of a subclass, so an
  // exact-name lookup finds nothing and reads as "no activities in this
  // heap". `matched_subclasses` reports which classes the walk accepted, so
  // the answer names what it counted.
  std::vector<ObjectId> instances_assignable_to(
      const std::string& class_name,
      std::vector<std::string>* matched_subclasses = nullptr) const;

  // The shortest chain of references from any root to `target`.
  //
  // Shortest, not "the" path: an object is usually held by several chains, and
  // the shortest is the one a reader can follow, not the only one. Searched
  // backwards from the target so the cost is the neighbourhood of the object
  // rather than the whole heap.
  //
  // `prefer_app_meaningful` returns a path anchored in the app's own code --
  // a thread frame, a JNI global -- in preference to one anchored in a VM
  // internal, when both exist within the bound.
  ReferencePath path_from_root(ObjectId target, std::size_t max_visited = 2'000'000,
                               bool prefer_app_meaningful = true) const;

  GraphLimits limits;
  // Provenance: what produced this graph, and when the dump was taken.
  std::string provider;
  std::string source_path;
  std::int64_t dump_time_ms = 0;
  // A dump taken without a preceding collection contains garbage that is
  // simply unswept. Whether one was requested is the caller's to state.
  bool gc_requested_before_dump = false;

  json::Value summary_json() const;

 private:
  void build_reverse_index() const;

  std::unordered_map<ObjectId, ClassInfo> classes_;
  std::unordered_map<ObjectId, Object> objects_;
  std::vector<Root> roots_;
  // Built on first path query: who points at whom, with the field.
  mutable bool reverse_built_ = false;
  mutable std::unordered_map<ObjectId, std::vector<ObjectId>> holders_;
  mutable std::unordered_map<ObjectId, RootKind> root_kinds_;
};

}  // namespace mpi::heap
