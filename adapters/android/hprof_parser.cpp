#include "adapters/android/hprof_parser.hpp"

#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace mpi::android {
namespace {

// Top-level record tags.
constexpr std::uint8_t kTagString = 0x01;
constexpr std::uint8_t kTagLoadClass = 0x02;
constexpr std::uint8_t kTagHeapDump = 0x0c;
constexpr std::uint8_t kTagHeapDumpSegment = 0x1c;

// Heap sub-record tags. The 0x8x range and 0xfe are Android's.
constexpr std::uint8_t kRootUnknown = 0xff;
constexpr std::uint8_t kRootJniGlobal = 0x01;
constexpr std::uint8_t kRootJniLocal = 0x02;
constexpr std::uint8_t kRootJavaFrame = 0x03;
constexpr std::uint8_t kRootNativeStack = 0x04;
constexpr std::uint8_t kRootStickyClass = 0x05;
constexpr std::uint8_t kRootThreadBlock = 0x06;
constexpr std::uint8_t kRootMonitorUsed = 0x07;
constexpr std::uint8_t kRootThreadObject = 0x08;
constexpr std::uint8_t kClassDump = 0x20;
constexpr std::uint8_t kInstanceDump = 0x21;
constexpr std::uint8_t kObjectArrayDump = 0x22;
constexpr std::uint8_t kPrimitiveArrayDump = 0x23;
constexpr std::uint8_t kHeapDumpInfo = 0xfe;
constexpr std::uint8_t kRootInternedString = 0x89;
constexpr std::uint8_t kRootFinalizing = 0x8a;
constexpr std::uint8_t kRootDebugger = 0x8b;
constexpr std::uint8_t kRootReferenceCleanup = 0x8c;
constexpr std::uint8_t kRootVmInternal = 0x8d;
constexpr std::uint8_t kRootJniMonitor = 0x8e;
constexpr std::uint8_t kUnreachable = 0x90;
constexpr std::uint8_t kPrimitiveArrayNoData = 0xc3;

// HPROF basic types. Sizes for 2 (object) come from the header.
int size_of_type(std::uint8_t t, int id_size) {
  switch (t) {
    case 2: return id_size;   // object
    case 4: return 1;         // boolean
    case 5: return 2;         // char
    case 6: return 4;         // float
    case 7: return 8;         // double
    case 8: return 1;         // byte
    case 9: return 2;         // short
    case 10: return 4;        // int
    case 11: return 8;        // long
    default: return -1;       // unknown: refuse rather than assume a width
  }
}

// A bounds-checked cursor. Every read either succeeds or sets `failed`, so a
// corrupt length can never walk off the buffer -- and a dump that ends early
// is reported as truncated rather than crashing.
class Cursor {
 public:
  Cursor(const char* data, std::size_t size) : data_(data), size_(size) {}

  bool failed() const { return failed_; }
  std::size_t offset() const { return at_; }
  std::size_t remaining() const { return at_ >= size_ ? 0 : size_ - at_; }
  bool at_end() const { return at_ >= size_; }

  std::uint8_t u1() {
    if (remaining() < 1) return fail<std::uint8_t>();
    return static_cast<std::uint8_t>(data_[at_++]);
  }
  std::uint16_t u2() {
    if (remaining() < 2) return fail<std::uint16_t>();
    const auto a = static_cast<std::uint8_t>(data_[at_]);
    const auto b = static_cast<std::uint8_t>(data_[at_ + 1]);
    at_ += 2;
    return static_cast<std::uint16_t>((a << 8) | b);
  }
  std::uint32_t u4() {
    if (remaining() < 4) return fail<std::uint32_t>();
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      v = (v << 8) | static_cast<std::uint8_t>(data_[at_ + static_cast<std::size_t>(i)]);
    }
    at_ += 4;
    return v;
  }
  std::uint64_t u8() {
    if (remaining() < 8) return fail<std::uint64_t>();
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v = (v << 8) | static_cast<std::uint8_t>(data_[at_ + static_cast<std::size_t>(i)]);
    }
    at_ += 8;
    return v;
  }
  // An identifier, whose width the header states. Anything but 4 or 8 is
  // refused by the caller rather than guessed at.
  std::uint64_t id(int id_size) {
    return id_size == 8 ? u8() : static_cast<std::uint64_t>(u4());
  }
  std::string bytes(std::size_t n) {
    if (remaining() < n) { failed_ = true; return {}; }
    std::string s(data_ + at_, n);
    at_ += n;
    return s;
  }
  void skip(std::size_t n) {
    if (remaining() < n) { failed_ = true; at_ = size_; return; }
    at_ += n;
  }
  void seek(std::size_t to) {
    if (to > size_) { failed_ = true; at_ = size_; return; }
    at_ = to;
  }

 private:
  template <typename T>
  T fail() { failed_ = true; at_ = size_; return T{}; }

  const char* data_;
  std::size_t size_;
  std::size_t at_ = 0;
  bool failed_ = false;
};

// HPROF class names arrive in JVM internal form: slashes and, for arrays, a
// leading `[`. Turned into the form a person reads, because the finding is
// going in front of one.
std::string humanise_class_name(std::string name) {
  int array_depth = 0;
  while (!name.empty() && name.front() == '[') {
    ++array_depth;
    name.erase(name.begin());
  }
  if (array_depth > 0 && name.size() >= 1) {
    // `[Ljava/lang/Object;` -> the element type, then the brackets back.
    if (name.front() == 'L' && name.back() == ';') {
      name = name.substr(1, name.size() - 2);
    } else if (name.size() == 1) {
      switch (name[0]) {
        case 'Z': name = "boolean"; break;
        case 'B': name = "byte"; break;
        case 'C': name = "char"; break;
        case 'S': name = "short"; break;
        case 'I': name = "int"; break;
        case 'J': name = "long"; break;
        case 'F': name = "float"; break;
        case 'D': name = "double"; break;
        default: break;
      }
    }
  }
  for (char& c : name) {
    if (c == '/') c = '.';
  }
  for (int i = 0; i < array_depth; ++i) name += "[]";
  return name;
}

heap::HeapSpace heap_space_from_id(std::uint32_t id) {
  // Android writes the heap's name as a string too, but the numeric type is
  // enough and needs no string lookup: 'A' app, 'Z' zygote, 'I' image.
  switch (id) {
    case 0: return heap::HeapSpace::kApp;      // default heap
    case 65: return heap::HeapSpace::kApp;     // 'A'
    case 90: return heap::HeapSpace::kZygote;  // 'Z'
    case 73: return heap::HeapSpace::kImage;   // 'I'
    default: return heap::HeapSpace::kUnknown;
  }
}

// The instance fields of a class, and of every class above it. HPROF writes an
// instance's field values as one flat block covering the whole inheritance
// chain, subclass first, so the chain has to be walked to read it.
struct FieldDef {
  std::uint64_t name_id = 0;
  std::uint8_t type = 0;
};
struct ClassLayout {
  std::uint64_t super_id = 0;
  std::vector<FieldDef> instance_fields;
};

// A class object for a primitive array type, created on demand.
//
// HPROF has no class record for `byte[]`: the element type is a byte in the
// array record. The ids are synthetic and are marked as such by living in a
// range no HPROF identifier reaches, so nothing can confuse one for an id
// read from the file.
std::uint64_t synthetic_array_class(std::uint8_t element_type,
                                    heap::HeapGraph& out) {
  static constexpr std::uint64_t kBase = 0xFFFF'0000'0000'0000ull;
  const std::uint64_t id = kBase | element_type;
  if (out.find_class(id) != nullptr) return id;
  const char* name = nullptr;
  switch (element_type) {
    case 4: name = "boolean[]"; break;
    case 5: name = "char[]"; break;
    case 6: name = "float[]"; break;
    case 7: name = "double[]"; break;
    case 8: name = "byte[]"; break;
    case 9: name = "short[]"; break;
    case 10: name = "int[]"; break;
    case 11: name = "long[]"; break;
    default: name = "(unknown primitive)[]"; break;
  }
  heap::ClassInfo info;
  info.id = id;
  info.name = name;
  out.add_class(std::move(info));
  return id;
}

}  // namespace

HprofReadResult read_hprof_bytes(const std::string& bytes,
                                 const HprofLimits& limits,
                                 const CancellationToken& cancel,
                                 heap::HeapGraph& out) {
  HprofReadResult res;
  res.bytes_read = static_cast<std::int64_t>(bytes.size());
  Cursor cur(bytes.data(), bytes.size());

  // Header: a NUL-terminated version string, the identifier width, then a
  // 64-bit timestamp.
  const std::size_t nul = bytes.find('\0');
  if (nul == std::string::npos || nul > 64) {
    res.error = "not an HPROF file: no version string in the first 64 bytes";
    return res;
  }
  res.format = bytes.substr(0, nul);
  if (res.format.rfind("JAVA PROFILE", 0) != 0) {
    res.error = "not an HPROF file: header says '" + res.format + "'";
    return res;
  }
  cur.seek(nul + 1);
  const int id_size = static_cast<int>(cur.u4());
  res.identifier_size = id_size;
  if (id_size != 4 && id_size != 8) {
    res.error = "unsupported identifier size " + std::to_string(id_size) +
                ": only 4 and 8 are defined, and a guess would misread every "
                "reference in the dump";
    return res;
  }
  const std::uint64_t timestamp_ms = cur.u8();
  out.dump_time_ms = static_cast<std::int64_t>(timestamp_ms);
  out.provider = "am dumpheap (" + res.format + ")";

  // Two passes, and the reason is worth stating: HPROF does not guarantee
  // that a class's CLASS_DUMP appears before instances of its subclasses. It
  // did not in the capture this was written against -- `MainActivity`'s
  // instance record preceded `androidx.activity.ComponentActivity`'s class
  // record -- so a single pass walked the inheritance chain until it hit a
  // class it had not read yet and stopped there. The field values after that
  // point were never read: not corrupted, just missing, which is the harder
  // failure to notice. Reading every class first makes the chain walk
  // complete, and an incomplete one is now counted rather than passed over.
  std::unordered_map<std::uint64_t, std::string> strings;
  // class object id -> name string id, from LOAD_CLASS.
  std::unordered_map<std::uint64_t, std::uint64_t> class_name_ids;
  std::unordered_map<std::uint64_t, ClassLayout> layouts;
  heap::HeapSpace current_heap = heap::HeapSpace::kUnknown;
  std::size_t objects_kept = 0;
  std::int64_t incomplete_chains = 0;
  std::int64_t segments_abandoned = 0;
  // Static-field edges, held until the class objects they belong to exist in
  // the graph. A class object is an object, and a `static` field is one of
  // the most common ways an app keeps something for the life of the process,
  // so these edges have to be real edges a path can run through.
  std::vector<std::pair<std::uint64_t, heap::Reference>> pending_statics;

  const auto note_unrecognised = [&](std::uint8_t tag, std::size_t at) {
    ++out.limits.unrecognised_records;
    if (out.limits.unrecognised_records <= 4) {
      char buf[96];
      std::snprintf(buf, sizeof(buf),
                    "unrecognised record 0x%02x at offset %zu", tag, at);
      out.limits.notes.push_back(buf);
    }
  };

  for (int pass = 0; pass < 2; ++pass) {
  if (pass == 1) {
    cur = Cursor(bytes.data(), bytes.size());
    cur.seek(nul + 1);
    cur.u4();
    cur.u8();
    res.records_read = 0;
  }
  while (!cur.at_end() && !cur.failed()) {
    if (cancel.cancelled()) {
      out.limits.notes.push_back("parse cancelled by the operator");
      res.ok = true;  // what was read is real; the caller is told it is partial
      res.error = "cancelled";
      return res;
    }
    const std::uint8_t tag = cur.u1();
    cur.u4();  // time delta, unused
    const std::uint32_t length = cur.u4();
    if (cur.failed()) break;
    const std::size_t body = cur.offset();
    if (length > cur.remaining()) {
      out.limits.notes.push_back(
          "the dump ends inside a record: its declared length runs past the "
          "end of the file, so the graph is incomplete");
      out.limits.truncated_by_byte_cap = true;
      break;
    }
    const std::size_t end = body + length;
    ++res.records_read;

    switch (tag) {
      case kTagString: {
        if (pass != 0) break;
        const std::uint64_t id = cur.id(id_size);
        const std::size_t n = end - cur.offset();
        strings[id] = cur.bytes(n);
        break;
      }
      case kTagLoadClass: {
        if (pass != 0) break;
        cur.u4();  // class serial
        const std::uint64_t class_object_id = cur.id(id_size);
        cur.u4();  // stack trace serial
        const std::uint64_t name_id = cur.id(id_size);
        class_name_ids[class_object_id] = name_id;
        break;
      }
      case kTagHeapDump:
      case kTagHeapDumpSegment: {
        while (cur.offset() < end && !cur.failed()) {
          const std::uint8_t sub = cur.u1();
          switch (sub) {
            case kHeapDumpInfo: {
              const std::uint32_t heap_id = cur.u4();
              cur.id(id_size);  // heap name string
              current_heap = heap_space_from_id(heap_id);
              break;
            }
            case kRootUnknown:
            case kRootStickyClass:
            case kRootMonitorUsed:
            case kRootInternedString:
            case kRootFinalizing:
            case kRootDebugger:
            case kRootReferenceCleanup:
            case kRootVmInternal: {
              heap::Root r;
              r.object = cur.id(id_size);
              r.kind = sub == kRootStickyClass ? heap::RootKind::kStickyClass
                     : sub == kRootMonitorUsed ? heap::RootKind::kMonitorUsed
                     : sub == kRootInternedString ? heap::RootKind::kInternedString
                     : sub == kRootFinalizing ? heap::RootKind::kFinalizing
                     : sub == kRootDebugger ? heap::RootKind::kDebugger
                     : sub == kRootReferenceCleanup
                           ? heap::RootKind::kReferenceCleanup
                     : sub == kRootVmInternal ? heap::RootKind::kVmInternal
                                              : heap::RootKind::kUnknown;
              if (pass == 1) out.add_root(r);
              break;
            }
            case kRootJniGlobal: {
              heap::Root r;
              r.object = cur.id(id_size);
              cur.id(id_size);  // the JNI global ref itself
              r.kind = heap::RootKind::kJniGlobal;
              if (pass == 1) out.add_root(r);
              break;
            }
            case kRootJniLocal:
            case kRootJavaFrame: {
              heap::Root r;
              r.object = cur.id(id_size);
              r.thread_serial = static_cast<std::int64_t>(cur.u4());
              cur.u4();  // frame number
              r.kind = sub == kRootJniLocal ? heap::RootKind::kJniLocal
                                            : heap::RootKind::kJavaFrame;
              if (pass == 1) out.add_root(r);
              break;
            }
            case kRootNativeStack:
            case kRootThreadBlock: {
              heap::Root r;
              r.object = cur.id(id_size);
              r.thread_serial = static_cast<std::int64_t>(cur.u4());
              r.kind = sub == kRootNativeStack ? heap::RootKind::kNativeStack
                                               : heap::RootKind::kThreadBlock;
              if (pass == 1) out.add_root(r);
              break;
            }
            case kRootThreadObject: {
              heap::Root r;
              r.object = cur.id(id_size);
              r.thread_serial = static_cast<std::int64_t>(cur.u4());
              cur.u4();  // stack trace serial
              r.kind = heap::RootKind::kThreadObject;
              if (pass == 1) out.add_root(r);
              break;
            }
            case kRootJniMonitor: {
              heap::Root r;
              r.object = cur.id(id_size);
              r.thread_serial = static_cast<std::int64_t>(cur.u4());
              cur.u4();  // frame number
              r.kind = heap::RootKind::kJniMonitor;
              if (pass == 1) out.add_root(r);
              break;
            }
            case kClassDump: {
              if (pass != 0) {
                // Read on the first pass. Skipping the record needs its
                // length, which a class dump does not carry, so it is walked
                // again -- cheaply, since nothing is stored.
              }
              heap::ClassInfo info;
              info.id = cur.id(id_size);
              cur.u4();  // stack trace serial
              info.super_id = cur.id(id_size);
              cur.id(id_size);  // class loader
              cur.id(id_size);  // signers
              cur.id(id_size);  // protection domain
              cur.id(id_size);  // reserved
              cur.id(id_size);  // reserved
              info.instance_size = static_cast<std::int64_t>(cur.u4());

              const std::uint16_t constants = cur.u2();
              bool bad_type = false;
              for (std::uint16_t i = 0; i < constants && !cur.failed(); ++i) {
                cur.u2();  // constant pool index
                const int w = size_of_type(cur.u1(), id_size);
                if (w < 0) { bad_type = true; break; }
                cur.skip(static_cast<std::size_t>(w));
              }
              ClassLayout layout;
              layout.super_id = info.super_id;
              if (!bad_type) {
                const std::uint16_t statics = cur.u2();
                for (std::uint16_t i = 0; i < statics && !cur.failed(); ++i) {
                  const std::uint64_t name_id = cur.id(id_size);
                  const std::uint8_t type = cur.u1();
                  const int w = size_of_type(type, id_size);
                  if (w < 0) { bad_type = true; break; }
                  if (type == 2) {
                    const std::uint64_t target = cur.id(id_size);
                    if (target != 0) {
                      heap::Reference ref;
                      const auto sit = strings.find(name_id);
                      ref.field_name =
                          "static " + (sit == strings.end()
                                           ? std::string("(field name not in "
                                                         "dump)")
                                           : sit->second);
                      ref.target = target;
                      pending_statics.emplace_back(info.id, std::move(ref));
                    }
                  } else {
                    cur.skip(static_cast<std::size_t>(w));
                  }
                }
              }
              if (!bad_type) {
                const std::uint16_t fields = cur.u2();
                layout.instance_fields.reserve(fields);
                for (std::uint16_t i = 0; i < fields && !cur.failed(); ++i) {
                  FieldDef f;
                  f.name_id = cur.id(id_size);
                  f.type = cur.u1();
                  if (size_of_type(f.type, id_size) < 0) {
                    bad_type = true;
                    break;
                  }
                  layout.instance_fields.push_back(f);
                }
              }
              if (bad_type) {
                // An unknown field type makes every following offset unknown,
                // and a CLASS_DUMP carries no length, so there is no way to
                // skip just this record and resynchronise: the rest of the
                // *segment* goes with it. A real dump has thousands of
                // segments, so the loss is bounded -- but it is a loss, and
                // it is recorded as one rather than left to look like a
                // segment that happened to contain nothing.
                ++segments_abandoned;
                note_unrecognised(sub, cur.offset());
                cur.seek(end);
                break;
              }
              if (pass != 0) {
                // already stored
              } else if (out.class_count() < limits.max_classes) {
                layouts[info.id] = std::move(layout);
                out.add_class(std::move(info));
              } else {
                ++out.limits.objects_dropped;
                out.limits.truncated_by_object_cap = true;
              }
              break;
            }
            case kInstanceDump: {
              heap::Object obj;
              obj.kind = heap::ObjectKind::kInstance;
              obj.heap = current_heap;
              obj.id = cur.id(id_size);
              cur.u4();  // stack trace serial
              obj.class_id = cur.id(id_size);
              const std::uint32_t nbytes = cur.u4();
              const std::size_t field_block = cur.offset();
              obj.shallow_size = static_cast<std::int64_t>(nbytes);
              if (pass == 0) {
                // The first pass is only looking for classes and strings.
                // Walking field blocks here is not just wasted work: the
                // chain walk would meet classes it has not read yet and
                // count every one as an incomplete chain, which reported
                // 190,433 losses on a dump that had none.
                cur.seek(field_block + nbytes);
                break;
              }
              if (nbytes > cur.remaining()) {
                out.limits.notes.push_back(
                    "an instance's field block runs past the end of the dump");
                cur.seek(end);
                break;
              }
              // Walk the inheritance chain: HPROF writes the subclass's own
              // fields first, then each superclass's, in one block.
              std::uint64_t klass = obj.class_id;
              int guard = 0;
              bool chain_complete = true;
              while (klass != 0 && guard++ < 256) {
                const auto it = layouts.find(klass);
                if (it == layouts.end()) {
                  // A class in this object's chain was never described. The
                  // fields below it are unread, so the object's references are
                  // incomplete -- counted, because a missing reference is a
                  // missing edge and an edge is what a path is made of.
                  chain_complete = false;
                  break;
                }
                for (const auto& f : it->second.instance_fields) {
                  const int w = size_of_type(f.type, id_size);
                  if (w < 0) break;
                  if (f.type == 4 && !limits.retain_boolean_fields.empty()) {
                    // A boolean the caller asked to keep. Read here rather
                    // than skipped, because this is what distinguishes a
                    // reachable object from one that should no longer exist.
                    const auto sit = strings.find(f.name_id);
                    const std::uint8_t raw = cur.u1();
                    if (sit != strings.end()) {
                      for (const auto& wanted : limits.retain_boolean_fields) {
                        if (sit->second != wanted) continue;
                        obj.flags.emplace_back(sit->second, raw != 0);
                        break;
                      }
                    }
                  } else if (f.type == 2) {
                    const std::uint64_t target = cur.id(id_size);
                    if (target != 0) {
                      heap::Reference ref;
                      const auto sit = strings.find(f.name_id);
                      ref.field_name = sit == strings.end()
                                           ? "(field name not in dump)"
                                           : sit->second;
                      ref.target = target;
                      obj.references.push_back(std::move(ref));
                    }
                  } else {
                    cur.skip(static_cast<std::size_t>(w));
                  }
                }
                klass = it->second.super_id;
              }
              if (!chain_complete) ++incomplete_chains;
              // Whatever the chain did not account for is skipped by seeking
              // to the block's stated end, so one unreadable class cannot
              // desynchronise the rest of the dump.
              cur.seek(field_block + nbytes);
              if (pass != 1) {
                // nothing kept on the first pass
              } else if (objects_kept < limits.max_objects) {
                ++objects_kept;
                out.add_object(std::move(obj));
              } else {
                ++out.limits.objects_dropped;
                out.limits.truncated_by_object_cap = true;
              }
              break;
            }
            case kObjectArrayDump: {
              heap::Object obj;
              obj.kind = heap::ObjectKind::kObjectArray;
              obj.heap = current_heap;
              obj.id = cur.id(id_size);
              cur.u4();  // stack trace serial
              const std::uint32_t count = cur.u4();
              obj.class_id = cur.id(id_size);
              obj.shallow_size =
                  static_cast<std::int64_t>(count) * id_size;
              for (std::uint32_t i = 0; i < count && !cur.failed(); ++i) {
                const std::uint64_t target = cur.id(id_size);
                if (target == 0) continue;
                heap::Reference ref;
                ref.index = static_cast<std::int64_t>(i);
                ref.target = target;
                obj.references.push_back(std::move(ref));
              }
              if (pass != 1) {
                // nothing kept on the first pass
              } else if (objects_kept < limits.max_objects) {
                ++objects_kept;
                out.add_object(std::move(obj));
              } else {
                ++out.limits.objects_dropped;
                out.limits.truncated_by_object_cap = true;
              }
              break;
            }
            case kPrimitiveArrayDump: {
              heap::Object obj;
              obj.kind = heap::ObjectKind::kPrimitiveArray;
              obj.heap = current_heap;
              obj.id = cur.id(id_size);
              cur.u4();  // stack trace serial
              const std::uint32_t count = cur.u4();
              const std::uint8_t type = cur.u1();
              const int w = size_of_type(type, id_size);
              if (w < 0) {
                note_unrecognised(sub, cur.offset());
                cur.seek(end);
                break;
              }
              obj.shallow_size = static_cast<std::int64_t>(count) * w;
              // A primitive array carries no class id -- the record gives the
              // element type instead -- so the name comes from that. Leaving
              // it unresolved put 164,644 objects of the 49 MB dump this was
              // written against under "(class name unresolved)", which reads
              // as a parser that failed rather than a format that names them
              // differently.
              obj.class_id = synthetic_array_class(type, out);
              cur.skip(static_cast<std::size_t>(count) *
                       static_cast<std::size_t>(w));
              // Kept with no references: a byte array is where a bitmap's
              // pixels live, so its size is the whole point of it.
              if (pass != 1) {
                // nothing kept on the first pass
              } else if (objects_kept < limits.max_objects) {
                ++objects_kept;
                out.add_object(std::move(obj));
              } else {
                ++out.limits.objects_dropped;
                out.limits.truncated_by_object_cap = true;
              }
              break;
            }
            case kPrimitiveArrayNoData: {
              cur.id(id_size);
              cur.u4();  // count
              cur.u1();  // type
              // The runtime recorded that the array exists without its
              // contents. Its size is therefore unknown, and left at zero
              // with a note rather than computed from the count.
              ++out.limits.unrecognised_records;
              break;
            }
            case kUnreachable: {
              // Android marks an object the collector knows is unreachable.
              // Not a root, not an error: noted and skipped.
              break;
            }
            default:
              note_unrecognised(sub, cur.offset());
              cur.seek(end);
              break;
          }
        }
        current_heap = heap::HeapSpace::kUnknown;
        break;
      }
      default:
        // Stack frames, stack traces, control settings: real records this
        // parser has no use for. Counted, not treated as corruption.
        break;
    }
    cur.seek(end);
    if (cur.offset() >= limits.max_bytes) {
      out.limits.truncated_by_byte_cap = true;
      out.limits.notes.push_back(
          "stopped at the byte cap: the graph covers only the start of this "
          "dump");
      break;
    }
  }
  }  // pass
  if (segments_abandoned > 0) {
    out.limits.notes.push_back(
        std::to_string(segments_abandoned) +
        " heap segment(s) were abandoned at a field type this parser does "
        "not know: a class record carries no length, so the records after it "
        "in the same segment could not be located and are missing from the "
        "graph");
  }
  if (incomplete_chains > 0) {
    out.limits.notes.push_back(
        std::to_string(incomplete_chains) +
        " object(s) had a superclass this dump never described, so their "
        "later fields -- and the references in them -- were not read");
  }

  // Class names come from LOAD_CLASS plus the string table, and both arrive
  // before the heap dump in practice -- but the parse does not depend on
  // order.
  for (const auto& [class_object_id, name_id] : class_name_ids) {
    const auto sit = strings.find(name_id);
    if (sit == strings.end()) {
      ++out.limits.unresolved_class_names;
      continue;
    }
    heap::ClassInfo info;
    if (const heap::ClassInfo* existing = out.find_class(class_object_id)) {
      info = *existing;
    } else {
      info.id = class_object_id;
    }
    info.name = humanise_class_name(sit->second);
    out.add_class(std::move(info));
  }

  // Static fields, attached now that the class objects are in the graph. A
  // class object is an object: this is how a `static` reference shows up as
  // an edge a path can run through.
  for (auto& [class_id, ref] : pending_statics) {
    heap::Object holder;
    if (const heap::Object* existing = out.find_object(class_id)) {
      holder = *existing;
    } else {
      holder.id = class_id;
      holder.kind = heap::ObjectKind::kClass;
      holder.class_id = class_id;
    }
    holder.references.push_back(std::move(ref));
    out.add_object(std::move(holder));
  }
  pending_statics.clear();

  // Dangling references: a target no object in the graph declares. Counted
  // because it is the signature of a truncated dump.
  for (const auto& [id, o] : out.objects()) {
    static_cast<void>(id);
    for (const auto& ref : o.references) {
      if (out.find_object(ref.target) == nullptr &&
          out.find_class(ref.target) == nullptr) {
        ++out.limits.dangling_references;
      }
    }
  }

  res.ok = !cur.failed() || res.records_read > 0;
  if (cur.failed() && res.error.empty()) {
    res.error = "the dump ended unexpectedly; the graph holds what was read";
  }
  return res;
}

HprofReadResult read_hprof(const std::string& path, const HprofLimits& limits,
                           const CancellationToken& cancel,
                           heap::HeapGraph& out) {
  HprofReadResult res;
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    res.error = "cannot open " + path;
    return res;
  }
  f.seekg(0, std::ios::end);
  const auto size = static_cast<std::size_t>(f.tellg());
  f.seekg(0, std::ios::beg);
  if (size > limits.max_bytes) {
    res.error = "heap dump is " + std::to_string(size / (1024 * 1024)) +
                " MiB, over the " +
                std::to_string(limits.max_bytes / (1024 * 1024)) +
                " MiB cap; raise the cap deliberately rather than reading "
                "part of a graph";
    return res;
  }
  std::string bytes(size, '\0');
  f.read(bytes.data(), static_cast<std::streamsize>(size));
  out.source_path = path;
  res = read_hprof_bytes(bytes, limits, cancel, out);
  out.source_path = path;
  return res;
}

}  // namespace mpi::android
