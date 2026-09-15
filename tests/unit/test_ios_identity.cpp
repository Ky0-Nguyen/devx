// iOS has two identifier namespaces, and they are not interchangeable.
//
// `devicectl` -- how physical iOS devices are discovered, and what app
// listing and launching go through -- reports a CoreDevice record id. The
// same phone is known to `xctrace`, which performs the recording, only by its
// hardware UDID. Measured on a real device:
//
//   devicectl identifier  3FF46431-775C-59BB-AD26-D316DFAFA5A6
//   xctrace / hardware    00008101-000978CA11A1001E
//
// Handing the first to the second failed every physical-device capture, and
// the symptom read as a broken connection rather than a mismatched name.
#include "adapters/ios/ios_adapter.hpp"
#include "core/model/identity.hpp"
#include "tests/unit/test_framework.hpp"

namespace {

mpi::model::DeviceRef iphone() {
  mpi::model::DeviceRef d;
  d.platform = mpi::model::Platform::kIos;
  d.form = mpi::model::DeviceForm::kPhysical;
  d.device_id = "3FF46431-775C-59BB-AD26-D316DFAFA5A6";
  d.hardware_udid = "00008101-000978CA11A1001E";
  return d;
}

}  // namespace

MPI_TEST(a_capture_uses_the_identifier_the_capture_tool_knows, {}) {
  const auto d = iphone();
  MPI_CHECK_MSG(d.capture_id() == "00008101-000978CA11A1001E",
                "xctrace is given the hardware UDID");
  MPI_CHECK_MSG(d.capture_id() != d.device_id,
                "which is not the id devicectl uses");
}

MPI_TEST(one_identifier_means_capture_id_is_that_identifier, {}) {
  // An Android serial, or a simulator UDID: the same string in every tool.
  mpi::model::DeviceRef android;
  android.device_id = "emulator-5554";
  MPI_CHECK_MSG(android.capture_id() == "emulator-5554",
                "with nothing to distinguish, capture_id is device_id");
  MPI_CHECK_MSG(android.hardware_udid.empty(),
                "and no second identifier is invented");

  // Serialising must keep the two cases distinguishable: a reader has to be
  // able to tell "this device has two names" from "these fields agree".
  const std::string dumped = android.to_json().dump();
  MPI_CHECK_MSG(dumped.find("\"hardware_udid\":null") != std::string::npos,
                "a device with one name reports null, not a duplicate");
  const std::string ios = iphone().to_json().dump();
  MPI_CHECK_MSG(ios.find("00008101-000978CA11A1001E") != std::string::npos,
                "and a device with two reports both");
}

MPI_TEST(either_identifier_resolves_the_device, {}) {
  const auto d = iphone();
  MPI_CHECK_MSG(d.matches_id("3FF46431-775C-59BB-AD26-D316DFAFA5A6"),
                "the CoreDevice id resolves");
  MPI_CHECK_MSG(d.matches_id("00008101-000978CA11A1001E"),
                "and so does the hardware UDID, which is what `xctrace list "
                "devices` prints and therefore what someone will paste");
  MPI_CHECK_MSG(!d.matches_id("00008101-DEADBEEFDEADBEEF"),
                "an id that is neither does not resolve");
  MPI_CHECK_MSG(!d.matches_id(""),
                "and an empty id matches nothing, rather than the first "
                "device in the list");

  // A prefix must not match: two devices from the same production run share
  // the leading digits of their UDIDs, and a prefix match would silently
  // retarget a capture.
  MPI_CHECK(!d.matches_id("00008101"));
  MPI_CHECK(!d.matches_id("3FF46431"));
}

MPI_TEST(a_device_with_one_name_is_not_matched_by_an_empty_second, {}) {
  // The subtle failure: `hardware_udid` empty on both sides comparing equal
  // would make every single-identifier device match every other.
  mpi::model::DeviceRef a, b;
  a.device_id = "emulator-5554";
  b.device_id = "emulator-5556";
  MPI_CHECK(!a.matches_id(b.device_id));
  MPI_CHECK(!a.matches_id(b.hardware_udid));   // both empty
  MPI_CHECK(!b.matches_id(a.hardware_udid));
}

MPI_TEST(a_cached_listing_is_not_a_statement_about_now, {}) {
  // `devicectl device info details` returns outcome:success in about a tenth
  // of a second for a device that is not present, answering from a cached
  // CoreDevice record. Measured on a phone that had been away four days: it
  // still reported developerModeStatus enabled. Every property from that
  // listing is therefore a description of the past, and the date has to
  // travel with it.
  auto d = iphone();
  d.trust = mpi::model::TrustState::kOffline;
  d.last_seen_at = "2025-09-11T10:10:27.063Z";

  const std::string dumped = d.to_json().dump();
  MPI_CHECK_MSG(dumped.find("2025-09-11T10:10:27.063Z") != std::string::npos,
                "the last-seen date is reported");

  mpi::model::DeviceRef android;
  android.device_id = "emulator-5554";
  MPI_CHECK_MSG(android.to_json().dump().find("\"last_seen_at\":null") !=
                    std::string::npos,
                "a provider that only lists what it can see right now "
                "reports null, not a fabricated date");
}

MPI_TEST(an_unusable_ios_device_is_explained_not_just_labelled, {}) {
  mpi::discovery::ProviderOptions opts;

  // A physical device that is away. No probe: the probe spawns a process and
  // this asserts the explanation, not the probing.
  auto phone = iphone();
  phone.trust = mpi::model::TrustState::kOffline;
  phone.last_seen_at = "2025-09-11T10:10:27.063Z";
  auto lines = mpi::ios::explain_unusable_device(phone, opts, /*probe=*/false);
  MPI_CHECK_MSG(!lines.empty(), "an offline phone gets an explanation");
  bool mentions_date = false;
  for (const auto& l : lines) {
    if (l.find("2025-09-11") != std::string::npos) mentions_date = true;
  }
  MPI_CHECK_MSG(mentions_date,
                "and it leads with when the device was last actually seen, "
                "which is the fact that distinguishes a connection problem "
                "from a device that is somewhere else");

  // A simulator's answer is completely different, and saying "connect a
  // cable" for one would be nonsense.
  mpi::model::DeviceRef sim;
  sim.platform = mpi::model::Platform::kIos;
  sim.form = mpi::model::DeviceForm::kSimulator;
  sim.device_id = "B11AD99F-1066-4B0B-B6F3-964A597A57E2";
  sim.trust = mpi::model::TrustState::kOffline;
  lines = mpi::ios::explain_unusable_device(sim, opts, /*probe=*/false);
  MPI_CHECK(lines.size() == 1);
  MPI_CHECK_MSG(lines.front().find("not booted") != std::string::npos,
                "an unbooted simulator is told to boot");
  MPI_CHECK_MSG(lines.front().find("mpi boot") != std::string::npos,
                "with the command that does it");
  MPI_CHECK_MSG(lines.front().find("cable") == std::string::npos,
                "and is never told to connect a cable");

  // Untrusted is a different blocker from unreachable, and conflating them
  // sends someone hunting for a cable when the phone is asking to be
  // trusted.
  auto untrusted = iphone();
  untrusted.trust = mpi::model::TrustState::kUntrusted;
  lines = mpi::ios::explain_unusable_device(untrusted, opts, /*probe=*/false);
  bool mentions_developer_mode = false;
  for (const auto& l : lines) {
    if (l.find("Developer Mode") != std::string::npos) mentions_developer_mode = true;
  }
  MPI_CHECK_MSG(mentions_developer_mode,
                "an untrusted device names pairing and Developer Mode");

  // Android is not this adapter's business, and answering for it would be
  // inventing advice.
  mpi::model::DeviceRef droid;
  droid.platform = mpi::model::Platform::kAndroid;
  droid.trust = mpi::model::TrustState::kOffline;
  MPI_CHECK(mpi::ios::explain_unusable_device(droid, opts, false).empty());
}

MPI_TEST(a_simulator_probe_distinguishes_not_running_from_unknown, {}) {
  using mpi::ios::Reachability;
  mpi::discovery::ProviderOptions opts;

  // A UDID that is not a simulator at all. simctl answers "Invalid device",
  // which is a statement about the simulator and not about the probe, so it
  // must come back as not_found rather than probe_failed -- the distinction
  // the enum exists for, and one the first version of this got wrong by
  // matching on wording simctl does not actually use.
  const auto bogus = mpi::ios::probe_reachability(
      "00000000-0000-0000-0000-000000000000", opts,
      mpi::model::DeviceForm::kSimulator);
  MPI_CHECK_MSG(bogus.state == Reachability::kNotFound,
                "an unknown simulator UDID is not running, which is a "
                "different answer from the probe failing to settle it");
  MPI_CHECK_MSG(bogus.evidence.find("simctl") != std::string::npos,
                "and the evidence names the simulator command, not devicectl");
  MPI_CHECK_MSG(bogus.took < std::chrono::milliseconds(5000),
                "and it answers quickly enough to run during discovery");

  // The form selects the question: the same id asked as a physical device
  // goes to devicectl and gets devicectl's answer.
  const auto as_physical = mpi::ios::probe_reachability(
      "00000000-0000-0000-0000-000000000000", opts,
      mpi::model::DeviceForm::kPhysical);
  MPI_CHECK_MSG(as_physical.evidence.find("devicectl") != std::string::npos,
                "a physical device is asked through devicectl");
  MPI_CHECK(as_physical.evidence != bogus.evidence);
}

MPI_TEST(reachability_states_are_all_distinct_answers, {}) {
  using mpi::ios::Reachability;
  // The three answers must not collapse: "not found" is about the device,
  // "probe failed" is about us, and treating the second as the first would
  // report a slow or missing devicectl as an absent phone.
  MPI_CHECK(std::string(mpi::ios::to_string(Reachability::kReachable)) ==
            "reachable");
  MPI_CHECK(std::string(mpi::ios::to_string(Reachability::kNotFound)) ==
            "not_found");
  MPI_CHECK(std::string(mpi::ios::to_string(Reachability::kProbeFailed)) ==
            "probe_failed");

  // An unsafe identifier never reaches a child process.
  mpi::discovery::ProviderOptions opts;
  const auto refused = mpi::ios::probe_reachability("--device", opts);
  MPI_CHECK(refused.state == Reachability::kProbeFailed);
  MPI_CHECK_MSG(refused.detail.find("refusing") != std::string::npos,
                "and is refused rather than run");
}

MPI_TEST(a_booted_simulator_that_does_not_answer_is_its_own_failure, {}) {
  // `simctl list` reports a *state*, and a state is not an answer:
  // CoreSimulator can hold a simulator in `Booted` while its runtime is
  // wedged. That read as usable and then failed on the first operation, with
  // an error about the operation rather than about the simulator -- the same
  // shape as trusting a physical device's cached `tunnelState`, left unfixed
  // for simulators.
  //
  // Discovery now asks a booted simulator to run a trivial process and marks
  // it kUnknown when it will not: neither "shut down" nor "usable", because
  // what to do about it differs from both.
  mpi::discovery::ProviderOptions opts;

  mpi::model::DeviceRef wedged;
  wedged.platform = mpi::model::Platform::kIos;
  wedged.form = mpi::model::DeviceForm::kSimulator;
  wedged.device_id = "456FA0D8-48C1-4BEC-B087-50E8A046EA5D";
  wedged.trust = mpi::model::TrustState::kUnknown;
  MPI_CHECK_MSG(!wedged.usable_for_capture(),
                "an unknown-state simulator is not offered for capture");

  const auto lines =
      mpi::ios::explain_unusable_device(wedged, opts, /*probe=*/false);
  MPI_CHECK(!lines.empty());
  bool says_booted = false, says_reboot = false, says_boot_it = false;
  for (const auto& l : lines) {
    if (l.find("reports Booted") != std::string::npos) says_booted = true;
    if (l.find("shutdown") != std::string::npos) says_reboot = true;
    if (l.find("it is not booted") != std::string::npos) says_boot_it = true;
  }
  MPI_CHECK_MSG(says_booted,
                "the explanation names the contradiction: booted, and not "
                "answering");
  MPI_CHECK_MSG(says_reboot, "and says to shut it down and boot it again");
  MPI_CHECK_MSG(!says_boot_it,
                "and never tells someone to boot a simulator that is already "
                "booted, which is what the single simulator branch did");

  // A genuinely shut-down simulator still gets the simple answer.
  mpi::model::DeviceRef down = wedged;
  down.trust = mpi::model::TrustState::kOffline;
  const auto simple =
      mpi::ios::explain_unusable_device(down, opts, /*probe=*/false);
  MPI_CHECK(simple.size() == 1);
  MPI_CHECK_MSG(simple.front().find("not booted") != std::string::npos,
                "a shut-down simulator is told to boot");
  MPI_CHECK_MSG(simple.front().find("liveness") == std::string::npos,
                "and is not given the wedged-runtime advice");
}

MPI_TEST(a_device_that_vanishes_mid_session_is_named_as_that, {}) {
  // The commonest real iOS connection event: the phone is authorized when
  // discovery runs and unplugged before apps are enumerated. It produced
  // Apple's raw text, even though the meaning of error 1011 was already
  // decoded for the reachability probe and simply not applied here.
  //
  // The exact string devicectl prints, from a real run against an absent
  // device.
  const std::string raw =
      "ERROR: CoreDeviceService was unable to locate a device matching the "
      "requested device identifier. (DeviceIdentifier: ecid_2666084064886814) "
      "(com.apple.dt.CoreDeviceError error 1011 (0x3F3))";
  const std::string described = mpi::ios::describe_devicectl_failure(raw);
  MPI_CHECK_MSG(described.find("no longer there") != std::string::npos,
                "it is named as a device that went away");
  MPI_CHECK_MSG(described.find("unplugging") != std::string::npos,
                "in terms of what actually happened");
  MPI_CHECK_MSG(described.find("Re-run discovery") != std::string::npos,
                "with what to do about it");
  MPI_CHECK_MSG(described.find("1011") != std::string::npos,
                "and the tool's own words are kept, because a tool's output "
                "is evidence and paraphrasing it away loses it");

  // 1000 is a different cause and must not be reported as a disconnect: it
  // means the identifier is not a CoreDevice device at all.
  const std::string not_a_device =
      "ERROR: The specified device was not found. (Name: 456FA0D8) "
      "(com.apple.dt.CoreDeviceError error 1000 (0x3E8))";
  const std::string other = mpi::ios::describe_devicectl_failure(not_a_device);
  MPI_CHECK_MSG(other.find("does not recognise") != std::string::npos,
                "an unrecognised identifier is its own answer");
  MPI_CHECK_MSG(other.find("no longer there") == std::string::npos,
                "and is not reported as a device that disconnected");
  MPI_CHECK_MSG(other.find("simulator UDID") != std::string::npos,
                "naming the most likely cause");

  // An unrecognised failure gets no invented explanation.
  const std::string odd = "ERROR: something nobody has seen before";
  const std::string passthrough = mpi::ios::describe_devicectl_failure(odd);
  MPI_CHECK_MSG(passthrough.find("something nobody has seen") != std::string::npos,
                "the raw text survives");
  MPI_CHECK_MSG(passthrough.find("Re-run discovery") == std::string::npos,
                "and no cause is invented for it");

  MPI_CHECK_MSG(mpi::ios::describe_devicectl_failure("").empty(),
                "an empty failure stays empty rather than gaining a story");
}

MPI_TEST(process_attribution_matches_real_container_paths, {}) {
  using mpi::ios::executable_path_names_bundle;
  const std::string bid = "io.pizzahut.hutbot.debug";

  // The real shape, from `simctl get_app_container` on this machine. The
  // bundle id is a segment *prefix* here, not a segment -- the old rule
  // required `"/" + bundle_id + "/"` and therefore matched nothing, so every
  // app reported not_running with no processes while actually running.
  const std::string real =
      "/private/var/containers/Bundle/Application/"
      "A303B644-4AD8-49C3-A0F6-872F48BA45EE/"
      "io.pizzahut.hutbot.debug-1789449967523.app/HutBot";
  MPI_CHECK_MSG(executable_path_names_bundle(real, bid),
                "a real container path is matched");

  // A file:// URL, which is how devicectl reports it.
  MPI_CHECK(executable_path_names_bundle("file://" + real, bid));

  // The plain form, and the bundle id as the final segment.
  MPI_CHECK(executable_path_names_bundle("/x/" + bid + "/Exec", bid));
  MPI_CHECK(executable_path_names_bundle("/x/" + bid, bid));
  MPI_CHECK(executable_path_names_bundle("/x/" + bid + ".app/Exec", bid));

  // A physical device's path names the product, not the bundle -- so this is
  // correctly NOT a match, and the caller must report unknown rather than
  // not_running for it.
  MPI_CHECK_MSG(!executable_path_names_bundle(
                    "/private/var/containers/Bundle/Application/"
                    "A303B644-4AD8-49C3-A0F6-872F48BA45EE/HutBot.app/HutBot",
                    bid),
                "a device-shaped path does not name the bundle id, and "
                "pretending otherwise would attribute a process on no "
                "evidence");

  // A different bundle id that merely starts with this one must not match,
  // or one app's processes would be attributed to another.
  MPI_CHECK(!executable_path_names_bundle("/x/" + bid + "extra/Exec", bid));
  MPI_CHECK(!executable_path_names_bundle("/x/" + bid + "2.app/Exec", bid));
  // Nor one that merely ends with it.
  MPI_CHECK(!executable_path_names_bundle("/x/com.other." + bid + "/Exec", bid));

  MPI_CHECK(!executable_path_names_bundle("", bid));
  MPI_CHECK(!executable_path_names_bundle(real, ""));
}
