# ADR-0002 -- No third-party libraries in the core

- Status: accepted
- Date: 2026-09-14
- Spec references: sections 5 (license review), 15 (bounded parsers), required
  output "third-party license inventory"

## Context

The specification requires a third-party license inventory and requires every
parser input to be treated as untrusted with bounded memory, depth and
container sizes.

## Decision

The core has no third-party dependencies. JSON parsing/serialization
(`core/util/json.*`), the child-process runner (`core/util/process.*`), the
source-map VLQ decoder and the R8 mapping reader are all written here.

## Consequences

- The third-party license inventory for the core and CLI is **empty**, which is
  the easiest possible inventory to review.
- The build needs no network access, which mattered directly: the whole
  project was built and tested offline.
- Every limit spec section 15 demands is enforced in code we control rather
  than configured on a library that may not expose it. `json::Limits` bounds
  bytes, depth, container elements and string length, and each is tested.
- We own the correctness burden. The JSON implementation is therefore tested
  against surrogate pairs, non-finite doubles, integer overflow, BOMs, depth
  bombs and malformed input (13 cases in `tests/unit/test_json.cpp`).

## Known cost

The `json::Value` DOM is memory-hungry: see ADR-0005 and
`docs/known-limitations.md` for the measured 1 GiB stress result and the
planned streaming reader.
