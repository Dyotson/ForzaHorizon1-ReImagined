// Exercise the private runtime policy from the applied SDK patch, not a copy.
#include "imgui_dialog_repaint.h"
#include <algorithm>
#include <cstdlib>
#include <vector>

static void Check(bool value) { if (!value) std::abort(); }

int main() {
  rex::ui::DialogRepaintHints hints;
  int passive, interactive;
  std::vector<const void*> dialogs;
  const auto needs_repaint = [&] {
    return std::any_of(dialogs.begin(), dialogs.end(),
                       [&](const void* dialog) { return hints.WantsContinuous(dialog); });
  };
  Check(!needs_repaint());
  dialogs.push_back(&passive);
  Check(needs_repaint());  // Existing dialogs retain continuous repaint by default.
  hints.SetContinuous(&passive, false);
  Check(!needs_repaint());
  dialogs.push_back(&interactive);
  Check(needs_repaint());  // A passive overlay must not suppress an interactive dialog.
  dialogs.pop_back();
  Check(!needs_repaint());
  hints.SetContinuous(&passive, true);
  Check(needs_repaint());
  hints.SetContinuous(&passive, false);
  hints.Forget(&passive);  // Removal/destruction must not leak the hint to a reused address.
  Check(needs_repaint());
  hints.SetContinuous(&passive, false);  // Re-enabling the overlay restores the hint.
  Check(!needs_repaint());
  hints.Forget(&passive);
  hints.Forget(&passive);
  hints.SetContinuous(nullptr, false);
  Check(needs_repaint());
}
