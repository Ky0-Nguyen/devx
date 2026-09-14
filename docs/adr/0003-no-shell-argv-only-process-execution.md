# ADR-0003 -- Child processes are never invoked through a shell

- Status: accepted
- Date: 2026-09-14
- Spec references: sections 5, 15; checklist J05, A20, G19

## Context

Almost every fact this tool learns comes from invoking a platform tool with an
app identifier, a device id, or a file path as an argument. All three are
attacker-influenced in the general case: an identifier comes off a device, a
source path comes out of a source map.

Spec section 5 forbids shell-string interpolation using app names,
identifiers, trace fields or source paths.

## Decision

`mpi::proc::run` takes `std::vector<std::string> argv` and calls `posix_spawnp`
directly. There is **no API that accepts a command line string**, so the unsafe
call cannot be written by accident.

Two further guards:

- `proc::is_safe_argument(arg, reject_option_like)` rejects embedded NULs, and
  optionally rejects a leading `-`. Adapters use the option-like check on app
  identifiers, because argv safety does not stop `pm` or `ps` from reading
  `--all` as a flag.
- `symbols::path_is_within_root` normalizes a path lexically (no filesystem
  access) and refuses anything that escapes the configured source root, before
  any `open()` happens.

## Consequences

- Command injection through an identifier or a trace field is structurally
  impossible rather than filtered.
- `tests/unit/test_process.cpp` asserts that `; id`, `&& whoami`, `$(pwd)` and
  `` `ls` `` all survive as literal argv elements.
- A hostile source map naming `../../../../etc/passwd` is rejected with a
  `mismatch` symbol status rather than opened
  (`tests/unit/test_symbols.cpp`).
