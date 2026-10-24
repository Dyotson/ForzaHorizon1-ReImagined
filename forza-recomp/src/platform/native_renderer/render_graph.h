#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace forza::native_renderer {

using ResourceId = uint64_t;
using PassId = uint32_t;

enum class Access { read, write, read_write, discard };
enum class Hazard {
  read_after_write,
  write_after_read,
  write_after_write,
  alias
};

struct Resource {
  ResourceId id;
  std::string name;
  // Zero means no known aliasing. Equal nonzero groups must be treated as
  // overlapping until the frontend supplies a more precise allocation model.
  uint64_t alias_group = 0;
  bool imported = false;
};

struct Use {
  ResourceId resource;
  Access access;
  uint32_t read_version = 0;
  uint32_t write_version = 0;
};

struct Dependency {
  PassId before;
  ResourceId resource;
  Hazard hazard;
};

struct Pass {
  PassId id;
  std::string name;
  std::vector<Use> uses;
  std::vector<Dependency> dependencies;
  // An imported resource's version zero is valid. A new native attachment's
  // version zero is undefined; this must be resolved before submitting GPU
  // work.
  std::vector<ResourceId> undefined_reads;
  std::vector<ResourceId> conflicting_aliases;
};

// A conservative, whole-resource dependency model. It does not submit GPU work
// or infer that a sampled image may borrow a mutable attachment's contents.
// Subresources, layouts, queue ownership and Xbox EDRAM aliases belong to the
// frontend/backend contracts, not guesses made by this planner.
class RenderGraph {
public:
  void Declare(Resource resource) {
    if (!passes_.empty())
      throw std::logic_error("Declare all resources before adding passes");
    if (!resource.id || states_.find(resource.id) != states_.end())
      throw std::invalid_argument("Resource IDs must be nonzero and unique");
    const ResourceId id = resource.id;
    const bool defined = resource.imported;
    states_.emplace(id, State{std::move(resource), 0, defined});
  }

  const Pass &
  AddPass(std::string name,
          const std::vector<std::pair<ResourceId, Access>> &accesses) {
    // Coalesce duplicate descriptor bindings before looking at previous uses.
    // Otherwise a read/write binding would acquire a dependency on itself.
    std::map<ResourceId, Access> combined;
    for (const auto &[resource, access] : accesses) {
      if (states_.find(resource) == states_.end())
        throw std::invalid_argument("Pass references an undeclared resource");
      auto [it, inserted] = combined.emplace(resource, access);
      if (!inserted) {
        if (it->second == Access::discard || access == Access::discard) {
          if (it->second != access)
            throw std::invalid_argument("Discard must be a separate operation");
        } else if (it->second != access) {
          it->second = Access::read_write;
        }
      }
    }
    Pass pass{PassId(passes_.size()), std::move(name)};
    std::set<std::pair<PassId, std::pair<ResourceId, Hazard>>> seen;
    auto depend = [&](PassId before, ResourceId resource, Hazard hazard) {
      if (before != pass.id &&
          seen.emplace(before, std::pair(resource, hazard)).second)
        pass.dependencies.push_back({before, resource, hazard});
    };
    // All dependencies are derived from the preceding graph, before this pass
    // changes any state. This also handles two aliased views in the same pass.
    for (const auto &[id, access] : combined) {
      const auto &state = states_.at(id);
      const bool read = Reads(access), write = Writes(access);
      if (read && !state.defined)
        pass.undefined_reads.push_back(id);
      if (state.has_writer && read)
        depend(state.writer, id, Hazard::read_after_write);
      if (state.has_writer && write)
        depend(state.writer, id, Hazard::write_after_write);
      if (write)
        for (PassId reader : state.readers)
          depend(reader, id, Hazard::write_after_read);
      if (state.resource.alias_group) {
        for (const auto &[other_id, other] : states_) {
          if (other_id == id ||
              other.resource.alias_group != state.resource.alias_group)
            continue;
          if (combined.find(other_id) != combined.end() &&
              (write || Writes(combined.at(other_id))))
            pass.conflicting_aliases.push_back(id);
          if (other.has_writer)
            depend(other.writer, id, Hazard::alias);
          if (write)
            for (PassId reader : other.readers)
              depend(reader, id, Hazard::alias);
        }
      }
      pass.uses.push_back({id, access, read ? state.version : 0,
                           write ? state.version + 1 : 0});
    }
    // A write to an aliased view invalidates other views. Keeping stale version
    // numbers or declaring those pixels initialized would conceal a copy that
    // the native frontend still needs to supply.
    for (const auto &[id, access] : combined) {
      if (!Writes(access))
        continue;
      const auto group = states_.at(id).resource.alias_group;
      if (!group)
        continue;
      for (auto &[other_id, other] : states_)
        if (other_id != id && other.resource.alias_group == group &&
            combined.find(other_id) == combined.end())
          other.defined = false;
    }
    for (const auto &[id, access] : combined) {
      auto &state = states_.at(id);
      if (Writes(access)) {
        state.version++;
        state.writer = pass.id;
        state.has_writer = true;
        state.readers.clear();
        state.defined = access != Access::discard;
      } else {
        state.readers.insert(pass.id);
      }
    }
    passes_.push_back(std::move(pass));
    return passes_.back();
  }

  const std::vector<Pass> &passes() const { return passes_; }
  bool ContentsDefined() const {
    return std::all_of(passes_.begin(), passes_.end(), [](const Pass &pass) {
      return pass.undefined_reads.empty() && pass.conflicting_aliases.empty();
    });
  }

private:
  static bool Reads(Access access) {
    return access == Access::read || access == Access::read_write;
  }
  static bool Writes(Access access) { return access != Access::read; }
  struct State {
    Resource resource;
    uint32_t version = 0;
    bool defined = false;
    PassId writer = 0;
    bool has_writer = false;
    std::set<PassId> readers;
  };
  std::map<ResourceId, State> states_;
  std::vector<Pass> passes_;
};

} // namespace forza::native_renderer
