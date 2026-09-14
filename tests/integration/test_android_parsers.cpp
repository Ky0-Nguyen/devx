// Android provider-output parsers.
//
// No Android device was connected during implementation, so the device-side
// fixtures here are hand-written and named `.synthetic.`. The adb *host*
// fixtures are real output from this machine. Which is which is stated per
// test, because the specification forbids presenting synthetic data as
// evidence of a working capability.
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "adapters/android/adb_adapter.hpp"
#include "adapters/android/atrace_parser.hpp"
#include "tests/unit/test_framework.hpp"

using namespace mpi;
using namespace mpi::android;

namespace {

std::string read_fixture(const char* rel) {
  const char* dir = std::getenv("MPI_FIXTURE_DIR");
  const std::string path = std::string(dir ? dir : "fixtures") + "/" + rel;
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}

const PsRow* find_pid(const std::vector<PsRow>& rows, std::int32_t pid) {
  for (const auto& r : rows) {
    if (r.pid == pid) return &r;
  }
  return nullptr;
}

const model::DeviceRef* find_device(const std::vector<model::DeviceRef>& v,
                                    const std::string& id) {
  for (const auto& d : v) {
    if (d.device_id == id) return &d;
  }
  return nullptr;
}

}  // namespace

MPI_TEST(parses_real_adb_version_output, {"A06"}) {
  // REAL output from this host's adb.
  const auto text = read_fixture("provider-output/adb-version.real.txt");
  MPI_CHECK_MSG(!text.empty(), "real adb version fixture is missing");
  MPI_CHECK(text.find("Android Debug Bridge") != std::string::npos);
}

MPI_TEST(real_adb_with_no_device_yields_an_empty_list_not_an_error, {"A01", "A12"}) {
  // REAL output: this host had no Android device connected.
  const auto text = read_fixture("provider-output/adb-devices-l.real.txt");
  MPI_CHECK(!text.empty());
  const auto devices = parse_adb_devices(text, "1.0.41");
  MPI_CHECK_MSG(devices.empty(),
                "the header line alone must not become a phantom device");
}

MPI_TEST(parses_device_states_and_forms, {"A02", "A04", "C20", "J18"}) {
  // SYNTHETIC fixture: no Android hardware was available.
  const auto devices =
      parse_adb_devices(read_fixture("provider-output/android-adb-devices-l.synthetic.txt"),
                        "1.0.41");
  MPI_CHECK_EQ(devices.size(), static_cast<std::size_t>(5));

  const auto* physical = find_device(devices, "R5CT30ABCDE");
  MPI_CHECK(physical != nullptr);
  MPI_CHECK(physical->trust == model::TrustState::kAuthorized);
  MPI_CHECK(physical->form == model::DeviceForm::kPhysical);
  MPI_CHECK(physical->connection == model::ConnectionType::kUsb);
  MPI_CHECK_EQ(physical->model, std::string("SM_G973F"));

  // An emulator is never classified as a physical device.
  const auto* emu = find_device(devices, "emulator-5554");
  MPI_CHECK(emu != nullptr);
  MPI_CHECK_MSG(emu->form == model::DeviceForm::kEmulator,
                "an emulator-NNNN serial must be classified as an emulator");

  // A host:port serial means a TCP transport.
  const auto* wireless = find_device(devices, "192.168.1.44:5555");
  MPI_CHECK(wireless != nullptr);
  MPI_CHECK(wireless->connection == model::ConnectionType::kWireless);

  // Unauthorized and offline are distinct states, and neither is usable.
  const auto* unauth = find_device(devices, "9A281FFBA00XYZ");
  MPI_CHECK(unauth != nullptr);
  MPI_CHECK(unauth->trust == model::TrustState::kUnauthorized);
  MPI_CHECK(!unauth->usable_for_capture());

  const auto* offline = find_device(devices, "FA6930303030");
  MPI_CHECK(offline != nullptr);
  MPI_CHECK(offline->trust == model::TrustState::kOffline);
  MPI_CHECK(!offline->usable_for_capture());
}

MPI_TEST(ps_parser_reads_the_header_rather_than_fixed_columns, {"A06"}) {
  // SYNTHETIC fixture.
  std::vector<std::string> warnings;
  const auto rows =
      parse_ps_output(read_fixture("provider-output/android-ps-A.synthetic.txt"),
                      warnings);
  MPI_CHECK_MSG(rows.size() == 10, "expected 10 rows, got " +
                                       std::to_string(rows.size()));
  const auto* main = find_pid(rows, 4242);
  MPI_CHECK(main != nullptr);
  MPI_CHECK_EQ(main->name, std::string("com.example.perf"));
  MPI_CHECK_EQ(main->ppid, 600);
  MPI_CHECK(main->uid.has_value());
  // u0_a234 -> user 0, app id 234 -> uid 10234.
  MPI_CHECK_EQ(*main->uid, 10234);
  MPI_CHECK(main->android_user_id.has_value());
  MPI_CHECK_EQ(*main->android_user_id, 0);
}

MPI_TEST(ps_parser_reorders_with_a_different_column_order, {"A06"}) {
  std::vector<std::string> warnings;
  const std::string reordered =
      "USER NAME PID PPID\n"
      "u0_a234 com.example.perf 4242 600\n";
  const auto rows = parse_ps_output(reordered, warnings);
  MPI_CHECK_EQ(rows.size(), static_cast<std::size_t>(1));
  MPI_CHECK_EQ(rows[0].pid, 4242);
  MPI_CHECK_EQ(rows[0].name, std::string("com.example.perf"));
}

MPI_TEST(ps_parser_refuses_to_guess_at_an_unknown_header, {"A06", "A12"}) {
  std::vector<std::string> warnings;
  const auto rows = parse_ps_output("total garbage\nnot a process table\n", warnings);
  MPI_CHECK_MSG(rows.empty(), "an unrecognised table must not be parsed by guesswork");
  MPI_CHECK_MSG(!warnings.empty(), "the refusal must be reported");
  bool said_so = false;
  for (const auto& w : warnings) {
    if (w.find("guesswork") != std::string::npos) said_so = true;
  }
  MPI_CHECK(said_so);
}

MPI_TEST(ps_parser_handles_empty_input, {"A12", "D16"}) {
  std::vector<std::string> warnings;
  MPI_CHECK(parse_ps_output("", warnings).empty());
  MPI_CHECK(parse_ps_output("\n\n\n", warnings).empty());
}

MPI_TEST(isolated_process_identity_is_recognised, {"B07"}) {
  std::vector<std::string> warnings;
  const auto rows =
      parse_ps_output(read_fixture("provider-output/android-ps-A.synthetic.txt"),
                      warnings);
  const auto* isolated = find_pid(rows, 5000);
  MPI_CHECK(isolated != nullptr);
  MPI_CHECK_EQ(isolated->user, std::string("u0_i15"));
  // An isolated uid is in the 99000 range, not the app's own uid.
  MPI_CHECK(isolated->uid.has_value());
  MPI_CHECK_MSG(*isolated->uid != 10234,
                "an isolated process must not carry the package uid");
  MPI_CHECK_EQ(*isolated->uid, 99015);
}

MPI_TEST(work_profile_user_is_parsed_distinctly, {"B08"}) {
  std::vector<std::string> warnings;
  const auto rows =
      parse_ps_output(read_fixture("provider-output/android-ps-A.synthetic.txt"),
                      warnings);
  const auto* work = find_pid(rows, 6000);
  MPI_CHECK(work != nullptr);
  MPI_CHECK(work->android_user_id.has_value());
  MPI_CHECK_EQ(*work->android_user_id, 10);
  MPI_CHECK(work->uid.has_value());
  // u10_a234 -> 10 * 100000 + 10000 + 234.
  MPI_CHECK_EQ(*work->uid, 1010234);
}

MPI_TEST(pm_list_packages_parses_package_and_uid, {"A13"}) {
  const auto rows = parse_pm_list_packages(
      read_fixture("provider-output/android-pm-list-packages-U.synthetic.txt"));
  MPI_CHECK_EQ(rows.size(), static_cast<std::size_t>(4));
  MPI_CHECK_EQ(rows[0].package, std::string("com.example.perf"));
  MPI_CHECK(rows[0].uid.has_value());
  MPI_CHECK_EQ(*rows[0].uid, 10234);
  // Two packages sharing a uid: this is exactly the case where a uid match
  // alone is insufficient ownership evidence (spec B06).
  MPI_CHECK_EQ(rows[1].package, std::string("com.other.sharedapp"));
  MPI_CHECK_EQ(*rows[1].uid, 10234);
}

MPI_TEST(pm_list_packages_without_uid_flag_still_parses, {"A13"}) {
  const auto rows = parse_pm_list_packages(
      "package:com.example.one\npackage:/data/app/base.apk=com.example.two\n");
  MPI_CHECK_EQ(rows.size(), static_cast<std::size_t>(2));
  MPI_CHECK_EQ(rows[0].package, std::string("com.example.one"));
  MPI_CHECK_EQ(rows[1].package, std::string("com.example.two"));
  MPI_CHECK_MSG(!rows[0].uid.has_value(),
                "an absent uid must stay absent, not default to 0");
}

MPI_TEST(pm_list_packages_ignores_noise, {"A12"}) {
  const auto rows = parse_pm_list_packages(
      "Error: could not access the Package Manager\nsome other line\n");
  MPI_CHECK(rows.empty());
}

MPI_TEST(proc_stat_starttime_is_extracted, {"B02", "B03"}) {
  const auto st = parse_proc_stat_starttime(
      read_fixture("provider-output/android-proc-stat.synthetic.txt"));
  MPI_CHECK_MSG(st.has_value(), "starttime must be readable from /proc/pid/stat");
  MPI_CHECK_EQ(*st, std::string("918273"));
}

MPI_TEST(proc_stat_handles_a_comm_name_containing_spaces, {"B02"}) {
  // Field 2 is parenthesised and may contain spaces and parentheses, so
  // parsing must start after the LAST ')'.
  std::string line = "1234 (weird name (with parens)) S 1 1 0 0 -1 0 0 0 0 0 "
                     "1 2 0 0 20 0 5 0 555666 0 0";
  const auto st = parse_proc_stat_starttime(line);
  MPI_CHECK(st.has_value());
  MPI_CHECK_EQ(*st, std::string("555666"));
}

MPI_TEST(proc_stat_malformed_returns_nothing_not_a_fake_value, {"B02", "J02"}) {
  MPI_CHECK(!parse_proc_stat_starttime("").has_value());
  MPI_CHECK(!parse_proc_stat_starttime("no parens here").has_value());
  MPI_CHECK(!parse_proc_stat_starttime("1 (x) S 1 2 3").has_value());
  MPI_CHECK(!parse_proc_stat_starttime("1 (x) S " + std::string(19, 'a')).has_value());
}

MPI_TEST(dumpsys_flags_parse_debuggable_and_version, {"C11", "C18"}) {
  const auto flags = parse_dumpsys_package_flags(
      "  flags=[ DEBUGGABLE HAS_CODE ALLOW_CLEAR_USER_DATA ]\n"
      "  versionName=3.4.5\n"
      "  versionCode=3040500 minSdk=24 targetSdk=34\n");
  MPI_CHECK(flags.debuggable.has_value());
  MPI_CHECK_EQ(*flags.debuggable, true);
  MPI_CHECK_EQ(flags.version_name, std::string("3.4.5"));
  MPI_CHECK(flags.version_code.has_value());
  MPI_CHECK_EQ(*flags.version_code, static_cast<std::int64_t>(3040500));
}

MPI_TEST(dumpsys_absent_flags_stay_unknown, {"C09", "C18"}) {
  const auto flags = parse_dumpsys_package_flags("nothing useful here\n");
  MPI_CHECK_MSG(!flags.debuggable.has_value(),
                "an unseen flag must be unknown, not false");
  MPI_CHECK(!flags.version_code.has_value());
}

MPI_TEST(adapter_probe_reports_unsupported_when_adb_is_absent, {"A06"}) {
  AdbAdapter adapter("mpi-nonexistent-adb-binary");
  model::CapabilityMatrix m;
  adapter.probe(m, discovery::ProviderOptions{});
  const auto* toolchain = m.find("android.toolchain.adb");
  MPI_CHECK(toolchain != nullptr);
  MPI_CHECK(toolchain->status == model::CapabilityStatus::kUnsupported);
  MPI_CHECK_MSG(!toolchain->recovery_action.empty(),
                "an unsupported capability must state how to fix it");
  // Downstream capabilities become unsupported for a stated reason, not unknown.
  const auto* devices = m.find("android.discovery.devices");
  MPI_CHECK(devices != nullptr);
  MPI_CHECK(devices->status == model::CapabilityStatus::kUnsupported);
  MPI_CHECK(devices->evidence.find("absent") != std::string::npos);
}

MPI_TEST(adapter_list_devices_reports_missing_adb_as_an_error, {"A06", "A12"}) {
  AdbAdapter adapter("mpi-nonexistent-adb-binary");
  std::vector<std::string> errors;
  const auto devices = adapter.list_devices(discovery::ProviderOptions{}, errors);
  MPI_CHECK(devices.empty());
  MPI_CHECK_MSG(!errors.empty(),
                "an absent adb must produce an error, not a silent empty list");
}

MPI_TEST(adapter_rejects_an_option_like_app_identifier, {"A20", "J05"}) {
  AdbAdapter adapter;
  model::DeviceRef device;
  device.device_id = "serialX";
  device.trust = model::TrustState::kAuthorized;
  model::ApplicationKey app;
  app.app_identifier = "--all";  // would be read as a flag by pm/ps
  std::vector<std::string> errors;
  const auto procs =
      adapter.resolve_processes(device, app, discovery::ProviderOptions{}, errors);
  MPI_CHECK(procs.empty());
  bool refused = false;
  for (const auto& e : errors) {
    if (e.find("command-line option") != std::string::npos) refused = true;
  }
  MPI_CHECK_MSG(refused, "an option-like identifier must be refused explicitly");
}

MPI_TEST(adapter_refuses_to_enumerate_an_unauthorized_device, {"A02"}) {
  AdbAdapter adapter;
  model::DeviceRef device;
  device.device_id = "serialX";
  device.trust = model::TrustState::kUnauthorized;
  std::vector<std::string> errors;
  bool failed = false;
  const auto apps =
      adapter.list_apps(device, discovery::ProviderOptions{}, errors, failed);
  MPI_CHECK(apps.empty());
  MPI_CHECK_MSG(failed, "this is an enumeration failure, not an empty device");
  MPI_CHECK(!errors.empty());
}

MPI_TEST(probe_never_claims_physical_verification_from_an_emulator, {"J18", "C20"}) {
  // The whole point of the tested-state field is that it cannot over-claim.
  // A capability exercised against an emulator must say so: an emulator's
  // behaviour is not evidence about physical hardware.
  AdbAdapter adapter;
  model::CapabilityMatrix m;
  discovery::ProviderOptions opts;
  opts.command_timeout_ms = 30000;
  adapter.probe(m, opts);

  std::vector<std::string> errors;
  const auto devices = adapter.list_devices(opts, errors);
  bool any_physical = false;
  bool any_emulator = false;
  for (const auto& d : devices) {
    if (!d.usable_for_capture()) continue;
    if (d.form == model::DeviceForm::kPhysical) any_physical = true;
    if (d.form == model::DeviceForm::kEmulator) any_emulator = true;
  }
  if (!any_emulator || any_physical) {
    std::cout << "       (no emulator-only Android setup here: not verified)\n";
    return;
  }

  for (const char* id : {"android.discovery.devices",
                         "android.discovery.installed_apps",
                         "android.discovery.running_processes",
                         "android.discovery.process_mapping"}) {
    const auto* c = m.find(id);
    MPI_CHECK_MSG(c != nullptr, std::string("missing capability ") + id);
    MPI_CHECK_MSG(c->tested != model::TestedState::kVerifiedOnPhysicalDevice,
                  std::string(id) +
                      " claims physical verification from an emulator-only "
                      "probe");
  }
}


// --- atrace / ftrace text ----------------------------------------------------

MPI_TEST(atrace_reads_a_real_cold_start_trace, {"E10", "DET-03", "DET-09", "J14"}) {
  const auto t = android::parse_atrace(
      read_fixture("provider-output/android-atrace-cold-start.real.txt"));

  // Every line in this real capture parses. atrace's own preamble
  // ("capturing trace... done", "TRACE:") is recognised rather than counted
  // as unreadable, so the unrecognised count keeps its meaning.
  MPI_CHECK(t.lines_read > 400);
  MPI_CHECK_EQ(t.lines_unrecognised, std::int64_t{0});

  MPI_CHECK(t.switches.size() > 150);
  MPI_CHECK(!t.wakings.empty());
  MPI_CHECK(!t.blocked.empty());

  // The buffer accounting the header carries. These are equal here, which is
  // what makes the unequal case worth reporting.
  MPI_CHECK(t.header_seen);
  MPI_CHECK_EQ(t.entries_in_buffer, t.entries_written);
  MPI_CHECK_EQ(t.dropped_events, std::int64_t{0});

  // The trace clock, paired with wall time by the kernel itself.
  MPI_CHECK(t.clock_sync.have_parent);
  MPI_CHECK(t.clock_sync.have_realtime);
  MPI_CHECK(t.clock_sync.parent_ns > 0);

  // A thread name with a space in it survives: `next_comm=Firebase Backgr`
  // must not be truncated to "Firebase".
  bool multiword = false;
  for (const auto& s : t.switches) {
    if (s.next_comm.find(' ') != std::string::npos ||
        s.prev_comm.find(' ') != std::string::npos) {
      multiword = true;
    }
  }
  MPI_CHECK_MSG(multiword, "a multi-word thread name should survive parsing");
}

MPI_TEST(atrace_distinguishes_blocked_from_merely_preempted, {"E10", "E06"}) {
  const auto t = android::parse_atrace(
      read_fixture("provider-output/android-atrace-cold-start.real.txt"));
  std::size_t runnable = 0;
  std::size_t sleeping = 0;
  std::size_t uninterruptible = 0;
  for (const auto& s : t.switches) {
    switch (s.prev_state) {
      case android::ThreadState::kRunning: ++runnable; break;
      case android::ThreadState::kSleeping: ++sleeping; break;
      case android::ThreadState::kUninterruptible: ++uninterruptible; break;
      default: break;
    }
  }
  // The distinction E10 asks for: a thread taken off the CPU while still
  // runnable is contention, not idleness.
  MPI_CHECK(runnable > 0);
  MPI_CHECK(sleeping > 0);
  // And this real cold start does show uninterruptible sleep.
  MPI_CHECK(uninterruptible > 0);

  // `R+` is still runnable: the '+' means preempted, not a different state.
  MPI_CHECK(android::thread_state_from_ftrace("R+") ==
            android::ThreadState::kRunning);
  MPI_CHECK(android::thread_state_from_ftrace("D|K") ==
            android::ThreadState::kUninterruptible);
  MPI_CHECK(android::thread_state_from_ftrace("") ==
            android::ThreadState::kOther);
}

MPI_TEST(atrace_only_treats_an_iowait_flag_as_io, {"DET-03", "C18"}) {
  const auto t = android::parse_atrace(
      read_fixture("provider-output/android-atrace-cold-start.real.txt"));
  std::size_t iowait = 0;
  std::size_t other = 0;
  for (const auto& b : t.blocked) {
    if (b.iowait) {
      ++iowait;
      // The kernel says where it blocked, which is the only location evidence
      // this provider gives.
      MPI_CHECK(!b.caller.empty());
    } else {
      ++other;
    }
  }
  // This capture has both, which is the point: a `D` state alone says
  // "blocked", and only `iowait=1` says "blocked on I/O".
  MPI_CHECK(iowait > 0);
  MPI_CHECK(other > 0);
}

MPI_TEST(atrace_reports_a_dropped_buffer_rather_than_absorbing_it,
         {"D04", "E13"}) {
  // The header form when the kernel dropped events. A trace with holes must
  // not read as a quiet device.
  const auto t = android::parse_atrace(
      "# tracer: nop\n"
      "# entries-in-buffer/entries-written: 1000/4096   #P:4\n"
      " app-10 (   10) [000] d..2. 100.000100: sched_switch: prev_comm=app "
      "prev_pid=10 prev_prio=120 prev_state=S ==> next_comm=swapper/0 "
      "next_pid=0 next_prio=120\n");
  MPI_CHECK(t.header_seen);
  MPI_CHECK_EQ(t.dropped_events, std::int64_t{3096});
  bool said = false;
  for (const auto& w : t.warnings) {
    if (w.find("not quiet periods on the device") != std::string::npos) said = true;
  }
  MPI_CHECK(said);
}

MPI_TEST(atrace_without_a_header_says_the_loss_is_unknown, {"E13", "C18"}) {
  const auto t = android::parse_atrace(
      " app-10 (   10) [000] d..2. 100.000100: sched_waking: comm=other "
      "pid=11 prio=120 target_cpu=001\n");
  MPI_CHECK(!t.header_seen);
  MPI_CHECK_EQ(t.wakings.size(), std::size_t{1});
  MPI_CHECK_EQ(t.wakings.front().waker_tid, 10);
  MPI_CHECK_EQ(t.wakings.front().target_tid, 11);
  bool said = false;
  for (const auto& w : t.warnings) {
    if (w.find("cannot be relied on") != std::string::npos) said = true;
  }
  MPI_CHECK_MSG(said, "an unknown drop count must be stated");
}

MPI_TEST(atrace_keeps_only_the_waking_that_identifies_a_waker, {"DET-09"}) {
  // `sched_waking` is emitted by the waking thread; `sched_wakeup` is emitted
  // on the target's CPU, so its emitter is not the waker. Keeping both would
  // attribute a wake to whichever CPU happened to run the target.
  const auto t = android::parse_atrace(
      " holder-50 (   50) [000] d..2. 100.000100: sched_waking: comm=waiter "
      "pid=60 prio=120 target_cpu=001\n"
      " <idle>-0 (-------) [001] dNh3. 100.000200: sched_wakeup: comm=waiter "
      "pid=60 prio=120 target_cpu=001\n");
  MPI_CHECK_EQ(t.wakings.size(), std::size_t{1});
  MPI_CHECK_EQ(t.wakings.front().waker_tid, 50);
  MPI_CHECK_EQ(t.wakings.front().waker_comm, std::string("holder"));
  MPI_CHECK_EQ(t.wakings.front().target_comm, std::string("waiter"));
}

MPI_TEST(atrace_reads_userspace_slices_and_tolerates_a_nameless_one,
         {"DET-03", "D18"}) {
  const auto t = android::parse_atrace(
      read_fixture("provider-output/android-atrace-cold-start.real.txt"));
  bool named = false;
  bool nameless = false;
  for (const auto& s : t.slices) {
    if (!s.begin) continue;
    if (s.name.empty()) {
      // Real output contains `B|3378|` with no name at all. It is kept as
      // read; a detector cannot name work from it, and must not invent one.
      nameless = true;
    } else {
      named = true;
    }
  }
  MPI_CHECK_MSG(named, "the real trace contains named slices");
  MPI_CHECK_MSG(nameless, "and one with no name, which must not be guessed");

  // Ends carry no name of their own.
  const auto ends = android::parse_atrace(
      " app-10 (   10) [000] ..... 100.000100: tracing_mark_write: B|10|doWork\n"
      " app-10 (   10) [000] ..... 100.000200: tracing_mark_write: E|10\n");
  MPI_CHECK_EQ(ends.slices.size(), std::size_t{2});
  MPI_CHECK(ends.slices[0].begin);
  MPI_CHECK_EQ(ends.slices[0].name, std::string("doWork"));
  MPI_CHECK(!ends.slices[1].begin);
  MPI_CHECK(ends.slices[1].name.empty());
}

MPI_TEST(atrace_timestamps_keep_their_precision, {"D13"}) {
  // A 5-hour uptime with microsecond resolution: parsed through a double this
  // loses digits, so the seconds and the fraction are handled separately.
  const auto t = android::parse_atrace(
      " app-10 (   10) [000] d..2. 17921.423446: sched_switch: prev_comm=app "
      "prev_pid=10 prev_prio=120 prev_state=D ==> next_comm=swapper/0 "
      "next_pid=0 next_prio=120\n");
  MPI_CHECK_EQ(t.switches.size(), std::size_t{1});
  MPI_CHECK_EQ(t.switches.front().timestamp_ns, model::TimeNs{17921423446000});
}

MPI_TEST(proc_stat_cpu_time_is_read_from_the_right_fields, {"E11"}) {
  // A real line from `/proc/<pid>/stat` on API 37. The field offsets are the
  // whole test: utime is field 14 and stime field 15, and counting them from
  // the start of the line rather than from the last ')' would read the
  // fault counters instead and produce a plausible wrong number.
  const std::string line =
      "5119 (ut.hutbot.debug) S 432 432 0 0 -1 4194624 125717 246 165 0 "
      "26763 7791 0 0 10 -10 76 0 1856103 52053442560 56168 18446744073709551615 "
      "1 1 0 0 0 0 4612 1 0 0 17 4 0 0 0 0 0 0 0 0 0 0 0 0 0";
  const auto cpu = android::parse_proc_stat_cpu_time(line);
  MPI_CHECK(cpu.has_value());
  if (!cpu.has_value()) return;
  MPI_CHECK_EQ(cpu->utime_ticks, std::int64_t{26763});
  MPI_CHECK_EQ(cpu->stime_ticks, std::int64_t{7791});
  MPI_CHECK_EQ(cpu->total_ticks(), std::int64_t{34554});
  // And starttime still reads from the same line, so the two agree about
  // where the fields begin.
  const auto start = android::parse_proc_stat_starttime(line);
  MPI_CHECK(start.has_value());
  MPI_CHECK_EQ(start.value_or(""), std::string("1856103"));
}

MPI_TEST(proc_stat_cpu_time_survives_a_comm_with_spaces_and_parens, {"E11", "D18"}) {
  // The comm field is attacker-adjacent: it is the thread name, it can hold
  // spaces and parentheses, and parsing from the left would be thrown off by
  // either.
  const std::string line =
      "77 (weird ) name) S 1 1 0 0 -1 0 0 0 0 0 11 22 0 0 20 0 1 0 999 0 0 0";
  const auto cpu = android::parse_proc_stat_cpu_time(line);
  MPI_CHECK(cpu.has_value());
  if (!cpu.has_value()) return;
  MPI_CHECK_EQ(cpu->utime_ticks, std::int64_t{11});
  MPI_CHECK_EQ(cpu->stime_ticks, std::int64_t{22});
}

MPI_TEST(a_short_or_non_numeric_proc_stat_yields_nothing, {"E11", "D18"}) {
  // No value rather than a zero: a process that used no CPU and a line that
  // could not be read are different facts, and only the first is a
  // measurement.
  MPI_CHECK(!android::parse_proc_stat_cpu_time("77 (x) S 1 1 0").has_value());
  MPI_CHECK(!android::parse_proc_stat_cpu_time("no parenthesis here").has_value());
  MPI_CHECK(!android::parse_proc_stat_cpu_time("").has_value());
  const std::string bad =
      "77 (x) S 1 1 0 0 -1 0 0 0 0 0 eleven 22 0 0 20 0 1 0 999";
  MPI_CHECK_MSG(!android::parse_proc_stat_cpu_time(bad).has_value(),
                "a non-numeric tick count is refused, not coerced to 0");
}

MPI_TEST(a_permissions_flags_line_is_not_the_packages_flags_line,
         {"C01", "B11", "D18"}) {
  // REAL output from emulator-5554. The bug this pins: `dumpsys package`
  // prints `flags=[ ... ]` for the package *and* for every granted
  // permission, and matching " flags=[" anywhere caught both. Since each
  // match overwrote the answer, a DEBUGGABLE app with any permission listed
  // after its flags line came out as not debuggable -- which feeds
  // profileability and the benchmark-eligibility verdict, so the tool was
  // wrong about what it could measure.
  const auto text =
      read_fixture("provider-output/android-dumpsys-package.real.txt");
  MPI_CHECK(!text.empty());
  const auto flags = parse_dumpsys_package_flags(text);

  MPI_CHECK(flags.debuggable.has_value());
  MPI_CHECK_MSG(flags.debuggable.value_or(false),
                "the package's own flags line says DEBUGGABLE, and eleven "
                "permission lines after it do not change that");

  // The build identity a session needs, so two captures either side of a
  // reinstall are distinguishable (spec B11).
  MPI_CHECK_EQ(flags.version_name, std::string("5.3.26"));
  MPI_CHECK_EQ(flags.version_code.value_or(-1), std::int64_t{1});
  MPI_CHECK(flags.code_path.find("io.pizzahut.hutbot.debug-") !=
            std::string::npos);
  MPI_CHECK_EQ(flags.last_update_time, std::string("2026-09-15 04:26:20"));
  MPI_CHECK_EQ(flags.first_install_time, std::string("2026-08-26 16:19:26"));
  MPI_CHECK_EQ(flags.signature_digest, std::string("51ed3f60"));
}

MPI_TEST(a_reinstall_changes_the_facts_that_identify_the_build, {"B11"}) {
  // Measured on emulator-5554: `adb install -r` of the same APK moved the
  // code path from `~~wWybFpP9kRfAKS-aEzqL5A==/...-kN8n58LMWmrbU2sS_xQ87Q==`
  // to `~~z8OivMT9DzkrwDtGylZqFA==/...-sPU7L5iqpKZ4j9VNvjuSxQ==`, changed
  // lastUpdateTime, and gave the process a new pid and start time -- while
  // versionName and versionCode stayed exactly the same.
  //
  // That is the case B11 is really about: a developer rebuilding one version
  // all day. A tool that identified a build by its version number alone would
  // call two different builds the same build.
  const std::string before =
      "    versionCode=1 minSdk=28 targetSdk=36\n"
      "    versionName=5.3.26\n"
      "    codePath=/data/app/~~wWybFpP9kRfAKS-aEzqL5A==/"
      "io.pizzahut.hutbot.debug-kN8n58LMWmrbU2sS_xQ87Q==\n"
      "    lastUpdateTime=2026-09-14 16:11:02\n"
      "    flags=[ DEBUGGABLE HAS_CODE ]\n";
  const std::string after =
      "    versionCode=1 minSdk=28 targetSdk=36\n"
      "    versionName=5.3.26\n"
      "    codePath=/data/app/~~z8OivMT9DzkrwDtGylZqFA==/"
      "io.pizzahut.hutbot.debug-sPU7L5iqpKZ4j9VNvjuSxQ==\n"
      "    lastUpdateTime=2026-09-15 04:26:20\n"
      "    flags=[ DEBUGGABLE HAS_CODE ]\n";

  const auto a = parse_dumpsys_package_flags(before);
  const auto b = parse_dumpsys_package_flags(after);
  MPI_CHECK_MSG(a.version_name == b.version_name,
                "the version did not change, which is the whole difficulty");
  MPI_CHECK_MSG(a.version_code == b.version_code, "nor the version code");
  MPI_CHECK_MSG(a.code_path != b.code_path,
                "but the install path did, and that is detectable");
  MPI_CHECK_MSG(a.last_update_time != b.last_update_time,
                "and so did the update time");
}
