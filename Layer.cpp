#include "Layer.hpp"

#include "Net.hpp"

unsigned Layer::getWorkerContextsPerTile() const {
  return net.getWorkerContextsPerTile();
}

unsigned Layer::getNumIPUs() const { return net.getNumIPUs(); }

unsigned Layer::getTilesPerIPU() const { return net.getTilesPerIPU(); }

Layer *Layer::getPrevLayer() const { return net.getPrevLayer(index); }

Layer *Layer::getNextLayer() const { return net.getNextLayer(index); }

const std::string &Layer::getDType() const { return net.getDType(); }

unsigned Layer::getDTypeSize() const {
  if (getDType() == "float")
    return 4;
  assert(getDType() == "short");
  return 2;
}

unsigned Layer::getBatchSize() const { return net.getBatchSize(); }

enum NetType Layer::getNetType() const { return net.getNetType(); }

void Layer::mapTensor(Tensor t, IPUModelEngineBuilder::TileMapping *mapping) {
  if (!mapping)
    return;
  std::uint64_t size = t.numElements();
  const auto numTiles = getTilesPerIPU() * getNumIPUs();
  for (unsigned i = 0; i < numTiles; ++i) {
    const auto begin = (size * i) / numTiles;
    const auto end = (size * (i + 1)) / numTiles;
    if (begin == end)
      continue;
    mapping->setMapping(t.flatten().slice(begin, end), i);
  }
}

void Layer::mapComputeSet(const Graph &graph, ComputeSet c,
                          IPUModelEngineBuilder::TileMapping *mapping) {
  if (!mapping)
    return;
  auto cs = graph.getComputeSet(c);
  std::uint64_t size = cs.size();
  const auto numTiles = getTilesPerIPU() * getNumIPUs();
  for (unsigned i = 0; i < numTiles; ++i) {
    const auto begin = (size * i) / numTiles;
    const auto end = (size * (i + 1)) / numTiles;
    if (begin == end)
      continue;
    for (unsigned j = begin; j != end; ++j) {
      mapping->setMapping(cs[j], i);
    }
  }
}

