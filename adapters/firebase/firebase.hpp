// The Firebase connector for DevX Intelligence (see docs/intelligence.md).
//
// Firebase has no supported REST API for Crashlytics issues. The supported
// way out of Firebase is the BigQuery export, which both Crashlytics and
// Performance Monitoring offer. So this connector reads export files the user
// produced themselves (`bq extract --destination_format=NEWLINE_DELIMITED_JSON`
// or `bq query --format=json`) from a local directory: it never scrapes the
// Console and never touches the network.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/signals/connector.hpp"

namespace mpi::intelligence {

std::unique_ptr<signals::SignalConnector> make_firebase_connector();

namespace firebase {

/// Microseconds since the epoch from a timestamp as BigQuery exports emit it:
/// ISO 8601 ("2026-10-08T01:15:44.123Z", with an offset or none),
/// BigQuery's own "2026-10-08 01:15:44.123456 UTC", or an epoch number (or a
/// numeric string) in seconds, milliseconds, microseconds or nanoseconds,
/// told apart by magnitude. Nullopt for anything else -- a zone name other
/// than UTC is refused rather than read as UTC.
std::optional<std::int64_t> parse_timestamp_us(const json::Value& v);

/// The nearest-rank percentile of an ascending, non-empty sample set: the
/// value at rank ceil(p/100 * n). Always one of the samples, never an
/// interpolation, so the same samples always give the same answer.
double nearest_rank(const std::vector<double>& sorted, int p);

}  // namespace firebase

}  // namespace mpi::intelligence
