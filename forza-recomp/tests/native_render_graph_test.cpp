#include "../src/platform/native_renderer/render_graph.h"
#include <iostream>
#include <stdexcept>

using namespace forza::native_renderer;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition))                                                          \
      throw std::runtime_error(#condition);                                    \
  } while (false)

bool Depends(const Pass &pass, PassId before, Hazard hazard) {
  for (const auto &dep : pass.dependencies)
    if (dep.before == before && dep.hazard == hazard)
      return true;
  return false;
}

int main() {
  RenderGraph graph;
  graph.Declare({1, "scene"});
  graph.Declare({2, "scene snapshot"});
  graph.Declare({3, "native HUD"});
  graph.AddPass("world", {{1, Access::write}});
  graph.AddPass("snapshot", {{1, Access::read}, {2, Access::write}});
  auto overwrite = graph.AddPass("next world", {{1, Access::write}});
  CHECK(Depends(overwrite, 0, Hazard::write_after_write));
  CHECK(Depends(overwrite, 1, Hazard::write_after_read));
  const auto &hud =
      graph.AddPass("HUD composite", {{2, Access::read}, {3, Access::write}});
  CHECK(Depends(hud, 1, Hazard::read_after_write));
  CHECK(hud.uses[0].read_version == 1 && hud.undefined_reads.empty());
  CHECK(graph.ContentsDefined());

  RenderGraph readers;
  readers.Declare({1, "persistent texture", 0, true});
  readers.AddPass("first reader", {{1, Access::read}});
  readers.AddPass("second reader", {{1, Access::read}});
  const auto &upload = readers.AddPass("replace texture", {{1, Access::write}});
  CHECK(Depends(upload, 0, Hazard::write_after_read));
  CHECK(Depends(upload, 1, Hazard::write_after_read));

  RenderGraph undefined;
  undefined.Declare({1, "new attachment"});
  CHECK(!undefined.AddPass("load before clear", {{1, Access::read}})
             .undefined_reads.empty());
  CHECK(!undefined.ContentsDefined());

  RenderGraph alias;
  alias.Declare({1, "reflection view", 7, true});
  alias.Declare({2, "reused allocation", 7, true});
  alias.AddPass("sample reflection", {{1, Access::read}});
  auto reuse = alias.AddPass("overwrite allocation", {{2, Access::write}});
  CHECK(Depends(reuse, 0, Hazard::alias));
  const auto &stale = alias.AddPass("stale reflection", {{1, Access::read}});
  CHECK(Depends(stale, 1, Hazard::alias) && !stale.undefined_reads.empty());
  RenderGraph feedback;
  feedback.Declare({1, "sampled view", 8, true});
  feedback.Declare({2, "attachment view", 8, true});
  const auto &conflicting = feedback.AddPass(
      "unresolved feedback", {{1, Access::read}, {2, Access::write}});
  CHECK(!conflicting.conflicting_aliases.empty() &&
        !feedback.ContentsDefined());

  RenderGraph inplace;
  inplace.Declare({1, "history", 0, true});
  const auto &rw =
      inplace.AddPass("accumulate", {{1, Access::read}, {1, Access::write}});
  CHECK(rw.uses.size() == 1 && rw.uses[0].access == Access::read_write);
  CHECK(rw.dependencies.empty() && rw.uses[0].read_version == 0 &&
        rw.uses[0].write_version == 1);
  inplace.AddPass("discard", {{1, Access::discard}});
  CHECK(!inplace.AddPass("read discarded", {{1, Access::read}})
             .undefined_reads.empty());

  bool rejected = false;
  try {
    inplace.AddPass("unknown", {{9, Access::read}});
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  CHECK(rejected && inplace.passes().size() == 3);
  rejected = false;
  try {
    inplace.Declare({2, "late declaration"});
  } catch (const std::logic_error &) {
    rejected = true;
  }
  CHECK(rejected);
  for (const auto &pass : alias.passes())
    for (const auto &dep : pass.dependencies)
      CHECK(dep.before < pass.id);
  std::cout << "Native render graph hazards, aliases and versions passed\n";
}
