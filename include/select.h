#pragma once

#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace chan {

// A "case" binds a channel reference to a handler F(T)
template <class Ch, class F> struct Case {
  Ch &ch; // link so Case is copyable and stable in a tuple
  F handler;
};

// Helper to build a Case with type deduction
template <class Ch, class F>
auto on(Ch &ch, F &&f) -> Case<Ch, std::decay_t<F>> {
  return {ch, std::forward<F>(f)};
}

// Internal: call handler for the I-th case if there is a value to read
template <std::size_t I, class Tuple> bool call_ith(Tuple &cases) {
  auto &c = std::get<I>(cases);
  if (auto v = c.ch.try_receive()) {
    c.handler(*v); // F(T) or F(T&&) both fine
    return true;
  }
  return false;
}

template <class Tuple, std::size_t... Is>
bool dispatch_by_index(std::size_t idx, Tuple &cases,
                       std::index_sequence<Is...>) {
  return (false || ... || (idx == Is && call_ith<Is>(cases)));
}

template <class... Cases> void Select(Cases... cs) {
  constexpr size_t kSize = sizeof...(cs);
  static_assert(kSize > 0, "Select requires at least one case");

  auto cases = std::make_tuple(std::move(cs)...);
  auto idx_ch = std::make_shared<BufferChannel<int>>(kSize);

  // Subscribe every non-closed channel with its compile-time index
  std::size_t i = 0;
  std::apply(
      [&](auto &...c) {
        (((static_cast<bool>(c.ch))
              ? c.ch.subscribe(idx_ch, static_cast<int>(i))
              : void(),
          ++i),
         ...);
      },
      cases);

  auto any_open = [&]() -> bool {
    bool any = false;
    std::apply(
        [&](auto &...c) { ((any = any || static_cast<bool>(c.ch)), ...); },
        cases);
    return any;
  };

  for (int idx = 0; idx < kSize; ++idx) {
    if (dispatch_by_index(static_cast<std::size_t>(idx), cases,
                          std::make_index_sequence<sizeof...(cs)>{})) {
      return;
    }
  }

  while (any_open()) {
    auto idx = idx_ch->receive();
    if (!idx.has_value())
      continue;

    if (dispatch_by_index(static_cast<std::size_t>(*idx), cases,
                          std::make_index_sequence<sizeof...(cs)>{})) {
      return;
    }
  }
}

} // namespace chan
