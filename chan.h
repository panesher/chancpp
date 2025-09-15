#pragma once

#include <boost/circular_buffer.hpp>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>

namespace chan {

class WriteToClosedChannelException : public std::exception {};
class ChannelReadNullIntoTException : public std::bad_optional_access {};

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
  Chan(const Chan&) = delete;
  Chan(Chan&&) = delete;

  template <typename... Args> void send(Args... args) {
    std::unique_lock lock(mu_);
    while (!closed_.load(std::memory_order_acquire)) {
      if (!queue_.full()) {
        queue_.push(std::forward<Args>(args)...);
        lock.unlock();
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
      if (auto result = try_receive_unsafe(lock)) {
        return result;
      }

      read_cv_.wait(lock);
    }

    return try_receive_unsafe(lock);
  }

  std::optional<T> try_receive() {
    std::unique_lock lock(mu_);
    return try_receive_unsafe(lock);
  }

  template <typename... Args> Chan &operator<<(Args... args) {
    send(std::forward<Args>(args)...);
    return *this;
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
  inline std::optional<T> try_receive_unsafe(std::unique_lock<Mutex>& lock){
    if (auto result = queue_.try_pop()) {
      lock.unlock();
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

template <typename Out, typename T, typename Mutex,
          typename ConditionalVariable, typename TQueue>
inline Out &operator<<(Out &out,
                       Chan<T, Mutex, ConditionalVariable, TQueue> &ch) {
  if constexpr (std::is_same_v<std::remove_cvref_t<Out>, T>) {
    auto result = ch.receive();
    if (!result.has_value()) {
      throw ChannelReadNullIntoTException{};
    }
    out = std::move(*result);
  } else {
    out = ch.receive();
  }
  return out;
}

template <typename T, typename Mutex = std::mutex,
          typename ConditionalVariable = std::condition_variable>
class EmptyChan {
public:
  EmptyChan() = default;
  EmptyChan(const EmptyChan&) = delete;
  EmptyChan(EmptyChan&&) = delete;

  template <typename... Args> void send(Args... args) {
    std::unique_lock lock(mu_);
    while (!closed_.load(std::memory_order_acquire)) {
      if (!buffer_.has_value()) {
        buffer_.emplace(std::forward<Args>(args)...);
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
      if (auto result = try_receive_unsafe(lock)) {
        return result;
      }

      read_cv_.wait(lock);
    }

    return try_receive_unsafe(lock);
  }

  std::optional<T> try_receive() {
    std::unique_lock lock(mu_);
    return try_receive_unsafe(lock);
  }

  template <typename... Args> EmptyChan &operator<<(Args... args) {
    send(std::forward<Args>(args)...);
    return *this;
  }

  void close() {
    closed_.store(true, std::memory_order_release);
    read_cv_.notify_all();
    write_cv_.notify_all();
    ticket_cv_.notify_all();
  }

  operator bool() const {
    std::unique_lock lock(mu_);
    return !(closed_.load(std::memory_order_relaxed) && !buffer_.has_value());
  }

private:
  inline std::optional<T> try_receive_unsafe(std::unique_lock<Mutex>& lock) {
    if (buffer_.has_value()) {
      auto result = std::move(buffer_);
      buffer_.reset();
      lock.unlock();
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

template <typename Out, typename T, typename Mutex,
          typename ConditionalVariable>
inline Out &operator<<(Out &out, EmptyChan<T, Mutex, ConditionalVariable> &ch) {
  if constexpr (std::is_same_v<std::remove_cvref_t<Out>, T>) {
    auto result = ch.receive();
    if (!result.has_value()) {
      throw ChannelReadNullIntoTException{};
    }
    out = std::move(*result);
  } else {
    out = ch.receive();
  }
  return out;
}

} // namespace chan
