#define BOOST_TEST_MODULE ChannelsTest
#include <boost/test/included/unit_test.hpp>

#include "chan.h"
#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace std::chrono_literals;

// Helper to collect all remaining values from a channel until it is closed
// (i.e., receive() returns std::nullopt).

template <typename Channel, typename T>
static std::vector<T> drain_channel(Channel &ch) {
  std::vector<T> out;
  while (true) {
    auto v = ch.receive();
    if (!v.has_value())
      break;
    out.push_back(*v);
  }
  return out;
}

BOOST_AUTO_TEST_CASE(buffered_channel_fifo_single_producer_consumer) {
  chan::Chan<int> c(3);

  // Writer pushes a known ordered sequence, then closes.
  std::thread writer([&] {
    for (int i = 0; i < 5; ++i) {
      c.send(i);
    }
    c.close();
  });

  // Reader drains until close and collects values.
  std::vector<int> received;
  while (true) {
    auto v = c.receive();
    if (!v.has_value())
      break;
    received.push_back(*v);
  }

  writer.join();

  // Expect FIFO order with a single producer/consumer.
  BOOST_TEST(received.size() == 5);
  for (int i = 0; i < 5; ++i) {
    BOOST_TEST(received[i] == i);
  }
}

BOOST_AUTO_TEST_CASE(receive_on_closed_empty_returns_nullopt) {
  chan::Chan<int> c(1);
  c.close();
  auto v = c.receive();
  BOOST_TEST(!v.has_value());
}

BOOST_AUTO_TEST_CASE(empty_channel_rendezvous) {
  chan::EmptyChan<int> ec;

  // Start a sender that posts after a short delay to ensure the receive blocks
  // until data arrives.
  std::thread sender([&] {
    std::this_thread::sleep_for(50ms);
    ec.send(42);
    std::this_thread::sleep_for(10ms);
    ec.close();
  });

  // First receive should get the value, second should observe closed channel
  // and return nullopt.
  auto first = ec.receive();
  BOOST_TEST(first.has_value());
  BOOST_TEST(*first == 42);

  auto second = ec.receive();
  BOOST_TEST(!second.has_value());

  sender.join();
}

BOOST_AUTO_TEST_CASE(multiple_writers_multiple_readers) {
  constexpr int writers_count = 5;
  constexpr int per_writer = 25;
  constexpr int readers_count = 6;

  chan::Chan<int> c(3);

  // Launch writers producing distinct ranges.
  std::vector<std::thread> writers;
  writers.reserve(writers_count);
  for (int w = 0; w < writers_count; ++w) {
    writers.emplace_back([&c, w] {
      for (int i = 0; i < per_writer; ++i) {
        c.send(w * 1000 + i);
        // tiny pause to encourage interleaving across threads
        std::this_thread::sleep_for(1ms);
      }
    });
  }

  // Collect results from several reader threads concurrently.
  std::mutex mtx;
  std::vector<int> collected;
  collected.reserve(writers_count * per_writer);

  std::vector<std::thread> readers;
  readers.reserve(readers_count);
  for (int r = 0; r < readers_count; ++r) {
    readers.emplace_back([&] {
      while (true) {
        auto v = c.receive();
        if (!v.has_value())
          break; // channel closed and drained
        std::lock_guard<std::mutex> lock(mtx);
        collected.push_back(*v);
      }
    });
  }

  // Wait for all writers to finish, then close channel to release readers when
  // drained.
  for (auto &t : writers)
    t.join();
  c.close();
  for (auto &t : readers)
    t.join();

  // We should have exactly writers_count * per_writer items, all unique and
  // present.
  BOOST_TEST(collected.size() ==
             static_cast<size_t>(writers_count * per_writer));

  std::unordered_set<int> seen(collected.begin(), collected.end());
  BOOST_TEST(seen.size() == collected.size()); // no duplicates expected

  for (int w = 0; w < writers_count; ++w) {
    for (int i = 0; i < per_writer; ++i) {
      int expected = w * 1000 + i;
      BOOST_TEST(seen.count(expected));
    }
  }
}

BOOST_AUTO_TEST_CASE(chan_try_receive_empty_then_after_close) {
  chan::Chan<int> c(2);

  // Empty & open: try_receive should be nullopt and operator bool should be
  // true
  auto v1 = c.try_receive();
  BOOST_TEST(!v1.has_value());
  BOOST_TEST(static_cast<bool>(c));

  // Close with no pending items: try_receive still nullopt, operator bool
  // becomes false
  c.close();
  auto v2 = c.try_receive();
  BOOST_TEST(!v2.has_value());
  BOOST_TEST(!static_cast<bool>(c));
}

BOOST_AUTO_TEST_CASE(chan_send_to_closed_throws) {
  chan::Chan<int> c(1);
  c.close();
  BOOST_CHECK_THROW(c.send(7), chan::WriteToClosedChannelException);
}

BOOST_AUTO_TEST_CASE(emptychan_send_to_closed_throws) {
  chan::EmptyChan<int> ec;
  ec.close();
  BOOST_CHECK_THROW(ec.send(7), chan::WriteToClosedChannelException);
}

BOOST_AUTO_TEST_CASE(chan_close_idempotent_and_drain) {
  chan::Chan<int> c(3);
  c.send(1);
  c.send(2);
  c.close();
  // Closing again should be harmless
  c.close();

  // Should drain queued items, then return nullopt
  auto a = c.receive();
  auto b = c.receive();
  auto z = c.receive();
  BOOST_TEST(a.has_value());
  BOOST_TEST(*a == 1);
  BOOST_TEST(b.has_value());
  BOOST_TEST(*b == 2);
  BOOST_TEST(!z.has_value());
  BOOST_TEST(!static_cast<bool>(c));
}

BOOST_AUTO_TEST_CASE(chan_send_blocks_when_full_until_receive_frees_slot) {
  chan::Chan<int> c(1); // capacity 1
  std::atomic<bool> second_send_started{false};
  std::atomic<bool> second_send_completed{false};

  // Fill buffer to make it full
  c.send(10);

  // Start a thread that will attempt to send another item; it should block
  std::thread sender([&] {
    second_send_started.store(true, std::memory_order_relaxed);
    c.send(20); // should block until a receive happens
    second_send_completed.store(true, std::memory_order_relaxed);
  });

  // Give the sender a moment to reach the blocking point
  std::this_thread::sleep_for(30ms);
  BOOST_TEST(second_send_started.load(std::memory_order_relaxed));
  BOOST_TEST(!second_send_completed.load(std::memory_order_relaxed));

  // Now receive one item to free space; this should unblock the sender
  auto first = c.receive();
  BOOST_TEST(first.has_value());
  BOOST_TEST(*first == 10);

  // Give time for the send to complete
  std::this_thread::sleep_for(30ms);
  BOOST_TEST(second_send_completed.load(std::memory_order_relaxed));

  // Drain the second value
  auto second = c.receive();
  BOOST_TEST(second.has_value());
  BOOST_TEST(*second == 20);

  c.close();
  sender.join();
}

BOOST_AUTO_TEST_CASE(emptychan_send_blocks_until_receive_then_unblocks) {
  chan::EmptyChan<int> ec;
  std::atomic<bool> send_entered{false};
  std::atomic<bool> send_returned{false};

  std::thread sender([&] {
    send_entered.store(true, std::memory_order_relaxed);
    ec.send(99); // should block until receive consumes
    send_returned.store(true, std::memory_order_relaxed);
  });

  // Allow sender to start and block
  std::this_thread::sleep_for(30ms);
  BOOST_TEST(send_entered.load(std::memory_order_relaxed));
  BOOST_TEST(!send_returned.load(std::memory_order_relaxed));

  // Now receive to release the sender
  auto v = ec.receive();
  BOOST_TEST(v.has_value());
  BOOST_TEST(*v == 99);

  // After receive, sender should have returned
  std::this_thread::sleep_for(30ms);
  BOOST_TEST(send_returned.load(std::memory_order_relaxed));

  ec.close();
  sender.join();
}

BOOST_AUTO_TEST_CASE(emptychan_close_while_sender_waiting_causes_throw) {
  chan::EmptyChan<int> ec;
  std::exception_ptr thrown;

  std::thread sender([&] {
    try {
      ec.send(123); // will block; we'll close before it is received
    } catch (...) {
      thrown = std::current_exception();
    }
  });

  // Let sender block with the value pending
  std::this_thread::sleep_for(30ms);
  ec.close(); // no receiver; send should observe close and throw

  sender.join();
  BOOST_TEST(static_cast<bool>(thrown));
  try {
    if (thrown)
      std::rethrow_exception(thrown);
  } catch (const chan::WriteToClosedChannelException &) {
    BOOST_TEST(true);
  } catch (...) {
    BOOST_ERROR("Unexpected exception type");
  }
}

BOOST_AUTO_TEST_CASE(emptychan_try_receive_semantics_open_and_closed) {
  chan::EmptyChan<int> ec;
  // No value yet
  auto a = ec.try_receive();
  BOOST_TEST(!a.has_value());

  // Send/receive rendezvous via separate thread
  std::thread sender([&] { ec.send(7); });
  std::this_thread::sleep_for(10ms);
  auto b = ec.try_receive();
  if (!b.has_value()) {
    // If the sender hasn't posted yet, block to ensure we do get it
    b = ec.receive();
  }
  BOOST_TEST(b.has_value());
  BOOST_TEST(*b == 7);
  sender.join();

  // Closing with empty buffer: try_receive is still nullopt and operator bool
  // is false
  ec.close();
  auto z = ec.try_receive();
  BOOST_TEST(!z.has_value());
  BOOST_TEST(!static_cast<bool>(ec));
}

BOOST_AUTO_TEST_CASE(chan_operator_bool_semantics) {
  chan::Chan<int> c(2);
  // Open & empty -> true
  BOOST_TEST(static_cast<bool>(c));

  // Add item -> still true
  c.send(1);
  BOOST_TEST(static_cast<bool>(c));

  // Close while item pending -> still true (not yet drained)
  c.close();
  BOOST_TEST(static_cast<bool>(c));

  // Drain last item -> now false
  auto v = c.receive();
  BOOST_TEST(v.has_value());
  BOOST_TEST(*v == 1);
  auto z = c.receive();
  BOOST_TEST(!z.has_value());
  BOOST_TEST(!static_cast<bool>(c));
}

BOOST_AUTO_TEST_CASE(operator_ll_gg) {
  chan::Chan<int> c(2);

  c << 1;
  std::optional<int> value;
  value << c;
  BOOST_TEST(value.value() == 1);
}

BOOST_AUTO_TEST_CASE(operator_ll_gg_empty_chan) {
  chan::EmptyChan<int> c;

  std::thread reader([&c] {
    std::optional<int> value;
    value << c;
    BOOST_TEST(value.value() == 1);
  });

  c << 1;
  c.close();
  reader.join();
}

BOOST_AUTO_TEST_CASE(operator_passing) {
  chan::EmptyChan<int> c1;
  chan::Chan<int> c2(2);
  std::atomic<int> equals;

  std::thread transfer([&c1, &c2, &equals] {
    int value;
    c2 << (value << c1);
    equals += (value == 1);
    c2.close();
  });

  std::thread reader([&c2, &equals] {
    std::optional<int> v;
    v << c2;
    equals += (v.value() == 1);
  });

  c1 << 1;
  c1.close();
  transfer.join();
  reader.join();

  BOOST_TEST(equals == 2);
}
