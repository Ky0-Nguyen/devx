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
