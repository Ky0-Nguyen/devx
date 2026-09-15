// iOS provider-output parsers.
//
// Every fixture named `.real.` in this file is genuine output captured from
// this host's Xcode toolchain (Xcode 26.6 / devicectl 518.33 / xctrace 16.0)
// during M0. The two paired iPhones were `unavailable` at capture time and one
// iOS simulator was booted, which is why the physical-device paths below are
// asserted on real *offline* output while the simulator paths are asserted on
// real *live* output.
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "adapters/ios/ios_adapter.hpp"
#include "adapters/ios/xctrace_collector.hpp"
#include "core/session/live_capture.hpp"
#include <csignal>

#include "tests/unit/test_framework.hpp"

using namespace mpi;
using namespace mpi::ios;

namespace {

std::string read_fixture(const char* rel) {
  const char* dir = std::getenv("MPI_FIXTURE_DIR");
  const std::string path = std::string(dir ? dir : "fixtures") + "/" + rel;
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}

json::Value parse_fixture(const char* rel) {
  json::ParseError err;
  auto v = json::parse(read_fixture(rel), json::Limits{}, &err);
  return v.value_or(json::Value::null());
}

const model::DeviceRef* find_device(const std::vector<model::DeviceRef>& v,
                                    const std::string& id) {
  for (const auto& d : v) {
    if (d.device_id == id) return &d;
  }
  return nullptr;
}

const InstalledApp* find_app(const std::vector<InstalledApp>& v,
                             const std::string& bundle_id) {
  for (const auto& a : v) {
    if (a.bundle_id == bundle_id) return &a;
  }
  return nullptr;
}

}  // namespace

MPI_TEST(parses_real_devicectl_device_listing, {"A03", "J15"}) {
  // REAL devicectl output from this host.
  const auto doc = parse_fixture("provider-output/devicectl-list-devices.real.json");
  MPI_CHECK_MSG(!doc.is_null(), "the real devicectl fixture must parse");
  const auto devices = parse_devicectl_devices(doc, "518.33");
  MPI_CHECK_MSG(devices.size() == 2,
                "expected the two paired devices, got " +
                    std::to_string(devices.size()));
  for (const auto& d : devices) {
    MPI_CHECK(d.platform == model::Platform::kIos);
    MPI_CHECK_MSG(d.form == model::DeviceForm::kPhysical,
                  "hardwareProperties.reality was 'physical'");
    MPI_CHECK_MSG(!d.device_id.empty(), "the CoreDevice identifier is the handle");
    MPI_CHECK_MSG(!d.os_version.empty(), "osVersionNumber must be read");
    MPI_CHECK_MSG(!d.model.empty(), "marketingName must be read");
  }
}

MPI_TEST(unreachable_paired_device_is_offline_not_authorized, {"A03", "J16"}) {
  // REAL output: both devices had tunnelState "unavailable".
  const auto doc = parse_fixture("provider-output/devicectl-list-devices.real.json");
  const auto devices = parse_devicectl_devices(doc, "518.33");
  for (const auto& d : devices) {
    MPI_CHECK_MSG(d.trust == model::TrustState::kOffline,
                  "a paired-but-unreachable device must read as offline, got " +
                      std::string(model::to_string(d.trust)));
    MPI_CHECK_MSG(!d.usable_for_capture(),
                  "an unreachable device must not be offered for capture");
  }
}

MPI_TEST(real_device_metadata_is_read_from_the_right_fields, {"A03"}) {
  const auto doc = parse_fixture("provider-output/devicectl-list-devices.real.json");
  const auto devices = parse_devicectl_devices(doc, "518.33");
  const auto* iphone = find_device(devices, "3FF46431-775C-59BB-AD26-D316DFAFA5A6");
  MPI_CHECK_MSG(iphone != nullptr, "the iPhone's CoreDevice identifier is the key");
  MPI_CHECK_EQ(iphone->model, std::string("iPhone 12 Pro Max"));
  MPI_CHECK(iphone->os_version.find("26.0") != std::string::npos);
  MPI_CHECK(iphone->os_version.find("23A340") != std::string::npos);
  MPI_CHECK_EQ(iphone->display_name, std::string("QuocBao's iPhone"));
}

MPI_TEST(readiness_reports_ddi_services_state, {"J15", "J16"}) {
  // REAL output: ddiServicesAvailable was false on both offline devices. This
  // is the gate for app and process enumeration on a physical device.
  const auto doc = parse_fixture("provider-output/devicectl-list-devices.real.json");
  const auto r = parse_devicectl_readiness(
      doc, "3FF46431-775C-59BB-AD26-D316DFAFA5A6");
  MPI_CHECK(r.has_value());
  MPI_CHECK_MSG(r->ddi_services_available.has_value(),
                "ddiServicesAvailable must be read, not assumed");
  MPI_CHECK_EQ(*r->ddi_services_available, false);
  MPI_CHECK_EQ(r->developer_mode_status, std::string("enabled"));
  MPI_CHECK_EQ(r->tunnel_state, std::string("unavailable"));
  MPI_CHECK_EQ(r->pairing_state, std::string("paired"));
}

MPI_TEST(readiness_for_an_unknown_identifier_is_absent, {"A24"}) {
  const auto doc = parse_fixture("provider-output/devicectl-list-devices.real.json");
  MPI_CHECK(!parse_devicectl_readiness(doc, "not-a-real-identifier").has_value());
}

MPI_TEST(devicectl_parsers_tolerate_unexpected_shapes, {"D18", "J02"}) {
  // A malformed or unexpected document must yield nothing, never a crash and
  // never a fabricated device.
  for (const char* bad : {"{}", "[]", "null", "{\"result\":{}}",
                          "{\"result\":{\"devices\":\"not-an-array\"}}",
                          "{\"result\":{\"devices\":[{},{\"identifier\":1}]}}"}) {
    json::ParseError err;
    auto doc = json::parse(bad, &err);
    MPI_CHECK(doc.has_value());
    MPI_CHECK_MSG(parse_devicectl_devices(*doc, "").empty(),
                  std::string("should yield no device: ") + bad);
    MPI_CHECK(parse_devicectl_apps(*doc).empty());
    MPI_CHECK(parse_devicectl_processes(*doc).empty());
  }
}

MPI_TEST(devicectl_apps_parser_reads_bundle_identifiers, {"A13"}) {
  // SYNTHETIC: no reachable device, so this shape is asserted directly against
  // the documented field names rather than captured output.
  json::ParseError err;
  auto doc = json::parse(R"({"result":{"apps":[
      {"bundleIdentifier":"com.example.app","name":"Example","version":"1.2.3",
       "installationType":"User","debuggable":true},
      {"bundleIdentifier":"com.apple.Preferences","name":"Settings",
       "installationType":"System"},
      {"name":"no bundle id, must be skipped"}
  ]}})", &err);
  MPI_CHECK(doc.has_value());
  const auto apps = parse_devicectl_apps(*doc);
  MPI_CHECK_EQ(apps.size(), static_cast<std::size_t>(2));
  const auto* user_app = find_app(apps, "com.example.app");
  MPI_CHECK(user_app != nullptr);
  MPI_CHECK_EQ(user_app->name, std::string("Example"));
  MPI_CHECK_EQ(user_app->app_type, std::string("User"));
  MPI_CHECK(user_app->profileable_hint.has_value());
  MPI_CHECK_EQ(*user_app->profileable_hint, true);
  // An app with no debuggable field keeps the hint unset, not false.
  const auto* system_app = find_app(apps, "com.apple.Preferences");
  MPI_CHECK(system_app != nullptr);
  MPI_CHECK_MSG(!system_app->profileable_hint.has_value(),
                "an absent debuggable flag must stay unknown");
}

MPI_TEST(devicectl_processes_parser_reads_pid_and_path, {"B09"}) {
  json::ParseError err;
  auto doc = json::parse(R"({"result":{"runningProcesses":[
      {"processIdentifier":412,"executable":"file:///private/var/containers/Bundle/Application/X/Example.app/Example"},
      {"processIdentifier":99},
      {"executable":"no pid"}
  ]}})", &err);
  MPI_CHECK(doc.has_value());
  const auto procs = parse_devicectl_processes(*doc);
  MPI_CHECK_EQ(procs.size(), static_cast<std::size_t>(2));
  MPI_CHECK_EQ(procs[0].pid, 412);
  MPI_CHECK(procs[0].executable_path.find("Example.app") != std::string::npos);
  // A process with no executable path is kept but carries no path to match on.
  MPI_CHECK_EQ(procs[1].pid, 99);
  MPI_CHECK(procs[1].executable_path.empty());
}

MPI_TEST(parses_real_simctl_device_listing, {"A04", "J18"}) {
  // REAL simctl output from this host.
  const auto doc = parse_fixture("provider-output/simctl-list-devices.real.json");
  MPI_CHECK(!doc.is_null());
  const auto devices = parse_simctl_devices(doc, "");
  MPI_CHECK_MSG(devices.size() > 10,
                "this host has many simulators, got " +
                    std::to_string(devices.size()));
  for (const auto& d : devices) {
    MPI_CHECK_MSG(d.form == model::DeviceForm::kSimulator,
                  "every simctl device must be classified as a simulator");
    MPI_CHECK(d.connection == model::ConnectionType::kLocal);
    MPI_CHECK_MSG(!d.os_version.empty(), "the runtime key must yield an OS version");
  }
}

MPI_TEST(only_a_booted_simulator_is_usable, {"A05"}) {
  // REAL output: exactly one simulator was booted on this host.
  const auto doc = parse_fixture("provider-output/simctl-list-devices.real.json");
  const auto devices = parse_simctl_devices(doc, "");
  std::size_t booted = 0;
  for (const auto& d : devices) {
    if (d.usable_for_capture()) ++booted;
  }
  MPI_CHECK_MSG(booted == 1,
                "expected exactly one booted simulator, got " +
                    std::to_string(booted));
  const auto* sim = find_device(devices, "456FA0D8-48C1-4BEC-B087-50E8A046EA5D");
  MPI_CHECK(sim != nullptr);
  MPI_CHECK(sim->trust == model::TrustState::kAuthorized);
  MPI_CHECK_EQ(sim->display_name, std::string("iPhone 17 Pro"));
  MPI_CHECK_EQ(sim->os_version, std::string("26.5"));
}

MPI_TEST(simctl_runtime_key_yields_the_os_version, {"A04"}) {
  json::ParseError err;
  auto doc = json::parse(R"({"devices":{
      "com.apple.CoreSimulator.SimRuntime.iOS-17-4":[
        {"udid":"AAA","name":"iPhone X","state":"Shutdown","isAvailable":true}],
      "com.apple.CoreSimulator.SimRuntime.watchOS-10-0":[
        {"udid":"BBB","name":"Watch","state":"Booted","isAvailable":true}]
  }})", &err);
  MPI_CHECK(doc.has_value());
  const auto devices = parse_simctl_devices(*doc, "");
  // Only the iOS runtime is in scope for this adapter.
  MPI_CHECK_EQ(devices.size(), static_cast<std::size_t>(1));
  MPI_CHECK_EQ(devices[0].os_version, std::string("17.4"));
}

MPI_TEST(unavailable_simulator_runtimes_are_skipped, {"A04"}) {
  json::ParseError err;
  auto doc = json::parse(R"({"devices":{
      "com.apple.CoreSimulator.SimRuntime.iOS-16-0":[
        {"udid":"AAA","name":"Old","state":"Shutdown","isAvailable":false}]
  }})", &err);
  MPI_CHECK(doc.has_value());
  MPI_CHECK(parse_simctl_devices(*doc, "").empty());
}

MPI_TEST(parses_real_simctl_listapps_output, {"A13"}) {
  // REAL output: `simctl listapps` emits an old-style NeXTSTEP plist, which
  // the adapter converts with plutil. This fixture is the converted JSON.
  const auto doc = parse_fixture("provider-output/simctl-listapps-booted.real.json");
  MPI_CHECK(!doc.is_null());
  const auto apps = parse_simctl_listapps(doc);
  MPI_CHECK_MSG(apps.size() > 20, "expected the simulator's app set, got " +
                                      std::to_string(apps.size()));
  const auto* calendar = find_app(apps, "com.apple.mobilecal");
  MPI_CHECK(calendar != nullptr);
  MPI_CHECK_EQ(calendar->name, std::string("Calendar"));
  MPI_CHECK_EQ(calendar->app_type, std::string("System"));
  // Results are sorted by bundle id, so the picker order is stable.
  MPI_CHECK(std::is_sorted(apps.begin(), apps.end(),
                           [](const InstalledApp& a, const InstalledApp& b) {
                             return a.bundle_id < b.bundle_id;
                           }));
}

MPI_TEST(parses_real_launchctl_list_and_extracts_bundle_ids, {"B09"}) {
  // REAL output from `simctl spawn <udid> launchctl list`.
  const auto text = read_fixture("provider-output/simctl-launchctl-list.real.txt");
  MPI_CHECK(!text.empty());
  const auto entries = parse_launchctl_list(text);
  MPI_CHECK_MSG(entries.size() > 50, "expected many launchd jobs, got " +
                                         std::to_string(entries.size()));

  std::size_t with_bundle = 0;
  std::size_t running = 0;
  bool found_calendar = false;
  for (const auto& e : entries) {
    if (!e.bundle_id.empty()) ++with_bundle;
    if (e.pid.has_value()) ++running;
    if (e.bundle_id == "com.apple.mobilecal") {
      found_calendar = true;
      MPI_CHECK_MSG(e.pid.has_value(), "Calendar was running with a real pid");
      // The label must be stripped of its instance hash.
      MPI_CHECK(e.label.find("UIKitApplication:com.apple.mobilecal[") == 0);
    }
  }
  MPI_CHECK_MSG(found_calendar,
                "the real fixture contains UIKitApplication:com.apple.mobilecal");
  MPI_CHECK_MSG(with_bundle >= 3, "at least three app jobs were present");
  MPI_CHECK(running > 10);
}

MPI_TEST(launchctl_entries_without_a_pid_are_not_running, {"A08", "A11"}) {
  const auto entries = parse_launchctl_list(
      "PID\tStatus\tLabel\n"
      "-\t0\tUIKitApplication:com.example.loaded[abcd][rb-legacy]\n"
      "4242\t0\tUIKitApplication:com.example.running[efgh][rb-legacy]\n"
      "-\t0\tcom.apple.somedaemon\n");
  MPI_CHECK_EQ(entries.size(), static_cast<std::size_t>(3));
  MPI_CHECK_MSG(!entries[0].pid.has_value(),
                "a '-' pid means loaded but not running");
  MPI_CHECK_EQ(entries[0].bundle_id, std::string("com.example.loaded"));
  MPI_CHECK(entries[1].pid.has_value());
  MPI_CHECK_EQ(*entries[1].pid, 4242);
  // A non-UIKitApplication label yields no bundle id at all.
  MPI_CHECK_MSG(entries[2].bundle_id.empty(),
                "a daemon label must not be mistaken for an app bundle id");
}

MPI_TEST(launchctl_parser_handles_empty_and_header_only_input, {"A12", "D16"}) {
  MPI_CHECK(parse_launchctl_list("").empty());
  MPI_CHECK(parse_launchctl_list("PID\tStatus\tLabel\n").empty());
  // Malformed rows are skipped rather than producing partial entries.
  MPI_CHECK(parse_launchctl_list("PID\tStatus\tLabel\nonly-one-column\n").empty());
}

MPI_TEST(adapter_probe_runs_against_the_real_toolchain, {"J15", "M0"}) {
  // This exercises the REAL installed toolchain on this host.
  IosAdapter adapter;
  model::CapabilityMatrix m;
  discovery::ProviderOptions opts;
  opts.command_timeout_ms = 45000;
  adapter.probe(m, opts);

  const auto* toolchain = m.find("ios.toolchain.xcrun");
  MPI_CHECK_MSG(toolchain != nullptr, "the toolchain capability must be reported");
  MPI_CHECK_MSG(toolchain->status == model::CapabilityStatus::kAvailable,
                "xcrun is present on this host");
  MPI_CHECK(toolchain->evidence.find("xcode-select") != std::string::npos);

  const auto* devices = m.find("ios.discovery.devices");
  MPI_CHECK(devices != nullptr);
  MPI_CHECK(devices->status == model::CapabilityStatus::kAvailable);
  MPI_CHECK_MSG(!devices->evidence.empty(),
                "a capability claim must carry its probe evidence");

  // The capability the specification insists must not be over-claimed.
  const auto* attach = m.find("ios.capture.attach");
  MPI_CHECK(attach != nullptr);
  MPI_CHECK_MSG(attach->status == model::CapabilityStatus::kUnknown,
                "deep attach cannot be established at the device level");
  MPI_CHECK(attach->tested == model::TestedState::kNotTested);
  bool mentions_no_bypass = false;
  for (const auto& l : attach->limitations) {
    if (l.find("no jailbreak") != std::string::npos) mentions_no_bypass = true;
  }
  MPI_CHECK(mentions_no_bypass);

  // Live capture must be explicitly unverified *for a physical device*, per
  // spec section 0.15.
  //
  // This used to assert `tested == kNotTested`, which stopped being the right
  // expression of it: a booted simulator streams through the host-process
  // collector, so `tested` legitimately becomes
  // verified_on_simulator_or_emulator. The invariant is that the physical
  // path is still declared unverified -- which lives in the limitations, and
  // is where a reader looks for what a capability does not cover.
  const auto* live = m.find("ios.capture.live_recording");
  MPI_CHECK(live != nullptr);
  MPI_CHECK_MSG(live->tested != model::TestedState::kVerifiedOnPhysicalDevice,
                "live physical capture was never demonstrated here, so it is "
                "never marked verified on a physical device");
  bool says_unverified = false;
  for (const auto& l : live->limitations) {
    if (l.find("UNVERIFIED") != std::string::npos) says_unverified = true;
  }
  MPI_CHECK_MSG(says_unverified,
                "the live-capture gap must be labelled UNVERIFIED, not omitted");
  bool names_the_device_gap = false;
  for (const auto& l : live->limitations) {
    if (l.find("UNVERIFIED on a physical device") != std::string::npos) {
      names_the_device_gap = true;
    }
  }
  MPI_CHECK_MSG(names_the_device_gap,
                "and it says *which* path is unverified, since the simulator "
                "path is not");
}

MPI_TEST(adapter_lists_real_devices_including_simulators, {"A04", "J18"}) {
  IosAdapter adapter;
  adapter.set_include_simulators(true);
  std::vector<std::string> errors;
  discovery::ProviderOptions opts;
  opts.command_timeout_ms = 45000;
  const auto with_sims = adapter.list_devices(opts, errors);
  MPI_CHECK_MSG(!with_sims.empty(), "this host has paired devices and simulators");

  adapter.set_include_simulators(false);
  std::vector<std::string> errors2;
  const auto without = adapter.list_devices(opts, errors2);
  MPI_CHECK_MSG(without.size() < with_sims.size(),
                "excluding simulators must shrink the list");
  for (const auto& d : without) {
    MPI_CHECK_MSG(d.form != model::DeviceForm::kSimulator,
                  "no simulator may survive --no-simulators");
  }
}

MPI_TEST(adapter_refuses_an_option_like_bundle_id, {"A20", "J05"}) {
  IosAdapter adapter;
  model::DeviceRef device;
  device.device_id = "sim-udid";
  device.form = model::DeviceForm::kSimulator;
  device.trust = model::TrustState::kAuthorized;
  model::ApplicationKey app;
  app.app_identifier = "-rf";
  std::vector<std::string> errors;
  const auto procs = adapter.resolve_processes(device, app,
                                               discovery::ProviderOptions{}, errors);
  MPI_CHECK(procs.empty());
  bool refused = false;
  for (const auto& e : errors) {
    if (e.find("command-line option") != std::string::npos) refused = true;
  }
  MPI_CHECK(refused);
}

MPI_TEST(adapter_reports_unreachable_device_as_enumeration_failure, {"A03", "A12"}) {
  IosAdapter adapter;
  model::DeviceRef device;
  device.device_id = "3FF46431-775C-59BB-AD26-D316DFAFA5A6";
  device.form = model::DeviceForm::kPhysical;
  device.trust = model::TrustState::kOffline;
  std::vector<std::string> errors;
  bool failed = false;
  const auto apps =
      adapter.list_apps(device, discovery::ProviderOptions{}, errors, failed);
  MPI_CHECK(apps.empty());
  MPI_CHECK_MSG(failed,
                "an unreachable device is an enumeration failure, not an app-less "
                "device");
  bool explained = false;
  for (const auto& e : errors) {
    if (e.find("not an empty app list") != std::string::npos) explained = true;
  }
  MPI_CHECK(explained);
}

MPI_TEST(simulator_apps_are_enumerated_from_the_real_booted_simulator,
         {"J15", "J18", "M0"}) {
  // This runs against the REAL booted simulator on this host. If none is
  // booted the test records that rather than failing, because a missing device
  // is not a code defect -- but it will then not have verified anything.
  IosAdapter adapter;
  std::vector<std::string> errors;
  discovery::ProviderOptions opts;
  opts.command_timeout_ms = 60000;
  const auto devices = adapter.list_devices(opts, errors);
  const model::DeviceRef* booted = nullptr;
  for (const auto& d : devices) {
    if (d.form == model::DeviceForm::kSimulator && d.usable_for_capture()) {
      booted = &d;
      break;
    }
  }
  if (!booted) {
    std::cout << "       (no booted simulator: this capability was NOT verified)\n";
    return;
  }

  std::vector<std::string> app_errors;
  bool failed = false;
  const auto apps = adapter.list_apps(*booted, opts, app_errors, failed);
  MPI_CHECK_MSG(!failed, "enumeration on a booted simulator should succeed");
  MPI_CHECK_MSG(apps.size() > 10, "expected the simulator's apps, got " +
                                      std::to_string(apps.size()));

  std::size_t running = 0;
  std::size_t provider_attributed = 0;
  for (const auto& a : apps) {
    MPI_CHECK_MSG(a.key.identifier_kind == model::IdentifierKind::kBundleId,
                  "iOS targets are keyed by bundle id");
    MPI_CHECK_MSG(a.runtime_state != model::RuntimeState::kSuspended,
                  "iOS exposes no suspended signal, so it must never be claimed");
    if (a.runtime_state == model::RuntimeState::kRunning) {
      ++running;
      for (const auto& p : a.processes) {
        if (p.ownership == model::OwnershipEvidence::kProviderAttributed) {
          ++provider_attributed;
          MPI_CHECK(p.ownership_note.find("launchd") != std::string::npos);
        }
      }
    }
    // Nothing on a simulator may be presented as device-representative.
    if (a.profiling == model::ProfilingAvailability::kLimited) {
      MPI_CHECK(a.profiling_reason.find("simulator only") != std::string::npos);
    }
  }
  MPI_CHECK_MSG(running > 0, "at least one app should be running on a booted sim");
  MPI_CHECK_MSG(provider_attributed > 0,
                "launchd labels give provider-attributed ownership");
}


// --- xctrace capture outcomes ------------------------------------------------
//
// The successful path needs a device this environment does not have. The
// failures are what a user actually hits, and every one of them is a
// different statement about what was and was not measured.

MPI_TEST(xctrace_timeout_is_a_provider_failure_not_an_empty_capture,
         {"J15", "E13", "D07"}) {
  // The measured behaviour on this host: xctrace accepts a simulator target
  // and then never starts recording. The signature is the *absence* of
  // "Ctrl-C to stop the recording" while still holding the target -- isolated
  // by comparison, since the identical command against a macOS process
  // honours --time-limit, exits by itself and writes a bundle that exports.
  const auto out = ios::interpret_record_output(
      "Starting recording with the Time Profiler template. Attaching to: "
      "Settings (87700). Time limit: 3.0 s\n",
      "", /*exit_code=*/-1, /*timed_out=*/true);
  MPI_CHECK(out.attached);
  MPI_CHECK(out.timed_out);
  MPI_CHECK(!out.wrote_bundle);
  MPI_CHECK_MSG(!out.began_recording,
                "no 'Ctrl-C to stop' line means it never started recording");
  MPI_CHECK(!out.refusal.empty());
  // The refusal must say *which* failure this is, because the two have
  // different answers: one may have a salvageable trace and this one cannot.
  MPI_CHECK_MSG(out.refusal.find("never started recording") != std::string::npos,
                "the refusal names the failure precisely");
  MPI_CHECK_MSG(out.refusal.find("simulator") != std::string::npos,
                "and says it is a simulator limitation, not the invocation");
  // The distinction the whole tool is built around, now carried where it
  // belongs: the caller states "no readable trace, so nothing was measured"
  // in the capability, and the refusal points at the path that does work.
  MPI_CHECK_MSG(out.refusal.find("live capture") != std::string::npos,
                "and points at the alternative that does work, rather than "
                "leaving the operator with a dead end");
}

MPI_TEST(a_recording_that_started_is_a_different_failure_from_one_that_did_not,
         {"J15", "D07"}) {
  // xctrace got as far as recording and then failed to exit. That trace may
  // be real and readable, so this refusal must NOT claim the simulator
  // limitation -- the caller tries the export before believing it.
  const auto out = ios::interpret_record_output(
      "Starting recording with the Time Profiler template. Attaching to: "
      "burn (77280). Time limit: 4000.0 ms\n"
      "Ctrl-C to stop the recording\n",
      "", /*exit_code=*/-1, /*timed_out=*/true);
  MPI_CHECK(out.attached);
  MPI_CHECK_MSG(out.began_recording, "the recording did start");
  MPI_CHECK_MSG(out.refusal.find("never started recording") == std::string::npos,
                "so it is not reported as the simulator hang");
  MPI_CHECK_MSG(out.refusal.find("did not exit") != std::string::npos,
                "it is reported as what it is: a process that would not exit");
}

MPI_TEST(a_target_never_accepted_is_not_blamed_on_the_simulator, {"J15"}) {
  // Nothing was accepted at all -- a different cause again, and claiming the
  // simulator limitation here would send someone to the wrong answer.
  const auto out = ios::interpret_record_output(
      "", "", /*exit_code=*/-1, /*timed_out=*/true);
  MPI_CHECK(!out.attached);
  MPI_CHECK(!out.began_recording);
  MPI_CHECK_MSG(out.refusal.find("never reported accepting") != std::string::npos,
                "the refusal says the target was never accepted");
  MPI_CHECK_MSG(out.refusal.find("simulator") == std::string::npos,
                "and does not attribute it to the simulator limitation");
}

MPI_TEST(xctrace_keeps_a_bundle_it_wrote_despite_reporting_errors,
         {"D19", "J15"}) {
  // Real output from this host: it says the recording failed and still writes
  // a usable 10 MB bundle. Discarding that would throw away real samples.
  const auto out = ios::interpret_record_output(
      "Recording failed with errors. Saving output file...\n"
      "Output file saved as: /tmp/mpi/run.trace\n",
      "", /*exit_code=*/2, /*timed_out=*/false);
  MPI_CHECK(out.wrote_bundle);
  MPI_CHECK_EQ(out.output_path, std::string("/tmp/mpi/run.trace"));
  // Kept, but the caveat travels with it.
  MPI_CHECK(out.refusal.empty());
  MPI_CHECK(!out.notes.empty());
}

MPI_TEST(xctrace_explains_an_attach_that_found_no_process, {"J15", "B15"}) {
  const auto out = ios::interpret_record_output(
      "", "Cannot find process for provided pid: 87700\n",
      /*exit_code=*/21, /*timed_out=*/false);
  MPI_CHECK(!out.attached);
  MPI_CHECK(!out.refusal.empty());
  MPI_CHECK(out.refusal.find("--device") != std::string::npos);
}

MPI_TEST(xctrace_reports_a_nonzero_exit_with_no_bundle, {"J15"}) {
  const auto out = ios::interpret_record_output("", "some other failure\n",
                                                /*exit_code=*/70,
                                                /*timed_out=*/false);
  MPI_CHECK(!out.wrote_bundle);
  MPI_CHECK(out.refusal.find("exited 70") != std::string::npos);
}

MPI_TEST(xctrace_export_xpath_is_built_in_one_place, {"J05", "J20"}) {
  // The quoting matters and is easy to get subtly wrong at a call site, so
  // there is exactly one place that builds it.
  MPI_CHECK_EQ(ios::export_xpath_for("time-profile"),
               std::string("/trace-toc/run[@number=\"1\"]/data/"
                           "table[@schema=\"time-profile\"]"));
  MPI_CHECK_EQ(ios::export_xpath_for("potential-hangs", 3),
               std::string("/trace-toc/run[@number=\"3\"]/data/"
                           "table[@schema=\"potential-hangs\"]"));
}

MPI_TEST(xctrace_collector_does_not_claim_to_stream, {"section-13"}) {
  // Instruments records a window and writes its bundle at the end. A live
  // view would mean inventing intermediate numbers.
  ios::XctraceCollector collector;
  MPI_CHECK(!collector.supports_streaming());
  MPI_CHECK(collector.platform() == model::Platform::kIos);
}

MPI_TEST(the_ios_collector_is_wired_but_cannot_stream, {"J18", "H05"}) {
  // This exists because of a message that was wrong. The desktop app told an
  // iOS user "the xctrace collector is not wired to the session controller
  // yet", which contradicted what the CLI does on the same device: the CLI
  // has constructed this collector for iOS for some time, and `mpi record`
  // against a simulator really does attach.
  //
  // The distinction that message should have drawn is the one asserted here.
  // The collector exists and is usable for a batch capture; what it cannot do
  // is stream, because `xctrace record` yields a trace bundle when it
  // finishes rather than events that can be read while it runs.
  ios::XctraceCollector collector;

  // Wired: it is a Collector, it names itself, and it claims iOS.
  session::Collector& as_collector = collector;
  MPI_CHECK(!as_collector.id().empty());
  MPI_CHECK(as_collector.platform() == model::Platform::kIos);

  // And it does not pretend to stream. A live session asked to drive it must
  // refuse on this, not on a claim about wiring.
  MPI_CHECK_MSG(!as_collector.supports_streaming(),
                "the iOS collector reports no streaming support, which is the "
                "real reason live capture is unavailable there");

  // A live session handed it refuses, and the refusal names streaming.
  session::LiveSession live;
  model::DeviceRef device;
  device.device_id = "sim-1";
  device.platform = model::Platform::kIos;
  device.form = model::DeviceForm::kSimulator;
  model::ProcessInstance p;
  p.pid = 1;
  const bool started =
      live.start(std::make_shared<ios::XctraceCollector>(), device, {p},
                 session::CaptureConfig{}, model::NormalizedTrace{});
  MPI_CHECK(!started);
  const auto snap = live.snapshot();
  MPI_CHECK(snap.state == session::LiveState::kFailed);
  MPI_CHECK_MSG(snap.error.find("streaming") != std::string::npos,
                "the refusal is about streaming: " + snap.error);
  MPI_CHECK_MSG(snap.error.find("not wired") == std::string::npos,
                "and never claims the collector is unwired");
}

MPI_TEST(xctrace_is_stopped_with_the_signal_it_responds_to, {"J15"}) {
  // test_process proves `proc::run` honours a stop signal and a grace
  // period. This proves *this collector* asks for the right ones, which is
  // the half that actually prevents the data loss: Instruments ignores a
  // polite SIGTERM and finalises its trace bundle on SIGINT, taking seconds
  // over it.
  MPI_CHECK_MSG(ios::xctrace_stop_signal() == SIGINT,
                "a recording is interrupted, not terminated: SIGTERM does "
                "not make Instruments write its bundle");
  MPI_CHECK_MSG(ios::xctrace_stop_signal() != SIGTERM,
                "and specifically not the default, which is what lost "
                "captures that had already succeeded");
  MPI_CHECK_MSG(ios::xctrace_stop_grace() >= std::chrono::milliseconds(5000),
                "with seconds to finish writing, not the default 500ms");
  MPI_CHECK_MSG(ios::xctrace_stop_grace() <= std::chrono::milliseconds(60000),
                "and still bounded, because a collector that hangs on "
                "shutdown is worse than a lost bundle");
}

MPI_TEST(a_stub_bundle_is_not_mistaken_for_a_capture, {"J15", "D07"}) {
  // The salvage path's decision, which used to be inline in the capture
  // function and therefore reachable only with a device attached -- the one
  // thing this environment has never had.
  //
  // It has to be strict in both directions. A recording xctrace finished but
  // did not exit from is real and must be kept; a stub it left behind looks
  // like a result and is not. Both were measured: a real macOS recording
  // produced a 10 MB bundle that `--toc` reads, and a simulator attempt
  // produced a 52 KB stub that `--toc` rejects with "Document Missing
  // Template Error".
  proc::Options opts;
  opts.timeout = std::chrono::milliseconds(20000);

  const auto missing = ios::bundle_is_readable("/nonexistent/path.trace", opts);
  MPI_CHECK_MSG(!missing.readable, "a bundle that does not exist is not one");
  MPI_CHECK_MSG(missing.detail.find("no bundle exists") != std::string::npos,
                "and that is reported as absence, which is a different "
                "failure from an unreadable bundle and has a different cause");

  MPI_CHECK_MSG(!ios::bundle_is_readable("", opts).readable,
                "an empty path is refused rather than passed to xctrace");

  // An option-shaped path must never reach the child process.
  const auto unsafe = ios::bundle_is_readable("--input", opts);
  MPI_CHECK(!unsafe.readable);
  MPI_CHECK_MSG(unsafe.detail.find("refusing") != std::string::npos ||
                    unsafe.detail.find("no bundle exists") != std::string::npos,
                "and is either refused outright or fails the existence check "
                "first; either way it is not handed to xctrace as a flag");

  // A directory that exists and is not a trace bundle: the shape a partial
  // or corrupted recording leaves behind. It must not pass.
  const std::string fake =
      std::filesystem::temp_directory_path().string() + "/mpi-fake.trace";
  std::filesystem::remove_all(fake);
  std::filesystem::create_directories(fake);
  {
    std::ofstream junk(fake + "/not-a-trace.txt");
    junk << "this is not a trace bundle";
  }
  const auto stub = ios::bundle_is_readable(fake, opts);
  MPI_CHECK_MSG(!stub.readable,
                "a directory that exists but is not a readable trace does "
                "not count as a capture");
  MPI_CHECK_MSG(!stub.detail.empty(), "and the tool's own reason is kept");
  std::filesystem::remove_all(fake);
}

MPI_TEST(the_hardest_ios_refusal_says_when_it_was_observed, {"J15"}) {
  // ddiServicesAvailable = false stops app enumeration outright and sends
  // someone to Xcode. `devicectl device info details` answers from a cached
  // record -- proven: it returns outcome:success in a tenth of a second for
  // a device that is not present -- so this reading is an observation with a
  // date, and stating it without the date is a claim with no timestamp.
  //
  // The date was already being parsed and then never used anywhere, which is
  // worse than not having it.
  ios::DeviceReadiness r;
  r.ddi_services_available = false;
  r.last_connection_date = "2025-09-11T10:10:27.063Z";
  const std::string with_date = ios::ddi_refusal_text(r);
  MPI_CHECK_MSG(with_date.find("ddiServicesAvailable = false") != std::string::npos,
                "the finding itself is still stated");
  MPI_CHECK_MSG(with_date.find("2025-09-11T10:10:27.063Z") != std::string::npos,
                "with the date devicectl last spoke to the device");
  MPI_CHECK_MSG(with_date.find("re-run discovery") != std::string::npos,
                "and what to do if the device has been reconnected since");

  // No date available: the sentence must still stand on its own rather than
  // trailing off into a dangling clause.
  ios::DeviceReadiness undated;
  undated.ddi_services_available = false;
  const std::string plain = ios::ddi_refusal_text(undated);
  MPI_CHECK(plain.find("ddiServicesAvailable = false") != std::string::npos);
  MPI_CHECK_MSG(plain.find("as of") == std::string::npos,
                "no date is claimed when none was reported");

  // Developer Mode is a separate prerequisite. Someone told only about the
  // disk image will go to Xcode when the device is asking for a setting.
  ios::DeviceReadiness both;
  both.ddi_services_available = false;
  both.developer_mode_status = "disabled";
  const std::string two = ios::ddi_refusal_text(both);
  MPI_CHECK_MSG(two.find("Developer Mode") != std::string::npos,
                "both blockers are named when both are present");

  // And it is not mentioned when it is not a blocker, so the message does
  // not send someone to check a setting that is already correct.
  ios::DeviceReadiness ok_mode;
  ok_mode.ddi_services_available = false;
  ok_mode.developer_mode_status = "enabled";
  MPI_CHECK(ios::ddi_refusal_text(ok_mode).find("Developer Mode") ==
            std::string::npos);
}

MPI_TEST(preflight_does_not_contradict_the_live_capture_it_can_do, {"J15"}) {
  // iOS live capture became two answers when SimulatorHostCollector landed:
  // a booted simulator streams without Instruments, while the physical path
  // is still undemonstrated. Preflight reported only the second, so it told
  // an operator live capture was unverified while the Live tab would run it
  // successfully on the simulator in front of them -- the same class of
  // defect as the old "the collector is not wired" message, which was also
  // false.
  ios::IosAdapter adapter;
  model::CapabilityMatrix matrix;
  discovery::ProviderOptions opts;
  adapter.probe(matrix, opts);

  const model::Capability* live = nullptr;
  for (const auto& c : matrix.capabilities) {
    if (c.id == "ios.capture.live_recording") live = &c;
  }
  MPI_CHECK_MSG(live != nullptr, "the capability is reported");

  // Whatever this host has, the physical-device caveat is always present: a
  // booted simulator does not make hardware verified, and dropping the note
  // when one happens to be running would let a green simulator answer stand
  // in for a path that has never been tested.
  bool says_device_unverified = false, mentions_simulator = false;
  for (const auto& l : live->limitations) {
    if (l.find("UNVERIFIED on a physical device") != std::string::npos) {
      says_device_unverified = true;
    }
    if (l.find("simulator") != std::string::npos) mentions_simulator = true;
  }
  MPI_CHECK_MSG(says_device_unverified,
                "the physical-device path is always declared unverified");
  MPI_CHECK_MSG(mentions_simulator,
                "and the simulator is named, since the answer differs there");

  // The name must cover both, or a reader scanning ids sees only half of what
  // the capability reports.
  MPI_CHECK_MSG(live->human_name.find("simulator") != std::string::npos,
                "the capability's name says it covers simulators too");

  // A prerequisite list that mentions only a physical device would send
  // someone hunting for hardware they do not need.
  bool prereq_mentions_simulator = false;
  for (const auto& pre : live->prerequisites) {
    if (pre.find("simulator") != std::string::npos) {
      prereq_mentions_simulator = true;
    }
  }
  MPI_CHECK(prereq_mentions_simulator);

  // When a booted simulator is present the status must not be `unknown`:
  // that is the contradiction this fixes. When none is, unknown is right.
  bool booted_simulator_present = false;
  std::vector<std::string> errs;
  for (const auto& d : adapter.list_devices(opts, errs)) {
    if (d.form == model::DeviceForm::kSimulator && d.usable_for_capture()) {
      booted_simulator_present = true;
    }
  }
  if (booted_simulator_present) {
    MPI_CHECK_MSG(live->status == model::CapabilityStatus::kLimited,
                  "with a booted simulator, live capture is reported as "
                  "limited-but-working rather than unknown");
    MPI_CHECK_MSG(live->evidence.find("host process") != std::string::npos,
                  "with the reason it works: the app is a host process");
    MPI_CHECK_MSG(live->tested ==
                      model::TestedState::kVerifiedOnSimulatorOrEmulator,
                  "and it is marked verified on a simulator, not untested");
  } else {
    MPI_CHECK_MSG(live->status == model::CapabilityStatus::kUnknown,
                  "with nothing to probe, unknown is the honest status");
  }
}
