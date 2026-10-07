#include "adapters/android/emulator/sdk_repository.hpp"

#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "core/util/process.hpp"
#include "core/util/xml.hpp"

namespace mpi::android {
namespace {

namespace fs = std::filesystem;

const char* kRepoBase = "https://dl.google.com/android/repository/";

bool is_dir(const std::string& p) {
  struct stat st {};
  return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string home() {
  const char* h = std::getenv("HOME");
  return h != nullptr ? h : "";
}

std::string read_file(const std::string& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string trim(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
  return s.substr(a, b - a);
}

std::string local_name(const std::string& n) {
  const auto c = n.find(':');
  return c == std::string::npos ? n : n.substr(c + 1);
}

std::string xml_escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

// One child element's text, by local-name path under the current element.
struct PackageFields {
  std::map<std::string, std::string> text;  // "type-details/api-level" -> "35"
  std::map<std::string, std::string> attrs; // "uses-license@ref", "channelRef@ref"
  std::vector<std::map<std::string, std::string>> archives;
};

PackageFields read_package(const std::string& element) {
  PackageFields f;
  xml::Parser p(element.data(), element.size());
  std::vector<std::string> path;
  std::map<std::string, std::string>* archive = nullptr;
  while (p.next()) {
    if (p.kind() == xml::NodeKind::kStartElement) {
      const std::string n = local_name(p.name());
      path.push_back(n);
      std::string key;
      for (std::size_t i = 1; i < path.size(); i++) key += (i > 1 ? "/" : "") + path[i];
      for (const auto& a : p.attributes()) f.attrs[key + "@" + local_name(a.name)] = a.value;
      if (n == "archive") {
        f.archives.emplace_back();
        archive = &f.archives.back();
      }
      if (p.self_closing()) path.pop_back();
    } else if (p.kind() == xml::NodeKind::kText) {
      std::string key;
      for (std::size_t i = 1; i < path.size(); i++) key += (i > 1 ? "/" : "") + path[i];
      const std::string t = trim(p.text());
      if (t.empty()) continue;
      if (archive != nullptr && key.rfind("archives/archive/", 0) == 0) {
        (*archive)[key.substr(17)] = t;
      } else {
        f.text[key] = t;
      }
    } else if (p.kind() == xml::NodeKind::kEndElement) {
      if (!path.empty()) {
        if (path.back() == "archive") archive = nullptr;
        path.pop_back();
      }
    }
  }
  return f;
}

std::string revision_of(const PackageFields& f) {
  std::string r;
  for (const char* part : {"revision/major", "revision/minor", "revision/micro"}) {
    const auto it = f.text.find(part);
    if (it == f.text.end()) break;
    r += (r.empty() ? "" : ".") + it->second;
  }
  const auto pre = f.text.find("revision/preview");
  if (pre != f.text.end()) r += " rc" + pre->second;
  return r;
}

bool run_ok(const std::vector<std::string>& argv, std::chrono::milliseconds timeout,
            const CancellationToken& cancel, std::string* out, std::string* err) {
  proc::Options po;
  po.timeout = timeout;
  po.cancel = cancel;
  const auto r = proc::run(argv, po);
  if (out != nullptr) *out = r.out;
  if (!r.ok() && err != nullptr) {
    *err = r.spawned ? (r.timed_out ? argv[0] + " timed out"
                                    : (r.cancelled ? std::string("cancelled") : trim(r.err)))
                     : r.spawn_error;
  }
  return r.ok();
}

}  // namespace

std::string devx_sdk_root() {
  return home() + "/Library/Application Support/DevX/android-sdk";
}

SdkLocation locate_sdk() {
  for (const char* var : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
    const char* v = std::getenv(var);
    if (v != nullptr && *v != '\0' && is_dir(v)) return {v, var, true};
  }
  const std::string studio = home() + "/Library/Android/sdk";
  if (is_dir(studio)) return {studio, "android_studio_default", true};
  const std::string own = devx_sdk_root();
  return {own, "devx", is_dir(own)};
}

Host this_host() {
  Host h;
  struct utsname u {};
  ::uname(&u);
  const std::string m = u.machine;
  const bool arm = m == "arm64" || m == "aarch64";
  h.arch = arm ? "aarch64" : "x64";
  h.abi = arm ? "arm64-v8a" : "x86_64";
  return h;
}

std::string package_directory(const std::string& root, const std::string& path) {
  std::string rel = path;
  std::replace(rel.begin(), rel.end(), ';', '/');
  return root + "/" + rel;
}

std::vector<InstalledPackage> installed_packages(const std::string& root) {
  std::vector<InstalledPackage> out;
  auto probe = [&](const std::string& path, const std::string& dir) {
    if (!is_dir(dir)) return;
    InstalledPackage p;
    p.path = path;
    p.directory = dir;
    const std::string props = read_file(dir + "/source.properties");
    std::istringstream in(props);
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("Pkg.Revision=", 0) == 0) p.revision = trim(line.substr(13));
      if (line.rfind("Pkg.Desc=", 0) == 0) p.display_name = trim(line.substr(9));
    }
    const std::string pkg = read_file(dir + "/package.xml");
    if (!pkg.empty()) {
      const auto a = pkg.find("<display-name>");
      const auto b = pkg.find("</display-name>");
      if (a != std::string::npos && b != std::string::npos && b > a) {
        p.display_name = pkg.substr(a + 14, b - a - 14);
      }
    }
    if (p.revision.empty() && pkg.empty()) return;  // a directory, not a package
    out.push_back(std::move(p));
  };
  probe("emulator", root + "/emulator");
  probe("platform-tools", root + "/platform-tools");
  const std::string sys = root + "/system-images";
  std::error_code ec;
  if (is_dir(sys)) {
    for (const auto& api : fs::directory_iterator(sys, ec)) {
      if (!api.is_directory()) continue;
      for (const auto& tag : fs::directory_iterator(api.path(), ec)) {
        if (!tag.is_directory()) continue;
        for (const auto& abi : fs::directory_iterator(tag.path(), ec)) {
          if (!abi.is_directory()) continue;
          probe("system-images;" + api.path().filename().string() + ";" +
                    tag.path().filename().string() + ";" + abi.path().filename().string(),
                abi.path().string());
        }
      }
    }
  }
  return out;
}

void parse_manifest(const std::string& xml, const std::string& base_url, const Host& host,
                    SdkCatalog& into) {
  // The root start tag, kept for package.xml's namespace declarations.
  std::string root_tag;
  {
    std::size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
      if (xml.compare(pos, 2, "<?") == 0 || xml.compare(pos, 4, "<!--") == 0) {
        pos++;
        continue;
      }
      root_tag = xml.substr(pos, xml.find('>', pos) - pos + 1);
      break;
    }
  }
  // Licenses.
  {
    xml::Parser p(xml.data(), xml.size());
    std::string id;
    std::string text;
    while (p.next()) {
      if (p.kind() == xml::NodeKind::kStartElement && local_name(p.name()) == "license") {
        id = p.attribute("id").value_or("");
        text.clear();
      } else if (p.kind() == xml::NodeKind::kText && !id.empty()) {
        text += p.text();
      } else if (p.kind() == xml::NodeKind::kEndElement && local_name(p.name()) == "license") {
        if (!id.empty()) into.licenses[id] = text;
        id.clear();
      }
    }
  }
  // Packages: each <remotePackage> element, verbatim, then read on its own.
  const std::string open_tag = "<remotePackage ";
  const std::string close_tag = "</remotePackage>";
  std::size_t pos = 0;
  while ((pos = xml.find(open_tag, pos)) != std::string::npos) {
    const auto end = xml.find(close_tag, pos);
    if (end == std::string::npos) break;
    const std::string element = xml.substr(pos, end + close_tag.size() - pos);
    pos = end;
    const PackageFields f = read_package(element);
    if (f.attrs.count("channelRef@ref") != 0 && f.attrs.at("channelRef@ref") != "channel-0") {
      continue;  // stable only, as Android Studio shows by default
    }
    SdkPackage pkg;
    {
      xml::Parser hp(element.data(), element.size());
      hp.next();
      pkg.path = hp.attribute("path").value_or("");
      pkg.obsolete = hp.attribute("obsolete").value_or("false") == "true";
    }
    if (pkg.path.empty()) continue;
    const bool sysimg = pkg.path.rfind("system-images;", 0) == 0;
    if (!sysimg && pkg.path != "emulator" && pkg.path != "platform-tools") continue;
    auto text = [&](const char* k) {
      const auto it = f.text.find(k);
      return it == f.text.end() ? std::string() : it->second;
    };
    pkg.display_name = text("display-name");
    pkg.revision = revision_of(f);
    if (f.attrs.count("uses-license@ref") != 0) pkg.license_id = f.attrs.at("uses-license@ref");
    if (sysimg) {
      pkg.api_level = std::atoi(text("type-details/api-level").c_str());
      pkg.codename = text("type-details/codename");
      pkg.extension_level = std::atoi(text("type-details/extension-level").c_str());
      pkg.tag_id = text("type-details/tag/id");
      pkg.tag_display = text("type-details/tag/display");
      pkg.abi = text("type-details/abi");
      if (pkg.abi.empty()) pkg.abi = text("type-details/abis/abi");
      if (pkg.abi != host.abi) continue;  // an image this Mac cannot run accelerated
    }
    bool found = false;
    for (const auto& a : f.archives) {
      const auto os = a.find("host-os");
      const auto arch = a.find("host-arch");
      if (os != a.end() && os->second != host.os) continue;
      if (arch != a.end() && arch->second != host.arch) continue;
      const auto url = a.find("complete/url");
      if (url == a.end()) continue;
      pkg.archive.url = url->second.rfind("https://", 0) == 0 ? url->second : base_url + url->second;
      pkg.archive.size = std::strtoull(a.count("complete/size") ? a.at("complete/size").c_str() : "0",
                                       nullptr, 10);
      pkg.archive.sha1 = a.count("complete/checksum") ? a.at("complete/checksum") : "";
      found = true;
      break;
    }
    if (!found) continue;
    pkg.element_xml = element;
    pkg.root_start_tag = root_tag;
    // A later stable entry for the same path supersedes an earlier one.
    auto existing = std::find_if(into.packages.begin(), into.packages.end(),
                                 [&](const SdkPackage& p) { return p.path == pkg.path; });
    if (existing != into.packages.end()) {
      *existing = std::move(pkg);
    } else {
      into.packages.push_back(std::move(pkg));
    }
  }
}

SdkCatalog fetch_catalog(std::chrono::milliseconds timeout, const CancellationToken& cancel) {
  SdkCatalog cat;
  const Host host = this_host();
  const std::vector<std::pair<std::string, std::string>> manifests = {
      {"repository2-3.xml", kRepoBase},
      {"sys-img/android/sys-img2-3.xml", std::string(kRepoBase) + "sys-img/android/"},
      {"sys-img/google_apis/sys-img2-3.xml", std::string(kRepoBase) + "sys-img/google_apis/"},
      {"sys-img/google_apis_playstore/sys-img2-3.xml",
       std::string(kRepoBase) + "sys-img/google_apis_playstore/"},
  };
  for (const auto& [rel, base] : manifests) {
    std::string body, err;
    if (!run_ok({"/usr/bin/curl", "-fsSL", "--max-time",
                 std::to_string(std::max<long long>(5, timeout.count() / 1000)),
                 std::string(kRepoBase) + rel},
                timeout, cancel, &body, &err)) {
      cat.errors.push_back(rel + ": " + (err.empty() ? "download failed" : err));
      continue;
    }
    parse_manifest(body, base, host, cat);
  }
  std::sort(cat.packages.begin(), cat.packages.end(), [](const SdkPackage& a, const SdkPackage& b) {
    if (a.api_level != b.api_level) return a.api_level > b.api_level;
    return a.path < b.path;
  });
  return cat;
}

json::Value SdkCatalog::to_json(const std::vector<InstalledPackage>& installed) const {
  json::Value o = json::Value::object();
  json::Value arr = json::Value::array();
  for (const auto& p : packages) {
    json::Value e = json::Value::object();
    e.set("path", json::Value::string(p.path));
    e.set("display_name", json::Value::string(p.display_name));
    e.set("revision", json::Value::string(p.revision));
    e.set("license_id", json::Value::string(p.license_id));
    e.set("size_bytes", json::Value::integer(static_cast<std::int64_t>(p.archive.size)));
    if (p.api_level > 0) {
      e.set("api_level", json::Value::integer(p.api_level));
      if (!p.codename.empty()) e.set("codename", json::Value::string(p.codename));
      if (p.extension_level > 0) e.set("extension_level", json::Value::integer(p.extension_level));
      e.set("tag", json::Value::string(p.tag_id));
      e.set("tag_display", json::Value::string(p.tag_display));
      e.set("abi", json::Value::string(p.abi));
    }
    std::string have;
    for (const auto& i : installed) {
      if (i.path == p.path) have = i.revision.empty() ? "installed" : i.revision;
    }
    e.set("installed_revision", have.empty() ? json::Value::null() : json::Value::string(have));
    arr.push_back(std::move(e));
  }
  o.set("packages", std::move(arr));
  json::Value lic = json::Value::array();
  for (const auto& [id, text] : licenses) {
    json::Value e = json::Value::object();
    e.set("id", json::Value::string(id));
    e.set("text", json::Value::string(trim(text)));
    lic.push_back(std::move(e));
  }
  o.set("licenses", std::move(lic));
  json::Value err = json::Value::array();
  for (const auto& e : errors) err.push_back(json::Value::string(e));
  o.set("errors", std::move(err));
  return o;
}

std::string license_hash(const std::string& text) {
  // SHA-1 through the system tool, fed on stdin: the text is the license, not
  // a secret, but it is long and has no place in argv.
  std::error_code tec;
  std::string pattern = (fs::temp_directory_path(tec) / "devx-license-XXXXXX").string();
  std::vector<char> tmpl(pattern.begin(), pattern.end());
  tmpl.push_back('\0');
  const int fd = ::mkstemp(tmpl.data());
  if (fd < 0) return "";
  const std::string t = trim(text);
  const ssize_t n = ::write(fd, t.data(), t.size());
  ::close(fd);
  std::string out, err;
  const bool ok = n == static_cast<ssize_t>(t.size()) &&
                  run_ok({"/usr/bin/shasum", "-a", "1", tmpl.data()}, std::chrono::seconds(10), {},
                         &out, &err);
  ::unlink(tmpl.data());
  return ok ? out.substr(0, 40) : "";
}

bool license_accepted(const std::string& root, const std::string& id, const std::string& text) {
  const std::string hash = license_hash(text);
  if (hash.empty()) return false;
  std::istringstream in(read_file(root + "/licenses/" + id));
  std::string line;
  while (std::getline(in, line)) {
    if (trim(line) == hash) return true;
  }
  return false;
}

bool accept_license(const std::string& root, const std::string& id, const std::string& text,
                    std::string* error) {
  if (id.empty() || id.find('/') != std::string::npos || id.find("..") != std::string::npos) {
    if (error != nullptr) *error = "not a license id: " + id;
    return false;
  }
  if (license_accepted(root, id, text)) return true;
  const std::string hash = license_hash(text);
  if (hash.empty()) {
    if (error != nullptr) *error = "could not hash the license text";
    return false;
  }
  std::error_code ec;
  fs::create_directories(root + "/licenses", ec);
  std::ofstream out(root + "/licenses/" + id, std::ios::app);
  // sdkmanager's format: each accepted version's hash on its own line.
  out << "\n" << hash;
  if (!out) {
    if (error != nullptr) *error = "could not write " + root + "/licenses/" + id;
    return false;
  }
  return true;
}

InstallResult install_package(const SdkPackage& pkg, const std::string& root,
                              const std::string& license_text,
                              const std::function<void(const InstallProgress&)>& progress,
                              const CancellationToken& cancel) {
  InstallResult res;
  auto say = [&](const char* phase, std::uint64_t done, std::uint64_t total) {
    if (progress) progress({phase, done, total});
  };
  if (!pkg.license_id.empty() && !license_accepted(root, pkg.license_id, license_text)) {
    res.error = "the license '" + pkg.license_id + "' has not been accepted";
    return res;
  }
  if (pkg.archive.url.rfind("https://dl.google.com/", 0) != 0) {
    res.error = "refusing a download from outside dl.google.com: " + pkg.archive.url;
    return res;
  }
  std::error_code ec;
  const std::string temp = root + "/.temp";
  fs::create_directories(temp, ec);
  const std::string name = pkg.archive.url.substr(pkg.archive.url.rfind('/') + 1);
  const std::string part = temp + "/" + name + ".part";

  // Download in a thread, so progress can be read off the file as it grows.
  // `-C -` resumes a .part an earlier attempt left.
  std::atomic<bool> finished{false};
  std::string dl_err;
  bool dl_ok = false;
  std::thread dl([&] {
    dl_ok = run_ok({"/usr/bin/curl", "-fsSL", "--retry", "3", "-C", "-", "-o", part, pkg.archive.url},
                   std::chrono::hours(6), cancel, nullptr, &dl_err);
    finished = true;
  });
  while (!finished) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    struct stat st {};
    say("downloading", ::stat(part.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_size) : 0,
        pkg.archive.size);
  }
  dl.join();
  if (!dl_ok) {
    res.error = cancel.cancelled() ? "cancelled; the partial download is kept and resumes next time"
                                   : "download failed: " + dl_err;
    return res;
  }

  say("verifying", 0, pkg.archive.size);
  std::string sum, err;
  if (!run_ok({"/usr/bin/shasum", "-a", "1", part}, std::chrono::minutes(5), cancel, &sum, &err) ||
      sum.substr(0, 40) != pkg.archive.sha1) {
    ::unlink(part.c_str());
    res.error = "the download does not match the SHA-1 Google publishes for it, so it was "
                "deleted: " + (err.empty() ? sum.substr(0, 40) + " != " + pkg.archive.sha1 : err);
    return res;
  }

  say("unpacking", 0, 0);
  const std::string unpack = temp + "/unpack-" + std::to_string(::getpid());
  fs::remove_all(unpack, ec);
  fs::create_directories(unpack, ec);
  if (!run_ok({"/usr/bin/ditto", "-x", "-k", part, unpack}, std::chrono::minutes(20), cancel,
              nullptr, &err)) {
    fs::remove_all(unpack, ec);
    res.error = "could not unpack: " + err;
    return res;
  }
  // Every archive holds one top-level directory: emulator/, platform-tools/,
  // arm64-v8a/ ... which becomes the package's directory.
  std::string top;
  int entries = 0;
  for (const auto& e : fs::directory_iterator(unpack, ec)) {
    if (e.path().filename().string().rfind("__MACOSX", 0) == 0) continue;
    entries++;
    top = e.path().string();
  }
  if (entries != 1 || !is_dir(top)) {
    fs::remove_all(unpack, ec);
    res.error = "the archive does not hold a single top-level directory";
    return res;
  }
  const std::string dest = package_directory(root, pkg.path);
  fs::create_directories(fs::path(dest).parent_path(), ec);
  if (is_dir(dest)) {
    const std::string old = temp + "/replaced-" + fs::path(dest).filename().string() + "-" +
                            std::to_string(::getpid());
    fs::rename(dest, old, ec);
    fs::remove_all(old, ec);
  }
  fs::rename(top, dest, ec);
  fs::remove_all(unpack, ec);
  if (ec) {
    res.error = "could not move the package into place: " + ec.message();
    return res;
  }

  // package.xml, as sdkmanager writes it: the manifest's own element, renamed
  // to localPackage, without the download details, under the namespaces the
  // manifest declared, with the license it was accepted under.
  std::string local = pkg.element_xml;
  local.replace(0, std::string("<remotePackage").size(), "<localPackage");
  local.replace(local.rfind("</remotePackage>"), std::string("</remotePackage>").size(),
                "</localPackage>");
  for (const auto& [open, close] : std::vector<std::pair<std::string, std::string>>{
           {"<channelRef", "/>"}, {"<archives>", "</archives>"}}) {
    const auto a = local.find(open);
    if (a == std::string::npos) continue;
    const auto b = local.find(close, a);
    if (b != std::string::npos) local.erase(a, b + close.size() - a);
  }
  std::string ns;
  {
    xml::Parser rp(pkg.root_start_tag.data(), pkg.root_start_tag.size());
    if (rp.next()) {
      for (const auto& a : rp.attributes()) {
        if (a.name.rfind("xmlns", 0) == 0) ns += " " + a.name + "=\"" + xml_escape(a.value) + "\"";
      }
    }
  }
  std::ofstream px(dest + "/package.xml");
  px << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
     << "<ns2:repository xmlns:ns2=\"http://schemas.android.com/repository/android/common/02\""
     << ns << ">";
  if (!pkg.license_id.empty()) {
    px << "<license id=\"" << xml_escape(pkg.license_id) << "\" type=\"text\">"
       << xml_escape(trim(license_text)) << "</license>";
  }
  px << local << "</ns2:repository>\n";
  ::unlink(part.c_str());
  res.ok = true;
  res.directory = dest;
  say("done", pkg.archive.size, pkg.archive.size);
  return res;
}

}  // namespace mpi::android
