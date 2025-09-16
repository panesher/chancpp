#define BOOST_TEST_MODULE ChannelsStressTest
#include <boost/test/included/unit_test.hpp>

#include "chan.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace std::chrono_literals;

// 1) High-contention MPMC test with random bursts
BOOST_AUTO_TEST_CASE(stress_mpmc_random_bursts) {
  constexpr int writers = 8;
  constexpr int readers = 8;
  constexpr int per_writer = 2000; // total 16k ops
  chan::BufferChannel<int> c(64);

  std::atomic<int> produced{0};
  std::atomic<int> consumed{0};
  std::atomic<bool> stop{false};

  std::vector<std::thread> ws;
  ws.reserve(writers);
  std::vector<std::thread> rs;
  rs.reserve(readers);

  for (int w = 0; w < writers; ++w) {
    ws.emplace_back([&, w] {
      std::mt19937 rng(static_cast<unsigned>(w * 7919 + 17));
      std::uniform_int_distribution<int> burst(1, 7);
      std::uniform_int_distribution<int> nap(0, 2);
      for (int i = 0; i < per_writer;) {
        int b = burst(rng);
        for (int k = 0; k < b && i < per_writer; ++k, ++i) {
          c.send(w * 1'000'000 + i);
          ++produced;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(nap(rng)));
      }
    });
  }

  std::mutex m;
  std::vector<int> bag;
  bag.reserve(writers * per_writer);

  for (int r = 0; r < readers; ++r) {
    rs.emplace_back([&] {
      while (true) {
        auto v = c.receive();
        if (!v.has_value())
          break;
        {
          std::lock_guard<std::mutex> lg(m);
          bag.push_back(*v);
        }
        ++consumed;
      }
    });
  }

  for (auto &t : ws) {
    if (t.joinable())
      t.join();
  }
  // All writers done, now close to release readers once drained
  c.close();
  for (auto &t : rs) {
    if (t.joinable())
      t.join();
  }

  BOOST_TEST(produced.load() == writers * per_writer);
  BOOST_TEST(consumed.load() == produced.load());

  // Uniqueness check (no drops/dups)
  std::unordered_set<int> set(bag.begin(), bag.end());
  BOOST_TEST(set.size() == bag.size());
}

// 2) Closing while writers are blocked on full buffer must throw inside senders
BOOST_AUTO_TEST_CASE(stress_close_while_blocked_writers) {
  chan::BufferChannel<int> c(2);

  // Fill and block several senders
  c.send(1);
  c.send(2); // buffer full

  std::atomic<int> threw{0};
  std::vector<std::thread> ws;
  for (int i = 0; i < 6; ++i) {
    ws.emplace_back([&] {
      try {
        c.send(42);
      } catch (const chan::WriteToClosedChannelException &) {
        threw.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // Give time for threads to block
  std::this_thread::sleep_for(30ms);
  c.close();
  for (auto &t : ws) {
    if (t.joinable())
      t.join();
  }

  // Drain the two existing values
  auto a = c.receive();
  auto b = c.receive();
  auto z = c.receive();
  BOOST_TEST(a.has_value());
  BOOST_TEST(b.has_value());
  BOOST_TEST(!z.has_value());

  // All blocked writers should have thrown
  BOOST_TEST(threw.load() == 6);
}

// 3) NoBufferChannel ping-pong for many iterations
BOOST_AUTO_TEST_CASE(stress_emptychan_ping_pong) {
  chan::NoBufferChannel<int> ec;
  constexpr int iters = 20000;

  std::atomic<int> got{0};

  std::thread pinger([&] {
    for (int i = 1; i <= iters; ++i) {
      ec.send(i); // blocks until receiver consumes
    }
    ec.close();
  });

  std::thread ponger([&] {
    while (true) {
      auto v = ec.receive();
      if (!v.has_value())
        break;
      ++got;
      // Optional tiny nap to vary scheduling
      if (*v % 1024 == 0)
        std::this_thread::sleep_for(1ms);
    }
  });

  if (pinger.joinable())
    pinger.join();
  if (ponger.joinable())
    ponger.join();

  BOOST_TEST(got.load() == iters);
}

// 4) Try-receive heavy polling under contention
BOOST_AUTO_TEST_CASE(stress_try_receive_with_polling) {
  chan::BufferChannel<int> c(8);
  constexpr int total = 10000;

  std::thread writer([&] {
    for (int i = 0; i < total; ++i)
      c.send(i);
    c.close();
  });

  std::atomic<int> sum{0};
  std::thread poller([&] {
    while (true) {
      if (auto v = c.try_receive()) {
        sum.fetch_add(*v, std::memory_order_relaxed);
      } else if (!c) {
        break; // closed and drained
      }
      // backoff
      std::this_thread::yield();
    }
  });

  if (writer.joinable())
    writer.join();
  if (poller.joinable())
    poller.join();

  // Final verification
  int expected = (total - 1) * total / 2; // sum 0..N-1
  BOOST_TEST(sum.load() == expected);
}

// 5) Heavy interleaving: many small channels created/destroyed rapidly
BOOST_AUTO_TEST_CASE(stress_many_small_channels_lifecycle) {
  constexpr int rounds = 2000;
  for (int r = 0; r < rounds; ++r) {
    chan::BufferChannel<int> c(1);
    c.send(r);
    auto v = c.receive();
    BOOST_REQUIRE(v.has_value());
    BOOST_TEST(*v == r);
    c.close();
    auto z = c.receive();
    BOOST_TEST(!z.has_value());
  }
}
