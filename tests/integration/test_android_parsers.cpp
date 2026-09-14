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
