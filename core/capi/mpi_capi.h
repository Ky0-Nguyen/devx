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

/* ---- layout ---------------------------------------------------------------
 *
 * How the screen an app is showing is built: views per screen, nesting depth,
 * hidden and off-screen views, navigation stacks. Nothing is added to the app:
 * Android is read through `dumpsys activity top`; an iOS simulator through
 * the layout probe, injected only when `relaunch` is non-zero -- which
 * restarts the app and loses its state, so a caller must have said so first.
 * Without it, an app not launched with the probe answers
 * `failure: "probe_not_loaded"` rather than being restarted.
 *
 * With `save_to_sessions_dir` set, the report (every view) is also saved as
 * an observation, and `saved: {id, path}` says where.
 *
 * The result is {ok, failure, error?, notes[], launched_pid?, saved?, report}. A
 * report is a snapshot of structure, never a performance measurement. */
char* mpi_layout_json(const char* device_id, const char* app_identifier,
                      int relaunch, int settle_ms, int include_tree,
                      int include_simulators, int timeout_ms,
                      const char* save_to_sessions_dir);

/* ---- Android emulator ----------------------------------------------------
 *
 * Android emulators without Android Studio (see docs/android-emulator.md).
 * The SDK is an existing one when there is one, otherwise DevX's own.
 * Nothing is installed whose license has not been accepted, and
 * mpi_android_accept_license_json must only be called after a person read the
 * license and agreed to it. */

/* Where the SDK is, what it holds, every AVD (with `running` when it is), and
 * running emulators. */
char* mpi_android_sdk_json(void);
/* What can be installed, with license texts. Network: fetches Google's
 * manifests. The result is cached for accept/install below. */
char* mpi_android_catalog_json(int timeout_ms);
char* mpi_android_accept_license_json(const char* license_id);
/* Installs one package in the background; poll for progress. One at a time. */
char* mpi_android_install_start_json(const char* package_path);
char* mpi_android_install_poll_json(void);
void mpi_android_install_cancel(void);
char* mpi_android_presets_json(void);
/* `preset_id` may be empty; then width/height/density are used. */
char* mpi_avd_create_json(const char* name, const char* system_image, const char* preset_id,
                          int width, int height, int density, int ram_mb);
char* mpi_avd_delete_json(const char* avd_id);
/* `override_running` != 0 changes a running device through `wm` (no restart,
 * 0 x 0 resets); otherwise the hardware size, at the next (cold) boot. */
char* mpi_avd_resize_json(const char* avd_id, int width, int height, int density,
                          int override_running);
/* Blocks until Android has booted (or the attempt fails); run it off the main
 * thread. Cancellable with mpi_cancel_all. */
char* mpi_emulator_start_json(const char* avd_id, int cold_boot, int wipe_data);
char* mpi_emulator_stop_json(const char* avd_id);

/* The screen of a running AVD, one session per device, so several can be
 * shown at once. Open returns {handle, path, box, serial, avd_id}: `path` is
 * a file of box*box*4 bytes the emulator writes RGBA8888 frames into. Opening
 * a device that is already shown replaces its session. Poll with
 * mpi_emulator_display_frame, which returns 0 once the session has ended (the
 * emulator stopped or restarted) and otherwise the latest frame's sequence
 * number and size. Close one handle, or every session with 0. */
char* mpi_emulator_display_open_json(const char* avd_id, int box);
int mpi_emulator_display_frame(int handle, unsigned int* seq, unsigned int* width,
                               unsigned int* height);
void mpi_emulator_display_close(int handle);
/* Input for a session, queued and sent off the caller's thread. Coordinates
 * are device pixels. `pressure` 0 lifts the finger. Key `phase`: 0 down, 1 up,
 * 2 press. */
void mpi_emulator_touch(int handle, int x, int y, int pressure);
void mpi_emulator_key(int handle, const char* key, int phase);
void mpi_emulator_text(int handle, const char* utf8);
/* For a session: rotate (0/90/180/270), extended controls (pane), status, and
 * a screenshot kept as an observation under `sessions_dir`. */
char* mpi_emulator_rotate_json(int handle, int degrees);
char* mpi_emulator_extended_controls_json(int handle, int pane);
char* mpi_emulator_status_json(int handle);
char* mpi_emulator_screenshot_json(int handle, const char* sessions_dir);

/* ---- BrowserStack -----------------------------------------------------------
 *
 * Real devices in BrowserStack's cloud. Credentials come from
 * BROWSERSTACK_USERNAME / BROWSERSTACK_ACCESS_KEY or the Keychain (service
 * "com.devx.browserstack", which the window writes). Every reply carries
 * BrowserStack's HTTP status and body as they came. */
/* Whether credentials exist and work: the App Automate plan. */
char* mpi_bs_status_json(void);
/* GET api-cloud.browserstack.com/<path>, e.g. "app-automate/devices.json",
 * "app-automate/builds.json", "app-automate/builds/<id>/sessions.json". */
char* mpi_bs_get_json(const char* path);
/* Uploads an app; `product` is "app-live" or "app-automate". */
char* mpi_bs_upload_json(const char* product, const char* file);
/* The App Live URL that opens `device` with an uploaded app, for a browser. */
char* mpi_bs_live_url_json(const char* os, const char* os_version, const char* device,
                           const char* app_url);
/* BrowserStack Local, the tunnel that lets BrowserStack's devices reach this
 * Mac (localhost, Metro). The binary is BrowserStack's, Intel-only (Rosetta
 * on Apple Silicon), downloaded on request; it takes the key in argv. */
char* mpi_bs_local_status_json(void);
char* mpi_bs_local_install_json(void);
char* mpi_bs_local_start_json(const char* identifier);
char* mpi_bs_local_stop_json(const char* identifier);
/* Keeps a BrowserStack reply as an observation, for AI tools. */
char* mpi_bs_save_json(const char* sessions_dir, const char* what, const char* json_text);
/* App Profiling of one App Automate session (a paid BrowserStack plan),
 * written as a DevX session. `build_id` may be empty: it is read from the
 * session. Blocks; mpi_cancel stops it. */
char* mpi_bs_import_profiling_json(const char* sessions_dir, const char* build_id,
                                   const char* session_id);
/* An App Automate session DevX starts and drives (adapters/browserstack/
 * automate.hpp). `spec_json` names the fields of AutomateSpec: app_url
 * (bs://), platform, device, os_version, network_profile, gps_location,
 * timezone, language, locale, orientation, biometric, camera_injection,
 * app_profiling, local, local_identifier. Blocks until BrowserStack has the
 * app running: seconds to a couple of minutes. */
char* mpi_bs_automate_start_json(const char* spec_json);
/* The screen, written to `out_path` as a PNG (atomically): {path, width, height}. */
char* mpi_bs_automate_screenshot_json(const char* session_id, const char* out_path);
/* One input, in screenshot pixels: {"tap": [x, y]}, {"swipe": [x1, y1, x2, y2]},
 * {"text": "..."} or {"key": "back" | "home" | "enter"}. */
char* mpi_bs_automate_input_json(const char* session_id, const char* action_json);
/* Ends the session. With a sessions directory, imports its App Profiling
 * there, waiting up to 90 s for BrowserStack to publish it. */
char* mpi_bs_automate_stop_json(const char* session_id, const char* import_to_sessions_dir);

/* ---- Intelligence ----------------------------------------------------------
 *
 * Production, CI/CD and other external signals, stored locally and
 * correlated with releases, code and sessions (docs/intelligence.md). Every
 * call takes the sessions directory (the store lives under it, in
 * intelligence/) and answers {"ok": ...}. Credentials are never passed here
 * or returned: connectors read them from the environment or the Keychain.
 * validate / discover / sync reach the provider and block; mpi_cancel stops
 * them. Everything else reads the local store and works offline. */
char* mpi_intelligence_providers_json(void);
/* What can leave this machine, connector by connector. */
char* mpi_intelligence_egress_json(const char* sessions_dir);
char* mpi_intelligence_workspaces_json(const char* sessions_dir);
/* {id, name, repository_root, app_identifiers[], environments[], retention{days}} */
char* mpi_intelligence_workspace_save_json(const char* sessions_dir, const char* workspace_json);
char* mpi_intelligence_workspace_delete_json(const char* sessions_dir, const char* workspace_id);
char* mpi_intelligence_integrations_json(const char* sessions_dir, const char* workspace_id);
/* {id, provider, name, settings{}, credential_ref, retention{days}, paused}.
 * A setting that looks like a secret is refused. */
char* mpi_intelligence_connector_save_json(const char* sessions_dir, const char* workspace_id,
                                           const char* connector_json);
char* mpi_intelligence_connector_remove_json(const char* sessions_dir, const char* workspace_id,
                                             const char* connector_id, int delete_local_data);
char* mpi_intelligence_validate_json(const char* sessions_dir, const char* workspace_id,
                                     const char* connector_id);
char* mpi_intelligence_discover_json(const char* sessions_dir, const char* workspace_id,
                                     const char* connector_id);
/* connector_id empty: every connector that is not paused. */
char* mpi_intelligence_sync_json(const char* sessions_dir, const char* workspace_id,
                                 const char* connector_id);
char* mpi_intelligence_overview_json(const char* sessions_dir, const char* workspace_id);
/* {provider, connector_id, kind, severity, environment, release_key, version,
 *  commit, since, until, text, limit} */
char* mpi_intelligence_signals_json(const char* sessions_dir, const char* workspace_id,
                                    const char* query_json);
char* mpi_intelligence_signal_json(const char* sessions_dir, const char* workspace_id,
                                   const char* signal_id, int include_raw);
char* mpi_intelligence_releases_json(const char* sessions_dir, const char* workspace_id);
char* mpi_intelligence_release_json(const char* sessions_dir, const char* workspace_id,
                                    const char* release_key);
char* mpi_intelligence_compare_json(const char* sessions_dir, const char* workspace_id,
                                    const char* base_release, const char* candidate_release);
char* mpi_intelligence_related_json(const char* sessions_dir, const char* workspace_id,
                                    const char* signal_id);
char* mpi_intelligence_code_context_json(const char* sessions_dir, const char* workspace_id,
                                         const char* signal_id);
/* {release?, signal_id?, question?, include_raw?, include_code?} */
char* mpi_intelligence_evidence_pack_json(const char* sessions_dir, const char* workspace_id,
                                          const char* scope_json);
/* apply 0: what retention would delete; 1: delete it. */
char* mpi_intelligence_retention_json(const char* sessions_dir, const char* workspace_id, int apply);
/* kind "signal" or "release". */
char* mpi_intelligence_pin_json(const char* sessions_dir, const char* workspace_id, const char* kind,
                                const char* id, int pinned);
char* mpi_intelligence_link_session_json(const char* sessions_dir, const char* workspace_id,
                                         const char* session_id, const char* release_key, int linked);

/* ---- the window's control endpoint ---------------------------------------
 *
 * Lets an AI tool, through `mpi mcp`, ask this window to show something and
 * read what it shows (core/capi/control.hpp). Loopback only, token per start,
 * endpoint written to `<dir>/control.json` with mode 0600. */
char* mpi_control_start_json(const char* dir);
void mpi_control_stop(void);
/* What the window shows, as JSON; returned verbatim to GET /v1/state. */
void mpi_control_set_state_json(const char* json_text);
/* The next command for the window ({id, command}) or {} when none. */
char* mpi_control_next_json(void);
/* The window's answer to command `id`. */
void mpi_control_reply(int id, const char* json_text);

/* ---- observations on disk ------------------------------------------------
 *
 * Results that are not session packages -- a layout snapshot, an inspect
 * observation -- kept under `<sessions dir>/observations/` so an AI tool
 * reading through `mpi mcp` can analyse the whole document. Owner-only files:
 * an inspect observation with detail holds bearer tokens verbatim.
 *
 * `kind` is a short lowercase word; `summary_json` and `document_json` are
 * JSON texts. Returns {ok, id, path} or {ok:false, error}. */
char* mpi_save_observation_json(const char* sessions_dir, const char* kind,
                                const char* app_identifier,
                                const char* device_id,
                                const char* summary_json,
                                const char* document_json);
/* The newest first, at most `limit` (0 for all); `kind` may be empty. */
char* mpi_observations_json(const char* sessions_dir, const char* kind, int limit);
/* One observation in full. */
char* mpi_observation_json(const char* sessions_dir, const char* id);

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
 * `stack_profile_seconds` > 0 takes a stack profile at the *end* of the
 * capture, on a platform where that is a separate step -- the iOS simulator
 * path runs `/usr/bin/sample` for that many seconds. It is an aggregate with
 * no timestamps: it says where the samples were and can never say when, so it
 * is opt-in rather than folded into the tick loop. 0 disables it.
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
                     int timeout_ms, int stack_profile_seconds);

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
