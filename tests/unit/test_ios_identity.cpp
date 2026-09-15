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
