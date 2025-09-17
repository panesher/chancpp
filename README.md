# chancpp

Go-style channels and a `select` primitive for modern C++20 projects.

## Features
- **Buffered, unbuffered, and size-aware channels** via `chan::BufferChannel`, `chan::NoBufferChannel`, and the unified `chan::Chan` wrapper.
- **Blocking, non-blocking, and streaming operators**: `send`, `try_send`, `receive`, `try_receive`, boolean state checks, and `operator<<` helpers.
- **Thread-safe multi-producer/multi-consumer support** implemented with mutexes and condition variables.
- **Graceful shutdown semantics** with `close()` and typed exceptions when attempting to write to closed channels.
- **`Select` combinator** that wakes the associated handler when any subscribed channel produces a value.

## Requirements
- A C++20 compiler (tested with `clang++`).
- Boost headers and libraries (the implementation depends on `boost::circular_buffer`).
- POSIX threads support.

By default the provided `Makefile` expects Boost to be installed at `/opt/homebrew/opt/boost`. Adjust `BOOST_DIR` if it lives elsewhere on your system.

## Basic Usage

Here’s a minimal example showing how to create a channel, send a value, and receive it in Go style:
```cpp
chan::Chan<int> c(3);

std::thread producer([&] {
  c << 2;
  c.close();
});

std::optional<int> value;
value << c;
```

Create a channel with a fixed capacity. Producers block when the buffer is full and consumers block when it is empty. Closing the channel causes subsequent receives to return `std::nullopt`.

```cpp
#include "chan.h"
#include <iostream>
#include <thread>

int main() {
  chan::Chan<int> numbers(3); // 0 would create a rendezvous (unbuffered) channel

  std::thread producer([&] {
    for (int i = 0; i < 5; ++i) {
      numbers.send(i);
    }
    numbers.close();
  });

  while (auto value = numbers.receive()) {
    std::cout << "got " << *value << '\n';
  }

  producer.join();
  return 0;
}
```

For unbuffered rendezvous semantics use `chan::NoBufferChannel<T>` or `chan::Chan` with 0 size. If the receiver is not waiting, `send` blocks until a receiver arrives. `try_receive` lets you poll without blocking and `operator bool` reports whether the channel still has buffered data or remains open.

```cpp
chan::NoBufferChannel<std::string> rendezvous;

std::thread writer([&] {
  rendezvous.send("hello");
  rendezvous.close();
});

while (auto msg = rendezvous.receive()) {
  // process message
}

writer.join();
```

Attempting to `send` after `close()` throws `chan::WriteToClosedChannelException`, so wrapping sends in try/catch lets you react to shutdowns initiated elsewhere.

## Select-style Multiplexing

Combine several channels and run the first handler whose channel produces a value. Channels subscribe/unsubscribe automatically as they are opened or closed.

```cpp
#include "chan.h"
#include "select.h"

chan::Chan<int> numbers(5);
chan::Chan<std::string> words(5);

Select(
  chan::on(numbers, [](int n) {
    std::cout << "number: " << n << '\n';
  }),
  chan::on(words, [](std::string s) {
    std::cout << "word: " << s << '\n';
  })
);
```

`Select` tries each case synchronously first, then waits on an internal notification channel until one of the subscribed channels becomes ready.

## Testing

```bash
make test          # unit tests for buffered/unbuffered channels
make test_stress    # stress scenarios with high contention
make test_select    # coverage for the Select combinator
make test_all       # run the full suite
```

Each target builds the corresponding test binary under `build/` with ThreadSanitizer enabled, then runs it.

## Customization Notes
- `chan::BufferChannel` accepts custom mutex, condition variable, and queue types if you need specialised primitives.
- `chan::Chan` chooses between buffered and unbuffered implementations at runtime based on the constructor capacity. A size of `0` yields `NoBufferChannel`; any other positive size creates a bounded buffer.
- Conversion operators let you pipe directly into POD types: `value << channel;` pulls the next element and assigns it.

## Project Layout
- `include/` – main headers (`chan.h`, `select.h`).
- `tests/` – Boost.Test-based suites covering correctness, stress scenarios, and select behaviour.
- `Makefile` – build recipes for the test binaries.

## License

This project is distributed under the terms of the gnu public license. See `LICENSE` for details.
