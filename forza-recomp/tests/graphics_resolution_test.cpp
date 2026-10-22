#include "../src/platform/graphics_resolution.h"
#include <cassert>
using namespace forza::graphics_menu;
int main() {
  // Default/720p and custom launcher modes must retain their identities instead
  // of becoming the first selectable live mode (1080p).
  assert(CurrentOutput("") == "720p");
  assert(CurrentOutput("720p") == "720p");
  assert(CurrentOutput("3440x1440") == "3440x1440");
  assert(OutputLabel(CurrentOutput("")) == "1280 x 720");
  assert(OutputLabel("1440p") == "2560 x 1440");
  assert(OutputLabel("2560x1080") == "2560x1080");
#ifdef __APPLE__
  assert(LiveOutputSupported("720p"));
#else
  assert(!LiveOutputSupported("720p"));
#endif
  assert(!LiveOutputSupported("2560x1080"));
  for (const char* value : {"1080p", "1440p", "4k"}) assert(LiveOutputSupported(value));
}
