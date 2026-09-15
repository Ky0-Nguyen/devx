#include "adapters/ios/sample_parser.hpp"

#include <sstream>

namespace mpi::ios {
namespace {

std::string rtrim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) {
    s.pop_back();
  }
  return s;
}

/// Where the sample count starts on a line, or npos.
///
/// Located by index rather than by matching the branch character, because
/// `sample` uses `+` here and other builds have been seen to use `!`, `|` and
/// `:` for the same purpose. Only whitespace and branch characters may
/// precede the count; anything else means this is not a graph line.
std::size_t count_column(const std::string& line) {
  for (std::size_t i = 0; i < line.size(); i++) {
    const char c = line[i];
    if (c >= '0' && c <= '9') return i;
    if (c == ' ' || c == '\t' || c == '+' || c == '!' || c == '|' ||
        c == ':') {
      continue;
    }
    return std::string::npos;
  }
  return std::string::npos;
}

/// The frame text with `sample`'s decorations removed.
///
/// Keeps the symbol and the module, which is what identifies a frame, and
/// drops the byte offset and the address, which change between runs of the
/// same binary and would make two identical stacks compare unequal.
std::string frame_label(const std::string& rest) {
  std::string out = rest;
  // Trailing `  [0x1027b04e4]`.
  const std::size_t addr = out.rfind("  [0x");
  if (addr != std::string::npos) out.resize(addr);
  // `  + 6992` offset, and the `  load address 0x... + 0x...` form that an
  // unsymbolised frame carries.
  const std::size_t load = out.find("  load address ");
  if (load != std::string::npos) out.resize(load);
  const std::size_t plus = out.rfind(" + ");
  if (plus != std::string::npos) {
    bool all_digits = plus + 3 < out.size();
    for (std::size_t i = plus + 3; i < out.size() && all_digits; i++) {
      if (out[i] < '0' || out[i] > '9') all_digits = false;
    }
    if (all_digits) out.resize(plus);
  }
  return rtrim(out);
}

}  // namespace

SampleParse parse_sample_output(const std::string& text,
                                const std::string& process_instance_id) {
  SampleParse out;
  const std::size_t graph_at = text.find("Call graph:");
  if (graph_at == std::string::npos) {
    out.error = "`sample` produced no call graph";
    return out;
  }

  // The path currently being walked: one entry per depth, each holding the
  // frame label, its own count, and how much of that count its children have
  // claimed so far.
  struct Node {
    std::string label;
    std::int64_t count = 0;
    std::int64_t children = 0;
  };
  std::vector<Node> path;
  std::string thread_id;
  std::size_t thread_index = 0;

  // Emitting happens when a node is popped, because only then is the sum of
  // its children known. A node's self time is what its children did not
  // claim.
  // Called while the node is still on `path`, so its own frame is included.
  // Emitting after the pop left every sample one frame short -- the deepest
  // stack ended at the caller of its leaf, which is a wrong answer that
  // conserves the weights and so survives a sample-count check.
  const auto emit = [&](const Node& node) {
    const std::int64_t self = node.count - node.children;
    if (self <= 0) return;
    model::CpuSample s;
    s.process_instance_id = process_instance_id;
    s.thread_instance_id = thread_id;
    s.provider = "/usr/bin/sample";
    // Outermost first, which is the order the graph is printed in, and
    // including this node: `path.back()` is the frame the samples stopped in.
    for (const Node& n : path) s.frames.push_back(n.label);
    // A weight, not a count of rows: this provider reports aggregates, and
    // pretending each was an individual sample would invent timestamps it
    // never gave.
    s.weight = static_cast<double>(self);
    out.attributed_samples += self;
    out.samples.push_back(std::move(s));
  };

  const auto unwind_to = [&](std::size_t depth) {
    while (path.size() > depth) {
      emit(path.back());
      const Node node = path.back();
      path.pop_back();
      if (!path.empty()) path.back().children += node.count;
    }
  };

  std::istringstream in(text.substr(graph_at));
  std::string line;
  std::getline(in, line);   // the "Call graph:" header itself
  while (std::getline(in, line)) {
    line = rtrim(line);
    if (line.empty()) {
      // A blank line ends the graph; what follows is sample's own summary.
      break;
    }
    const std::size_t at = count_column(line);
    if (at == std::string::npos) {
      // Not a graph line. The section after the graph starts this way, so
      // stop rather than counting the whole summary as unparsed.
      break;
    }
    std::size_t end = at;
    while (end < line.size() && line[end] >= '0' && line[end] <= '9') end++;
    const std::int64_t count =
        std::strtoll(line.substr(at, end - at).c_str(), nullptr, 10);
    std::string rest = line.substr(end);
    while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
    if (rest.empty()) {
      out.unparsed_lines++;
      continue;
    }

    // A thread header: the count sits at the base indent with no branch
    // character, and the text names a thread.
    const bool has_branch = line.find_first_of("+!|:") != std::string::npos &&
                            line.find_first_of("+!|:") < at;
    if (!has_branch && rest.rfind("Thread_", 0) == 0) {
      unwind_to(0);
      SampleThread t;
      const std::size_t sp = rest.find_first_of(" \t");
      t.label = sp == std::string::npos ? rest : rest.substr(0, sp);
      if (sp != std::string::npos) {
        std::string desc = rest.substr(sp);
        while (!desc.empty() && desc.front() == ' ') desc.erase(desc.begin());
        t.description = desc;
      }
      t.total_samples = count;
      t.is_main = t.description.find("main-thread") != std::string::npos;
      thread_index = out.threads.size();
      thread_id = process_instance_id + ":" + t.label;
      out.threads.push_back(std::move(t));
      continue;
    }
    if (out.threads.empty()) {
      // A frame before any thread header: the format is not what this
      // expects, and guessing a thread for it would invent attribution.
      out.unparsed_lines++;
      continue;
    }
    (void)thread_index;

    // Depth from the column the count sits in. The first frame under a
    // thread sits two characters in ("+ "), and each level adds two.
    const std::size_t base = 6;
    const std::size_t depth = at >= base ? (at - base) / 2 : 0;
    unwind_to(depth);
    if (path.size() != depth) {
      // A jump deeper than one level: the tree is not shaped as expected and
      // inventing intermediate frames would fabricate a call path.
      out.unparsed_lines++;
      continue;
    }
    Node node;
    node.label = frame_label(rest);
    node.count = count;
    path.push_back(std::move(node));
  }
  unwind_to(0);

  out.ok = !out.threads.empty();
  if (!out.ok) out.error = "the call graph contained no threads";
  return out;
}

}  // namespace mpi::ios
