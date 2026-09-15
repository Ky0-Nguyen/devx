#include <sstream>

#include "core/discovery/boot.hpp"
#include "core/discovery/discovery_service.hpp"
#include "core/model/capability.hpp"
#include "core/model/identity.hpp"
#include "tests/unit/test_framework.hpp"

using namespace mpi;
using namespace mpi::model;

namespace {
ApplicationKey key(Platform p, const char* device, const char* app,
                   std::optional<int> user = std::nullopt) {
  ApplicationKey k;
  k.platform = p;
  k.device_id = device;
  k.app_identifier = app;
  k.android_user_id = user;
  return k;
}
}  // namespace

MPI_TEST(same_identifier_on_two_platforms_stays_separate, {"A15", "B12"}) {
  const auto a = key(Platform::kAndroid, "dev1", "com.example.app");
  const auto i = key(Platform::kIos, "dev1", "com.example.app");
  MPI_CHECK(!(a == i));
  MPI_CHECK(a.canonical() != i.canonical());
}

MPI_TEST(same_identifier_on_two_devices_stays_separate, {"A15", "B12"}) {
  const auto a = key(Platform::kAndroid, "serialA", "com.example.app");
  const auto b = key(Platform::kAndroid, "serialB", "com.example.app");
  MPI_CHECK(!(a == b));
}

MPI_TEST(android_work_profile_is_a_distinct_target, {"B08"}) {
  const auto owner = key(Platform::kAndroid, "dev", "com.example.app", 0);
  const auto work = key(Platform::kAndroid, "dev", "com.example.app", 10);
  MPI_CHECK(!(owner == work));
  MPI_CHECK(work.canonical().find("user=10") != std::string::npos);
}

MPI_TEST(process_restart_is_a_new_instance, {"B03"}) {
  ProcessInstance a;
  a.app = key(Platform::kAndroid, "dev", "com.example.app");
  a.pid = 4242;
  a.process_start_time = "918273";
  ProcessInstance b = a;
  b.process_start_time = "918999";  // same pid, later start
  MPI_CHECK_MSG(a.canonical() != b.canonical(),
                "a restart with the same PID must be a different instance");
}

MPI_TEST(pid_reuse_does_not_merge_processes, {"B02"}) {
  ProcessInstance mine;
  mine.app = key(Platform::kAndroid, "dev", "com.example.app");
  mine.pid = 1234;
  mine.process_start_time = "500";
  ProcessInstance theirs;
  theirs.app = key(Platform::kAndroid, "dev", "com.other.app");
  theirs.pid = 1234;  // the OS reused the pid
  theirs.process_start_time = "9000";
  MPI_CHECK(mine.canonical() != theirs.canonical());
}

MPI_TEST(reboot_invalidates_process_identity, {"B04"}) {
  ProcessInstance before;
  before.app = key(Platform::kAndroid, "dev", "com.example.app");
  before.pid = 77;
  before.process_start_time = "100";
  before.boot_id = "boot-aaaa";
  ProcessInstance after = before;
  after.boot_id = "boot-bbbb";
  MPI_CHECK(before.canonical() != after.canonical());
}

MPI_TEST(ambiguous_ownership_is_excluded_from_app_totals, {"B06", "B07", "B10"}) {
  ProcessInstance p;
  p.ownership = OwnershipEvidence::kAmbiguous;
  MPI_CHECK_MSG(!p.counts_toward_app_totals(),
                "an ambiguous process must not contaminate app totals");
  p.ownership = OwnershipEvidence::kUnknown;
  MPI_CHECK(!p.counts_toward_app_totals());
  p.ownership = OwnershipEvidence::kUidAndProcessName;
  MPI_CHECK(p.counts_toward_app_totals());
  p.ownership = OwnershipEvidence::kProviderAttributed;
  MPI_CHECK(p.counts_toward_app_totals());
}

MPI_TEST(unknown_runtime_state_serializes_as_unknown, {"A11"}) {
  AppEntry e;
  e.key = key(Platform::kIos, "sim", "com.example.app");
  e.runtime_state = RuntimeState::kUnknown;
  const auto j = e.to_json();
  MPI_CHECK_EQ(j.find("runtime_state")->as_string(), std::string("unknown"));
  MPI_CHECK_MSG(j.find("runtime_state")->as_string() != "not_running",
                "unknown must never be rendered as not_running");
}

MPI_TEST(installed_is_null_when_never_observed, {"A18", "C18"}) {
  AppEntry e;
  e.installed_known = false;
  e.installed = false;
  // The provider never told us; the field must be null, not false.
  MPI_CHECK(e.to_json().find("installed")->is_null());
  e.installed_known = true;
  MPI_CHECK(e.to_json().find("installed")->is_bool());
}

MPI_TEST(empty_list_differs_from_enumeration_failure, {"A12"}) {
  DiscoverySnapshot empty;
  empty.enumeration_failed = false;
  DiscoverySnapshot failed;
  failed.enumeration_failed = true;
  MPI_CHECK_EQ(empty.to_json().find("enumeration_failed")->as_bool(), false);
  MPI_CHECK_EQ(failed.to_json().find("enumeration_failed")->as_bool(), true);
}

MPI_TEST(only_authorized_devices_are_usable, {"A02", "A03"}) {
  DeviceRef d;
  for (const auto t : {TrustState::kUnauthorized, TrustState::kUntrusted,
                       TrustState::kLocked, TrustState::kOffline,
                       TrustState::kUnknown}) {
    d.trust = t;
    MPI_CHECK_MSG(!d.usable_for_capture(),
                  std::string("must not be usable when ") + to_string(t));
  }
  d.trust = TrustState::kAuthorized;
  MPI_CHECK(d.usable_for_capture());
}

MPI_TEST(simulator_form_survives_serialization, {"C20", "J18"}) {
  DeviceRef d;
  d.form = DeviceForm::kSimulator;
  MPI_CHECK_EQ(d.to_json().find("form")->as_string(), std::string("simulator"));
  d.form = DeviceForm::kEmulator;
  MPI_CHECK_EQ(d.to_json().find("form")->as_string(), std::string("emulator"));
  d.form = DeviceForm::kPhysical;
  MPI_CHECK_EQ(d.to_json().find("form")->as_string(), std::string("physical"));
}


// --- waiting for a launched app's process (spec A22) -------------------------

namespace {

// A provider whose app takes a few polls to show up, which is what a real
// launch looks like: `am start -W` returns before the process is listable.
class LateProcessProvider final : public discovery::Provider {
 public:
  explicit LateProcessProvider(int appear_on_poll)
      : appear_on_poll_(appear_on_poll) {}

  model::Platform platform() const override { return model::Platform::kAndroid; }
  std::string name() const override { return "fake.late-process"; }
  void probe(model::CapabilityMatrix&, const discovery::ProviderOptions&) const override {}

  std::vector<model::DeviceRef> list_devices(const discovery::ProviderOptions&,
                                             std::vector<std::string>&) const override {
    model::DeviceRef d;
    d.platform = model::Platform::kAndroid;
    d.device_id = "fake-1";
    d.trust = model::TrustState::kAuthorized;
    d.form = model::DeviceForm::kEmulator;
    return {d};
  }

  std::vector<model::AppEntry> list_apps(const model::DeviceRef&,
                                         const discovery::ProviderOptions&,
                                         std::vector<std::string>&,
                                         bool&) const override {
    return {};
  }

  std::vector<model::ProcessInstance> resolve_processes(
      const model::DeviceRef&, const model::ApplicationKey& app,
      const discovery::ProviderOptions&,
      std::vector<std::string>&) const override {
    ++polls;
    if (polls < appear_on_poll_) return {};
    model::ProcessInstance p;
    p.app = app;
    p.pid = 4242;
    p.process_name = app.app_identifier;
    p.is_primary = true;
    p.ownership = model::OwnershipEvidence::kProviderAttributed;
    p.process_start_time = "12345";
    p.boot_id = "boot";
    return {p};
  }

  mutable int polls = 0;

 private:
  int appear_on_poll_;
};

model::ApplicationKey fake_key() {
  model::ApplicationKey k;
  k.platform = model::Platform::kAndroid;
  k.device_id = "fake-1";
  k.app_identifier = "com.example.launched";
  k.identifier_kind = model::IdentifierKind::kPackageName;
  return k;
}

model::DeviceRef fake_device() {
  model::DeviceRef d;
  d.platform = model::Platform::kAndroid;
  d.device_id = "fake-1";
  d.trust = model::TrustState::kAuthorized;
  d.form = model::DeviceForm::kEmulator;
  return d;
}

}  // namespace

MPI_TEST(a_launched_app_is_waited_for_until_its_process_appears, {"A22", "A21"}) {
  discovery::DiscoveryService svc;
  auto provider = std::make_shared<LateProcessProvider>(/*appear_on_poll=*/3);
  svc.add_provider(provider);

  discovery::ProviderOptions opts;
  const auto waited = svc.wait_for_app_process(
      fake_device(), fake_key(), std::chrono::milliseconds(2000),
      std::chrono::milliseconds(5), opts);

  MPI_CHECK(waited.appeared);
  MPI_CHECK_EQ(waited.processes.size(), std::size_t{1});
  MPI_CHECK(waited.polls >= 3);
  MPI_CHECK(!waited.cancelled);
}

MPI_TEST(a_wait_that_times_out_says_so_rather_than_reporting_no_processes,
         {"A22", "E13"}) {
  discovery::DiscoveryService svc;
  // Never appears.
  svc.add_provider(std::make_shared<LateProcessProvider>(/*appear_on_poll=*/100000));

  discovery::ProviderOptions opts;
  const auto waited = svc.wait_for_app_process(
      fake_device(), fake_key(), std::chrono::milliseconds(30),
      std::chrono::milliseconds(5), opts);

  MPI_CHECK(!waited.appeared);
  MPI_CHECK(waited.processes.empty());
  // The distinction that matters: a timeout is missing evidence, not a
  // finding that the app runs no processes.
  bool said_timeout = false;
  for (const auto& n : waited.notes) {
    if (n.find("this is a timeout, not evidence") != std::string::npos) {
      said_timeout = true;
    }
  }
  MPI_CHECK(said_timeout);
}

MPI_TEST(a_zero_budget_still_gets_one_look, {"A22"}) {
  discovery::DiscoveryService svc;
  svc.add_provider(std::make_shared<LateProcessProvider>(/*appear_on_poll=*/1));
  discovery::ProviderOptions opts;
  // An app that is already up must not need a waiting budget to be found.
  const auto waited = svc.wait_for_app_process(
      fake_device(), fake_key(), std::chrono::milliseconds(0),
      std::chrono::milliseconds(5), opts);
  MPI_CHECK(waited.appeared);
  MPI_CHECK_EQ(waited.polls, 1);
}

MPI_TEST(a_cancelled_wait_stops_and_reports_the_cancellation, {"A22", "J11"}) {
  discovery::DiscoveryService svc;
  svc.add_provider(std::make_shared<LateProcessProvider>(/*appear_on_poll=*/100000));
  discovery::ProviderOptions opts;
  CancellationSource source;
  opts.cancel = source.token();
  source.cancel();

  const auto waited = svc.wait_for_app_process(
      fake_device(), fake_key(), std::chrono::milliseconds(5000),
      std::chrono::milliseconds(5), opts);
  MPI_CHECK(waited.cancelled);
  MPI_CHECK(!waited.appeared);
  MPI_CHECK_EQ(waited.polls, 0);
}

// --- guarantees that are about this tool, not about a device ----------------
//
// Four of these were deferred with "needs a device with two such apps
// installed" or similar. That confuses confirmation with the requirement: the
// checklist asks whether *this tool* distinguishes, refuses or keeps things
// apart, and that is decided by its own logic. Hardware would confirm the
// behaviour; a test establishes it. Where a real capture adds something the
// logic cannot, the capability matrix says so.

MPI_TEST(two_apps_sharing_a_display_name_stay_distinct, {"A14"}) {
    // A label is what a person recognises and it is not unique: two vendors
    // ship "Camera", a work profile clones an app's label, a test build keeps
    // the shipping name. Collapsing them would make the picker select the
    // wrong target while looking right.
    model::DiscoverySnapshot snap;
    model::AppEntry a;
    a.key.device_id = "d1";
    a.key.platform = model::Platform::kAndroid;
    a.key.app_identifier = "com.vendor.camera";
    a.display_name = "Camera";
    a.runtime_state = model::RuntimeState::kRunning;
    model::AppEntry b = a;
    b.key.app_identifier = "com.other.camera";
    b.runtime_state = model::RuntimeState::kNotRunning;
    snap.apps.push_back(a);
    snap.apps.push_back(b);

    MPI_CHECK_EQ(snap.apps.size(), std::size_t{2});
    MPI_CHECK_MSG(!(snap.apps[0].key == snap.apps[1].key),
                  "the same label does not make the same key");
    MPI_CHECK(snap.apps[0].key.canonical() != snap.apps[1].key.canonical());
    // And each is addressable by identifier, which is what the picker selects
    // by -- the label is only ever shown.
    const model::AppEntry* found = nullptr;
    for (const auto& e : snap.apps) {
        if (e.key.app_identifier == "com.other.camera") found = &e;
    }
    MPI_CHECK(found != nullptr);
    if (found == nullptr) return;
    MPI_CHECK(found->runtime_state == model::RuntimeState::kNotRunning);
    // Serialization keeps both, so a session records which one was profiled.
    const std::string text = snap.to_json().dump();
    MPI_CHECK(text.find("com.vendor.camera") != std::string::npos);
    MPI_CHECK(text.find("com.other.camera") != std::string::npos);
}

MPI_TEST(profileable_is_independent_of_running_and_visible, {"B14"}) {
    // Three orthogonal facts that are easy to conflate: whether the app is
    // running, whether this provider could see it, and whether it can be
    // profiled. Inferring any from another is how "it's running, so we can
    // profile it" gets into a tool.
    struct Case {
        model::RuntimeState state;
        model::DiscoveryScope scope;
        model::ProfilingAvailability profiling;
    };
    const Case cases[] = {
        // Running and visible, but a release build: not profileable.
        {model::RuntimeState::kRunning, model::DiscoveryScope::kCompleteForProvider,
         model::ProfilingAvailability::kUnavailable},
        // Not running, yet profileable once started.
        {model::RuntimeState::kNotRunning, model::DiscoveryScope::kCompleteForProvider,
         model::ProfilingAvailability::kAvailable},
        // Seen only partially, and profileable anyway.
        {model::RuntimeState::kUnknown, model::DiscoveryScope::kPartial,
         model::ProfilingAvailability::kAvailable},
        // Running, partially visible, and nothing known about profiling.
        {model::RuntimeState::kRunning, model::DiscoveryScope::kPartial,
         model::ProfilingAvailability::kUnknown},
    };
    for (const auto& c : cases) {
        model::AppEntry e;
        e.key.device_id = "d1";
        e.key.platform = model::Platform::kAndroid;
        e.key.app_identifier = "com.example.app";
        e.runtime_state = c.state;
        e.visibility_scope = c.scope;
        e.profiling = c.profiling;
        const auto round_trip = e.to_json().dump();
        // Each field survives independently: none is derived from another, so
        // every combination is representable and none is normalised away.
        MPI_CHECK_MSG(round_trip.find(model::to_string(c.state)) != std::string::npos,
                      "runtime state survives");
        MPI_CHECK_MSG(round_trip.find(model::to_string(c.profiling)) != std::string::npos,
                      "profiling availability survives independently");
        MPI_CHECK_MSG(round_trip.find(model::to_string(c.scope)) != std::string::npos,
                      "visibility scope survives independently");
    }
}

MPI_TEST(there_is_no_foreground_state_to_guess_at, {"A09"}) {
    // The spec's rule is that suspended and foreground states are never
    // guessed. The strongest form of that guarantee is structural: this model
    // cannot express "foreground" at all, so no provider can claim it and no
    // view can render it. `running` means a process exists -- a comment in
    // the UI says so too -- and `suspended` is only ever reported when a
    // provider observed it.
    for (const auto s : {model::RuntimeState::kRunning,
                         model::RuntimeState::kNotRunning,
                         model::RuntimeState::kSuspended,
                         model::RuntimeState::kUnknown}) {
        const std::string name = model::to_string(s);
        MPI_CHECK_MSG(name.find("foreground") == std::string::npos,
                      "no state is called foreground: got " + name);
        MPI_CHECK_MSG(name.find("background") == std::string::npos,
                      "nor background: got " + name);
    }
    // And a provider that did not observe the state leaves it unknown, which
    // is not `kNotRunning` -- the failure that would read as "the app is not
    // running" when nobody looked.
    model::AppEntry e;
    e.key.device_id = "d1";
    e.key.platform = model::Platform::kAndroid;
    e.key.app_identifier = "com.example.app";
    MPI_CHECK(e.runtime_state == model::RuntimeState::kUnknown);
    // Asserted through the document rather than on its exact spacing: the
    // claim is that the field serializes as unknown, not how the writer
    // indents.
    const auto doc = e.to_json();
    const json::Value* field = doc.find("runtime_state");
    MPI_CHECK(field != nullptr);
    if (field != nullptr) {
        MPI_CHECK_EQ(field->as_string(), std::string("unknown"));
    }
    // A suspended app is running in the sense that matters for identity: it
    // has a process. It is a distinct state rather than a flavour of either.
    MPI_CHECK(std::string(model::to_string(model::RuntimeState::kSuspended)) ==
              "suspended");
}

namespace {

/// A provider whose app is there on the first look and gone on the next.
///
/// The A17 scenario exactly: a picker lists a running app, the operator
/// presses Record, and in between the app exits. Verified end to end on
/// emulator-5554 -- listed running, force-stopped, and `mpi record` refused
/// with "no live process ... could be resolved" -- and pinned here so the
/// refusal cannot regress without a device attached.
class VanishingAppProvider final : public discovery::Provider {
 public:
  model::Platform platform() const override { return model::Platform::kAndroid; }
  std::string name() const override { return "fake.vanishing"; }
  void probe(model::CapabilityMatrix&, const discovery::ProviderOptions&) const override {}

  std::vector<model::DeviceRef> list_devices(const discovery::ProviderOptions&,
                                             std::vector<std::string>&) const override {
    model::DeviceRef d;
    d.platform = model::Platform::kAndroid;
    d.device_id = "fake-1";
    d.trust = model::TrustState::kAuthorized;
    d.form = model::DeviceForm::kEmulator;
    return {d};
  }

  std::vector<model::AppEntry> list_apps(const model::DeviceRef&,
                                         const discovery::ProviderOptions&,
                                         std::vector<std::string>&,
                                         bool&) const override {
    return {};
  }

  std::vector<model::ProcessInstance> resolve_processes(
      const model::DeviceRef&, const model::ApplicationKey& app,
      const discovery::ProviderOptions&,
      std::vector<std::string>&) const override {
    if (gone) return {};
    model::ProcessInstance p;
    p.pid = 4242;
    p.app = app;
    p.is_primary = true;
    p.process_start_time = "1856103";
    p.boot_id = "boot-1";
    return {p};
  }

  mutable bool gone = false;
};

}  // namespace

MPI_TEST(an_app_that_exits_before_record_is_caught_by_revalidation,
         {"A17", "A16"}) {
  discovery::DiscoveryService svc;
  auto provider = std::make_shared<VanishingAppProvider>();
  svc.add_provider(provider);
  discovery::ProviderOptions opts;

  // The listing: one process, and this is what a picker would have shown.
  const auto first = svc.revalidate(fake_device(), fake_key(), {}, opts);
  MPI_CHECK(first.app_still_present);
  MPI_CHECK_EQ(first.processes.size(), std::size_t{1});

  // The app exits. Revalidation before recording must notice, and must not
  // hand back the previous process set -- capturing against a dead pid would
  // produce a session attributed to a process that no longer existed.
  provider->gone = true;
  const auto second =
      svc.revalidate(fake_device(), fake_key(), first.processes, opts);
  MPI_CHECK_MSG(!second.app_still_present,
                "the app is reported absent rather than assumed present");
  MPI_CHECK(second.processes.empty());
  MPI_CHECK_MSG(second.process_set_changed,
                "the change is flagged, not just the absence");
  bool says_so = false;
  for (const auto& n : second.notes) {
    if (n.find("no longer running") != std::string::npos ||
        n.find("no process could be attributed") != std::string::npos) {
      says_so = true;
    }
  }
  MPI_CHECK_MSG(says_so,
                "and the reason is stated: an empty process set with no note "
                "reads like an app that has no processes");
}

MPI_TEST(a_restarted_app_is_not_silently_retargeted, {"A17", "B03"}) {
  // The other half of A17, and the more dangerous one: the app is still
  // "there" but it is a different process. Carrying the old pid forward would
  // attribute the new process's work to the old instance.
  discovery::DiscoveryService svc;
  auto provider = std::make_shared<VanishingAppProvider>();
  svc.add_provider(provider);
  discovery::ProviderOptions opts;

  model::ProcessInstance old_instance;
  old_instance.pid = 4242;
  old_instance.app = fake_key();
  old_instance.is_primary = true;
  // Same pid, different start time: a restart, and pid reuse at that.
  old_instance.process_start_time = "1000000";
  old_instance.boot_id = "boot-1";

  const auto again =
      svc.revalidate(fake_device(), fake_key(), {old_instance}, opts);
  MPI_CHECK(again.app_still_present);
  MPI_CHECK_MSG(again.process_set_changed,
                "a process with the same pid but a different start time is a "
                "different instance, and the change is reported");
}

// --- starting a simulator or emulator ---------------------------------------

MPI_TEST(a_boot_refuses_an_unusable_identifier, {"A02", "J05"}) {
    // The identifier reaches a command line, so it goes through the same
    // argument check as every other external input. An AVD called `--help`
    // would otherwise be handed to the emulator as a flag.
    discovery::BootOptions opts;
    discovery::BootTarget t;
    t.platform = model::Platform::kAndroid;

    t.identifier = "";
    auto r = discovery::boot(t, opts);
    MPI_CHECK(!r.started);
    MPI_CHECK(r.error.find("no target identifier") != std::string::npos);

    t.identifier = "--help";
    r = discovery::boot(t, opts);
    MPI_CHECK(!r.started);
    MPI_CHECK_MSG(r.error.find("read as a command-line option") !=
                      std::string::npos,
                  "an option-like identifier is refused: " + r.error);
    MPI_CHECK(r.device_id.empty());
}

MPI_TEST(a_boot_on_an_unknown_platform_is_refused_not_attempted, {"A02"}) {
    discovery::BootOptions opts;
    discovery::BootTarget t;
    t.platform = model::Platform::kUnknown;
    t.identifier = "something";
    const auto r = discovery::boot(t, opts);
    MPI_CHECK(!r.started);
    MPI_CHECK(r.error.find("no way to boot") != std::string::npos);
}

MPI_TEST(a_boot_result_keeps_started_and_ready_apart, {"A02", "H01"}) {
    // The distinction the whole command turns on. A process that launched and
    // a device that answers are two facts, and collapsing them would report a
    // half-booted device as ready -- which is measured on this machine: a
    // second AVD started and never reported `sys.boot_completed` inside
    // 150 s, with adb seeing it as `offline` throughout.
    discovery::BootResult r;
    r.started = true;
    r.ready = false;
    r.waited = std::chrono::milliseconds(150655);
    r.notes.push_back("the emulator started and emulator-5556 appeared, but "
                      "it never reported sys.boot_completed");
    const auto doc = r.to_json();
    const json::Value* started = doc.find("started");
    const json::Value* ready = doc.find("ready");
    const json::Value* id = doc.find("device_id");
    MPI_CHECK(started != nullptr && started->as_bool());
    MPI_CHECK(ready != nullptr && !ready->as_bool());
    // Null, not "": an unconfirmed device id is not an empty one, and a
    // caller that treats "" as a device id would try to record against it.
    MPI_CHECK_MSG(id != nullptr && id->is_null(),
                  "an unconfirmed device id serializes as null");

    // And the ready case carries the id that was observed.
    r.ready = true;
    r.device_id = "emulator-5556";
    const auto ok = r.to_json();
    const json::Value* id2 = ok.find("device_id");
    MPI_CHECK(id2 != nullptr && id2->is_string());
    MPI_CHECK_EQ(id2->as_string(), std::string("emulator-5556"));
}

MPI_TEST(a_boot_target_is_not_a_device, {"A02", "A15"}) {
    // An AVD name is not a device id: the device id exists only once it runs,
    // and a second emulator lands on 5556 rather than the 5554 everyone
    // assumes. So a target carries an `identifier` and no device id at all.
    discovery::BootTarget t;
    t.platform = model::Platform::kAndroid;
    t.identifier = "Pixel_9_Pro";
    t.display_name = "Pixel_9_Pro";
    const auto doc = t.to_json();
    MPI_CHECK(doc.find("identifier") != nullptr);
    MPI_CHECK_MSG(doc.find("device_id") == nullptr,
                  "a bootable target has no device id to report");
    // An AVD's API level is not in `emulator -list-avds` output, so the field
    // stays empty rather than being filled with a guess.
    const json::Value* os = doc.find("os_version");
    MPI_CHECK(os != nullptr && os->as_string().empty());
}

MPI_TEST(an_empty_boot_list_with_a_provider_error_is_not_nothing, {"A12"}) {
    // The same rule as device discovery: a provider that could not be asked
    // is reported, because an empty list plus a silent failure reads as "this
    // machine has no simulators".
    discovery::BootTargets t;
    t.errors.push_back("`emulator -list-avds` failed: not found");
    const auto doc = t.to_json();
    const json::Value* arr = doc.find("boot_targets");
    const json::Value* errs = doc.find("errors");
    MPI_CHECK(arr != nullptr && arr->items().empty());
    MPI_CHECK(errs != nullptr && errs->items().size() == 1);
}
