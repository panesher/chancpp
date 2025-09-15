#pragma once

#include <boost/circular_buffer.hpp>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>

namespace chan {

class WriteToClosedChannelException : public std::exception {};

template <typename T> class Queue {
public:
  Queue(size_t size) : queue_(size) {}

  template <typename... Args> void push(Args... args) {
    assert(!full());
    queue_.push_back(std::forward<Args>(args)...);
  }

  std::optional<T> try_pop() {
    if (queue_.empty()) {
      return std::nullopt;
    }
    auto result = std::move(queue_.front());
    queue_.pop_front();
    return result;
  }

  inline bool empty() const { return queue_.empty(); }

  inline bool full() const { return queue_.size() == queue_.capacity(); }

private:
  boost::circular_buffer<T> queue_;
};

template <typename T, typename Mutex = std::mutex,
          typename ConditionalVariable = std::condition_variable,
          typename TQueue = Queue<T>>
class Chan {
public:
  Chan(size_t buffer_size) : queue_(buffer_size) {}

  template <typename... Args> void send(Args... args) {
    std::unique_lock lock(mu_);
    while (!closed_.load(std::memory_order_acquire)) {
      if (!queue_.full()) {
        queue_.push(std::forward<Args>(args)...);
        read_cv_.notify_one();
        return;
      }

      write_cv_.wait(lock);
    }
    throw WriteToClosedChannelException{};
  }

  std::optional<T> receive() {
    std::unique_lock lock(mu_);
    while (!closed_.load(std::memory_order_relaxed)) {
      if (auto result = try_receive_unsafe()) {
        return result;
      }

      read_cv_.wait(lock);
    }

    return try_receive_unsafe();
  }

  std::optional<T> try_receive() {
    std::unique_lock lock(mu_);
    return try_receive_unsafe();
  }

  void close() {
    closed_.store(true, std::memory_order_relaxed);
    read_cv_.notify_all();
    write_cv_.notify_all();
  }

  operator bool() const {
    std::unique_lock lock(mu_);
    return !(closed_.load(std::memory_order_relaxed) && queue_.empty());
  }

private:
  inline std::optional<T> try_receive_unsafe() {
    if (auto result = queue_.try_pop()) {
      write_cv_.notify_one();
      return result;
    }
    return std::nullopt;
  }

  TQueue queue_;
  ConditionalVariable write_cv_{};
  ConditionalVariable read_cv_{};
  mutable Mutex mu_{};
  std::atomic<bool> closed_{false};
};

template <typename T, typename Mutex = std::mutex,
          typename ConditionalVariable = std::condition_variable>
class EmptyChan {
public:
  EmptyChan() = default;

  void send(T value) {
    std::unique_lock lock(mu_);
    while (!closed_.load(std::memory_order_acquire)) {
      if (!buffer_.has_value()) {
        buffer_ = std::move(value);
        auto own_ticket = ++ticket_;
        read_cv_.notify_one();
        while (!closed_.load(std::memory_order_acquire) &&
               buffer_.has_value() && own_ticket == ticket_) {
          ticket_cv_.wait(lock);
        }
        if (buffer_.has_value() &&
            own_ticket ==
                ticket_) { /// Nobody read at this point, but channel closed
          throw WriteToClosedChannelException{};
        }
        return;
      }

      write_cv_.wait(lock);
    }
    throw WriteToClosedChannelException{};
  }

  std::optional<T> receive() {
    std::unique_lock lock(mu_);
    while (!closed_.load(std::memory_order_relaxed)) {
      if (auto result = try_receive_unsafe()) {
        return result;
      }

      read_cv_.wait(lock);
    }

    return try_receive_unsafe();
  }

  std::optional<T> try_receive() {
    std::unique_lock lock(mu_);
    return try_receive_unsafe();
  }

  void close() {
    closed_.store(true, std::memory_order_relaxed);
    read_cv_.notify_all();
    write_cv_.notify_all();
    ticket_cv_.notify_all();
  }

  operator bool() const {
    std::unique_lock lock(mu_);
    return !(closed_.load(std::memory_order_relaxed) && !buffer_.has_value());
  }

private:
  inline std::optional<T> try_receive_unsafe() {
    if (buffer_.has_value()) {
      auto result = std::move(buffer_);
      buffer_.reset();
      ticket_cv_.notify_one();
      write_cv_.notify_one();
      return result;
    }
    return std::nullopt;
  }

  std::optional<T> buffer_{std::nullopt};
  ConditionalVariable write_cv_{};
  ConditionalVariable read_cv_{};
  ConditionalVariable ticket_cv_{};
  mutable Mutex mu_{};
  uint64_t ticket_{0};
  std::atomic<bool> closed_{false};
};

} // namespace chan
