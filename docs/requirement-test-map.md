# Requirement -> test mapping

Derived from the test binaries, not maintained by hand: every test case declares
the specification checklist ids it covers, and the framework prints them with
`--list-requirements`. Both harnesses do -- the C++ one and DevX's Swift one --
so a UI behaviour that is tested appears here. Regenerate with
`python3 tools/gen-requirement-map.py`.

**Coverage of specification section 18: 173 of 198 checklist items have at
least one automated test.** The remaining 25 are listed below with a stated
reason.

Two cautions on reading this:

1. A checklist item having a test does **not** mean the capability is verified
   on real hardware. `docs/capabilities/tested-capability-matrix.md` is the
   authority on that.
2. Several covered items are covered by a test of the *negative* guarantee
   (that the tool refuses to claim something) rather than of the positive
   behaviour.

## Covered (173 items, 600 test-case links)

| Checklist id | Test binary | Test case |
|---|---|---|
| A01 | `test_android_parsers` | `real_adb_with_no_device_yields_an_empty_list_not_an_error` |
| A02 | `test_android_collector` | `simpleperf_handles_the_not_debuggable_refusal` |
| A02 | `test_android_parsers` | `adapter_refuses_to_enumerate_an_unauthorized_device` |
| A02 | `test_android_parsers` | `parses_device_states_and_forms` |
| A02 | `test_identity` | `only_authorized_devices_are_usable` |
| A03 | `test_identity` | `only_authorized_devices_are_usable` |
| A03 | `test_ios_parsers` | `adapter_reports_unreachable_device_as_enumeration_failure` |
| A03 | `test_ios_parsers` | `parses_real_devicectl_device_listing` |
| A03 | `test_ios_parsers` | `real_device_metadata_is_read_from_the_right_fields` |
| A03 | `test_ios_parsers` | `unreachable_paired_device_is_offline_not_authorized` |
| A04 | `test_android_parsers` | `parses_device_states_and_forms` |
| A04 | `test_ios_parsers` | `adapter_lists_real_devices_including_simulators` |
| A04 | `test_ios_parsers` | `parses_real_simctl_device_listing` |
| A04 | `test_ios_parsers` | `simctl_runtime_key_yields_the_os_version` |
| A04 | `test_ios_parsers` | `unavailable_simulator_runtimes_are_skipped` |
| A05 | `test_ios_parsers` | `only_a_booted_simulator_is_usable` |
| A06 | `test_android_parsers` | `adapter_list_devices_reports_missing_adb_as_an_error` |
| A06 | `test_android_parsers` | `adapter_probe_reports_unsupported_when_adb_is_absent` |
| A06 | `test_android_parsers` | `parses_real_adb_version_output` |
| A06 | `test_android_parsers` | `ps_parser_reads_the_header_rather_than_fixed_columns` |
| A06 | `test_android_parsers` | `ps_parser_refuses_to_guess_at_an_unknown_header` |
| A06 | `test_android_parsers` | `ps_parser_reorders_with_a_different_column_order` |
| A06 | `test_devx_http` | `server_reports_a_bind_failure_rather_than_pretending` |
| A06 | `test_process` | `process_reports_missing_executable_distinctly` |
| A06 | `test_process` | `process_which_resolves_and_reports_absence` |
| A08 | `test_ios_parsers` | `launchctl_entries_without_a_pid_are_not_running` |
| A09 | `test_identity` | `there_is_no_foreground_state_to_guess_at` |
| A11 | `test_identity` | `unknown_runtime_state_serializes_as_unknown` |
| A11 | `test_ios_parsers` | `launchctl_entries_without_a_pid_are_not_running` |
| A12 | `test_android_collector` | `framestats_handles_empty_and_headerless_blocks` |
| A12 | `test_android_collector` | `framestats_refuses_a_header_missing_required_columns` |
| A12 | `test_android_parsers` | `adapter_list_devices_reports_missing_adb_as_an_error` |
| A12 | `test_android_parsers` | `pm_list_packages_ignores_noise` |
| A12 | `test_android_parsers` | `ps_parser_handles_empty_input` |
| A12 | `test_android_parsers` | `ps_parser_refuses_to_guess_at_an_unknown_header` |
| A12 | `test_android_parsers` | `real_adb_with_no_device_yields_an_empty_list_not_an_error` |
| A12 | `test_compare` | `a_missing_file_is_an_error_not_an_empty_set` |
| A12 | `test_heap` | `a_non_hprof_file_is_refused` |
| A12 | `test_identity` | `empty_list_differs_from_enumeration_failure` |
| A12 | `test_ingestion` | `missing_file_is_an_error_not_an_empty_trace` |
| A12 | `test_ios_parsers` | `adapter_reports_unreachable_device_as_enumeration_failure` |
| A12 | `test_ios_parsers` | `launchctl_parser_handles_empty_and_header_only_input` |
| A12 | `test_process` | `process_nonzero_exit_is_not_an_error` |
| A13 | `test_android_parsers` | `pm_list_packages_parses_package_and_uid` |
| A13 | `test_android_parsers` | `pm_list_packages_without_uid_flag_still_parses` |
| A13 | `test_ios_parsers` | `devicectl_apps_parser_reads_bundle_identifiers` |
| A13 | `test_ios_parsers` | `parses_real_simctl_listapps_output` |
| A14 | `test_identity` | `two_apps_sharing_a_display_name_stay_distinct` |
| A15 | `test_identity` | `same_identifier_on_two_devices_stays_separate` |
| A15 | `test_identity` | `same_identifier_on_two_platforms_stays_separate` |
| A16 | `test_identity` | `an_app_that_exits_before_record_is_caught_by_revalidation` |
| A17 | `test_identity` | `a_restarted_app_is_not_silently_retargeted` |
| A17 | `test_identity` | `an_app_that_exits_before_record_is_caught_by_revalidation` |
| A18 | `test_identity` | `installed_is_null_when_never_observed` |
| A19 | `test_sdk` | `sdk_absence_is_reported_as_no_evidence_not_as_a_clean_app` |
| A19 | `test_sdk` | `sdk_handshake_becomes_runtime_build_facts` |
| A19 | `test_sdk` | `sdk_markers_need_a_handshake_first` |
| A19 | `test_sdk` | `sdk_refuses_a_handshake_that_does_not_identify_its_producer` |
| A20 | `test_android_collector` | `collector_refuses_an_option_like_identifier` |
| A20 | `test_android_collector` | `streaming_refuses_an_option_like_identifier` |
| A20 | `test_android_parsers` | `adapter_rejects_an_option_like_app_identifier` |
| A20 | `test_ios_parsers` | `adapter_refuses_an_option_like_bundle_id` |
| A20 | `test_process` | `process_rejects_option_like_identifier` |
| A21 | `test_android_collector` | `resolve_activity_reads_the_component_not_the_details` |
| A21 | `test_identity` | `a_launched_app_is_waited_for_until_its_process_appears` |
| A22 | `test_identity` | `a_cancelled_wait_stops_and_reports_the_cancellation` |
| A22 | `test_identity` | `a_launched_app_is_waited_for_until_its_process_appears` |
| A22 | `test_identity` | `a_wait_that_times_out_says_so_rather_than_reporting_no_processes` |
| A22 | `test_identity` | `a_zero_budget_still_gets_one_look` |
| A23 | `devx_swift_tests` | `an app not enumerated is not in the listing` |
| A23 | `devx_swift_tests` | `never labelled 'not running': a listing that could not see an app and an app that is gone are different facts` |
| A23 | `devx_swift_tests` | `no enumeration means nothing can be said` |
| A23 | `devx_swift_tests` | `the favourite survived 40 later targets: someone marked it on purpose and dropping it would look like it was never marked` |
| A24 | `test_ios_parsers` | `readiness_for_an_unknown_identifier_is_absent` |
| A25 | `devx_swift_tests` | `a field nothing records offers no options, so a filter cannot be set to something that matches nothing` |
| A25 | `devx_swift_tests` | `an issue with no screen does not match a screen filter` |
| A25 | `devx_swift_tests` | `the suppressed issue is counted as hidden` |
| B01 | `test_android_collector` | `atrace_mapping_attributes_only_the_apps_threads` |
| B02 | `test_android_parsers` | `proc_stat_handles_a_comm_name_containing_spaces` |
| B02 | `test_android_parsers` | `proc_stat_malformed_returns_nothing_not_a_fake_value` |
| B02 | `test_android_parsers` | `proc_stat_starttime_is_extracted` |
| B02 | `test_identity` | `pid_reuse_does_not_merge_processes` |
| B03 | `test_android_parsers` | `proc_stat_starttime_is_extracted` |
| B03 | `test_identity` | `a_restarted_app_is_not_silently_retargeted` |
| B03 | `test_identity` | `process_restart_is_a_new_instance` |
| B04 | `test_identity` | `reboot_invalidates_process_identity` |
| B06 | `test_identity` | `ambiguous_ownership_is_excluded_from_app_totals` |
| B06 | `test_rules` | `ambiguous_ownership_is_surfaced_as_a_data_quality_note` |
| B07 | `test_android_parsers` | `isolated_process_identity_is_recognised` |
| B07 | `test_heap` | `objects_are_attributed_to_their_heap` |
| B07 | `test_identity` | `ambiguous_ownership_is_excluded_from_app_totals` |
| B07 | `test_ingestion` | `chrome_reader_marks_ownership_as_unestablished` |
| B07 | `test_rules` | `ambiguous_ownership_is_surfaced_as_a_data_quality_note` |
| B08 | `test_android_parsers` | `work_profile_user_is_parsed_distinctly` |
| B08 | `test_identity` | `android_work_profile_is_a_distinct_target` |
| B09 | `test_ios_parsers` | `devicectl_processes_parser_reads_pid_and_path` |
| B09 | `test_ios_parsers` | `parses_real_launchctl_list_and_extracts_bundle_ids` |
| B10 | `test_identity` | `ambiguous_ownership_is_excluded_from_app_totals` |
| B11 | `test_android_parsers` | `a_permissions_flags_line_is_not_the_packages_flags_line` |
| B11 | `test_android_parsers` | `a_reinstall_changes_the_facts_that_identify_the_build` |
| B12 | `test_identity` | `same_identifier_on_two_devices_stays_separate` |
| B12 | `test_identity` | `same_identifier_on_two_platforms_stays_separate` |
| B12 | `test_ingestion` | `process_filter_excludes_foreign_events` |
| B13 | `test_symbols` | `missing_symbol_artifacts_yield_unavailable_with_a_reason` |
| B14 | `test_identity` | `profileable_is_independent_of_running_and_visible` |
| B15 | `test_android_collector` | `collector_refuses_an_empty_process_set` |
| B15 | `test_android_collector` | `streaming_refuses_to_begin_without_a_process` |
| B15 | `test_ios_parsers` | `xctrace_explains_an_attach_that_found_no_process` |
| C01 | `test_android_parsers` | `a_permissions_flags_line_is_not_the_packages_flags_line` |
| C01 | `test_eligibility` | `diagnostic_mode_never_certifies_release` |
| C01 | `test_eligibility` | `js_dev_mode_makes_benchmark_ineligible` |
| C03 | `test_eligibility` | `js_dev_mode_makes_benchmark_ineligible` |
| C04 | `test_eligibility` | `clean_optimized_build_is_benchmark_eligible` |
| C05 | `test_sdk` | `sdk_handshake_becomes_runtime_build_facts` |
| C06 | `test_eligibility` | `debuggable_without_attached_debugger_is_still_ineligible` |
| C07 | `test_eligibility` | `debugger_attached_invalidates_benchmark` |
| C08 | `test_eligibility` | `debugger_attached_invalidates_benchmark` |
| C09 | `test_android_parsers` | `dumpsys_absent_flags_stay_unknown` |
| C10 | `test_eligibility` | `conflicting_facts_are_recorded_and_block_certification` |
| C10 | `test_eligibility` | `stronger_source_wins_but_conflict_is_kept` |
| C11 | `test_android_collector` | `simpleperf_meta_info_yields_real_build_facts` |
| C11 | `test_android_parsers` | `dumpsys_flags_parse_debuggable_and_version` |
| C11 | `test_ingestion` | `xctrace_time_profile_reads_samples_stacks_and_binaries` |
| C13 | `test_eligibility` | `sanitizers_invalidate_benchmark` |
| C14 | `test_rules` | `obfuscated_frames_resolve_only_with_a_bound_mapping` |
| C14 | `test_symbols` | `missing_file_is_an_error_not_a_silent_empty_map` |
| C14 | `test_symbols` | `r8_map_deobfuscates_and_binds_to_a_build` |
| C14 | `test_symbols` | `r8_map_from_another_build_is_reported_as_mismatch` |
| C15 | `test_symbols` | `absent_expected_bundle_id_downgrades_to_partial` |
| C15 | `test_symbols` | `matching_bundle_id_yields_exact_build_match` |
| C15 | `test_symbols` | `mismatched_bundle_id_is_rejected_not_used_silently` |
| C15 | `test_symbols` | `missing_file_is_an_error_not_a_silent_empty_map` |
| C16 | `test_sdk` | `sdk_handshake_becomes_runtime_build_facts` |
| C16 | `test_sdk` | `sdk_notices_a_reload_onto_a_different_bundle` |
| C17 | `test_symbols` | `dirty_checkout_blocks_exact_source_claims` |
| C18 | `test_android_collector` | `atrace_mapping_keeps_states_apart_and_only_calls_iowait_io` |
| C18 | `test_android_collector` | `det03_will_not_promote_a_bare_uninterruptible_state` |
| C18 | `test_android_collector` | `meminfo_absent_fields_stay_absent` |
| C18 | `test_android_collector` | `simpleperf_meta_info_yields_real_build_facts` |
| C18 | `test_android_parsers` | `atrace_only_treats_an_iowait_flag_as_io` |
| C18 | `test_android_parsers` | `atrace_without_a_header_says_the_loss_is_unknown` |
| C18 | `test_android_parsers` | `dumpsys_absent_flags_stay_unknown` |
| C18 | `test_android_parsers` | `dumpsys_flags_parse_debuggable_and_version` |
| C18 | `test_eligibility` | `missing_fact_returns_unknown_not_false` |
| C18 | `test_eligibility` | `unknown_fact_yields_insufficient_evidence_not_a_pass` |
| C18 | `test_identity` | `installed_is_null_when_never_observed` |
| C18 | `test_ingestion` | `xctrace_toc_reads_the_run_without_claiming_a_platform` |
| C18 | `test_json` | `json_null_is_distinct_from_absent` |
| C18 | `test_report` | `unknown_values_are_shown_as_unknown_not_omitted` |
| C18 | `test_symbols` | `absent_expected_bundle_id_downgrades_to_partial` |
| C18 | `test_timeline` | `js_tasks_on_an_unmapped_clock_are_not_placed` |
| C18 | `test_xml` | `xml_absent_attribute_differs_from_an_empty_one` |
| C19 | `test_compare` | `det08_says_a_forced_pair_certifies_nothing` |
| C19 | `test_compare` | `ineligible_side_blocks_certification` |
| C19 | `test_eligibility` | `eligibility_reports_every_reason_not_just_the_first` |
| C19 | `test_eligibility` | `user_override_is_audited_and_never_certifies` |
| C19 | `test_process` | `process_env_override_applies` |
| C20 | `test_android_parsers` | `parses_device_states_and_forms` |
| C20 | `test_android_parsers` | `probe_never_claims_physical_verification_from_an_emulator` |
| C20 | `test_identity` | `simulator_form_survives_serialization` |
| D01 | `test_session_lifecycle` | `a_capture_starts_ticks_and_stops` |
| D01 | `test_session_lifecycle` | `a_collector_that_refuses_to_begin_writes_nothing` |
| D01 | `test_session_lifecycle` | `a_snapshot_before_the_close_is_preliminary` |
| D02 | `test_session_lifecycle` | `stopping_during_startup_leaves_no_half_state` |
| D03 | `test_session_lifecycle` | `stopping_twice_is_idempotent` |
| D04 | `test_android_parsers` | `atrace_reports_a_dropped_buffer_rather_than_absorbing_it` |
| D04 | `test_json` | `stream_parser_can_abandon_an_array_midway` |
| D04 | `test_process` | `process_honours_cancellation` |
| D04 | `test_rules` | `cancellation_stops_analysis_and_marks_rules_skipped` |
| D05 | `test_heap` | `a_cyclic_superclass_chain_does_not_hang` |
| D05 | `test_heap` | `a_non_hprof_file_is_refused` |
| D05 | `test_heap` | `a_primitive_array_is_named_from_its_element_type` |
| D05 | `test_heap` | `a_superclass_described_after_its_instance_is_still_read` |
| D05 | `test_heap` | `a_truncated_dump_reports_what_it_got` |
| D05 | `test_heap` | `an_incomplete_chain_is_counted_not_ignored` |
| D05 | `test_heap` | `an_unknown_field_type_abandons_the_rest_of_its_segment` |
| D05 | `test_heap` | `an_unsupported_identifier_size_is_refused_not_guessed` |
| D06 | `test_session_lifecycle` | `a_missing_artifact_fails_the_write_rather_than_the_manifest` |
| D06 | `test_session_lifecycle` | `an_interrupted_write_is_never_mistaken_for_a_session` |
| D07 | `test_ios_parsers` | `xctrace_timeout_is_a_provider_failure_not_an_empty_capture` |
| D08 | `test_heap` | `a_truncated_dump_reports_what_it_got` |
| D08 | `test_ingestion` | `missing_file_is_an_error_not_an_empty_trace` |
| D08 | `test_ingestion` | `rejects_malformed_traces_without_crashing` |
| D08 | `test_json` | `json_rejects_malformed` |
| D08 | `test_json` | `stream_parser_reports_malformed_input` |
| D08 | `test_timeline` | `a_capture_with_no_window_yields_no_axis` |
| D08 | `test_timeline` | `a_gap_makes_its_bin_partial_and_says_how_much` |
| D08 | `test_timeline` | `an_uncovered_bin_has_no_value_at_all` |
| D08 | `test_timeline` | `the_last_bin_stops_at_the_window` |
| D08 | `test_timeline` | `the_peak_ignores_partial_bins` |
| D09 | `test_ingestion` | `derived_coverage_detects_late_start_and_early_stop` |
| D09 | `test_ingestion` | `normalize_turns_provider_drops_into_visible_notes` |
| D09 | `test_report` | `coverage_gaps_are_reported_with_their_semantics` |
| D09 | `test_timeline` | `a_gap_makes_its_bin_partial_and_says_how_much` |
| D09 | `test_trace_model` | `coverage_clamps_gaps_to_the_window` |
| D09 | `test_trace_model` | `coverage_gap_is_not_measured_zero` |
| D10 | `test_ingestion` | `normalize_flags_duplicates_without_deleting_them` |
| D10 | `test_ingestion` | `normalize_records_out_of_order_before_sorting` |
| D10 | `test_timeline` | `a_reading_at_the_windows_last_instant_is_inside_it` |
| D10 | `test_timeline` | `an_event_outside_the_window_is_counted_as_unplaced` |
| D11 | `test_ingestion` | `chrome_reader_derives_js_tasks_only_from_complete_spans` |
| D11 | `test_ingestion` | `chrome_reader_pairs_spans_and_flags_unpaired` |
| D11 | `test_rules` | `det01_excludes_unjudgeable_frames_from_the_denominator` |
| D11 | `test_rules` | `det02_ignores_spans_with_no_duration` |
| D11 | `test_timeline` | `only_a_judgeable_frame_counts_as_missed` |
| D11 | `test_trace_model` | `frame_without_presentation_cannot_be_judged` |
| D12 | `test_ingestion` | `chrome_reader_refuses_to_claim_a_clock_base` |
| D12 | `test_ingestion` | `normalize_flags_unmappable_clock_domains` |
| D12 | `test_ingestion` | `unmeasured_clock_mapping_is_flagged_at_read_time` |
| D12 | `test_timeline` | `a_measured_mapping_places_them` |
| D12 | `test_timeline` | `js_tasks_on_an_unmapped_clock_are_not_placed` |
| D12 | `test_trace_model` | `unmapped_clock_domain_is_not_silently_aligned` |
| D13 | `test_android_parsers` | `atrace_timestamps_keep_their_precision` |
| D14 | `test_compare` | `a_duration_is_never_derived_from_the_wall_clock` |
| D16 | `test_android_collector` | `framestats_handles_empty_and_headerless_blocks` |
| D16 | `test_android_collector` | `simpleperf_handles_empty_input` |
| D16 | `test_android_parsers` | `ps_parser_handles_empty_input` |
| D16 | `test_ingestion` | `empty_capture_reads_but_produces_no_data` |
| D16 | `test_ios_parsers` | `launchctl_parser_handles_empty_and_header_only_input` |
| D16 | `test_json` | `stream_parser_handles_empty_arrays_and_objects` |
| D16 | `test_timeline` | `a_counter_bin_holds_a_reading_not_a_sum` |
| D17 | `test_process` | `process_times_out_and_says_so` |
| D18 | `test_android_collector` | `displayed_log_reads_each_unit_rather_than_assuming_a_shape` |
| D18 | `test_android_collector` | `framestats_reads_the_header_rather_than_fixed_columns` |
| D18 | `test_android_collector` | `framestats_refuses_a_header_missing_required_columns` |
| D18 | `test_android_parsers` | `a_permissions_flags_line_is_not_the_packages_flags_line` |
| D18 | `test_android_parsers` | `a_short_or_non_numeric_proc_stat_yields_nothing` |
| D18 | `test_android_parsers` | `atrace_reads_userspace_slices_and_tolerates_a_nameless_one` |
| D18 | `test_android_parsers` | `proc_stat_cpu_time_survives_a_comm_with_spaces_and_parens` |
| D18 | `test_ingestion` | `unsupported_schema_version_is_reported_not_guessed` |
| D18 | `test_ios_parsers` | `devicectl_parsers_tolerate_unexpected_shapes` |
| D18 | `test_json` | `json_bom_is_tolerated` |
| D18 | `test_json` | `stream_parser_skips_without_materialising` |
| D18 | `test_json` | `stream_parser_tolerates_a_bom` |
| D18 | `test_session_lifecycle` | `a_malformed_suppression_file_is_an_error_not_an_empty_list` |
| D18 | `test_symbols` | `source_map_rejects_wrong_version` |
| D18 | `test_xml` | `xml_reads_cdata_as_text` |
| D18 | `test_xml` | `xml_reads_elements_attributes_and_text` |
| D18 | `test_xml` | `xml_skip_element_walks_past_a_subtree` |
| D18 | `test_xml` | `xml_skips_the_declaration_and_comments` |
| D19 | `test_ingestion` | `xctrace_export_refuses_a_malformed_document` |
| D19 | `test_ios_parsers` | `xctrace_keeps_a_bundle_it_wrote_despite_reporting_errors` |
| D19 | `test_report` | `partial_capture_is_announced` |
| D19 | `test_rules` | `partial_capture_is_surfaced_and_never_a_clean_pass` |
| D19 | `test_sdk` | `sdk_rejects_malformed_and_unknown_markers` |
| D19 | `test_xml` | `xml_reports_malformed_documents_instead_of_guessing` |
| D20 | `test_session_lifecycle` | `a_capture_that_loses_its_device_says_so_and_keeps_what_it_had` |
| D20 | `test_session_lifecycle` | `a_healthy_capture_is_never_called_partial` |
| D21 | `test_session_lifecycle` | `a_second_capture_on_a_running_session_is_refused` |
| D22 | `test_session_lifecycle` | `delete_refuses_a_directory_that_is_not_the_named_session` |
| E01 | `test_android_collector` | `framestats_parses_real_emulator_output` |
| E01 | `test_android_collector` | `framestats_yields_the_refresh_rate_from_the_platform` |
| E01 | `test_rules` | `det01_skips_when_refresh_rate_unobserved` |
| E01 | `test_trace_model` | `deadline_comes_from_observed_refresh_rate` |
| E01 | `test_trace_model` | `frame_without_deadline_cannot_be_judged` |
| E01 | `test_trace_model` | `variable_refresh_yields_no_single_deadline` |
| E02 | `test_android_collector` | `framestats_yields_the_refresh_rate_from_the_platform` |
| E02 | `test_trace_model` | `refresh_rate_change_mid_session_selects_the_right_interval` |
| E02 | `test_trace_model` | `variable_refresh_yields_no_single_deadline` |
| E03 | `test_android_collector` | `framestats_skips_platform_excluded_frames` |
| E04 | `test_android_collector` | `framestats_skips_platform_excluded_frames` |
| E06 | `test_android_collector` | `det09_reports_waits_on_visible_threads_and_excludes_the_rest` |
| E06 | `test_android_parsers` | `atrace_distinguishes_blocked_from_merely_preempted` |
| E10 | `test_android_collector` | `atrace_mapping_keeps_states_apart_and_only_calls_iowait_io` |
| E10 | `test_android_parsers` | `atrace_distinguishes_blocked_from_merely_preempted` |
| E10 | `test_android_parsers` | `atrace_reads_a_real_cold_start_trace` |
| E10 | `test_timeline` | `a_covered_empty_bin_is_a_measured_zero` |
| E10 | `test_timeline` | `an_interval_is_spread_not_spiked` |
| E11 | `test_android_parsers` | `a_short_or_non_numeric_proc_stat_yields_nothing` |
| E11 | `test_android_parsers` | `proc_stat_cpu_time_is_read_from_the_right_fields` |
| E11 | `test_android_parsers` | `proc_stat_cpu_time_survives_a_comm_with_spaces_and_parens` |
| E12 | `test_rules` | `det04_reports_self_share_as_disjoint_and_states_its_scope` |
| E12 | `test_trace_model` | `cpu_percentage_declares_its_normalization` |
| E13 | `test_android_collector` | `am_start_w_refuses_the_zero_of_an_app_already_running` |
| E13 | `test_android_collector` | `uptime_is_read_or_refused_never_defaulted` |
| E13 | `test_android_parsers` | `atrace_reports_a_dropped_buffer_rather_than_absorbing_it` |
| E13 | `test_android_parsers` | `atrace_without_a_header_says_the_loss_is_unknown` |
| E13 | `test_identity` | `a_wait_that_times_out_says_so_rather_than_reporting_no_processes` |
| E13 | `test_ingestion` | `derived_coverage_treats_absent_collector_as_a_full_gap` |
| E13 | `test_ios_parsers` | `xctrace_timeout_is_a_provider_failure_not_an_empty_capture` |
| E13 | `test_report` | `coverage_gaps_are_reported_with_their_semantics` |
| E13 | `test_rules` | `det04_skips_when_no_samples_and_says_it_is_not_idle` |
| E13 | `test_sdk` | `sdk_counts_what_the_app_dropped_before_sending` |
| E13 | `test_sdk` | `sdk_records_a_lost_batch_as_a_gap_not_as_silence` |
| E13 | `test_trace_model` | `coverage_gap_is_not_measured_zero` |
| E14 | `test_rules` | `det04_skips_below_minimum_sample_population` |
| E15 | `test_android_collector` | `simpleperf_stacks_are_outermost_first` |
| E15 | `test_ingestion` | `hermes_reader_unwinds_stacks_outermost_first` |
| E15 | `test_ingestion` | `xctrace_time_profile_reads_samples_stacks_and_binaries` |
| E16 | `test_rules` | `nested_js_spans_are_not_double_counted` |
| E17 | `test_android_collector` | `atrace_mapping_attributes_only_the_apps_threads` |
| E20 | `test_rules` | `det02_overlap_is_a_candidate_cause_never_proven` |
| E20 | `test_rules` | `det11_observes_the_delay_and_blames_nobody` |
| E21 | `test_rules` | `det01_excludes_unjudgeable_frames_from_the_denominator` |
| E21 | `test_rules` | `det01_observed_requires_presentation_truth` |
| E21 | `test_rules` | `det01_proxy_source_downgrades_to_suspected` |
| E21 | `test_timeline` | `only_a_judgeable_frame_counts_as_missed` |
| E21 | `test_trace_model` | `frame_without_presentation_cannot_be_judged` |
| E21 | `test_trace_model` | `proxy_frame_source_is_not_presentation_truth` |
| E22 | `test_compare` | `cross_platform_pair_cannot_gate` |
| E22 | `test_ingestion` | `chrome_reader_refuses_to_claim_a_clock_base` |
| F01 | `test_rules` | `det05_reports_growth_as_suspected_and_never_as_a_leak` |
| F02 | `test_rules` | `det05_calls_warm_up_warm_up_rather_than_retention` |
| F03 | `test_rules` | `det05_reports_growth_as_suspected_and_never_as_a_leak` |
| F04 | `test_rules` | `det05_reports_growth_as_suspected_and_never_as_a_leak` |
| F05 | `test_rules` | `det05_reports_native_growth_while_the_js_heap_stays_put` |
| F07 | `test_heap` | `a_primitive_array_is_named_from_its_element_type` |
| F07 | `test_heap` | `objects_are_attributed_to_their_heap` |
| F07 | `test_timeline` | `a_counter_bin_holds_a_reading_not_a_sum` |
| F07 | `test_timeline` | `a_covered_bin_without_a_sample_is_not_a_zero` |
| F07 | `test_timeline` | `a_reading_at_the_windows_last_instant_is_inside_it` |
| F07 | `test_timeline` | `memory_families_stay_separate_tracks` |
| F08 | `test_android_collector` | `meminfo_parses_real_output_and_keeps_families_distinct` |
| F08 | `test_rules` | `det05_never_sums_memory_families_and_says_so` |
| F08 | `test_rules` | `det05_reports_native_growth_while_the_js_heap_stays_put` |
| F09 | `test_rules` | `memory_families_are_never_summed_across_processes` |
| F10 | `test_heap` | `a_reference_chain_is_read_from_the_dump` |
| F10 | `test_heap` | `a_static_field_is_an_edge_a_path_can_run_through` |
| F10 | `test_heap` | `an_app_root_is_preferred_over_vm_bookkeeping` |
| F10 | `test_heap` | `an_array_element_path_names_its_index` |
| F10 | `test_heap` | `det06_reports_a_destroyed_object_that_is_still_held` |
| F10 | `test_heap` | `det06_says_when_only_the_runtime_holds_the_object` |
| F11 | `test_android_collector` | `atrace_mapping_attributes_only_the_apps_threads` |
| F11 | `test_process` | `process_separates_stdout_and_stderr` |
| F13 | `test_rules` | `det12_excludes_name_only_matches_from_attribution` |
| F14 | `test_compare` | `debug_versus_release_is_not_a_certified_comparison` |
| F14 | `test_eligibility` | `diagnostic_mode_never_certifies_release` |
| F14 | `test_report` | `attribution_section_forbids_subtraction_in_prose` |
| F14 | `test_rules` | `det12_forbids_release_estimation_by_subtraction` |
| F14 | `test_rules` | `diagnostic_session_never_reports_a_certified_benchmark` |
| F14 | `test_trace_model` | `attribution_forbids_subtraction_structurally` |
| F16 | `test_timeline` | `json_writes_an_unmeasured_bin_as_null` |
| F17 | `test_report` | `inclusive_stacks_are_labelled_in_the_markdown` |
| F17 | `test_rules` | `det04_reports_self_share_as_disjoint_and_states_its_scope` |
| F17 | `test_trace_model` | `inclusive_share_is_not_summable_as_disjoint_cost` |
| G01 | `test_android_collector` | `simpleperf_finds_the_react_native_js_thread` |
| G01 | `test_ingestion` | `hermes_reader_produces_samples_not_tasks` |
| G01 | `test_rules` | `det02_refuses_sampling_only_input` |
| G04 | `test_sdk` | `sdk_notices_a_reload_onto_a_different_bundle` |
| G07 | `test_sdk` | `sdk_accepts_the_marker_kinds_the_spec_names` |
| G08 | `test_sdk` | `sdk_accepts_the_marker_kinds_the_spec_names` |
| G10 | `test_rules` | `det10_reports_a_render_pattern_and_refuses_to_call_it_a_defect` |
| G10 | `test_rules` | `det10_will_not_accept_cpu_samples_as_render_data` |
| G11 | `test_eligibility` | `remote_js_execution_invalidates_benchmark` |
| G14 | `test_symbols` | `exact_match_resolves_to_a_real_local_file` |
| G14 | `test_symbols` | `matching_bundle_id_yields_exact_build_match` |
| G14 | `test_symbols` | `source_map_v3_loads_and_decodes_vlq` |
| G15 | `test_sdk` | `sdk_handshake_becomes_runtime_build_facts` |
| G15 | `test_sdk` | `sdk_notices_a_reload_onto_a_different_bundle` |
| G16 | `test_symbols` | `exact_match_resolves_to_a_real_local_file` |
| G16 | `test_symbols` | `missing_local_file_downgrades_to_partial` |
| G16 | `test_trace_model` | `non_exact_symbol_match_is_not_safe_to_open` |
| G17 | `test_symbols` | `monorepo_source_root_remap_applies` |
| G18 | `test_symbols` | `a_mismatch_anywhere_dominates_the_aggregate_status` |
| G18 | `test_symbols` | `mismatched_bundle_id_is_rejected_not_used_silently` |
| G18 | `test_symbols` | `r8_map_deobfuscates_and_binds_to_a_build` |
| G18 | `test_symbols` | `source_map_rejects_malformed_vlq` |
| G18 | `test_trace_model` | `non_exact_symbol_match_is_not_safe_to_open` |
| G19 | `test_symbols` | `path_traversal_in_a_source_map_is_rejected` |
| G19 | `test_trace_model` | `non_exact_symbol_match_is_not_safe_to_open` |
| H01 | `devx_swift_tests` | `an issue with no screen does not match a screen filter` |
| H01 | `devx_swift_tests` | `an unmeasured bin stays flat even with a value attached` |
| H01 | `devx_swift_tests` | `an unrecognised state claims nothing rather than guessing` |
| H01 | `devx_swift_tests` | `never labelled 'not running': a listing that could not see an app and an app that is gone are different facts` |
| H01 | `test_android_collector` | `det03_reports_a_main_thread_block_with_its_slice` |
| H01 | `test_compare` | `a_run_with_no_value_keeps_its_place` |
| H01 | `test_compare` | `an_unstated_eligibility_is_insufficient_evidence` |
| H01 | `test_compare` | `det08_reports_a_regression_as_an_issue` |
| H01 | `test_heap` | `an_incomplete_chain_is_counted_not_ignored` |
| H01 | `test_heap` | `an_object_not_in_the_dump_yields_no_claim` |
| H01 | `test_heap` | `an_unreachable_object_is_garbage_not_retention` |
| H01 | `test_heap` | `det06_calls_an_unrooted_destroyed_object_garbage` |
| H01 | `test_heap` | `det06_qualifies_a_dump_taken_without_a_collection` |
| H01 | `test_rules` | `det01_observed_requires_presentation_truth` |
| H01 | `test_rules` | `det07_reports_a_real_launch_over_budget` |
| H01 | `test_session_lifecycle` | `a_collector_that_refuses_to_begin_writes_nothing` |
| H01 | `test_timeline` | `a_capture_with_no_window_yields_no_axis` |
| H01 | `test_timeline` | `a_covered_bin_without_a_sample_is_not_a_zero` |
| H01 | `test_timeline` | `a_covered_empty_bin_is_a_measured_zero` |
| H01 | `test_timeline` | `a_real_interval_is_never_widened` |
| H01 | `test_timeline` | `an_instant_issue_band_is_widened_and_says_so` |
| H01 | `test_timeline` | `an_uncovered_bin_has_no_value_at_all` |
| H01 | `test_timeline` | `json_writes_an_unmeasured_bin_as_null` |
| H02 | `test_rules` | `det05_finds_nothing_in_a_real_capture_that_did_not_grow` |
| H02 | `test_rules` | `healthy_capture_runs_detectors_and_finds_nothing` |
| H03 | `test_rules` | `det01_proxy_source_downgrades_to_suspected` |
| H03 | `test_rules` | `det01_skips_when_no_frame_collector_ran` |
| H03 | `test_rules` | `det04_skips_below_minimum_sample_population` |
| H04 | `test_rules` | `threshold_override_changes_the_verdict` |
| H05 | `test_heap` | `det06_without_a_dump_refuses_the_substitute` |
| H05 | `test_report` | `skipped_detectors_and_their_reasons_appear_in_the_report` |
| H05 | `test_rules` | `det05_needs_cycles_from_the_app_and_says_where_they_come_from` |
| H05 | `test_rules` | `det06_without_a_heap_dump_says_what_is_missing` |
| H05 | `test_rules` | `det07_says_a_capture_without_a_launch_has_no_startup` |
| H05 | `test_rules` | `det08_over_a_capture_says_it_needs_two_run_sets` |
| H05 | `test_rules` | `det10_will_not_accept_cpu_samples_as_render_data` |
| H05 | `test_rules` | `det11_says_it_cannot_observe_the_network_itself` |
| H05 | `test_rules` | `every_catalog_detector_is_registered` |
| H05 | `test_rules` | `no_detector_is_unimplemented_any_more` |
| H05 | `test_trace_model` | `rule_outcome_distinguishes_skipped_from_found_nothing` |
| H06 | `test_rules` | `severity_is_independent_of_causal_confidence` |
| H07 | `test_rules` | `every_evidence_reference_resolves_to_captured_data` |
| H08 | `test_android_collector` | `det09_leaves_an_io_wait_to_det03` |
| H09 | `test_rules` | `ruleset_and_engine_versions_are_recorded` |
| H09 | `test_rules` | `threshold_override_changes_the_verdict` |
| H09 | `test_session_lifecycle` | `a_capture_that_loses_its_device_says_so_and_keeps_what_it_had` |
| H09 | `test_session_lifecycle` | `a_healthy_capture_is_never_called_partial` |
| H10 | `test_eligibility` | `user_override_is_audited_and_never_certifies` |
| H10 | `test_report` | `suppressed_issues_can_be_omitted_or_kept_with_their_reason` |
| H10 | `test_rules` | `a_suppression_inside_its_expiry_still_applies` |
| H10 | `test_rules` | `a_suppression_with_no_expiry_applies_forever` |
| H10 | `test_rules` | `an_expired_suppression_is_not_applied_and_says_so` |
| H10 | `test_rules` | `an_unreadable_expiry_keeps_the_suppression_and_reports_it` |
| H10 | `test_rules` | `suppression_retains_reason_and_expiry` |
| H10 | `test_session_lifecycle` | `a_suppression_list_round_trips_with_everything_that_audits_it` |
| H10 | `test_session_lifecycle` | `a_suppression_with_no_reason_is_refused_on_read_and_write` |
| H11 | `devx_swift_tests` | `an unrecognised state claims nothing rather than guessing` |
| H11 | `devx_swift_tests` | `inconclusive is a caution, never a pass` |
| H11 | `devx_swift_tests` | `inconclusive says it is not 'no change': got no verdict could be reached. This is not 'no change': it means the evidence does not support any conclusion, and each metric says why` |
| H11 | `test_compare` | `det08_records_an_undecided_metric_rather_than_passing_it` |
| H11 | `test_heap` | `det06_says_nothing_about_a_live_object` |
| H11 | `test_json` | `json_null_is_distinct_from_absent` |
| H11 | `test_report` | `no_findings_is_distinguished_from_no_analysis` |
| H11 | `test_report` | `skipped_detectors_and_their_reasons_appear_in_the_report` |
| H11 | `test_rules` | `det01_skips_when_no_frame_collector_ran` |
| H11 | `test_rules` | `det05_finds_nothing_in_a_real_capture_that_did_not_grow` |
| H11 | `test_rules` | `det07_under_budget_runs_and_says_what_it_measured` |
| H11 | `test_rules` | `every_catalog_detector_is_registered` |
| H11 | `test_rules` | `healthy_capture_runs_detectors_and_finds_nothing` |
| H11 | `test_rules` | `no_detector_is_unimplemented_any_more` |
| H11 | `test_sdk` | `sdk_absence_is_reported_as_no_evidence_not_as_a_clean_app` |
| H11 | `test_timeline` | `a_collector_that_never_reported_coverage_measures_nothing` |
| H11 | `test_trace_model` | `rule_outcome_distinguishes_skipped_from_found_nothing` |
| H12 | `test_devx_http` | `request_param_lookup` |
| H12 | `test_devx_http` | `response_error_is_machine_readable` |
| H12 | `test_json` | `json_large_integer_does_not_wrap` |
| H12 | `test_json` | `json_non_finite_double_stays_valid_json` |
| H12 | `test_json` | `json_roundtrip_preserves_key_order` |
| H12 | `test_json` | `stream_parser_agrees_with_the_dom_parser` |
| H12 | `test_report` | `comparison_report_renders_in_both_formats` |
| H12 | `test_report` | `json_report_is_valid_and_carries_the_schema_version` |
| H12 | `test_report` | `markdown_and_json_agree_on_issue_count` |
| H13 | `test_session_lifecycle` | `a_missing_artifact_fails_the_write_rather_than_the_manifest` |
| H13 | `test_session_lifecycle` | `a_stored_artifact_is_checksummed_and_found_again` |
| H13 | `test_session_lifecycle` | `a_written_package_reopens_with_no_device` |
| H14 | `devx_swift_tests` | `the suppressed issue is counted as hidden` |
| H14 | `test_rules` | `a_suppression_inside_its_expiry_still_applies` |
| H14 | `test_rules` | `an_expired_suppression_is_not_applied_and_says_so` |
| H14 | `test_session_lifecycle` | `a_malformed_suppression_file_is_an_error_not_an_empty_list` |
| H14 | `test_session_lifecycle` | `a_missing_suppression_file_is_not_an_error` |
| H14 | `test_session_lifecycle` | `a_suppression_list_round_trips_with_everything_that_audits_it` |
| H14 | `test_session_lifecycle` | `a_suppression_with_no_reason_is_refused_on_read_and_write` |
| H14 | `test_session_lifecycle` | `an_empty_fingerprint_suppresses_the_whole_rule` |
| H14 | `test_timeline` | `a_suppressed_issue_is_not_drawn` |
| H15 | `devx_swift_tests` | `the settings view exists` |
| H15 | `test_ingestion` | `mark_synthetic_option_forces_the_label` |
| H15 | `test_ingestion` | `reads_native_trace_and_keeps_synthetic_label` |
| H15 | `test_report` | `synthetic_data_is_announced_in_both_formats` |
| H15 | `test_rules` | `synthetic_input_is_labelled_at_the_top_level` |
| H16 | `test_rules` | `no_issue_carries_an_uncalibrated_numeric_confidence` |
| H17 | `test_rules` | `every_candidate_cause_lists_missing_evidence` |
| I01 | `test_compare` | `clear_regression_is_detected` |
| I02 | `devx_swift_tests` | `inconclusive is a caution, never a pass` |
| I02 | `devx_swift_tests` | `inconclusive says it is not 'no change': got no verdict could be reached. This is not 'no change': it means the evidence does not support any conclusion, and each metric says why` |
| I02 | `test_compare` | `an_unstated_eligibility_is_insufficient_evidence` |
| I02 | `test_compare` | `det08_skips_when_conditions_are_not_comparable` |
| I02 | `test_compare` | `device_model_mismatch_blocks_a_verdict` |
| I02 | `test_compare` | `simulator_vs_physical_is_never_comparable` |
| I02 | `test_eligibility` | `simulator_is_not_benchmark_eligible` |
| I03 | `test_compare` | `a_run_set_without_conditions_is_refused` |
| I03 | `test_compare` | `os_and_refresh_mismatch_blocks_a_verdict` |
| I04 | `test_compare` | `thermal_and_power_mismatch_is_visible` |
| I05 | `test_compare` | `thermal_and_power_mismatch_is_visible` |
| I06 | `test_rules` | `a_background_workload_is_never_attributed_to_the_app` |
| I07 | `test_compare` | `launch_class_mismatch_blocks_a_verdict` |
| I08 | `test_compare` | `cache_network_and_input_mismatch_blocks_a_verdict` |
| I09 | `test_compare` | `det08_records_an_undecided_metric_rather_than_passing_it` |
| I09 | `test_compare` | `too_few_runs_is_inconclusive` |
| I10 | `test_compare` | `det08_records_an_undecided_metric_rather_than_passing_it` |
| I10 | `test_compare` | `high_variance_is_inconclusive` |
| I11 | `test_compare` | `missing_baseline_is_inconclusive_not_a_pass` |
| I11 | `test_compare` | `run_with_no_measurement_is_excluded_and_visible` |
| I12 | `test_compare` | `zero_baseline_avoids_division` |
| I13 | `test_compare` | `both_absolute_and_relative_thresholds_must_clear` |
| I13 | `test_compare` | `clear_improvement_is_detected` |
| I13 | `test_compare` | `clear_regression_is_detected` |
| I13 | `test_compare` | `det08_reports_a_regression_as_an_issue` |
| I13 | `test_compare` | `small_change_is_not_significant` |
| I14 | `test_compare` | `explicit_exclusion_reason_is_honoured_and_shown` |
| I14 | `test_compare` | `run_with_no_measurement_is_excluded_and_visible` |
| I15 | `test_compare` | `a_run_set_without_runs_is_refused` |
| I15 | `test_compare` | `a_run_with_no_value_keeps_its_place` |
| I15 | `test_compare` | `a_run_without_a_stated_outcome_is_excluded` |
| I15 | `test_compare` | `incomplete_scenario_is_excluded_not_counted_as_fast` |
| I16 | `test_compare` | `a_warm_up_run_is_marked_as_one` |
| I16 | `test_compare` | `debug_versus_release_is_not_a_certified_comparison` |
| I16 | `test_compare` | `det08_refuses_a_debug_versus_release_pair` |
| I16 | `test_compare` | `det08_says_a_forced_pair_certifies_nothing` |
| I16 | `test_eligibility` | `diagnostic_mode_never_certifies_release` |
| I16 | `test_rules` | `diagnostic_session_never_reports_a_certified_benchmark` |
| I17 | `test_android_collector` | `capture_config_records_its_preset_and_sources` |
| I17 | `test_android_collector` | `capture_config_records_the_streaming_cadence` |
| I17 | `test_compare` | `collector_sample_rate_mismatch_blocks_a_verdict` |
| I18 | `test_compare` | `a_percentile_is_never_reported_from_too_few_runs` |
| I19 | `test_compare` | `cross_platform_pair_cannot_gate` |
| I19 | `test_compare` | `det08_refuses_a_cross_platform_pair` |
| I19 | `test_compare` | `unknown_platform_blocks_equivalence` |
| I21 | `test_timeline` | `the_bin_count_does_not_follow_the_capture_size` |
| J01 | `test_devx_http` | `html_escape_neutralises_markup` |
| J01 | `test_json` | `json_escapes_on_output` |
| J01 | `test_report` | `hostile_identifiers_cannot_break_the_markdown_table` |
| J01 | `test_report` | `source_paths_are_excluded_from_export_by_default` |
| J01 | `test_sdk` | `sdk_rejects_malformed_and_unknown_markers` |
| J02 | `test_android_parsers` | `proc_stat_malformed_returns_nothing_not_a_fake_value` |
| J02 | `test_ingestion` | `hermes_reader_survives_cyclic_parent_chain` |
| J02 | `test_ingestion` | `rejects_malformed_traces_without_crashing` |
| J02 | `test_ingestion` | `xctrace_export_refuses_a_malformed_document` |
| J02 | `test_ios_parsers` | `devicectl_parsers_tolerate_unexpected_shapes` |
| J02 | `test_json` | `json_control_characters_must_be_escaped` |
| J02 | `test_json` | `json_rejects_malformed` |
| J02 | `test_json` | `json_unicode_surrogate_pairs` |
| J02 | `test_json` | `stream_parser_reports_malformed_input` |
| J02 | `test_symbols` | `source_map_rejects_malformed_vlq` |
| J02 | `test_xml` | `xml_decodes_the_entities_xctrace_actually_emits` |
| J02 | `test_xml` | `xml_refuses_a_document_type_declaration` |
| J02 | `test_xml` | `xml_refuses_an_unknown_entity_rather_than_passing_it_through` |
| J02 | `test_xml` | `xml_reports_malformed_documents_instead_of_guessing` |
| J03 | `test_json` | `json_rejects_container_element_bomb` |
| J03 | `test_json` | `json_rejects_depth_bomb` |
| J03 | `test_json` | `json_rejects_oversized_input` |
| J03 | `test_json` | `stream_parser_enforces_depth_inside_a_skip` |
| J03 | `test_json` | `stream_parser_enforces_the_byte_limit` |
| J03 | `test_process` | `process_bounds_output_size` |
| J03 | `test_sdk` | `sdk_refuses_a_batch_over_the_limit` |
| J03 | `test_xml` | `xml_enforces_its_limits` |
| J03 | `test_xml` | `xml_refuses_a_document_type_declaration` |
| J04 | `test_symbols` | `path_root_containment_check` |
| J04 | `test_symbols` | `path_traversal_in_a_source_map_is_rejected` |
| J05 | `test_android_collector` | `collector_refuses_an_option_like_identifier` |
| J05 | `test_android_collector` | `streaming_refuses_an_option_like_identifier` |
| J05 | `test_android_parsers` | `adapter_rejects_an_option_like_app_identifier` |
| J05 | `test_devx_http` | `url_decode_handles_escapes_and_plus` |
| J05 | `test_devx_http` | `url_decode_leaves_malformed_escapes_literal` |
| J05 | `test_ios_parsers` | `adapter_refuses_an_option_like_bundle_id` |
| J05 | `test_ios_parsers` | `xctrace_export_xpath_is_built_in_one_place` |
| J05 | `test_process` | `process_argv_is_never_shell_interpreted` |
| J05 | `test_process` | `process_rejects_nul_in_argument` |
| J05 | `test_process` | `process_rejects_option_like_identifier` |
| J05 | `test_process` | `process_runs_and_captures_output` |
| J06 | `test_devx_http` | `server_generates_a_distinct_token_per_instance` |
| J06 | `test_sdk` | `sdk_bridge_binds_loopback_and_requires_a_token` |
| J06 | `test_sdk` | `sdk_markers_need_a_handshake_first` |
| J07 | `test_sdk` | `sdk_applies_backpressure_instead_of_growing_without_limit` |
| J07 | `test_sdk` | `sdk_counts_what_the_app_dropped_before_sending` |
| J07 | `test_sdk` | `sdk_does_not_store_a_retried_batch_twice` |
| J07 | `test_sdk` | `sdk_records_a_lost_batch_as_a_gap_not_as_silence` |
| J07 | `test_sdk` | `sdk_refuses_a_batch_over_the_limit` |
| J07 | `test_session_lifecycle` | `a_written_package_reopens_with_no_device` |
| J08 | `test_session_lifecycle` | `delete_refuses_a_directory_that_is_not_the_named_session` |
| J09 | `test_rules` | `a_trace_carrying_instructions_is_data_and_stays_data` |
| J10 | `test_rules` | `a_trace_carrying_instructions_is_data_and_stays_data` |
| J11 | `test_android_collector` | `live_session_start_stop_is_safe_without_a_collector` |
| J11 | `test_identity` | `a_cancelled_wait_stops_and_reports_the_cancellation` |
| J11 | `test_json` | `json_accessors_do_not_throw_on_type_mismatch` |
| J11 | `test_process` | `process_honours_cancellation` |
| J11 | `test_rules` | `cancellation_stops_analysis_and_marks_rules_skipped` |
| J11 | `test_timeline` | `cancellation_stops_the_build` |
| J13 | `test_session_lifecycle` | `a_package_from_another_schema_version_is_named_not_migrated` |
| J13 | `test_session_lifecycle` | `an_interrupted_write_is_never_mistaken_for_a_session` |
| J14 | `test_android_collector` | `am_start_w_parses_a_real_cold_launch` |
| J14 | `test_android_collector` | `det03_leaves_a_background_threads_io_in_the_background` |
| J14 | `test_android_collector` | `displayed_log_parses_the_platform_first_frame_figure` |
| J14 | `test_android_collector` | `framestats_parses_real_emulator_output` |
| J14 | `test_android_collector` | `meminfo_parses_real_output_and_keeps_families_distinct` |
| J14 | `test_android_collector` | `simpleperf_handles_the_not_debuggable_refusal` |
| J14 | `test_android_collector` | `simpleperf_parses_real_samples_and_callchains` |
| J14 | `test_android_parsers` | `atrace_reads_a_real_cold_start_trace` |
| J14 | `test_rules` | `det05_finds_nothing_in_a_real_capture_that_did_not_grow` |
| J15 | `test_ingestion` | `xctrace_toc_reads_the_run_without_claiming_a_platform` |
| J15 | `test_ios_parsers` | `adapter_probe_runs_against_the_real_toolchain` |
| J15 | `test_ios_parsers` | `parses_real_devicectl_device_listing` |
| J15 | `test_ios_parsers` | `readiness_reports_ddi_services_state` |
| J15 | `test_ios_parsers` | `simulator_apps_are_enumerated_from_the_real_booted_simulator` |
| J15 | `test_ios_parsers` | `xctrace_explains_an_attach_that_found_no_process` |
| J15 | `test_ios_parsers` | `xctrace_keeps_a_bundle_it_wrote_despite_reporting_errors` |
| J15 | `test_ios_parsers` | `xctrace_reports_a_nonzero_exit_with_no_bundle` |
| J15 | `test_ios_parsers` | `xctrace_timeout_is_a_provider_failure_not_an_empty_capture` |
| J16 | `test_ios_parsers` | `readiness_reports_ddi_services_state` |
| J16 | `test_ios_parsers` | `unreachable_paired_device_is_offline_not_authorized` |
| J18 | `test_android_parsers` | `parses_device_states_and_forms` |
| J18 | `test_android_parsers` | `probe_never_claims_physical_verification_from_an_emulator` |
| J18 | `test_compare` | `simulator_vs_physical_is_never_comparable` |
| J18 | `test_eligibility` | `simulator_is_not_benchmark_eligible` |
| J18 | `test_identity` | `simulator_form_survives_serialization` |
| J18 | `test_ios_parsers` | `adapter_lists_real_devices_including_simulators` |
| J18 | `test_ios_parsers` | `parses_real_simctl_device_listing` |
| J18 | `test_ios_parsers` | `simulator_apps_are_enumerated_from_the_real_booted_simulator` |
| J19 | `test_rules` | `native_evidence_without_js_evidence_invents_none` |
| J20 | `test_ingestion` | `xctrace_toc_reads_the_run_without_claiming_a_platform` |
| J20 | `test_ios_parsers` | `xctrace_export_xpath_is_built_in_one_place` |

## Additional coverage keyed to specification sections and detector ids (27)

| Reference | Test binary | Test case |
|---|---|---|
| DET-01 | `test_rules` | `det01_observed_requires_presentation_truth` |
| DET-02 | `test_android_collector` | `simpleperf_finds_the_react_native_js_thread` |
| DET-02 | `test_ingestion` | `hermes_reader_produces_samples_not_tasks` |
| DET-02 | `test_rules` | `det02_refuses_sampling_only_input` |
| DET-02 | `test_rules` | `det02_unmapped_clock_blocks_any_ui_claim` |
| DET-03 | `test_android_collector` | `atrace_mapping_keeps_states_apart_and_only_calls_iowait_io` |
| DET-03 | `test_android_collector` | `det03_leaves_a_background_threads_io_in_the_background` |
| DET-03 | `test_android_collector` | `det03_reports_a_main_thread_block_with_its_slice` |
| DET-03 | `test_android_collector` | `det03_will_not_promote_a_bare_uninterruptible_state` |
| DET-03 | `test_android_collector` | `det09_leaves_an_io_wait_to_det03` |
| DET-03 | `test_android_parsers` | `atrace_only_treats_an_iowait_flag_as_io` |
| DET-03 | `test_android_parsers` | `atrace_reads_a_real_cold_start_trace` |
| DET-03 | `test_android_parsers` | `atrace_reads_userspace_slices_and_tolerates_a_nameless_one` |
| DET-04 | `test_android_collector` | `simpleperf_parses_real_samples_and_callchains` |
| DET-04 | `test_ingestion` | `xctrace_time_profile_reads_samples_stacks_and_binaries` |
| DET-04 | `test_rules` | `det04_refuses_to_name_a_function_without_symbols` |
| DET-04 | `test_rules` | `obfuscated_frames_resolve_only_with_a_bound_mapping` |
| DET-05 | `test_rules` | `det05_calls_warm_up_warm_up_rather_than_retention` |
| DET-05 | `test_rules` | `det05_finds_nothing_in_a_real_capture_that_did_not_grow` |
| DET-05 | `test_rules` | `det05_needs_cycles_from_the_app_and_says_where_they_come_from` |
| DET-05 | `test_rules` | `det05_never_sums_memory_families_and_says_so` |
| DET-05 | `test_rules` | `det05_refuses_an_unmeasured_mapping_as_well` |
| DET-05 | `test_rules` | `det05_refuses_to_compare_two_unmapped_clocks` |
| DET-05 | `test_rules` | `det05_reports_growth_as_suspected_and_never_as_a_leak` |
| DET-05 | `test_rules` | `det05_reports_native_growth_while_the_js_heap_stays_put` |
| DET-06 | `test_heap` | `a_reference_chain_is_read_from_the_dump` |
| DET-06 | `test_heap` | `a_static_field_is_an_edge_a_path_can_run_through` |
| DET-06 | `test_heap` | `a_superclass_described_after_its_instance_is_still_read` |
| DET-06 | `test_heap` | `an_app_root_is_preferred_over_vm_bookkeeping` |
| DET-06 | `test_heap` | `an_unreachable_object_is_garbage_not_retention` |
| DET-06 | `test_heap` | `det06_calls_an_unrooted_destroyed_object_garbage` |
| DET-06 | `test_heap` | `det06_qualifies_a_dump_taken_without_a_collection` |
| DET-06 | `test_heap` | `det06_reports_a_destroyed_object_that_is_still_held` |
| DET-06 | `test_heap` | `det06_says_nothing_about_a_live_object` |
| DET-06 | `test_heap` | `det06_says_when_only_the_runtime_holds_the_object` |
| DET-06 | `test_heap` | `det06_without_a_dump_refuses_the_substitute` |
| DET-06 | `test_heap` | `instances_of_a_base_class_are_found_through_subclasses` |
| DET-06 | `test_rules` | `det06_without_a_heap_dump_says_what_is_missing` |
| DET-06 | `test_session_lifecycle` | `a_stored_artifact_is_checksummed_and_found_again` |
| DET-07 | `test_android_collector` | `am_start_w_parses_a_real_cold_launch` |
| DET-07 | `test_android_collector` | `am_start_w_refuses_the_zero_of_an_app_already_running` |
| DET-07 | `test_android_collector` | `displayed_log_parses_the_platform_first_frame_figure` |
| DET-07 | `test_rules` | `det07_needs_a_budget_before_it_will_judge_a_startup` |
| DET-07 | `test_rules` | `det07_reports_a_real_launch_over_budget` |
| DET-07 | `test_rules` | `det07_says_a_capture_without_a_launch_has_no_startup` |
| DET-07 | `test_rules` | `det07_under_budget_runs_and_says_what_it_measured` |
| DET-08 | `test_compare` | `det08_does_not_report_an_improvement_as_an_issue` |
| DET-08 | `test_compare` | `det08_records_an_undecided_metric_rather_than_passing_it` |
| DET-08 | `test_compare` | `det08_refuses_a_cross_platform_pair` |
| DET-08 | `test_compare` | `det08_refuses_a_debug_versus_release_pair` |
| DET-08 | `test_compare` | `det08_reports_a_regression_as_an_issue` |
| DET-08 | `test_compare` | `det08_says_a_forced_pair_certifies_nothing` |
| DET-08 | `test_compare` | `det08_skips_when_conditions_are_not_comparable` |
| DET-08 | `test_rules` | `det08_over_a_capture_says_it_needs_two_run_sets` |
| DET-09 | `test_android_collector` | `det09_leaves_an_io_wait_to_det03` |
| DET-09 | `test_android_collector` | `det09_never_names_a_lock_owner` |
| DET-09 | `test_android_collector` | `det09_reports_waits_on_visible_threads_and_excludes_the_rest` |
| DET-09 | `test_android_parsers` | `atrace_keeps_only_the_waking_that_identifies_a_waker` |
| DET-09 | `test_android_parsers` | `atrace_reads_a_real_cold_start_trace` |
| DET-10 | `test_rules` | `det10_reports_a_render_pattern_and_refuses_to_call_it_a_defect` |
| DET-10 | `test_rules` | `det10_will_not_accept_cpu_samples_as_render_data` |
| DET-11 | `test_rules` | `det11_does_not_report_a_request_that_merely_overlapped_a_little` |
| DET-11 | `test_rules` | `det11_observes_the_delay_and_blames_nobody` |
| DET-11 | `test_rules` | `det11_says_it_cannot_observe_the_network_itself` |
| H18 | `test_session_lifecycle` | `a_snapshot_before_the_close_is_preliminary` |
| M0 | `test_ios_parsers` | `adapter_probe_runs_against_the_real_toolchain` |
| M0 | `test_ios_parsers` | `simulator_apps_are_enumerated_from_the_real_booted_simulator` |
| section-0.6 | `test_report` | `synthetic_data_is_announced_in_both_formats` |
| section-0.6 | `test_rules` | `synthetic_input_is_labelled_at_the_top_level` |
| section-0.8 | `test_rules` | `det02_overlap_is_a_candidate_cause_never_proven` |
| section-10.1 | `test_report` | `report_explains_how_to_read_detection_versus_cause` |
| section-10.3 | `test_rules` | `det02_threshold_is_labelled_a_heuristic_not_a_standard` |
| section-10.3 | `test_rules` | `det07_needs_a_budget_before_it_will_judge_a_startup` |
| section-10.3 | `test_rules` | `every_rule_declares_prerequisites_and_a_phase` |
| section-11 | `test_ingestion` | `hermes_samples_stay_on_an_unmapped_js_clock` |
| section-11 | `test_rules` | `det02_unmapped_clock_blocks_any_ui_claim` |
| section-11 | `test_rules` | `det05_needs_cycles_from_the_app_and_says_where_they_come_from` |
| section-11 | `test_rules` | `det05_refuses_to_compare_two_unmapped_clocks` |
| section-11 | `test_rules` | `det10_reports_a_render_pattern_and_refuses_to_call_it_a_defect` |
| section-11 | `test_sdk` | `sdk_absence_is_reported_as_no_evidence_not_as_a_clean_app` |
| section-11 | `test_sdk` | `sdk_accepts_the_marker_kinds_the_spec_names` |
| section-11 | `test_trace_model` | `screen_is_null_when_not_observed` |
| section-12 | `test_compare` | `median_and_spread_are_both_reported` |
| section-12 | `test_compare` | `warm_up_runs_are_excluded_and_visible` |
| section-13 | `test_android_collector` | `live_session_refuses_a_non_streaming_collector` |
| section-13 | `test_android_collector` | `live_snapshot_states_that_it_is_preliminary` |
| section-13 | `test_android_collector` | `live_update_serialises_its_deltas_and_cost` |
| section-13 | `test_android_collector` | `streaming_is_declared_and_batch_is_not_removed` |
| section-13 | `test_ios_parsers` | `xctrace_collector_does_not_claim_to_stream` |
| section-13 | `test_report` | `unknown_values_are_shown_as_unknown_not_omitted` |
| section-13 | `test_rules` | `issue_interval_lies_inside_the_capture_window` |
| section-13 | `test_rules` | `issues_are_sorted_by_severity_then_stably` |
| section-14 | `test_devx_http` | `index_page_substitutes_the_token_once` |
| section-14 | `test_devx_http` | `server_binds_loopback_on_an_ephemeral_port` |
| section-14 | `test_devx_http` | `server_generates_a_distinct_token_per_instance` |
| section-14 | `test_report` | `raw_events_are_excluded_from_the_export_by_default` |
| section-14 | `test_report` | `source_paths_are_excluded_from_export_by_default` |
| section-14 | `test_sdk` | `sdk_bridge_binds_loopback_and_requires_a_token` |
| section-15 | `test_devx_http` | `html_escape_neutralises_markup` |
| section-15 | `test_ingestion` | `normalize_is_idempotent` |
| section-15 | `test_json` | `stream_parser_skips_without_materialising` |
| section-15 | `test_json` | `stream_parser_walks_members_in_order` |
| section-15 | `test_report` | `hostile_identifiers_cannot_break_the_markdown_table` |
| section-15 | `test_report` | `markdown_escapes_table_breaking_and_html_characters` |
| section-15 | `test_rules` | `fingerprints_are_stable_across_reanalysis` |
| section-15 | `test_xml` | `xml_enforces_its_limits` |
| section-2.2 | `test_android_collector` | `live_snapshot_states_that_it_is_preliminary` |
| section-6 | `test_rules` | `det05_refuses_an_unmeasured_mapping_as_well` |
| section-6 | `test_rules` | `det05_refuses_to_compare_two_unmapped_clocks` |
| section-6 | `test_trace_model` | `unmapped_clock_domain_is_not_silently_aligned` |
| section-7.3 | `test_eligibility` | `eligibility_never_implies_zero_overhead` |
| section-8 | `test_android_collector` | `am_start_w_refuses_the_zero_of_an_app_already_running` |
| section-8 | `test_android_collector` | `meminfo_absent_fields_stay_absent` |
| section-8 | `test_android_collector` | `uptime_is_read_or_refused_never_defaulted` |
| section-8 | `test_rules` | `det05_never_sums_memory_families_and_says_so` |
| section-8 | `test_trace_model` | `unknown_metric_value_serializes_as_null_not_zero` |
| section-9 | `test_rules` | `det12_excludes_name_only_matches_from_attribution` |
| section-9 | `test_rules` | `det12_preserves_original_total_alongside_slices` |
| section-9 | `test_trace_model` | `attribution_forbids_subtraction_structurally` |

## Not yet covered (25)

| Checklist id | Why not, stated |
|---|---|
| A07 | no refresh target is declared, so there is nothing to measure against; discovery is on demand rather than polled, and the launch path waits for the process instead (A22, tested) |
| A10 | partial scope is asserted via iOS launchd-only entries; a dedicated Android case needs hardware |
| B05 | the app available on this emulator runs a single process, so there is no secondary service to include or exclude |
| C02 | needs a built app with native debug and JS dev off |
| C12 | needs an iOS build with a debug entitlement and optimized code |
| D15 | an emulator's KEYCODE_SLEEP turns the screen off but does not suspend the shell or the app: measured, a 6 s screen-off mid-capture left the tick cadence unchanged at ~710 ms and lost nothing, so that exercises a screen-off rather than a suspend. A true doze needs a physical device |
| E05 | DET-04 says in every finding that a sampled share carries no claim about user-visible harm; inducing high CPU with provably unharmed frames needs a device whose frame timing is not the host's |
| E07 | needs a real React Native capture |
| E08 | needs a real React Native capture |
| E09 | GPU evidence is not collected (M5) |
| E18 | recorder overhead needs paired controlled runs (M4) |
| E19 | DET-01 groups by surface; a multi-surface real capture would confirm |
| F06 | image and GPU allocation accounting has no provider on either platform |
| F12 | in-process tooling stays in the process total by construction, and the real captures here are of a debug build that contains it; separating it would need a release build to compare against |
| F15 | endpoint/library ambiguity needs more attribution rules |
| G02 | the Hermes reader refuses unexpected shapes; a versioned corpus is M3 |
| G03 | non-Hermes runtime needs such a build (M3) |
| G05 | Fast Refresh needs a live RN app (M3) |
| G06 | multiple runtimes needs a live RN app (M3) |
| G09 | the SDK sends async span markers and they are tested end to end; correlating two JS runtimes needs a live app with two of them |
| G12 | Expo's prerequisite is documented in samples/react-native/README.md: this SDK needs no native module, so it loads in Expo Go, and a native SDK would need a development build |
| G13 | the architecture matrix needs RN builds (M3) |
| I20 | overhead measurement needs paired controlled runs on hardware (M4) |
| J12 | signing, packaging and the (empty) license inventory are documentation, in docs/packaging-and-signing.md; no test binary can assert a Developer ID this environment does not have |
| J17 | App Store app depth needs a reachable device with such an app |

## Totals

- test binaries: 20
- test cases declaring at least one id: 459
- checklist-item links: 600
- section-18 coverage: 173/198 (87%)
