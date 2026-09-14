// Android's HPROF heap dump, read directly.
//
// `am dumpheap` writes "JAVA PROFILE 1.0.3", which is HPROF with Android's own
// additions. The platform ships `hprof-conv` to turn it into the 1.0.2 format
// the Java tools read, and this parser deliberately does **not** use it,
// because that conversion throws away the two things a retention question
// needs most:
//
//   * **Root kinds.** In a converted dump nearly every root becomes
//     ROOT_UNKNOWN -- 269,081 of 290,000 in the capture this was written
//     against. A path anchored at "unknown" cannot distinguish an object held
//     by one of the app's threads from one the runtime interned.
//   * **Which heap an object is on.** Android's HEAP_DUMP_INFO records
//     separate the app's heap from the zygote and the boot image. An object on
//     the image heap is shared with every process on the device and is not the
//     app's to release; counting it against the app would be wrong.
//
// Reading the platform's own format also removes a dependency on a tool that
// must be found, version-matched and trusted.
//
// The parser is bounded in both directions: a cap on bytes read and a cap on
// objects kept, each reported when it fires. A heap dump of a real app is tens
// of megabytes -- the one this was verified against is 49 MB with 345,858
// instances -- so silently truncating would produce a graph whose gaps look
// like facts.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/heap/heap_graph.hpp"
#include "core/util/cancel.hpp"

namespace mpi::android {

struct HprofLimits {
  // Boolean instance fields to keep, by name. Everything else primitive is
  // skipped: see `heap::Object::flags` for why the list is short.
  //
  // The default is Android's own lifecycle state on Activity and Fragment,
  // which is the one expectation the platform documents well enough to draw a
  // conclusion from -- a destroyed Activity is supposed to be released.
  std::vector<std::string> retain_boolean_fields = {
      "mDestroyed", "mFinished", "mResumed", "mStopped", "mRemoving",
      "mDetached", "mInLayout",
  };
  // 512 MiB: larger than any dump seen, small enough that a corrupt length
  // field cannot ask for the address space.
  std::size_t max_bytes = 512ull * 1024 * 1024;
  std::size_t max_objects = 4'000'000;
  std::size_t max_classes = 500'000;
};

struct HprofReadResult {
  bool ok = false;
  std::string error;
  // Filled even on failure, so a partially read dump reports what it got.
  std::int64_t records_read = 0;
  std::int64_t bytes_read = 0;
  std::string format;  // the header's own version string
  int identifier_size = 0;
};

// Parses `path` into `out`. `out.limits` carries everything the parse could
// not do.
HprofReadResult read_hprof(const std::string& path, const HprofLimits& limits,
                           const CancellationToken& cancel,
                           heap::HeapGraph& out);

// Parses from memory, for tests and for a dump already in hand.
HprofReadResult read_hprof_bytes(const std::string& bytes,
                                 const HprofLimits& limits,
                                 const CancellationToken& cancel,
                                 heap::HeapGraph& out);

}  // namespace mpi::android
