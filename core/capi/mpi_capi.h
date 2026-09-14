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

/* One session's binned timeline: `bin_count` bins per track, each carrying a
 * state and a nullable value. A bin's `value` is null whenever its `state` is
 * not "measured" or "partial"; a caller that renders null as zero is drawing
 * a measurement that was never taken. Bounded by `bin_count`, not by the
 * capture's size. */
char* mpi_session_timeline_json(const char* sessions_dir,
                                const char* session_id, int bin_count);

/* Compares two run-set files. Never returns a regression verdict when the
 * conditions are incompatible, the runs are too few, or the variance is too
 * high -- the thresholds travel back in the result so the caller can show
 * what was applied. A zero or negative threshold means "use the default". */
char* mpi_compare_json(const char* baseline_path, const char* candidate_path,
                       int min_valid_runs, double min_relative_delta,
                       double min_absolute_delta, double max_relative_spread);

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

/* ---- live capture -------------------------------------------------------
 *
 * A capture you can watch while it runs. mpi_live_start returns as soon as the
 * collector has prepared the device; mpi_live_poll_json returns the current
 * snapshot, which a UI can call on a timer.
 *
 * Every snapshot carries `analysis_is_preliminary` and an `analysis_caveat`,
 * because findings over a window that is still open are preliminary by
 * definition (spec sections 2.2 and 13). The caveat is in the document rather
 * than left to the caller to remember.
 *
 * One live session at a time per process. */
char* mpi_live_start(const char* sessions_dir, const char* device_id,
                     const char* app_identifier, int sample_hz,
                     int collect_frames, int collect_cpu, int collect_memory,
                     int reset_frame_history, int tick_ms, int cpu_window_ms,
                     int timeout_ms);

/* The current snapshot: counts, per-source status, and the preliminary
 * analysis. Safe to call at any time, including before a start. */
char* mpi_live_poll_json(void);

/* Stops the session, closes the window, runs the final (non-preliminary)
 * analysis and writes the session package. Returns what was written, or the
 * reason nothing was. */
char* mpi_live_stop_json(void);

/* True while a session is running. */
int mpi_live_is_running(void);

/* Requests cancellation of whatever the calling thread is running. The core's
 * long operations poll this, so a capture or analysis unwinds cleanly. */
void mpi_cancel_all(void);
/* Clears the cancellation flag so subsequent calls can run. */
void mpi_cancel_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MPI_CAPI_H */
