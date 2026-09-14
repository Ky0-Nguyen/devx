// The heap dump parser, the reference-path search, and DET-06.
//
// Every dump here is built byte by byte in this file. That is deliberate and
// it is labelled: a heap dump of a real app is tens of megabytes, which has no
// place in a repository, and the cases worth testing -- a destroyed object
// still held, a superclass described after its own instances, a truncated file
// -- cannot be produced on demand from a running app anyway.
//
// The real 49 MB dump this was developed against is not committed. What it
// verified is recorded in `docs/known-limitations.md`: 580,140 objects,
// 292,343 roots, zero unrecognised records, and one real retention chain
// through `ReactHostImpl.defaultHardwareBackBtnHandler`.
#include <sstream>

#include "adapters/android/hprof_parser.hpp"
#include "core/rules/engine.hpp"
#include "core/symbols/symbol_service.hpp"
#include "tests/unit/test_framework.hpp"

using namespace mpi;

namespace {

// Builds HPROF 1.0.3 bytes with 4-byte identifiers.
class HprofBuilder {
 public:
  HprofBuilder() {
    out_ += "JAVA PROFILE 1.0.3";
    out_ += '\0';
    u4(4);        // identifier size
    u4(0); u4(0); // 64-bit timestamp
  }

  std::uint32_t string(const std::string& text) {
    const std::uint32_t id = next_id();
    record(0x01, [&] { u4(id); out_ += text; });
    return id;
  }

  std::uint32_t load_class(std::uint32_t class_id, const std::string& name) {
    const std::uint32_t name_id = string(name);
    record(0x02, [&] {
      u4(1);          // class serial
      u4(class_id);
      u4(0);          // stack trace serial
      u4(name_id);
    });
    return class_id;
  }

  struct Field {
    std::uint32_t name_id;
    std::uint8_t type;  // 2 = object, 4 = boolean
  };

  // A CLASS_DUMP. `statics` are {name id, target id} object references.
  void class_dump(std::uint32_t class_id, std::uint32_t super_id,
                  std::uint32_t instance_size,
                  const std::vector<Field>& fields,
                  const std::vector<std::pair<std::uint32_t, std::uint32_t>>&
                      statics = {}) {
    heap_sub([&] {
      u1(0x20);
      u4(class_id);
      u4(0);          // stack trace serial
      u4(super_id);
      u4(0); u4(0); u4(0); u4(0); u4(0);  // loader, signers, domain, 2 reserved
      u4(instance_size);
      u2(0);          // constant pool
      u2(static_cast<std::uint16_t>(statics.size()));
      for (const auto& [name_id, target] : statics) {
        u4(name_id);
        u1(2);        // object
        u4(target);
      }
      u2(static_cast<std::uint16_t>(fields.size()));
      for (const auto& f : fields) {
        u4(f.name_id);
        u1(f.type);
      }
    });
  }

  // An INSTANCE_DUMP whose field block is built from `values` in the order the
  // class chain declares them: subclass first, as HPROF requires.
  void instance(std::uint32_t object_id, std::uint32_t class_id,
                const std::string& field_block) {
    heap_sub([&] {
      u1(0x21);
      u4(object_id);
      u4(0);
      u4(class_id);
      u4(static_cast<std::uint32_t>(field_block.size()));
      out_ += field_block;
    });
  }

  void object_array(std::uint32_t object_id, std::uint32_t class_id,
                    const std::vector<std::uint32_t>& elements) {
    heap_sub([&] {
      u1(0x22);
      u4(object_id);
      u4(0);
      u4(static_cast<std::uint32_t>(elements.size()));
      u4(class_id);
      for (const auto e : elements) u4(e);
    });
  }

  void primitive_array(std::uint32_t object_id, std::uint8_t type,
                       std::uint32_t count) {
    heap_sub([&] {
      u1(0x23);
      u4(object_id);
      u4(0);
      u4(count);
      u1(type);
      const int width = type == 8 ? 1 : type == 10 ? 4 : 8;
      for (std::uint32_t i = 0; i < count * static_cast<std::uint32_t>(width);
           ++i) {
        u1(0);
      }
    });
  }

  void root(std::uint8_t tag, std::uint32_t object_id) {
    heap_sub([&] {
      u1(tag);
      u4(object_id);
      // The root kinds used here carry only an id, except java_frame, which
      // carries a thread serial and a frame number.
      if (tag == 0x03) { u4(7); u4(0); }
      if (tag == 0x01) { u4(0); }  // jni global: the ref itself
    });
  }

  // Android's HEAP_DUMP_INFO: everything after it is on that heap.
  void heap_info(std::uint32_t heap_type, const std::string& name) {
    const std::uint32_t name_id = string(name);
    heap_sub([&] {
      u1(0xfe);
      u4(heap_type);
      u4(name_id);
    });
  }

  // Emits a raw sub-record, for the malformed cases.
  void raw_sub(const std::string& bytes) {
    heap_sub([&] { out_ += bytes; });
  }

  std::string finish() {
    flush_heap();
    record(0x2c, [] {});  // HEAP_DUMP_END
    return out_;
  }

  // Everything written so far, cut short mid-record.
  std::string truncated(std::size_t keep) {
    flush_heap();
    return out_.substr(0, keep);
  }

  std::uint32_t next_id() { return ++id_; }

 private:
  void u1(std::uint8_t v) { out_ += static_cast<char>(v); }
  void u2(std::uint16_t v) {
    out_ += static_cast<char>((v >> 8) & 0xff);
    out_ += static_cast<char>(v & 0xff);
  }
  void u4(std::uint32_t v) {
    for (int i = 3; i >= 0; --i) {
      out_ += static_cast<char>((v >> (i * 8)) & 0xff);
    }
  }

  template <typename Fn>
  void record(std::uint8_t tag, Fn&& body) {
    flush_heap();
    out_ += static_cast<char>(tag);
    u4(0);  // time delta
    const std::size_t len_at = out_.size();
    u4(0);  // length, patched below
    const std::size_t start = out_.size();
    body();
    patch_u4(len_at, static_cast<std::uint32_t>(out_.size() - start));
  }

  // Heap sub-records accumulate into one segment, the way a real dump does.
  template <typename Fn>
  void heap_sub(Fn&& body) {
    if (!in_heap_) {
      out_ += static_cast<char>(0x1c);
      u4(0);
      heap_len_at_ = out_.size();
      u4(0);
      heap_start_ = out_.size();
      in_heap_ = true;
    }
    body();
  }

  void flush_heap() {
    if (!in_heap_) return;
    patch_u4(heap_len_at_, static_cast<std::uint32_t>(out_.size() - heap_start_));
    in_heap_ = false;
  }

  void patch_u4(std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      out_[at + static_cast<std::size_t>(i)] =
          static_cast<char>((v >> ((3 - i) * 8)) & 0xff);
    }
  }

  std::string out_;
  std::uint32_t id_ = 0x1000;
  bool in_heap_ = false;
  std::size_t heap_len_at_ = 0;
  std::size_t heap_start_ = 0;
};

std::string be4(std::uint32_t v) {
  std::string s;
  for (int i = 3; i >= 0; --i) s += static_cast<char>((v >> (i * 8)) & 0xff);
  return s;
}

heap::HeapGraph parse(const std::string& bytes,
                      android::HprofReadResult* out_result = nullptr) {
  heap::HeapGraph g;
  android::HprofLimits limits;
  const auto r = android::read_hprof_bytes(bytes, limits,
                                           CancellationToken::none(), g);
  if (out_result != nullptr) *out_result = r;
  return g;
}

}  // namespace

MPI_TEST(a_non_hprof_file_is_refused, {"D05", "A12"}) {
  android::HprofReadResult r;
  parse("not a heap dump at all", &r);
  MPI_CHECK(!r.ok);
  MPI_CHECK(r.error.find("not an HPROF file") != std::string::npos);
}

MPI_TEST(an_unsupported_identifier_size_is_refused_not_guessed, {"D05"}) {
  // A width other than 4 or 8 would make every reference in the file a
  // misread. Guessing one is how a graph of plausible nonsense gets built.
  std::string bytes = "JAVA PROFILE 1.0.3";
  bytes += '\0';
  bytes += be4(3);
  bytes += be4(0);
  bytes += be4(0);
  android::HprofReadResult r;
  parse(bytes, &r);
  MPI_CHECK(!r.ok);
  MPI_CHECK(r.error.find("identifier size 3") != std::string::npos);
  MPI_CHECK(r.error.find("guess") != std::string::npos);
}

MPI_TEST(a_reference_chain_is_read_from_the_dump, {"F10", "DET-06"}) {
  HprofBuilder b;
  const auto holder_class = b.load_class(0x100, "com.example.Holder");
  const auto held_class = b.load_class(0x200, "com.example.Held");
  const auto field = b.string("theThing");
  b.class_dump(holder_class, 0, 16, {{field, 2}});
  b.class_dump(held_class, 0, 8, {});
  b.instance(0x300, held_class, "");
  b.instance(0x400, holder_class, be4(0x300));
  b.root(0x03, 0x400);  // a java frame holds the holder

  const auto g = parse(b.finish());
  MPI_CHECK_EQ(g.objects().size(), std::size_t{2});
  const auto path = g.path_from_root(0x300);
  MPI_CHECK(path.found);
  if (!path.found) return;
  MPI_CHECK(path.root.kind == heap::RootKind::kJavaFrame);
  MPI_CHECK(heap::root_is_app_meaningful(path.root.kind));
  MPI_CHECK_EQ(path.steps.size(), std::size_t{1});
  if (path.steps.empty()) return;
  MPI_CHECK_EQ(path.steps.front().holder_class, std::string("com.example.Holder"));
  MPI_CHECK_EQ(path.steps.front().via_field, std::string("theThing"));
  MPI_CHECK_EQ(path.target_class, std::string("com.example.Held"));
  // The root carries a thread serial, which is what makes a java-frame root
  // worth naming.
  MPI_CHECK(path.root.thread_serial.has_value());
}

MPI_TEST(an_unreachable_object_is_garbage_not_retention, {"H01", "DET-06"}) {
  // The distinction this rule turns on. An object no root reaches was not
  // being held by anything -- it is garbage the collector had not swept.
  HprofBuilder b;
  const auto c = b.load_class(0x100, "com.example.Orphan");
  b.class_dump(c, 0, 8, {});
  b.instance(0x300, c, "");
  const auto g = parse(b.finish());
  const auto path = g.path_from_root(0x300);
  MPI_CHECK(!path.found);
  MPI_CHECK(!path.truncated);
  MPI_CHECK(path.not_found_reason.find("garbage") != std::string::npos);
  MPI_CHECK_MSG(path.not_found_reason.find("not yet swept") != std::string::npos
                    || path.not_found_reason.find("had not yet swept") !=
                           std::string::npos,
                "the reason must say the collector had not swept it");
}

MPI_TEST(an_object_not_in_the_dump_yields_no_claim, {"H01"}) {
  HprofBuilder b;
  const auto g = parse(b.finish());
  const auto path = g.path_from_root(0x999);
  MPI_CHECK(!path.found);
  MPI_CHECK(path.not_found_reason.find("not in this dump") != std::string::npos);
}

MPI_TEST(a_superclass_described_after_its_instance_is_still_read,
         {"D05", "DET-06"}) {
  // The bug that made this parser two-pass, found on a real dump: HPROF does
  // not guarantee a class appears before instances of its subclasses. A
  // single pass walked the inheritance chain until it met a class it had not
  // read, then stopped -- silently dropping every field below that point. On
  // the real capture that cost 37 of one object's 59 references.
  HprofBuilder b;
  const auto base = b.load_class(0x100, "com.example.Base");
  const auto derived = b.load_class(0x200, "com.example.Derived");
  const auto target = b.load_class(0x300, "com.example.Target");
  const auto derived_field = b.string("fromDerived");
  const auto base_field = b.string("fromBase");

  // Derived's own class dump and its instance come first; Base's arrives
  // afterwards, which is the order that used to break.
  b.class_dump(derived, base, 16, {{derived_field, 2}});
  b.class_dump(target, 0, 8, {});
  b.instance(0x500, target, "");
  b.instance(0x600, target, "");
  // Subclass fields first, then the superclass's.
  b.instance(0x700, derived, be4(0x500) + be4(0x600));
  b.class_dump(base, 0, 8, {{base_field, 2}});
  b.root(0x03, 0x700);

  const auto g = parse(b.finish());
  const heap::Object* holder = g.find_object(0x700);
  MPI_CHECK(holder != nullptr);
  if (holder == nullptr) return;
  MPI_CHECK_MSG(holder->references.size() == 2,
                "both the subclass's and the superclass's references are read");
  // And the superclass's field is reachable by name, which is what a path
  // needs to be readable.
  const auto path = g.path_from_root(0x600);
  MPI_CHECK(path.found);
  if (path.found && !path.steps.empty()) {
    MPI_CHECK_EQ(path.steps.front().via_field, std::string("fromBase"));
  }
}

MPI_TEST(an_incomplete_chain_is_counted_not_ignored, {"D05", "H01"}) {
  // A class this dump never describes at all: the fields below it cannot be
  // read, and that has to be reported rather than passed over -- a missing
  // reference is a missing edge, and an edge is what a path is made of.
  HprofBuilder b;
  const auto derived = b.load_class(0x200, "com.example.Derived");
  const auto field = b.string("ref");
  b.class_dump(derived, 0x999 /* never described */, 16, {{field, 2}});
  b.instance(0x700, derived, be4(0x500) + be4(0x600));
  const auto g = parse(b.finish());
  bool says_so = false;
  for (const auto& note : g.limits.notes) {
    if (note.find("superclass this dump never described") != std::string::npos) {
      says_so = true;
    }
  }
  MPI_CHECK(says_so);
}

MPI_TEST(a_truncated_dump_reports_what_it_got, {"D05", "D08"}) {
  HprofBuilder b;
  const auto c = b.load_class(0x100, "com.example.Thing");
  b.class_dump(c, 0, 8, {});
  b.instance(0x300, c, "");
  const auto whole = b.finish();
  android::HprofReadResult r;
  const auto g = parse(whole.substr(0, whole.size() - 12), &r);
  MPI_CHECK_MSG(g.limits.truncated_by_byte_cap ||
                    !g.limits.notes.empty(),
                "a dump that ends inside a record says so");
  bool says_incomplete = false;
  for (const auto& note : g.limits.notes) {
    if (note.find("ends inside a record") != std::string::npos ||
        note.find("incomplete") != std::string::npos) {
      says_incomplete = true;
    }
  }
  MPI_CHECK(says_incomplete);
}

MPI_TEST(an_unknown_field_type_abandons_the_rest_of_its_segment, {"D05"}) {
  // An unknown field type makes every following offset unknown, and a class
  // record carries no length -- so there is no way to skip just that record.
  // The rest of the segment is lost with it, and the honest thing is to say
  // so: a dump has thousands of segments, so the loss is bounded but real.
  HprofBuilder b;
  const auto bad = b.load_class(0x100, "com.example.Bad");
  const auto good = b.load_class(0x200, "com.example.Good");
  const auto field = b.string("f");
  // Type 99 is not a defined HPROF type.
  b.class_dump(bad, 0, 8, {{field, 99}});
  b.class_dump(good, 0, 8, {});
  b.instance(0x300, good, "");  // same segment: lost with it
  // A LOAD_CLASS record closes the segment, so what follows is a new one.
  const auto later = b.load_class(0x400, "com.example.Later");
  b.class_dump(later, 0, 8, {});
  b.instance(0x500, later, "");

  const auto g = parse(b.finish());
  MPI_CHECK(g.limits.unrecognised_records > 0);
  bool says_abandoned = false;
  for (const auto& note : g.limits.notes) {
    if (note.find("abandoned at a field type") != std::string::npos) {
      says_abandoned = true;
    }
  }
  MPI_CHECK_MSG(says_abandoned,
                "the graph must report the segment it could not read");
  MPI_CHECK_MSG(g.find_object(0x300) == nullptr,
                "the record after the bad one is genuinely lost");
  MPI_CHECK_MSG(g.find_object(0x500) != nullptr,
                "a later segment still parses: one bad class does not cost "
                "the whole graph");
}

MPI_TEST(a_primitive_array_is_named_from_its_element_type, {"D05", "F07"}) {
  // HPROF gives a primitive array no class id, only an element type. Leaving
  // it unresolved put 132,239 objects of a real 49 MB dump under "(class name
  // unresolved)", which reads as a parser that failed.
  HprofBuilder b;
  b.primitive_array(0x300, 8, 64);   // byte[64]
  b.primitive_array(0x400, 10, 16);  // int[16]
  const auto g = parse(b.finish());
  const auto hist = g.instance_histogram();
  MPI_CHECK(hist.count("byte[]") == 1);
  MPI_CHECK(hist.count("int[]") == 1);
  MPI_CHECK(hist.count("(class name unresolved)") == 0);
  const heap::Object* arr = g.find_object(0x300);
  MPI_CHECK(arr != nullptr);
  if (arr != nullptr) MPI_CHECK_EQ(arr->shallow_size, std::int64_t{64});
}

MPI_TEST(an_array_element_path_names_its_index, {"F10"}) {
  HprofBuilder b;
  const auto arr_class = b.load_class(0x100, "java.lang.Object[]");
  const auto held = b.load_class(0x200, "com.example.Held");
  b.class_dump(held, 0, 8, {});
  b.instance(0x300, held, "");
  b.object_array(0x400, arr_class, {0, 0, 0x300});
  b.root(0x01, 0x400);
  const auto g = parse(b.finish());
  const auto path = g.path_from_root(0x300);
  MPI_CHECK(path.found);
  if (!path.found || path.steps.empty()) return;
  MPI_CHECK(path.steps.front().via_index.has_value());
  MPI_CHECK_EQ(path.steps.front().via_index.value_or(-1), std::int64_t{2});
  // A null element is not an edge, so the earlier slots do not appear.
  const heap::Object* arr = g.find_object(0x400);
  MPI_CHECK(arr != nullptr);
  if (arr != nullptr) MPI_CHECK_EQ(arr->references.size(), std::size_t{1});
}

MPI_TEST(an_app_root_is_preferred_over_vm_bookkeeping, {"F10", "DET-06"}) {
  // Both roots reach the object. One says the app's own thread holds it; the
  // other says the runtime interned something. Reporting the second as the
  // reason would say the app retains an object it does not.
  HprofBuilder b;
  const auto holder = b.load_class(0x100, "com.example.Holder");
  const auto held = b.load_class(0x200, "com.example.Held");
  const auto f = b.string("thing");
  b.class_dump(holder, 0, 16, {{f, 2}});
  b.class_dump(held, 0, 8, {});
  b.instance(0x300, held, "");
  b.instance(0x400, holder, be4(0x300));
  b.instance(0x500, holder, be4(0x300));
  b.root(0x8d, 0x400);  // vm internal
  b.root(0x03, 0x500);  // java frame
  const auto g = parse(b.finish());
  const auto path = g.path_from_root(0x300);
  MPI_CHECK(path.found);
  if (!path.found) return;
  MPI_CHECK_MSG(path.root.kind == heap::RootKind::kJavaFrame,
                "the app-meaningful root wins at equal depth");
  MPI_CHECK(heap::root_is_app_meaningful(path.root.kind));
}

MPI_TEST(objects_are_attributed_to_their_heap, {"F07", "B07"}) {
  // An object on the boot image heap is shared with every process on the
  // device and is not the app's to release.
  HprofBuilder b;
  const auto c = b.load_class(0x100, "com.example.Thing");
  b.class_dump(c, 0, 8, {});
  b.heap_info(65, "app");
  b.instance(0x300, c, "");
  b.heap_info(73, "image");
  b.instance(0x400, c, "");
  b.heap_info(90, "zygote");
  b.instance(0x500, c, "");
  const auto g = parse(b.finish());
  MPI_CHECK(g.find_object(0x300)->heap == heap::HeapSpace::kApp);
  MPI_CHECK(g.find_object(0x400)->heap == heap::HeapSpace::kImage);
  MPI_CHECK(g.find_object(0x500)->heap == heap::HeapSpace::kZygote);
}

MPI_TEST(a_static_field_is_an_edge_a_path_can_run_through, {"F10", "DET-06"}) {
  // The most common way an app keeps something for the life of the process.
  // A class object is an object, and a static field has to be a real edge or
  // the chain that explains the retention is invisible.
  HprofBuilder b;
  const auto holder = b.load_class(0x100, "com.example.Registry");
  const auto held = b.load_class(0x200, "com.example.Held");
  const auto static_name = b.string("INSTANCE");
  b.class_dump(held, 0, 8, {});
  b.class_dump(holder, 0, 8, {}, {{static_name, 0x300}});
  b.instance(0x300, held, "");
  b.root(0x05, 0x100);  // the class is a sticky-class root
  const auto g = parse(b.finish());
  const auto path = g.path_from_root(0x300);
  MPI_CHECK(path.found);
  if (!path.found || path.steps.empty()) return;
  MPI_CHECK(path.root.kind == heap::RootKind::kStickyClass);
  MPI_CHECK_EQ(path.steps.front().via_field, std::string("static INSTANCE"));
}

MPI_TEST(instances_of_a_base_class_are_found_through_subclasses,
         {"DET-06"}) {
  // Nothing is ever an instance of `android.app.Activity` itself. An
  // exact-name lookup finds none and reads as "no activities in this heap".
  HprofBuilder b;
  const auto activity = b.load_class(0x100, "android.app.Activity");
  const auto middle = b.load_class(0x200, "androidx.appcompat.app.AppCompatActivity");
  const auto mine = b.load_class(0x300, "com.example.MainActivity");
  b.class_dump(activity, 0, 8, {});
  b.class_dump(middle, activity, 8, {});
  b.class_dump(mine, middle, 8, {});
  b.instance(0x400, mine, "");
  const auto g = parse(b.finish());
  MPI_CHECK_EQ(g.instances_of("android.app.Activity").size(), std::size_t{0});
  std::vector<std::string> matched;
  const auto found = g.instances_assignable_to("android.app.Activity", &matched);
  MPI_CHECK_EQ(found.size(), std::size_t{1});
  MPI_CHECK_EQ(matched.size(), std::size_t{1});
  if (!matched.empty()) {
    MPI_CHECK_EQ(matched.front(), std::string("com.example.MainActivity"));
  }
}

MPI_TEST(a_cyclic_superclass_chain_does_not_hang, {"D05"}) {
  HprofBuilder b;
  const auto a = b.load_class(0x100, "com.example.A");
  const auto c = b.load_class(0x200, "com.example.B");
  b.class_dump(a, c, 8, {});
  b.class_dump(c, a, 8, {});  // a cycle a corrupt dump can describe
  b.instance(0x300, a, "");
  const auto g = parse(b.finish());
  // The point is that this returns at all.
  MPI_CHECK(g.instances_assignable_to("com.example.Z").empty());
  MPI_CHECK_EQ(g.instances_assignable_to("com.example.A").size(), std::size_t{1});
}

// --- DET-06 -----------------------------------------------------------------

namespace {

// A dump with one Activity subclass whose `mDestroyed` is whatever the caller
// says, held by a static field on a registry class.
std::string activity_dump(bool destroyed, bool rooted = true) {
  HprofBuilder b;
  const auto activity = b.load_class(0x100, "android.app.Activity");
  const auto mine = b.load_class(0x200, "com.example.MainActivity");
  const auto registry = b.load_class(0x300, "com.example.Leaky");
  const auto destroyed_field = b.string("mDestroyed");
  const auto finished_field = b.string("mFinished");
  const auto static_name = b.string("sLastActivity");

  b.class_dump(activity, 0, 16, {{destroyed_field, 4}, {finished_field, 4}});
  b.class_dump(mine, activity, 16, {});
  b.class_dump(registry, 0, 8, {}, {{static_name, 0x500}});
  // Two booleans, in the order android.app.Activity declares them.
  std::string block;
  block += static_cast<char>(destroyed ? 1 : 0);
  block += static_cast<char>(0);
  b.instance(0x500, mine, block);
  if (rooted) b.root(0x05, 0x300);  // the registry class is a sticky root
  return b.finish();
}

model::NormalizedTrace bare_trace() {
  model::NormalizedTrace t;
  t.session_id = "heap-test";
  t.primary_clock_domain = "android.boottime.ns";
  t.window_start_ns = 1000;
  t.window_end_ns = 2000;
  return t;
}

const model::RuleRunRecord* record_for(const model::AnalysisResult& r,
                                       const std::string& id) {
  for (const auto& rec : r.rule_runs) {
    if (rec.rule_id == id) return &rec;
  }
  return nullptr;
}

const model::Issue* issue_for(const model::AnalysisResult& r,
                              const std::string& id) {
  for (const auto& i : r.issues) {
    if (i.rule_id == id) return &i;
  }
  return nullptr;
}

model::AnalysisResult analyze_with(const heap::HeapGraph* g) {
  const auto trace = bare_trace();
  symbols::SymbolService symbols;
  rules::EngineOptions opts;
  opts.mode = model::MeasurementMode::kDiagnostic;
  opts.heap_graph = g;
  return rules::analyze(trace, symbols, opts);
}

bool has_text(const std::vector<std::string>& v, const std::string& needle) {
  for (const auto& s : v) {
    if (s.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

MPI_TEST(det06_without_a_dump_refuses_the_substitute, {"DET-06", "H05"}) {
  const auto r = analyze_with(nullptr);
  const auto* rec = record_for(r, "DET-06");
  MPI_CHECK(rec != nullptr);
  if (rec == nullptr) return;
  MPI_CHECK(rec->outcome == model::RuleOutcome::kSkipped);
  MPI_CHECK(has_text(rec->skipped_reasons, "no heap dump was collected"));
  MPI_CHECK_MSG(has_text(rec->skipped_reasons, "never by what"),
                "memory counters say how much is held, never by what");
}

MPI_TEST(det06_reports_a_destroyed_object_that_is_still_held,
         {"DET-06", "F10"}) {
  auto g = parse(activity_dump(/*destroyed=*/true));
  g.gc_requested_before_dump = true;
  const auto r = analyze_with(&g);
  const auto* issue = issue_for(r, "DET-06");
  MPI_CHECK(issue != nullptr);
  if (issue == nullptr) return;

  MPI_CHECK(issue->title.find("mDestroyed") != std::string::npos);
  // The chain is read from the dump, so detection is observed; whether
  // holding it is a fault is not measured at all.
  MPI_CHECK(issue->detection_status == model::DetectionStatus::kObserved);
  MPI_CHECK(issue->cause_status == model::CauseStatus::kCandidate);
  MPI_CHECK(issue->severity == model::Severity::kMedium);
  MPI_CHECK(issue->threshold_origin.find("platform_contract") !=
            std::string::npos);

  // The word this rule must never use.
  const std::string all = issue->title + issue->confidence_basis +
                          issue->severity_rationale;
  MPI_CHECK_MSG(all.find("leak") == std::string::npos,
                "DET-06 never says 'leak': reachable is not leaked");

  // The chain itself, with the field that holds the object.
  MPI_CHECK(!issue->candidate_stacks.empty());
  if (!issue->candidate_stacks.empty()) {
    const auto& note = issue->candidate_stacks.front().note;
    MPI_CHECK(note.find("static sLastActivity") != std::string::npos);
    MPI_CHECK(note.find("com.example.MainActivity") != std::string::npos);
  }

  // Shallow size, named as shallow, with retained size named as absent.
  bool shallow_named = false;
  bool retained_refused = false;
  for (const auto& m : issue->metrics) {
    if (m.name.find("shallow") != std::string::npos) shallow_named = true;
    for (const auto& lim : m.limitations) {
      if (lim.find("retained size is not computed") != std::string::npos) {
        retained_refused = true;
      }
    }
  }
  MPI_CHECK(shallow_named);
  MPI_CHECK_MSG(retained_refused,
                "the number people want is retained size, and giving them "
                "shallow size under that name would be the worst thing here");

  MPI_CHECK(has_text(issue->missing_evidence, "retained size"));
  MPI_CHECK(has_text(issue->missing_evidence, "a second dump"));
  MPI_CHECK(has_text(issue->alternative_explanations, "by design"));
  MPI_CHECK(has_text(issue->alternative_explanations, "framework-private"));
}

MPI_TEST(det06_says_nothing_about_a_live_object, {"DET-06", "H11"}) {
  // The same dump with `mDestroyed` false: a reachable Activity that has not
  // been destroyed is just a running screen.
  auto g = parse(activity_dump(/*destroyed=*/false));
  g.gc_requested_before_dump = true;
  const auto r = analyze_with(&g);
  MPI_CHECK(issue_for(r, "DET-06") == nullptr);
  const auto* rec = record_for(r, "DET-06");
  MPI_CHECK(rec != nullptr);
  if (rec == nullptr) return;
  MPI_CHECK(rec->outcome == model::RuleOutcome::kSkipped);
  MPI_CHECK_MSG(has_text(rec->skipped_reasons, "no object with a lifecycle "
                                               "state this build knows how to "
                                               "check"),
                "the skip names what it looked for");
}

MPI_TEST(det06_calls_an_unrooted_destroyed_object_garbage, {"DET-06", "H01"}) {
  // Destroyed and unreachable is the opposite of retention.
  auto g = parse(activity_dump(/*destroyed=*/true, /*rooted=*/false));
  g.gc_requested_before_dump = true;
  const auto r = analyze_with(&g);
  MPI_CHECK_MSG(issue_for(r, "DET-06") == nullptr,
                "an object nothing holds is not being retained");
  const auto* rec = record_for(r, "DET-06");
  MPI_CHECK(rec != nullptr);
  if (rec == nullptr) return;
  MPI_CHECK(has_text(rec->skipped_reasons, "garbage awaiting collection"));
}

MPI_TEST(det06_qualifies_a_dump_taken_without_a_collection, {"DET-06", "H01"}) {
  auto g = parse(activity_dump(/*destroyed=*/true));
  g.gc_requested_before_dump = false;
  const auto r = analyze_with(&g);
  const auto* issue = issue_for(r, "DET-06");
  MPI_CHECK(issue != nullptr);
  if (issue == nullptr) return;
  MPI_CHECK_MSG(has_text(issue->missing_evidence, "a collection before the "
                                                  "dump"),
                "without a collection first, presence may just be unswept");
  const auto* rec = record_for(r, "DET-06");
  MPI_CHECK(rec != nullptr);
  if (rec != nullptr) {
    MPI_CHECK(has_text(rec->skipped_reasons, "not have been swept"));
  }
}

MPI_TEST(det06_says_when_only_the_runtime_holds_the_object,
         {"DET-06", "F10"}) {
  // A chain anchored in VM bookkeeping says the runtime holds the object, not
  // that the app does, and the finding has to draw that line.
  HprofBuilder b;
  const auto activity = b.load_class(0x100, "android.app.Activity");
  const auto mine = b.load_class(0x200, "com.example.MainActivity");
  const auto holder = b.load_class(0x300, "java.lang.ref.FinalizerReference");
  const auto destroyed_field = b.string("mDestroyed");
  const auto finished_field = b.string("mFinished");
  const auto f = b.string("referent");
  b.class_dump(activity, 0, 16, {{destroyed_field, 4}, {finished_field, 4}});
  b.class_dump(mine, activity, 16, {});
  b.class_dump(holder, 0, 8, {{f, 2}});
  std::string block;
  block += static_cast<char>(1);
  block += static_cast<char>(0);
  b.instance(0x500, mine, block);
  b.instance(0x600, holder, be4(0x500));
  b.root(0x8d, 0x600);  // vm internal only

  auto g = parse(b.finish());
  g.gc_requested_before_dump = true;
  const auto r = analyze_with(&g);
  const auto* issue = issue_for(r, "DET-06");
  MPI_CHECK(issue != nullptr);
  if (issue == nullptr) return;
  MPI_CHECK(has_text(issue->missing_evidence,
                     "a root in the app's own code"));
}
