#include "core/net/grpc_client.hpp"

#include <cstdlib>

namespace mpi::net {
namespace {

// grpc-message is percent-encoded (gRPC over HTTP2, "Responses").
std::string percent_decode(const std::string& in) {
  std::string out;
  for (std::size_t i = 0; i < in.size(); i++) {
    if (in[i] == '%' && i + 2 < in.size()) {
      out.push_back(static_cast<char>(std::strtol(in.substr(i + 1, 2).c_str(), nullptr, 16)));
      i += 2;
    } else {
      out.push_back(in[i]);
    }
  }
  return out;
}

}  // namespace

std::string grpc_frame(std::string_view message) {
  std::string out;
  out.reserve(5 + message.size());
  out.push_back(0);  // not compressed
  const auto n = static_cast<std::uint32_t>(message.size());
  out.push_back(static_cast<char>((n >> 24) & 0xff));
  out.push_back(static_cast<char>((n >> 16) & 0xff));
  out.push_back(static_cast<char>((n >> 8) & 0xff));
  out.push_back(static_cast<char>(n & 0xff));
  out.append(message.data(), message.size());
  return out;
}

GrpcChannel::GrpcChannel(std::uint16_t port, std::string authorization)
    : port_(port), auth_(std::move(authorization)) {}

bool GrpcChannel::connect(std::string* error) { return conn_.connect(port_, error); }

Headers GrpcChannel::request_headers(const std::string& path) const {
  Headers h = {{":method", "POST"},
               {":scheme", "http"},
               {":path", path},
               {":authority", "127.0.0.1:" + std::to_string(port_)},
               {"content-type", "application/grpc"},
               {"te", "trailers"},
               {"user-agent", "devx-mpi"}};
  if (!auth_.empty()) h.emplace_back("authorization", auth_);
  return h;
}

void GrpcStream::feed(std::string_view data) {
  buffer_.append(data.data(), data.size());
  std::size_t pos = 0;
  while (buffer_.size() - pos >= 5) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(buffer_.data() + pos);
    const std::uint32_t n = (static_cast<std::uint32_t>(p[1]) << 24) |
                            (static_cast<std::uint32_t>(p[2]) << 16) |
                            (static_cast<std::uint32_t>(p[3]) << 8) | p[4];
    if (buffer_.size() - pos - 5 < n) break;
    if (p[0] != 0) {
      end(-1, "the server sent a compressed message, which this client did not ask for");
      return;
    }
    if (on_message_) on_message_(std::string_view(buffer_.data() + pos + 5, n));
    pos += 5 + n;
  }
  buffer_.erase(0, pos);
}

void GrpcStream::headers(const Headers& h, bool end_stream) {
  std::string status, message;
  bool has_status = false;
  for (const auto& [k, v] : h) {
    if (k == "grpc-status") {
      status = v;
      has_status = true;
    } else if (k == "grpc-message") {
      message = percent_decode(v);
    } else if (k == ":status" && v != "200") {
      message = "HTTP status " + v;
    }
  }
  if (has_status) {
    end(std::atoi(status.c_str()), message);
  } else if (end_stream) {
    end(-1, message.empty() ? "the stream ended without a gRPC status" : message);
  }
}

void GrpcStream::end(int code, std::string message) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (done_) return;
    done_ = true;
    status_.code = code;
    status_.message = std::move(message);
  }
  cv_.notify_all();
}

void GrpcStream::cancel() {
  if (channel_ != nullptr && id_ != 0) channel_->conn_.cancel_stream(id_);
  end(1, "cancelled by this side");  // 1 is gRPC CANCELLED
}

bool GrpcStream::finished() const {
  std::lock_guard<std::mutex> lock(mu_);
  return done_;
}

bool GrpcStream::wait(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mu_);
  return cv_.wait_for(lock, timeout, [this] { return done_; });
}

GrpcStatus GrpcStream::status() const {
  std::lock_guard<std::mutex> lock(mu_);
  return status_;
}

std::shared_ptr<GrpcStream> GrpcChannel::server_stream(
    const std::string& path, std::string_view request,
    std::function<void(std::string_view)> on_message) {
  auto s = std::make_shared<GrpcStream>();
  s->channel_ = this;
  s->on_message_ = std::move(on_message);
  std::weak_ptr<GrpcStream> weak = s;
  Http2StreamHandlers h;
  h.on_headers = [weak](const Headers& hs, bool end) {
    if (auto st = weak.lock()) st->headers(hs, end);
  };
  h.on_data = [weak](std::string_view data, bool end) {
    if (auto st = weak.lock()) {
      st->feed(data);
      if (end) st->end(-1, "the stream ended without trailers");
    }
  };
  h.on_aborted = [weak](const std::string& why) {
    if (auto st = weak.lock()) st->end(-1, why);
  };
  s->id_ = conn_.open_stream(request_headers(path), grpc_frame(request), true, std::move(h));
  if (s->id_ == 0) {
    s->end(-1, "the connection to the emulator is closed: " + conn_.close_reason());
  }
  return s;
}

GrpcStatus GrpcChannel::unary(const std::string& path, std::string_view request,
                              std::string* response, std::chrono::milliseconds timeout) {
  std::string got;
  bool have = false;
  auto s = server_stream(path, request, [&](std::string_view msg) {
    // Runs on the reader thread while this thread waits; guarded by the wait.
    got.assign(msg.data(), msg.size());
    have = true;
  });
  if (!s->wait(timeout)) {
    s->cancel();
    GrpcStatus st;
    st.message = path + " did not answer within " + std::to_string(timeout.count()) + " ms";
    return st;
  }
  GrpcStatus st = s->status();
  if (st.ok() && response != nullptr) {
    if (!have) {
      st.code = -1;
      st.message = path + " returned no message";
    } else {
      *response = std::move(got);
    }
  }
  return st;
}

}  // namespace mpi::net
