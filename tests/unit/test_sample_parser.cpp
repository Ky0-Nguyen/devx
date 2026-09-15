// `/usr/bin/sample`'s call graph, and the arithmetic that makes it a profile.
//
// The fixture is REAL output from a simulator app, because the parser's whole
// job is surviving the shape the tool prints. Two things in it are easy to
// get wrong and impossible to notice without checking:
//
//   * each node's count includes its children, so the quantity that means
//     anything is `self = count - sum(children)`. Emitting every node at its
//     full count multiply-counts the same samples down the whole path;
//   * a node must appear in its own stack. Emitting after popping the path
//     left every sample one frame short -- the deepest stack ended at the
//     *caller* of its leaf. That conserves the weights, so a sample-count
//     check passes and the stacks are still wrong.
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "adapters/ios/sample_parser.hpp"
#include "tests/unit/test_framework.hpp"

namespace {

std::string read_fixture() {
  const char* dir = std::getenv("MPI_FIXTURE_DIR");
  const std::string path =
      (dir != nullptr ? std::string(dir) : std::string("fixtures")) +
      "/sample/recorded-simulator-callgraph.txt";
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

MPI_TEST(a_real_call_graph_conserves_every_sample, {}) {
  const auto r = mpi::ios::parse_sample_output(read_fixture(), "proc:1");
  MPI_CHECK_MSG(r.ok, "the recorded output parses");
  MPI_CHECK_MSG(r.error.empty(), "with no error");
  MPI_CHECK_MSG(r.threads.size() == 3, "three thread sections were kept");
  MPI_CHECK_MSG(r.unparsed_lines == 0,
                "and every line inside the graph was understood");

  std::int64_t declared = 0;
  for (const auto& t : r.threads) declared += t.total_samples;
  MPI_CHECK_MSG(declared > 0, "the threads declare samples");
  // The invariant: self times partition the total. A node counted at its full
  // count would inflate this, and a lost frame would deflate it.
  MPI_CHECK_MSG(r.attributed_samples == declared,
                "the self times sum exactly to what the threads declared -- "
                "no sample counted twice and none dropped");
}

MPI_TEST(a_frame_appears_in_its_own_stack, {}) {
  const auto r = mpi::ios::parse_sample_output(read_fixture(), "proc:1");
  MPI_CHECK(r.ok);

  const mpi::model::CpuSample* deepest = nullptr;
  for (const auto& s : r.samples) {
    if (deepest == nullptr || s.frames.size() > deepest->frames.size()) {
      deepest = &s;
    }
  }
  MPI_CHECK(deepest != nullptr);
  // Outermost first, per the model's contract.
  MPI_CHECK_MSG(deepest->frames.front().find("start") != std::string::npos,
                "the first frame is the outermost one");
  // The leaf of the main thread's stack in the recording. If the node is
  // emitted after being popped, this is mach_msg2_internal instead -- its
  // caller -- and the weights still balance.
  MPI_CHECK_MSG(deepest->frames.back().find("mach_msg2_trap") !=
                    std::string::npos,
                "and the last frame is the one the samples stopped in, not "
                "its caller");
  MPI_CHECK_MSG(deepest->weight.has_value() && *deepest->weight > 0,
                "a weight is carried, because this provider reports "
                "aggregates rather than individual samples");
  MPI_CHECK_MSG(!deepest->thread_instance_id.empty(),
                "and the sample is attributed to a thread");
  MPI_CHECK_MSG(deepest->provider == "/usr/bin/sample",
                "with the provider named, since this is not xctrace's data");
}

MPI_TEST(self_time_is_split_from_time_in_callees, {}) {
  // Constructed, and labelled as such: the recorded graph happens to be
  // single-child chains throughout, so it cannot exercise a frame that has
  // both self time and callees -- which is the case the arithmetic exists
  // for. `outer` sampled 100 times, 60 of them inside `inner`, so `outer`
  // holds 40 of its own.
  const std::string synthetic =
      "Call graph:\n"
      "    100 Thread_1   main-thread\n"
      "    + 100 outer  (in App) + 4  [0x1]\n"
      "    +   60 inner  (in App) + 8  [0x2]\n";
  const auto r = mpi::ios::parse_sample_output(synthetic, "p");
  MPI_CHECK(r.ok);
  MPI_CHECK_MSG(r.samples.size() == 2,
                "both frames are reported: one with self time under a callee, "
                "and the callee itself");
  MPI_CHECK(r.attributed_samples == 100);

  const mpi::model::CpuSample* outer = nullptr;
  const mpi::model::CpuSample* inner = nullptr;
  for (const auto& s : r.samples) {
    if (s.frames.size() == 1) outer = &s;
    if (s.frames.size() == 2) inner = &s;
  }
  MPI_CHECK(outer != nullptr && inner != nullptr);
  MPI_CHECK_MSG(*inner->weight == 60.0, "the callee keeps its own 60");
  MPI_CHECK_MSG(*outer->weight == 40.0,
                "and the caller keeps 40, not 100 -- its samples inside the "
                "callee belong to the callee");
  MPI_CHECK(outer->frames.front().find("outer") != std::string::npos);
  MPI_CHECK(inner->frames.back().find("inner") != std::string::npos);
}

MPI_TEST(a_frame_label_drops_what_changes_between_runs, {}) {
  // Offsets and addresses differ run to run for the same binary, so keeping
  // them would make two identical stacks compare unequal and defeat any
  // aggregation across captures.
  const std::string synthetic =
      "Call graph:\n"
      "    5 Thread_1\n"
      "    + 5 UIApplicationMain  (in UIKitCore) + 120  [0x186b9f910]\n";
  const auto r = mpi::ios::parse_sample_output(synthetic, "p");
  MPI_CHECK(r.ok && r.samples.size() == 1);
  const std::string& f = r.samples.front().frames.back();
  MPI_CHECK_MSG(f.find("UIApplicationMain") != std::string::npos,
                "the symbol survives");
  MPI_CHECK_MSG(f.find("UIKitCore") != std::string::npos,
                "and the module, which identifies which one it is");
  MPI_CHECK_MSG(f.find("0x186b9f910") == std::string::npos,
                "the address is dropped");
  MPI_CHECK_MSG(f.find("+ 120") == std::string::npos,
                "and so is the byte offset");
}

MPI_TEST(output_without_a_call_graph_is_refused_not_guessed, {}) {
  const auto empty = mpi::ios::parse_sample_output("", "p");
  MPI_CHECK_MSG(!empty.ok, "empty input produces no profile");
  MPI_CHECK_MSG(empty.error.find("call graph") != std::string::npos,
                "and says what was missing");
  MPI_CHECK(empty.samples.empty());

  const auto noise = mpi::ios::parse_sample_output(
      "Sampling process 123 for 1 second...\nSample analysis failed.\n", "p");
  MPI_CHECK_MSG(!noise.ok,
                "sample's own failure message is not mistaken for a profile");

  // A header with no threads is not a profile either.
  const auto bare = mpi::ios::parse_sample_output("Call graph:\n\n", "p");
  MPI_CHECK(!bare.ok);
  MPI_CHECK(bare.samples.empty());

  // A frame before any thread header cannot be attributed, and is counted
  // rather than assigned to a thread that was never named.
  const auto orphan = mpi::ios::parse_sample_output(
      "Call graph:\n    + 5 someFrame  (in X)\n", "p");
  MPI_CHECK_MSG(orphan.unparsed_lines > 0,
                "an unattributable frame is counted, not guessed at");
  MPI_CHECK(orphan.samples.empty());
}
