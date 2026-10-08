// Finding the failure in a CI job log, locally and deterministically.
//
// A failed job's log is mostly noise: runner preparation, dependency
// downloads, progress bars and colour codes. What a person (or an AI host
// reading DevX's evidence) needs first is the one line that says why the job
// failed, and a bounded window of log around it. This file finds both with a
// fixed table of rules -- no model, no network, no guessing -- so the same log
// always yields the same answer, and a test can pin it.
//
// Cleaning (clean_ci_log)
//   * ANSI escape sequences (CSI such as colours and `ESC[0K`, OSC, and the
//     two- and three-byte forms) are removed.
//   * GitLab's collapsible-section markers -- `section_start:<epoch>:<name>`
//     and `section_end:<epoch>:<name>`, with an optional `[options]` suffix
//     and the `\r` that follows -- are removed. A line that held nothing but
//     markers is dropped entirely, as GitLab's own viewer does.
//   * A line rewritten with `\r` (a progress bar) keeps only its last segment
//     that still has text after the escapes are gone: that is what a terminal
//     would have shown. A CRLF line ending therefore costs nothing.
//   Line numbers everywhere below are 1-based lines of the CLEANED log, so
//   find_failure() and excerpt_lines() always agree.
//
// Rules (higher priority wins; among lines of the highest priority found, the
// EARLIEST line is the root). Each line takes the highest-priority rule it
// matches; matching is on the line with leading whitespace removed.
//
//   prio  rule                      matches
//   ----  ------------------------  -------------------------------------------
//   100   compiler_error            `<file>:<l>:<c>: error:` / `: fatal error:`
//                                   (clang, gcc, swiftc, javac, old kotlinc),
//                                   `e: <file>` (kotlinc under Gradle),
//                                   `<file>(l,c): error TS1234:` (tsc),
//                                   `error[E0308]` (rustc)
//    95   test_failure              `✕ ` / `✗ ` (Jest, others), `FAIL <path>`
//                                   (Jest suite), `FAILED <id>` (pytest),
//                                   `--- FAIL:` (go test), `<class> > <test>
//                                   FAILED` (Gradle), `Test Case '...' failed`
//                                   (XCTest)
//    90   gradle_what_went_wrong    the first non-empty line after
//                                   `* What went wrong:` (Gradle's own summary
//                                   of why the build failed)
//    86   jvm_caused_by             `Caused by: ...`
//    85   jvm_exception             `Exception in thread "..." ...`
//    85   python_traceback          the exception line that closes a
//                                   `Traceback (most recent call last):` block
//                                   (the first later non-indented line, looked
//                                   for within 500 lines)
//    80   xcodebuild_error          `xcodebuild: error: ...`
//    80   error_line                `error: ...` at the start of a line
//    75   gradle_execution_failed   `Execution failed for task ...`
//    70   js_error                  `<Name>Error: ...` / `<Name>Exception: ...`
//                                   at the start of a line (JS, Python, ...)
//    65   npm_error                 `npm ERR! ...` / `npm error ...`, except the
//                                   bookkeeping lines below
//    60   gradle_task_failed        `> Task :module:task FAILED`
//    55   error_prefix              `ERROR: ...` (pip, many tools), except the
//                                   runner summary below
//    50   fatal                     `fatal: ...` (git and others)
//    45   xcodebuild_failed         `** BUILD FAILED **`, `** TEST FAILED **`
//    40   build_summary             `The following build commands failed:`,
//                                   `error Command failed with exit code`
//                                   (yarn)
//    35   gradle_build_failed       `FAILURE: Build failed with an exception.`,
//                                   `BUILD FAILED in ...`
//    30   npm_error_meta            `npm ERR! code|errno|path|syscall|command|
//                                   cwd ...`, `npm ERR! A complete log ...`,
//                                   a bare `npm ERR!`
//    10   runner_job_failed         `ERROR: Job failed: ...` -- GitLab Runner's
//                                   own summary, true of every failed job and
//                                   therefore the root only when nothing else
//                                   in the log says more
//
// Why this order: the most specific statement of the cause wins. A compiler
// diagnostic names a file and line; a failing test names a test; Gradle's
// "What went wrong" is Gradle's summary of those (for a compile failure it
// says only "Compilation error"), and build-tool and runner summaries are true
// of every failure and say the least. Where a rule cannot tell, it says so by
// a low priority rather than by guessing.
//
// Cost: one pass to clean and one pass to classify, line by line, with no
// regular expression; a 20 MB log is a few hundred milliseconds even in a
// debug build. Memory is the cleaned copy of the log plus a bounded candidate
// list.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/util/json.hpp"

namespace mpi::intelligence {

struct FailureCandidate {
  std::size_t line = 0;   // 1-based, in the cleaned log
  std::string text;       // cleaned, trimmed, at most 300 bytes
  std::string rule;       // a rule name from the table above
  int priority = 0;
};

struct FailureSummary {
  bool found = false;
  std::string root_error;            // <= 300 bytes, cleaned
  std::string root_rule;             // which rule chose it
  std::size_t root_line = 0;         // 1-based, in the cleaned log
  /// The most significant matches (by priority, then earliest), at most 20,
  /// in log order. Always contains the root when one was found.
  std::vector<FailureCandidate> candidates;
  /// The 1-based inclusive line range of `excerpt`. It is the context window
  /// around the root, shrunk (farthest lines first) when the window would not
  /// fit in max_excerpt_bytes -- so it always names exactly what `excerpt`
  /// holds.
  std::size_t excerpt_start = 0, excerpt_end = 0;
  std::string excerpt;               // those lines, <= max_excerpt_bytes
  std::size_t total_lines = 0;
  json::Value to_json() const;
};

/// The log as a person would have read it in GitLab: see "Cleaning" above.
std::string clean_ci_log(const std::string& raw);

/// The root failure of a job log, its strongest candidates, and an excerpt
/// around it. found=false (and no root) when no rule matched: a log that
/// does not say why it failed is reported as such, never given a cause.
FailureSummary find_failure(const std::string& raw_log, std::size_t context_before = 20,
                            std::size_t context_after = 40,
                            std::size_t max_excerpt_bytes = 8192);

/// Lines [first, last] (1-based, inclusive, clamped to the log) of the
/// cleaned log, joined with '\n' and cut to at most max_bytes on a UTF-8
/// boundary. For cutting an excerpt from stored raw evidence on demand.
std::string excerpt_lines(const std::string& raw_log, std::size_t first, std::size_t last,
                          std::size_t max_bytes);

}  // namespace mpi::intelligence
