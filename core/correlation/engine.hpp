// Correlation: releases, and how every piece of evidence relates to them
// (section 14, ADR-0012).
//
// The two evidence planes meet here and only here. Signals (production,
// CI/CD, imports) and DevX sessions (local captures, BrowserStack runs) are
// grouped around release identity, and every edge says what it rests on:
//
//   1. the same full commit SHA                              exact
//   2. the same app identifier + version + build             exact
//   3. a provider stating the relationship (Sentry's release
//      for an event, GitLab's commit for a pipeline)         provider_attributed
//   4. a short SHA prefix, a version without its build, or
//      timing (first seen after a deploy, with no identity)  candidate
//
// Exact always outranks candidate: a signal with an exact edge gets no
// candidate edge to another release. Candidate edges are stored apart, are
// never called a cause, and the UI and the AI tools render them differently.
// When two sources disagree about a release (two commits for one build) the
// conflict is kept and shown; nothing is chosen silently.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/signals/signal.hpp"
#include "core/signals/signal_store.hpp"

namespace mpi::correlation {

struct CorrelationEdge {
  std::string from_id;   // a signal id, "session:<id>", "release:<key>", "commit:<sha>"
  std::string to_id;
  /// built_from | deployed_as | observed_in | measured_by | tested_in | same_commit
  std::string relation;
  signals::EvidenceBasis basis = signals::EvidenceBasis::kUnknown;
  std::string evidence;  // one human-auditable sentence
  std::optional<std::string> source_ref;
  json::Value to_json() const;
};

/// A DevX session package, reduced to what correlation needs.
struct SessionRef {
  std::string id;
  std::string created_at;
  std::string platform;
  std::string device_id;
  std::string app_identifier;
  std::optional<std::string> version, build;  // from build facts
  std::string version_source;                 // which fact said so
  bool browserstack = false;
  json::Value to_json() const;
};

/// Reads the newest `limit` session packages under `sessions_dir`.
std::vector<SessionRef> scan_sessions(const std::string& sessions_dir, std::size_t limit = 500);

struct Release {
  std::string key;
  signals::ReleaseIdentity identity;  // merged; conflicting facts go to `conflicts`
  std::vector<std::string> conflicts;
  std::vector<std::string> signal_ids;     // exact or provider-attributed members
  std::vector<std::string> session_ids;    // exact members
  std::string first_activity_at, last_activity_at;
  std::optional<std::string> deployed_at;  // earliest finished deploy
};

struct Graph {
  std::string workspace_id;
  std::map<std::string, Release> releases;          // by key
  std::vector<CorrelationEdge> edges;               // exact + provider_attributed
  std::vector<CorrelationEdge> candidate_edges;     // never mixed with the above
  std::map<std::string, signals::SignalRecord> signals;  // by id
  std::map<std::string, SessionRef> sessions;            // by id
  std::vector<std::string> problems;
};

struct Inputs {
  signals::ProjectWorkspace workspace;
  std::vector<signals::SignalRecord> signals;
  std::vector<SessionRef> sessions;
  /// Sessions the user tied to a release by hand: session id -> release key.
  std::map<std::string, std::string> session_links;
};

Graph build_graph(const Inputs& in);

/// Builds the graph straight from the store and the sessions directory.
Graph load_graph(const signals::SignalStore& store, const std::string& sessions_dir,
                 const std::string& ws, std::string* error);

/// User assertions: `mpi intelligence link-session` and the window.
bool link_session(const signals::SignalStore& store, const std::string& ws,
                  const std::string& session_id, const std::string& release_key, bool linked,
                  std::string* error);
std::map<std::string, std::string> session_links(const signals::SignalStore& store,
                                                 const std::string& ws);

/// Every release, newest activity first, with evidence counts.
json::Value releases_json(const Graph& g);
/// One release: identity and conflicts, a build -> test -> deploy -> first
/// production signal timeline, evidence by provider, sessions, exact and
/// candidate links apart, and what evidence is missing.
json::Value release_overview(const Graph& g, const std::string& key,
                             const json::Value& freshness);
/// Two releases side by side: counts by kind and severity, crash and issue
/// totals, CI failures, and metric deltas for the same metric in both.
json::Value release_compare(const Graph& g, const std::string& base, const std::string& candidate);
/// The edges that touch one signal, and its release's other evidence.
json::Value related(const Graph& g, const std::string& signal_id);
/// A workspace summary for the Overview: latest releases with production, CI,
/// test and session counts, freshness, and stale or partial connectors.
json::Value overview(const Graph& g, const json::Value& integrations);

}  // namespace mpi::correlation
