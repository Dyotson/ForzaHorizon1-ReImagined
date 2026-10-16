// Compile-only regression check for the SDK's custom clock conversions.
#include <rex/chrono/chrono_steady_cast.h>
#include <concepts>
#include <utility>

template <typename To, typename From>
constexpr bool ConvertsToClock = std::same_as<
    decltype(std::chrono::clock_cast<To>(std::declval<typename From::time_point>())),
    typename To::time_point>;
using Host = rex::chrono::WinSystemClock;
using Guest = rex::chrono::XSystemClock;
using Steady = std::chrono::steady_clock;
static_assert(ConvertsToClock<Host, Guest>);
static_assert(ConvertsToClock<Guest, Host>);
static_assert(ConvertsToClock<Host, Steady>);
static_assert(ConvertsToClock<Steady, Host>);
