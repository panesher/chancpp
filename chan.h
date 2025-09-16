#pragma once

#include <boost/circular_buffer.hpp>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <variant>

namespace chan {

template <typename T, typename Mutex, typename ConditionalVariable,
          typename TQueue>
class Chan;

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
class BufferChannel {
public:
  BufferChannel(size_t buffer_size) : queue_(buffer_size) {}
  BufferChannel(const BufferChannel &) = delete;
  BufferChannel(BufferChannel &&) = delete;

  template <typename... Args> void send(Args... args) {
    std::unique_lock lock(mu_);
    while (!closed_) {
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

  template <typename... Args> bool try_send(Args... args) {
    std::unique_lock lock(mu_);
    if (closed_) {
      return false;
    }
    if (queue_.full()) {
      return false;
    }
    queue_.push(std::forward<Args>(args)...);
    lock.unlock();
    read_cv_.notify_one();
    return true;
  }

  std::optional<T> receive() {
    std::unique_lock lock(mu_);
    while (!closed_) {
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

  template <typename... Args> BufferChannel &operator<<(Args... args) {
    send(std::forward<Args>(args)...);
    return *this;
  }

  void close() {
    {
      std::unique_lock lock(mu_);
      closed_ = true;
    }
    read_cv_.notify_all();
    write_cv_.notify_all();
  }

  operator bool() const {
    std::unique_lock lock(mu_);
    return !(closed_ && queue_.empty());
  }

private:
  inline std::optional<T> try_receive_unsafe(std::unique_lock<Mutex> &lock) {
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
  bool closed_{false};
};

template <typename Out, typename T, typename Mutex,
          typename ConditionalVariable, typename TQueue>
inline Out &
operator<<(Out &out, BufferChannel<T, Mutex, ConditionalVariable, TQueue> &ch) {
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
class NoBufferChannel {
public:
  NoBufferChannel() = default;
  NoBufferChannel(size_t) {}
  NoBufferChannel(const NoBufferChannel &) = delete;
  NoBufferChannel(NoBufferChannel &&) = delete;

  template <typename... Args> void send(Args... args) {
    std::unique_lock lock(mu_);
    while (!closed_) {
      if (!buffer_.has_value()) {
        buffer_.emplace(std::forward<Args>(args)...);
        auto own_ticket = ++ticket_;
        read_cv_.notify_one();
        while (!closed_ && buffer_.has_value() && own_ticket == ticket_) {
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
    while (!closed_) {
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

  template <typename... Args> NoBufferChannel &operator<<(Args... args) {
    send(std::forward<Args>(args)...);
    return *this;
  }

  void close() {
    {
      std::unique_lock lock(mu_);
      closed_ = true;
    }
    read_cv_.notify_all();
    write_cv_.notify_all();
    ticket_cv_.notify_all();
  }

  operator bool() const {
    std::unique_lock lock(mu_);
    return !(closed_ && !buffer_.has_value());
  }

private:
  template <typename Any, typename AnyMutex, typename AnyCV, typename AnyQueue>
  friend class Chan;

  template <typename... Args> uint64_t send_without_wait(Args... args) {
    std::unique_lock lock(mu_);
    while (!closed_) {
      if (!buffer_.has_value()) {
        buffer_.emplace(std::forward<Args>(args)...);
        auto own_ticket = ++ticket_;
        read_cv_.notify_one();
        return own_ticket;
      }

      write_cv_.wait(lock);
    }
    throw WriteToClosedChannelException{};
  }

  template <typename... Args> void wait_on_ticket(uint64_t own_ticket) {
    std::unique_lock lock(mu_);
    while (!closed_ && buffer_.has_value() && own_ticket == ticket_) {
      ticket_cv_.wait(lock);
    }
    if (buffer_.has_value() &&
        own_ticket ==
            ticket_) { /// Nobody read at this point, but channel closed
      throw WriteToClosedChannelException{};
    }
  }

  inline std::optional<T> try_receive_unsafe(std::unique_lock<Mutex> &lock) {
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
  bool closed_{false};
};

template <typename Mutex, typename ConditionalVariable> struct Subscriber {
  std::shared_ptr<BufferChannel<int, Mutex, ConditionalVariable>> ch;
  int name;
};

template <typename Out, typename T, typename Mutex,
          typename ConditionalVariable>
inline Out &operator<<(Out &out,
                       NoBufferChannel<T, Mutex, ConditionalVariable> &ch) {
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
          typename ConditionalVariable = std::condition_variable,
          typename TQueue = Queue<T>>
class Chan {
public:
  Chan(size_t size)
      : channel_(size == 0 ? std::variant<BufChan, NoBufChan>(
                                 std::in_place_type<NoBufChan>)
                           : std::variant<BufChan, NoBufChan>(
                                 std::in_place_type<BufChan>, size)) {}
  Chan(const Chan &) = delete;
  Chan(Chan &&) = delete;

  template <typename... Args> void send(Args... args) {
    std::visit(
        [&](auto &ch) {
          using ChT = std::decay_t<decltype(ch)>;
          if constexpr (std::is_same_v<ChT, BufChan>) {
            ch.send(std::forward<Args>(args)...);
            notify_subscribers();
          } else {
            auto ticket = ch.send_without_wait(std::forward<Args>(args)...);
            notify_subscribers();
            ch.wait_on_ticket(ticket);
          }
        },
        channel_);
  }

  std::optional<T> receive() {
    return std::visit([](auto &ch) { return ch.receive(); }, channel_);
  }

  std::optional<T> try_receive() {
    return std::visit([](auto &ch) { return ch.try_receive(); }, channel_);
  }

  template <typename... Args> Chan &operator<<(Args... args) {
    send(std::forward<Args>(args)...);
    return *this;
  }

  void close() {
    std::visit([](auto &ch) { ch.close(); }, channel_);
  }

  operator bool() const {
    return std::visit([](auto &ch) -> bool { return !!ch; }, channel_);
  }

  void
  subscribe(std::shared_ptr<BufferChannel<int, Mutex, ConditionalVariable>> ch,
            int name) {
    std::unique_lock sub_lock(subscriber_mu_);
    subscribers_.emplace_back(ch, name);
  }

private:
  void notify_subscribers() {
    std::unique_lock sub_lock(subscriber_mu_);
    for (auto &sub : subscribers_ | std::views::reverse) {
      sub.ch->try_send(sub.name);
    }
    subscribers_.clear();
  }

  using NoBufChan = NoBufferChannel<T, Mutex, ConditionalVariable>;
  using BufChan = BufferChannel<T, Mutex, ConditionalVariable, TQueue>;

  std::variant<BufChan, NoBufChan> channel_{};
  std::vector<Subscriber<Mutex, ConditionalVariable>> subscribers_{};
  Mutex subscriber_mu_{};
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

} // namespace chan
