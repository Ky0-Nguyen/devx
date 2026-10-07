// The Android SDK without Android Studio: where it is, what is in it, what can
// be added to it, and adding it.
//
// Google publishes the SDK as XML manifests of packages at dl.google.com --
// the same manifests `sdkmanager` and Android Studio read. This reads them,
// downloads with the system's /usr/bin/curl, verifies the SHA-1 each manifest
// states, unpacks with /usr/bin/ditto, and writes the `package.xml` and license
// records `sdkmanager` would, so Android Studio sees what was installed as
// installed. Nothing here needs Java, Android Studio or a package manager.
//
// A license is never accepted on anyone's behalf: installing a package whose
// license has not been accepted is refused, and accept_license() is only ever
// called after the person has been shown the text and said yes.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/util/cancel.hpp"
#include "core/util/json.hpp"

namespace mpi::android {

/// Where the SDK is. An existing one is preferred -- `ANDROID_HOME`,
/// `ANDROID_SDK_ROOT`, then Android Studio's default -- and DevX's own
/// directory is the fallback, so a machine without Android Studio still gets
/// one.
struct SdkLocation {
  std::string root;
  /// "ANDROID_HOME", "ANDROID_SDK_ROOT", "android_studio_default", "devx".
  std::string source;
  bool exists = false;
};
SdkLocation locate_sdk();
std::string devx_sdk_root();

struct InstalledPackage {
  std::string path;      // "emulator", "system-images;android-35;google_apis_playstore;arm64-v8a"
  std::string revision;  // as the package states it
  std::string directory;
  std::string display_name;
};
/// Packages with a package.xml or source.properties under `root`: the
/// emulator, platform-tools, and every system image.
std::vector<InstalledPackage> installed_packages(const std::string& root);

struct SdkArchive {
  std::string url;  // absolute
  std::uint64_t size = 0;
  std::string sha1;
};

struct SdkPackage {
  std::string path;
  std::string display_name;
  std::string revision;
  std::string license_id;
  bool obsolete = false;
  // System images only.
  int api_level = 0;
  std::string codename;     // a preview's letter, empty for a release
  std::string tag_id;       // default | google_apis | google_apis_playstore | ...
  std::string tag_display;
  std::string abi;          // arm64-v8a | x86_64
  int extension_level = 0;
  SdkArchive archive;       // the one for this host
  /// The package's <remotePackage> element and the manifest's root start tag,
  /// verbatim, from which package.xml is written.
  std::string element_xml;
  std::string root_start_tag;
};

struct SdkCatalog {
  std::vector<SdkPackage> packages;
  std::map<std::string, std::string> licenses;  // id -> text
  std::vector<std::string> errors;              // a manifest that could not be read
  json::Value to_json(const std::vector<InstalledPackage>& installed) const;
};

/// This Mac's host identifiers as the manifests spell them.
struct Host {
  std::string os = "macosx";
  std::string arch;  // aarch64 | x64
  std::string abi;   // arm64-v8a | x86_64: the system image ABI that runs fast here
};
Host this_host();

/// Parses one manifest. Archives for other hosts are dropped, and so are
/// system images for an ABI this host cannot run with acceleration.
void parse_manifest(const std::string& xml, const std::string& base_url, const Host& host,
                    SdkCatalog& into);

/// Downloads the four manifests: the main repository (emulator,
/// platform-tools) and the default, Google APIs and Google Play system images.
SdkCatalog fetch_catalog(std::chrono::milliseconds timeout, const CancellationToken& cancel);

/// sha1(trimmed license text), the form sdkmanager records.
std::string license_hash(const std::string& text);
bool license_accepted(const std::string& root, const std::string& id, const std::string& text);
/// Records acceptance. Called only after a person accepted the text shown.
bool accept_license(const std::string& root, const std::string& id, const std::string& text,
                    std::string* error);

struct InstallProgress {
  std::string phase;  // downloading | verifying | unpacking | done
  std::uint64_t done = 0;
  std::uint64_t total = 0;
};

struct InstallResult {
  bool ok = false;
  std::string directory;
  std::string error;
};

InstallResult install_package(const SdkPackage& pkg, const std::string& root,
                              const std::string& license_text,
                              const std::function<void(const InstallProgress&)>& progress,
                              const CancellationToken& cancel);

/// "system-images;android-35;google_apis_playstore;arm64-v8a" ->
/// "<root>/system-images/android-35/google_apis_playstore/arm64-v8a".
std::string package_directory(const std::string& root, const std::string& path);

}  // namespace mpi::android
