#include "core/packs/registry.hpp"

#include <algorithm>
#include <iterator>
#include <span>

#include "core/packs/applies.hpp"
#include "core/packs/schema.hpp"

namespace myclean::packs {

namespace {

void append(auto& to, const auto& from) {
  std::ranges::copy(from, std::back_inserter(to));
}

void add(Registry& registry, const Pack& pack) {
  registry.packs.push_back(pack.manifest.id);
  append(registry.clean, pack.clean);
  append(registry.attribution, pack.attribution);
  append(registry.relocate, pack.relocate);
  append(registry.compress, pack.compress);
  append(registry.kb, pack.kb);
  append(registry.classes.matches, pack.classes);
}

}  // namespace

Registry make_registry(std::span<const Pack> packs) {
  Registry registry;
  for (const Pack& pack : packs) {
    add(registry, pack);
  }
  return registry;
}

Registry make_registry(std::span<const Pack> packs, const Probes& probes) {
  Registry registry;
  for (const Pack& pack : packs) {
    if (applies(pack.manifest.applies_when, probes)) {
      add(registry, pack);
    }
  }
  return registry;
}

}  // namespace myclean::packs
