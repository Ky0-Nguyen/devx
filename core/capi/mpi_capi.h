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

/* ---- starting a simulator or emulator -----------------------------------
 *
 * Discovery lists what is there; these start something that is not. "Started"
 * and "ready" come back as separate booleans, because a device that was asked
 * to boot and never came up is a real outcome -- and a capture taken against
 * a half-booted device measures the boot. */

/* What could be started: Android AVDs and iOS simulators. */
char* mpi_boot_targets_json(void);

/* Starts one and waits until it answers. `identifier` is an AVD name or a
 * simulator UDID, exactly as `mpi_boot_targets_json` reported it. The device
 * id in the result is the one *observed* afterwards, or null when none could
 * be confirmed -- an AVD name is not a device id. */
char* mpi_boot_json(const char* identifier, int ready_timeout_s);

/* Why an unusable iOS device is unusable, and what to do about it.
 *
 * "offline" is true and useless on its own: it does not say whether to reach
 * for a cable, unlock the screen, trust the computer, or enable Developer
 * Mode. Everything needed to answer that is in the listing already.
 *
 * `probe` runs a live reachability check, which costs about a tenth of a
 * second and is the only way to tell "not here" from "here, but the cached
 * listing is stale".
 */
char* mpi_device_advice_json(const char* device_id, int probe);

/* Reads a running React Native app's network calls, console output and Redux
 * state through the inspector the app already runs -- nothing is added to the
 * app. See core/observe/inspect.hpp for what that reaches and what it does
 * not; the returned document carries those limits in its `caveats` array, so
 * a caller that renders the data has the caveats to hand.
 *
 * `app_id` may be empty, meaning whatever is attached. `seconds` is the
 * observation window. `metro_port` 0 means the default 8081. `flags` is a bit
 * set: 1 read the Redux store, 2 include its values, 4 take screenshots
 * (which needs `device_id`).
 */
char* mpi_inspect_json(const char* app_id, int seconds, int metro_port,
                       int flags, const char* device_id,
                       const char* screenshot_dir,
                       const char* target_device);

/* What is attachable right now, without holding the debugger slot. */
char* mpi_inspect_targets_json(int metro_port);

/* Live observation, so the data arrives while it happens.
 *
 * `mpi_inspect_json` returns when its window closes, which is fine for a
 * script and wrong for a person: the interesting API calls happen when you
 * tap something, and a report that appears fifteen seconds later cannot be
 * connected to what you just did.
 *
 * Start once, poll as often as you like, stop when done. Each poll returns
 * the whole observation to date, not a delta -- a network exchange is
 * assembled from three separate events, and a caller stitching deltas would
 * have to re-implement that.
 *
 * Only one stream at a time: the debugger slot is single-occupancy, and so is
 * this. Starting a second returns an error rather than displacing the first.
 */
char* mpi_inspect_stream_start(const char* app_id, int metro_port, int flags,
                               const char* device_id,
                               const char* screenshot_dir,
                               const char* target_device);
/* Reads for up to `budget_ms`, then returns the observation so far. */
char* mpi_inspect_stream_poll(int budget_ms);
/* Stops and returns the final observation. */
char* mpi_inspect_stream_stop(void);

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
/* `collect_scheduling` and `collect_heap` are the heavier collectors: the
 * first traces the whole device, the second pauses the app and writes tens of
 * megabytes. A caller is expected to have said so before passing them
 * (spec section 13: explain the overhead before enabling a heavier
 * collector). */
char* mpi_record_json(const char* sessions_dir, const char* device_id,
                      const char* app_identifier, int duration_s,
                      int sample_hz, int collect_frames, int collect_cpu,
                      int collect_memory, int reset_frame_history,
                      int collect_scheduling, int collect_heap,
                      int timeout_ms);

/* Copies a stored report out of a session package to `out_path`.
 *
 * `format` is "json" or "markdown". The package's own report is copied rather
 * than regenerated, so what is exported is what was recorded -- checksum
 * failures are reported alongside so the caller knows whether to trust it.
 * Nothing is uploaded anywhere: this writes a local file. */
char* mpi_export_session_json(const char* sessions_dir, const char* session_id,
                              const char* format, const char* out_path);

/* ---- suppressions -------------------------------------------------------
 *
 * A project's suppression list, shared with the CLI's `--suppressions`. A
 * reason is mandatory: an entry without one is refused rather than written,
 * because a suppression nobody can review is permanent by accident. An entry
 * whose expiry has passed is reported and NOT applied. */

/* Reads the list at `path`. A missing file is not an error: it yields an
 * empty list, which is what a project with no suppressions has. */
char* mpi_suppressions_json(const char* path);

/* Appends one entry and rewrites the file atomically. `reason` must be
 * non-empty. An empty `fingerprint` suppresses every finding from the rule. */
char* mpi_add_suppression_json(const char* path, const char* rule_id,
                               const char* fingerprint, const char* reason,
                               const char* expiry, const char* author,
                               const char* reference);

/* Removes the entry matching `rule_id` and `fingerprint` exactly. */
char* mpi_remove_suppression_json(const char* path, const char* rule_id,
                                  const char* fingerprint);

/* Re-runs the analysis over a stored session's trace, applying the
 * suppression list at `suppressions_path` (empty for none). The session on
 * disk is not modified: the raw trace is immutable, and a re-analysis is a
 * view of it rather than a replacement. */
char* mpi_reanalyze_session_json(const char* sessions_dir,
                                 const char* session_id,
                                 const char* suppressions_path);

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
