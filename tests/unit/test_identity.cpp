#include <sstream>

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
