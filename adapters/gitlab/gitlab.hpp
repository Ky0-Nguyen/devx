// The GitLab CI connector for DevX Intelligence (see docs/intelligence.md).
//
// Reads, through GitLab's REST API v4, what CI/CD said about each commit:
// pipelines, their jobs, the test report summary, the log of each failed job
// (kept as raw evidence and reduced to a root error by ci_log.hpp), and
// deployments. Nothing is ever written to GitLab, and job artifacts are never
// downloaded.
//
// Records (kind -> external_id):
//   pipeline  pipeline:<id>   one per pipeline updated in the window
//   ci_job    job:<id>        one per job of those pipelines
//   test      tests:<id>      a pipeline's test report summary, when it has one
//   deploy    deploy:<id>     one per deployment updated in the window
// Each carries the commit GitLab says it built or deployed, with basis exact:
// the pipeline's own `sha` is GitLab's statement, not an inference.
//
// The token, when there is one, is sent only as the PRIVATE-TOKEN header to
// the host of base_url (net::HttpRequest::allowed_hosts). It never appears in
// a URL, an argv, a record, a raw file or an error. A public project needs no
// token at all (ConnectorInfo::credential_optional).
//
// Settings:
//   base_url               https://gitlab.com, or a self-managed instance
//   project                numeric id, or "group/name" path
//   ref                    only pipelines of this branch or tag (optional)
//   lookback_days          first sync window, default 14
//   max_pipelines          per sync, default 50
//   fetch_failed_job_logs  default true
//   max_log_bytes          per job log, default 20 MB
//   max_job_logs           failed-job logs fetched per sync, default 20
#pragma once

#include <memory>

#include "core/signals/connector.hpp"

namespace mpi::intelligence {

std::unique_ptr<signals::SignalConnector> make_gitlab_connector();

}  // namespace mpi::intelligence
