// Sample after a real physics substep. The outer step also runs while paused;
// sub_82D4DE30 (TU4 82E100B0) is called only by its substep loop. Each substep also tells the
// racing wheel that the car is being driven (platform/driving_gate.h, platform/wheel_bridge.cpp).
#include "../platform/driving_gate.h"
#include "../platform/guest_addr.h"
#include "../platform/telemetry.h"
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
extern "C" REX_FUNC(__imp__sub_82D4DE30);
extern "C" REX_FUNC(sub_82D4DE30) {
  __imp__sub_82D4DE30(ctx, base);
  forza::telemetry::Simulation().Step(forza::telemetry::DrivingNowMs());
  forza::telemetry::Physics(base);
}
