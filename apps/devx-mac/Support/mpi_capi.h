/* C ABI over the Mobile Performance Inspector core.
 *
 * This is the seam ADR-0004 described and ADR-0008 builds DevX on. Every
 * function returns a NUL-terminated UTF-8 JSON document allocated with
 * malloc(); the caller frees it with mpi_string_free().
 *
 * Three guarantees the boundary enforces, per spec section 5:
 *
 *   - No C++ exception crosses this boundary. Every entry point catches
 *     everything and returns a JSON error document instead.
 *   - No function returns NULL. A failure is a JSON object with an "error"
 *     member, so the caller always has something to parse.
 *   - Nothing here interprets or reformats a result. The JSON is exactly what
 *     the core serializes, so a UI built on this cannot state anything the
 *     engine would not.
 */
#ifndef MPI_CAPI_H
#define MPI_CAPI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Frees a string returned by any function below. Safe on NULL. */
void mpi_string_free(char* s);

/* {"engine_version": ..., "ruleset_version": ...} */
char* mpi_version_json(void);

/* A DiscoverySnapshot. `include_simulators` is a boolean. */
char* mpi_devices_json(int include_simulators, int timeout_ms);

/* Apps on one device, with the listing's own scope and freshness. */
char* mpi_apps_json(const char* device_id, int include_simulators,
                    int timeout_ms);

/* Capability matrix plus eligibility; `app_identifier` may be NULL or "". */
char* mpi_preflight_json(const char* device_id, const char* app_identifier,
                         int include_simulators, int timeout_ms);

/* Every registered detector with its prerequisites and thresholds. */
char* mpi_rules_json(void);

/* Session packages under `sessions_dir`, newest first. */
char* mpi_sessions_json(const char* sessions_dir);

/* One session's stored report, plus its checksum verification result. */
char* mpi_session_json(const char* sessions_dir, const char* session_id);

/* Runs a live capture. Blocks for `duration_s`. The result carries a
 * per-source status array whether or not a session was written; a capture
 * that measured nothing is reported, not saved. */
char* mpi_record_json(const char* sessions_dir, const char* device_id,
                      const char* app_identifier, int duration_s,
                      int sample_hz, int collect_frames, int collect_cpu,
                      int collect_memory, int reset_frame_history,
                      int timeout_ms);

/* Analyses a trace file that is not part of a session package. */
char* mpi_analyze_trace_json(const char* trace_path);

/* Requests cancellation of whatever the calling thread is running. The core's
 * long operations poll this, so a capture or analysis unwinds cleanly. */
void mpi_cancel_all(void);
/* Clears the cancellation flag so subsequent calls can run. */
void mpi_cancel_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MPI_CAPI_H */
