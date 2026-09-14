#pragma once

#include <atomic>
#include <memory>

namespace mpi {

// Cooperative cancellation shared between the UI/CLI thread and workers.
// Spec section 5 requires cancellation for every long operation; spec J11
// requires cancel to free buffers and workers.
class CancellationSource;

class CancellationToken {
 public:
  CancellationToken() = default;
  bool cancelled() const {
    return flag_ && flag_->load(std::memory_order_acquire);
  }
  // A default-constructed token is never cancelled, so call sites can omit it.
  static CancellationToken none() { return CancellationToken(); }

 private:
  friend class CancellationSource;
  explicit CancellationToken(std::shared_ptr<std::atomic_bool> f)
      : flag_(std::move(f)) {}
  std::shared_ptr<std::atomic_bool> flag_;
};

class CancellationSource {
 public:
  CancellationSource() : flag_(std::make_shared<std::atomic_bool>(false)) {}
  void cancel() { flag_->store(true, std::memory_order_release); }
  bool cancelled() const { return flag_->load(std::memory_order_acquire); }
  CancellationToken token() const { return CancellationToken(flag_); }

 private:
  std::shared_ptr<std::atomic_bool> flag_;
};

}  // namespace mpi
