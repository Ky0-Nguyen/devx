// Android live capture over Android Platform Tools.
//
// Deliberately built on the *text* interfaces rather than on Perfetto's
// protobuf output. Two reasons, both recorded in ADR-0006:
//
//  * Perfetto writes protobuf. Consuming it needs a protobuf runtime, which
//    would end the empty third-party licence inventory that ADR-0002 exists to
//    protect, for data these three sources already provide.
//  * `dumpsys gfxinfo <pkg> framestats` is the canonical Android frame-timing
//    source and reports a *provider-supplied deadline* per frame, which is
//    exactly what DET-01 needs and strictly better than a derived one.
//
// Sources, each independently probed and independently able to fail:
//
//  | Source                          | Yields                                |
//  |---------------------------------|---------------------------------------|
//  | dumpsys gfxinfo PKG framestats  | FrameRecord with a real deadline       |
//  | simpleperf record + report-sample | CpuSample with symbolised callchains |
//  | dumpsys meminfo PKG             | CounterSeries per memory family        |
//
// simpleperf is invoked with `--app`, which routes through run-as and so works
// only on a debuggable or profileable package. That is Android's own model and
// this adapter does not attempt to work around it: on a non-profileable target
// the CPU source reports `permission_required` with the setup that would fix
// it.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/session/collector.hpp"

namespace mpi::android {

// One row of the framestats CSV. Field names match the CSV header so the
// mapping to the normalized model stays auditable against real output.
struct FrameStatsRow {
  std::int64_t flags = 0;
  std::int64_t intended_vsync = 0;
  std::int64_t vsync = 0;
  std::int64_t frame_deadline = 0;
  std::int64_t frame_completed = 0;
  std::int64_t gpu_completed = 0;
  std::int64_t display_present_time = 0;
  std::int64_t frame_interval = 0;
  std::int64_t draw_start = 0;
};

// Parses the `---PROFILEDATA---` blocks of `dumpsys gfxinfo PKG framestats`.
// Reads the column order from the header rather than assuming it, because the
// column set has changed across Android releases.
struct FrameStats {
  std::vector<FrameStatsRow> rows;
  std::vector<std::string> warnings;
  // Distinct FrameInterval values observed, which give the refresh rate
  // without having to ask for it separately.
  std::vector<std::int64_t> observed_frame_intervals;
};
FrameStats parse_gfxinfo_framestats(const std::string& text);

// Parses `simpleperf report-sample --show-callchain`.
struct SimpleperfSamples {
  struct Sample {
    std::int64_t time_ns = 0;
    std::int64_t event_count = 0;  // sample weight
    std::int32_t thread_id = 0;
    std::string thread_name;
    // Outermost first, after reversing simpleperf's leaf-first order.
    std::vector<std::string> frames;
  };
  std::vector<Sample> samples;
  // From the meta_info block: these are real build facts read off the device.
  std::string event_type;
  std::string app_package_name;
  std::string app_type;       // "debuggable" / "profileable" / ...
  std::string build_type;     // "user" / "userdebug" / "eng"
  std::optional<int> sdk_version;
  std::vector<std::string> warnings;
};
SimpleperfSamples parse_simpleperf_report_sample(const std::string& text);

// Parses `dumpsys meminfo PKG`. Only the families the specification requires
// to stay distinct are extracted; they are never summed together.
struct MemInfo {
  std::optional<double> pss_total_bytes;
  std::optional<double> rss_total_bytes;
  std::optional<double> private_dirty_bytes;
  std::optional<double> native_heap_rss_bytes;
  std::optional<double> dalvik_heap_rss_bytes;
  std::optional<std::int32_t> pid;
};
MemInfo parse_dumpsys_meminfo(const std::string& text);

class AdbCollector final : public session::Collector {
 public:
  explicit AdbCollector(std::string adb_path = "adb");

  std::string id() const override { return "android.adb.text-sources"; }
  model::Platform platform() const override { return model::Platform::kAndroid; }

  session::CaptureResult capture(
      const model::DeviceRef& device,
      const std::vector<model::ProcessInstance>& processes,
      const session::CaptureConfig& config,
      model::NormalizedTrace& out) override;

 private:
  std::string adb_path_;
  std::vector<std::string> shell_argv(const std::string& serial,
                                      const std::vector<std::string>& args) const;
};

}  // namespace mpi::android
