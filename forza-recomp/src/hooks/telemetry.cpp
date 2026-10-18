// Sample after a real physics substep. The outer step also runs while paused;
// sub_82D4DE30 (TU4 82E100B0) is called only by its substep loop.
#include "../platform/guest_addr.h"
#include "../platform/telemetry.h"
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
extern "C" REX_FUNC(__imp__sub_82D4DE30);
extern "C" REX_FUNC(sub_82D4DE30) {
  __imp__sub_82D4DE30(ctx, base);
  forza::telemetry::Physics(base);
}
