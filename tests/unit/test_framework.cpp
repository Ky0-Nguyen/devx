#include "tests/unit/test_framework.hpp"

#include <cstdlib>
#include <sstream>

namespace mpi::test {
namespace {

std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

struct Failure {
  std::string expr;
  std::string file;
  int line;
  std::string detail;
};

std::vector<Failure>& current_failures() {
  static std::vector<Failure> f;
  return f;
}

}  // namespace

int register_case(const char* name, std::function<void()> fn,
                  std::vector<std::string> requirement_ids) {
  registry().push_back(Case{name, std::move(fn), std::move(requirement_ids)});
  return 0;
}

void fail(const std::string& expr, const char* file, int line,
          const std::string& detail) {
  current_failures().push_back(Failure{expr, file, line, detail});
}

int run_all(int argc, char** argv) {
  std::string filter;
  bool list_requirements = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--list-requirements") {
      list_requirements = true;
    } else if (a.rfind("--filter=", 0) == 0) {
      filter = a.substr(9);
    }
  }

  if (list_requirements) {
    // Feeds the requirement -> test mapping the specification requires.
    for (const auto& c : registry()) {
      for (const auto& r : c.requirement_ids) {
        std::cout << r << "\t" << c.name << "\n";
      }
    }
    return 0;
  }

  std::size_t passed = 0;
  std::vector<std::string> failed;
  for (const auto& c : registry()) {
    if (!filter.empty() && c.name.find(filter) == std::string::npos) continue;
    current_failures().clear();
    c.fn();
    if (current_failures().empty()) {
      ++passed;
      std::cout << "  ok   " << c.name;
      if (!c.requirement_ids.empty()) {
        std::cout << "  [";
        for (std::size_t i = 0; i < c.requirement_ids.size(); ++i) {
          if (i) std::cout << " ";
          std::cout << c.requirement_ids[i];
        }
        std::cout << "]";
      }
      std::cout << "\n";
    } else {
      failed.push_back(c.name);
      std::cout << "  FAIL " << c.name << "\n";
      for (const auto& f : current_failures()) {
        std::cout << "       " << f.file << ":" << f.line << ": " << f.expr << "\n";
        if (!f.detail.empty()) std::cout << "       " << f.detail << "\n";
      }
    }
  }

  std::cout << "\n" << passed << " passed, " << failed.size() << " failed\n";
  return failed.empty() ? 0 : 1;
}

}  // namespace mpi::test

int main(int argc, char** argv) { return mpi::test::run_all(argc, argv); }
