#include "../src/platform/unicorn_eligibility.h"
#include <cassert>
#include <initializer_list>

int main() {
  using namespace forza::unicorn;
  for (auto model : {320u, 326u, 330u, 1082u, 1197u, 1233u, 1488u}) {
    assert(IsUnicorn(model));
    for (auto online_mode : {5u, 7u, 8u, 9u, 10u}) {
      assert(!Eligible(true, TestMode::Deny, true, online_mode, true, model));
      assert(Eligible(true, TestMode::Granted, true, online_mode, true, model));
      assert(Eligible(true, TestMode::Off, true, online_mode, true, model));
      assert(Eligible(true, TestMode::Deny, true, online_mode, false, model));
      assert(!Eligible(false, TestMode::Granted, true, online_mode, true, model));
    }
    assert(Eligible(true, TestMode::Deny, false, 5, true, model));
    assert(Eligible(true, TestMode::Deny, true, 0, true, model));
  }
  for (auto model : {0u, 309u, 348u, 1009u, 1107u, 0xffffffffu}) {
    assert(!IsUnicorn(model));
    assert(Eligible(true, TestMode::Deny, true, 5, true, model));
    assert(!Eligible(false, TestMode::Deny, true, 5, true, model));
  }
}
