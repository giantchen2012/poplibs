#include "popops/Collectives.hpp"

#include "poplibs_support/Compiler.hpp"
#include "poplibs_support/OptionParsing.hpp"
#include "popops/ElementWise.hpp"
#include "popops/Pad.hpp"
#include "popops/Reduce.hpp"
#include "poputil/TileMapping.hpp"
#include "poputil/Util.hpp"
#include "poputil/exceptions.hpp"
#include <cassert>
#include <boost/optional/optional.hpp>

using namespace poplar;
using namespace poplar::program;

namespace  {

enum class CollectiveMethod {
  AUTO,
  // Send fragments clockwise around the ring. The number of fragments
  // is equal to the number of IPUs in the ring.
  CLOCKWISE_RING,
  // Send fragments anticlockwise around the ring. The number of fragments
  // is equal to the number of IPUs in the ring.
  ANTICLOCKWISE_RING,
  // Split the data into two halves and use the clockwise ring algorithm on
  // one half and the anticlockwise ring algorithm on the other in order
  // to fully utilize the links in both directions. The number of fragments
  // is equal to twice the number of IPUs in the ring.
  BIDIRECTIONAL_RING_PAIR,
  // Send half the fragments half way around the ring in the clockwise
  // direction and half the fragments half way around the ring in the
  // anticlockwise direction, meeting in the middle. The number of fragments
  // is equal to the number of IPUs in the ring. The disadvantage compared
  // to the BIDIRECTIONAL_RING_PAIR method is that the usage of available
  // bandwidth is not quite optimal, in particular the final step only uses
  // the links in one direction (assuming an even number of IPUs). The
  // advantage is the that it requires fewer steps and allows the use of
  // larger fragments.
  MEET_IN_MIDDLE_RING,
};

struct CollectiveOptions {
  CollectiveMethod method = CollectiveMethod::AUTO;
  bool useReplicatedImplementation = false;
};

} // End anonymous namespace.

namespace popops {



static std::vector<unsigned>
invertPermutation(const std::vector<unsigned> &permutation) {
  std::vector<unsigned> inverse(permutation.size());
  for (unsigned i = 0; i != permutation.size(); ++i) {
    inverse[permutation[i]] = i;
  }
  return inverse;
}

// Return the IPUs in clockwise direction around the ring starting at IPU 0.
static std::vector<unsigned> createRing(const unsigned n) {
  std::vector<unsigned> ring(n);
  unsigned i = 0;
  std::generate(ring.begin(), ring.begin() + ((n+1)/ 2),
                  [&] {i += 2; return i - 2;});
  if ((n & 1) != 0) {
    i -= 3;
  } else {
    --i;
  }
  std::generate(ring.begin() + ((n+1) / 2), ring.end(),
                  [&] {i -= 2; return i + 2;});
  return ring;
}

namespace {

enum Direction {
  CLOCKWISE,
  ANTICLOCKWISE
};

Direction opposite(Direction direction) {
  switch (direction) {
  case CLOCKWISE: return ANTICLOCKWISE;
  case ANTICLOCKWISE: return CLOCKWISE;
  }
}

class RingTopology {
  // IPUs in clockwise direction around the ring starting at IPU 0.
  std::vector<unsigned> ringIndexToRank;
  std::vector<unsigned> rankToRingIndex;

public:
  RingTopology(unsigned n) {
    ringIndexToRank = createRing(n);
    rankToRingIndex = invertPermutation(ringIndexToRank);
  }

  /// Return the number of IPU that is the specified number of steps in the
  /// specified direction around the ring, starting at the specified base
  /// IPU.
  unsigned getRank(unsigned base, Direction direction, unsigned steps) const {
    auto numRanks = ringIndexToRank.size();
    auto index = rankToRingIndex[base];
    switch (direction) {
    case CLOCKWISE:
      index = (index + steps) % numRanks;
      break;
    case ANTICLOCKWISE:
      steps = steps % numRanks;
      index = (index + numRanks - steps) % numRanks;
      break;
    }
    return ringIndexToRank[index];
  }
};

} // End anonymous namespace.

static CollectiveOptions
parseCollectiveOptions(const poplar::OptionFlags &optionFlags) {
  CollectiveOptions options;
  using poplibs::OptionHandler;
  using poplibs::OptionSpec;
  const OptionSpec spec{
    { "method",
      OptionHandler::createWithEnum(
        options.method,
        {
          { "auto", CollectiveMethod::AUTO },
          { "clockwise_ring", CollectiveMethod::CLOCKWISE_RING },
          { "anticlockwise_ring", CollectiveMethod::ANTICLOCKWISE_RING },
          { "bidirectional_ring_pair",
            CollectiveMethod::BIDIRECTIONAL_RING_PAIR },
          { "meet_in_middle_ring", CollectiveMethod::MEET_IN_MIDDLE_RING }
        }
      )
    },
    { "useReplicatedImplementation",
      OptionHandler::createWithBool(options.useReplicatedImplementation)
    }
  };
  for (const auto &entry : optionFlags) {
    spec.parse(entry.first, entry.second);
  }
  return options;
}

// All the operations in the all reduce (splitIntoFragments and
// concat model parallel chunks) aim to preserve the order of the tensor
// on the ipu and only perform transforms of elements on different ipus.
// This means that when ever we get the elements of a tensor on an ipu (which
// uses this mapping) as long as we ensure that when creating the tensor
// the on ipu elements order is preserved then the final tensor's order will
// be preserved. This function returns the intervals of the tensor on each ipu
// ordered by the intervals
static std::vector<std::vector<Interval>>
getIpuMapping(const Graph &graph, const Tensor &t) {
  // find all intervals on each ipu
  const auto &tileMapping = graph.getTileMapping(t);
  std::vector<std::vector<Interval>> ipuMapping(graph.getTarget().getNumIPUs());
  for (unsigned tile = 0; tile < tileMapping.size(); ++tile) {
    const unsigned ipu = tile / graph.getTarget().getTilesPerIPU();
    for (const auto &interval : tileMapping[tile]) {
      ipuMapping[ipu].push_back(interval);
    }
  }

  // sort intervals
  for (unsigned ipu = 0; ipu < ipuMapping.size(); ++ipu) {
    std::sort(ipuMapping[ipu].begin(), ipuMapping[ipu].end(),
                          [&](Interval A, Interval B) {
      return A.begin() < B.begin();
    });
  }

  // compress intervals
  std::vector<std::vector<Interval>> result(ipuMapping.size());
  for (unsigned ipu = 0; ipu < ipuMapping.size(); ++ipu) {
    for (unsigned i = 0; i < ipuMapping[ipu].size(); ++i) {
      if (result[ipu].empty() ||
          result[ipu].back().end() != ipuMapping[ipu][i].begin()) {
        result[ipu].push_back(ipuMapping[ipu][i]);
      } else {
        result[ipu].back() = Interval(result[ipu].back().begin(),
                                      ipuMapping[ipu][i].end());
      }
    }
  }
  return result;
}

static std::vector<std::size_t>
getNumElementsPerIpu(const Graph &graph, const Tensor &t) {
  const auto tilesPerIpu = graph.getTarget().getTilesPerIPU();
  const auto &tileMapping = graph.getTileMapping(t);
  std::vector<std::size_t> numElements(graph.getTarget().getNumIPUs());
  for (unsigned tile = 0; tile < tileMapping.size(); ++tile) {
    const unsigned ipu = tile / tilesPerIpu;
    for (const auto &interval : tileMapping[tile]) {
      numElements[ipu] += interval.size();
    }
  }
  return numElements;
}

static Tensor
concatSlices(const Tensor &t, Graph &graph,
             const std::vector<Interval> &intervals) {
  assert(t.rank() == 1);
  std::vector<Tensor> toConcat;
  for (const auto &interval : intervals) {
    toConcat.push_back(t.slice(interval.begin(), interval.end()));
  }
  if (toConcat.empty()) {
    return graph.addVariable(t.elementType(), {0});
  }
  return concat(toConcat);
}

// Take a tensor and return a vector of tensors where each element
// is a slice of the original tensor that spans only one ipu.
static std::vector<Tensor>
getPerIpuTensors(const Tensor &t, Graph &graph) {
  const auto ipuMapping = getIpuMapping(graph, t);
  const auto numIpus = ipuMapping.size();
  std::vector<Tensor> result;
  for (unsigned ipu = 0; ipu != numIpus; ++ipu) {
    result.push_back(concatSlices(t, graph, ipuMapping[ipu]));
  }
  return result;
}

static unsigned
getReplicationFactor(const Graph &parentGraph, const Tensor &t) {
  return const_cast<Graph&>(parentGraph).getNonReplicatedTensor(t).dim(0);
}

static CollectiveMethod
pickAllGatherMethod(const Graph &graph, const Graph &parentGraph,
                    const Tensor &toGather) {
  const auto ipusPerRank = graph.getTarget().getNumIPUs();
  const auto numRanks = getReplicationFactor(parentGraph, toGather);
  if (ipusPerRank > 1 || numRanks <= 2)
    return CollectiveMethod::CLOCKWISE_RING;
  const auto &target = graph.getTarget();
  const auto bytesPerIpu =
    toGather.numElements() * target.getTypeSize(toGather.elementType()) /
    ipusPerRank;
  // Thresholds where the BIDIRECTIONAL_RING_PAIR method starts to beat the
  // MEET_IN_MIDDLE_RING method determined experimentally.
  // TODO Lots has changed since these thresholds were set - check if they are
  // still appropriate.
  if (bytesPerIpu < 622592 ||
      (numRanks > 4 && bytesPerIpu < 2490368) ||
      (numRanks > 8 && bytesPerIpu < 19922944)) {
    return CollectiveMethod::MEET_IN_MIDDLE_RING;
  }
  return CollectiveMethod::BIDIRECTIONAL_RING_PAIR;
}

static CollectiveMethod
pickReduceScatterMethod(const Graph &graph, const Graph &parentGraph,
                        const Tensor &t, popops::Operation op) {
  const auto ipusPerRank = graph.getTarget().getNumIPUs();
  const auto numRanks = getReplicationFactor(parentGraph, t);
  if (ipusPerRank > 1 || numRanks <= 2)
    return CollectiveMethod::CLOCKWISE_RING;
  const auto &target = graph.getTarget();
  unsigned bytesPerIpu = t.numElements() *
                         target.getTypeSize(t.elementType()) / ipusPerRank;
  // Thresholds where the BIDIRECTIONAL_RING_PAIR method starts to beat the
  // MEET_IN_MIDDLE_RING method determined experimentally.
  // TODO Lots has changed since these thresholds were set - check if they are
  // still appropriate.
  if (bytesPerIpu < 1245184 ||
      (numRanks > 4 && bytesPerIpu < 4980736) ||
      (numRanks > 8 && bytesPerIpu < 39845888)) {
    return CollectiveMethod::MEET_IN_MIDDLE_RING;
  }
  return CollectiveMethod::BIDIRECTIONAL_RING_PAIR;
}

// Split a tensor into the specified number of fragments such that the
// number of elements and the IPU mapping of each fragment is identical,
// adding padding if necessary to achieve this.
static Tensor
replicatedSplitIntoFragments(const Tensor &t, unsigned numFragments,
                             Graph &graph) {
  std::vector<Tensor> perIpuFragments;
  for (auto &ipuTensor : getPerIpuTensors(t, graph)) {
    unsigned padding = (numFragments - ipuTensor.dim(0) % numFragments) %
                       numFragments;
    auto padded = pad(graph, ipuTensor, {0}, {padding}, 0.0f,
                      padding::MappingMethod::EDGE);
    auto split = padded.reshape({numFragments, padded.dim(0) / numFragments});
    perIpuFragments.push_back(split);
  }
  return concat(perIpuFragments, 1);
}

static void
replicatedRankSlice(Graph &graph, Graph &parentGraph,
                    const Tensor &src, const Tensor &dst,
                    Sequence &prog,
                    std::function<unsigned (unsigned)> mapping) {
  assert(src.rank() == dst.rank() + 1);
  assert(src[0].shape() == dst.shape());
  auto srcNonReplicated = parentGraph.getNonReplicatedTensor(src);
  auto dstNonReplicated = parentGraph.getNonReplicatedTensor(dst);
  assert(dstNonReplicated.dim(0) == srcNonReplicated.dim(0));
  unsigned replicationFactor = srcNonReplicated.dim(0);
  std::vector<Tensor> srcTensors;
  std::vector<Tensor> dstTensors;
  for (unsigned i = 0; i != replicationFactor; ++i) {
    auto sliceIndex = mapping(i);
    srcTensors.push_back(srcNonReplicated[i][sliceIndex]);
    dstTensors.push_back(dstNonReplicated[i]);
  }
  prog.add(Copy(concat(srcTensors), concat(dstTensors)));
}

static void
replicatedRankUpdate(Graph &graph, Graph &parentGraph,
                     const Tensor &src, const Tensor &dst,
                     Sequence &prog,
                     std::function<unsigned (unsigned)> mapping) {
  assert(dst.rank() == src.rank() + 1);
  assert(dst[0].shape() == src.shape());
  auto srcNonReplicated = parentGraph.getNonReplicatedTensor(src);
  auto dstNonReplicated = parentGraph.getNonReplicatedTensor(dst);
  assert(dstNonReplicated.dim(0) == srcNonReplicated.dim(0));
  unsigned replicationFactor = srcNonReplicated.dim(0);
  std::vector<Tensor> srcTensors;
  std::vector<Tensor> dstTensors;
  for (unsigned i = 0; i != replicationFactor; ++i) {
    auto sliceIndex = mapping(i);
    srcTensors.push_back(srcNonReplicated[i]);
    dstTensors.push_back(dstNonReplicated[i][sliceIndex]);
  }
  prog.add(Copy(concat(srcTensors), concat(dstTensors)));
}

static void
crossReplicaCopy(Graph &graph, Graph &parentGraph, const Tensor &src,
                 const Tensor &dst, Sequence &prog,
                 std::function<unsigned (unsigned)> mapping) {
  assert(src.shape() == dst.shape());
  unsigned replicationFactor = parentGraph.getNonReplicatedTensor(src).dim(0);
  std::map<unsigned, unsigned> replicaMap;
  for (unsigned i = 0; i != replicationFactor; ++i) {
    replicaMap.emplace(i, mapping(i));
  }
  prog.add(CrossReplicaCopy(src, dst, replicaMap));
}

static void
opInPlace(Graph &graph, popops::Operation op, const Tensor &a,
          const Tensor &b, Sequence &prog, const std::string &debugPrefix) {
  using namespace popops::expr;
  switch (op) {
  case Operation::ADD: return addInPlace(graph, a, b, prog, debugPrefix);
  case Operation::MUL: return mulInPlace(graph, a, b, prog, debugPrefix);
  case Operation::MIN:
    return minInPlace(graph, a, b, prog, debugPrefix);
  case Operation::MAX:
    return maxInPlace(graph, a, b, prog, debugPrefix);
  case Operation::LOGICAL_AND:
    return logicalAndInPlace(graph, a, b, prog, debugPrefix);
  case Operation::LOGICAL_OR:
    return logicalOrInPlace(graph, a, b, prog, debugPrefix);
  case Operation::SQUARE_ADD:
    throw poputil::poplibs_error("Collective reduction using the SQUARE_ADD "
                                 "operation is not yet supported");
  }
}

// Map a buffer so each element is mapped to the same IPU as the
// corresponding elements in the fragments.
static void mapBuffer(Graph &graph,
                      const Tensor &buffer,
                      const Tensor &fragments) {
  assert(buffer.numElements() == fragments[0].numElements());
  // The IPU mapping of all fragments should be identical so we only need
  // to look at the first fragment.
  auto ipuMapping = getIpuMapping(graph, fragments[0]);
  const auto numIpus = ipuMapping.size();
  unsigned tilesPerIpu = graph.getTarget().getTilesPerIPU();
  for (unsigned ipu = 0; ipu != numIpus; ++ipu) {
    auto virtualGraph = graph.createVirtualGraph(ipu * tilesPerIpu,
                                                 (ipu + 1) * tilesPerIpu);
    poputil::mapTensorLinearly(virtualGraph,
                               concatSlices(buffer, virtualGraph,
                                            ipuMapping[ipu]));
  }
}

static Tensor
unidirectionalRingReduceScatter(Graph &graph, Graph &parentGraph,
                                const Tensor &toReduce,
                                popops::Operation op,
                                Direction direction,
                                Sequence &prog,
                                const std::string &debugPrefix) {
  const auto replicationFactor = getReplicationFactor(parentGraph, toReduce);
  if (replicationFactor == 1) {
    return toReduce;
  }
  RingTopology ring(replicationFactor);
  auto numFragments = replicationFactor;
  auto numSteps = replicationFactor;
  auto fragments =
      replicatedSplitIntoFragments(toReduce, numFragments, graph);
  auto fragmentSize = fragments.dim(1);
  auto srcBuffer = graph.addVariable(toReduce.elementType(), {fragmentSize});
  mapBuffer(graph, srcBuffer, fragments);
  auto dstBuffer = graph.clone(srcBuffer);
  Sequence reduceProg;
  opInPlace(graph, op, srcBuffer, dstBuffer, reduceProg,
            debugPrefix + "/Reduce");
  for (unsigned step = 0; step != numSteps; ++step) {
    if (step != 0) {
      crossReplicaCopy(graph, parentGraph, srcBuffer, dstBuffer, prog,
                       [&](unsigned src) {
        return ring.getRank(src, direction, 1);
      });
    }
    replicatedRankSlice(graph, parentGraph, fragments, srcBuffer, prog,
                        [&](unsigned rank) {
      auto stepsRemaining = numSteps - 1 - step;
      return ring.getRank(rank, direction, stepsRemaining);
    });
    if (step != 0) {
      prog.add(reduceProg);
    }
  }
  return srcBuffer;
}

static Tensor
bidirectionalRingPairReduceScatter(Graph &graph, Graph &parentGraph,
                                   const Tensor &toReduce,
                                   popops::Operation op,
                                   Sequence &prog,
                                   const std::string &debugPrefix) {
  const auto replicationFactor = getReplicationFactor(parentGraph, toReduce);
  if (replicationFactor == 1) {
    return toReduce;
  }
  RingTopology ring(replicationFactor);
  auto numFragments = replicationFactor;
  auto numSteps = replicationFactor;
  auto fragments =
      replicatedSplitIntoFragments(toReduce, numFragments, graph);
  auto fragmentSize = fragments.dim(1);
  auto clockwiseFragments = fragments.slice(0, fragmentSize / 2, 1);
  auto anticlockwiseFragments =
    fragments.slice(fragmentSize / 2, fragmentSize, 1);
  auto srcBuffer = graph.addVariable(toReduce.elementType(), {fragmentSize});
  mapBuffer(graph, srcBuffer, fragments);
  auto dstBuffer = graph.clone(srcBuffer);
  auto clockwiseSrcBuffer = srcBuffer.slice(0, fragmentSize / 2);
  auto anticlockwiseSrcBuffer = srcBuffer.slice(fragmentSize / 2, fragmentSize);
  auto clockwiseDstBuffer = dstBuffer.slice(0, fragmentSize / 2);
  auto anticlockwiseDstBuffer = dstBuffer.slice(fragmentSize / 2, fragmentSize);
  Sequence reduceProg;
  opInPlace(graph, op, srcBuffer, dstBuffer, reduceProg,
            debugPrefix + "/Reduce");
  for (unsigned step = 0; step != numSteps; ++step) {
    if (step != 0) {
      crossReplicaCopy(graph, parentGraph, clockwiseSrcBuffer,
                       clockwiseDstBuffer, prog, [&](unsigned src) {
        return ring.getRank(src, CLOCKWISE, 1);
      });
      crossReplicaCopy(graph, parentGraph, anticlockwiseSrcBuffer,
                       anticlockwiseDstBuffer, prog, [&](unsigned src) {
        return ring.getRank(src, ANTICLOCKWISE, 1);
      });
    }
    replicatedRankSlice(graph, parentGraph, clockwiseFragments,
                        clockwiseSrcBuffer, prog, [&](unsigned rank) {
      auto stepsRemaining = numSteps - 1 - step;
      return ring.getRank(rank, CLOCKWISE, stepsRemaining);
    });
    replicatedRankSlice(graph, parentGraph, anticlockwiseFragments,
                        anticlockwiseSrcBuffer, prog, [&](unsigned rank) {
      auto stepsRemaining = numSteps - 1 - step;
      return ring.getRank(rank, ANTICLOCKWISE, stepsRemaining);
    });
    if (step != 0) {
      prog.add(reduceProg);
    }
  }
  return srcBuffer.flatten();
}

static Tensor
ringMeetInMiddleReduceScatter(Graph &graph, Graph &parentGraph,
                              const Tensor &toReduce,
                              popops::Operation op, Sequence &prog,
                              const std::string &debugPrefix) {
  const auto replicationFactor = getReplicationFactor(parentGraph, toReduce);
  if (replicationFactor <= 2) {
    return unidirectionalRingReduceScatter(graph, parentGraph, toReduce,
                                           op, CLOCKWISE, prog, debugPrefix);
  }

  RingTopology ring(replicationFactor);
  auto numFragments = replicationFactor;
  auto numSteps = 1 + numFragments / 2;
  auto fragments =
      replicatedSplitIntoFragments(toReduce, numFragments, graph);
  auto fragmentSize = fragments.dim(1);
  auto clockwiseSrcBuffer =
      graph.addVariable(toReduce.elementType(), {fragmentSize});
  mapBuffer(graph, clockwiseSrcBuffer, fragments);
  auto clockwiseDstBuffer = graph.clone(clockwiseSrcBuffer);
  auto anticlockwiseSrcBuffer = graph.clone(clockwiseSrcBuffer);
  auto anticlockwiseDstBuffer = graph.clone(anticlockwiseSrcBuffer);
  Sequence reduceProg;
  opInPlace(graph, op, concat(clockwiseSrcBuffer, anticlockwiseSrcBuffer),
            concat(clockwiseDstBuffer, anticlockwiseDstBuffer), reduceProg,
            debugPrefix + "/Reduce");
  for (unsigned step = 0; step != numSteps; ++step) {
    if (step != 0) {
      if (step != numSteps - 1) {
        crossReplicaCopy(graph, parentGraph, clockwiseSrcBuffer,
                         clockwiseDstBuffer, prog, [&](unsigned src) {
          return ring.getRank(src, CLOCKWISE, 1);
        });
      }
      crossReplicaCopy(graph, parentGraph, anticlockwiseSrcBuffer,
                       anticlockwiseDstBuffer, prog, [&](unsigned src) {
        return ring.getRank(src, ANTICLOCKWISE, 1);
      });
    }
    if (step == numSteps - 1) {
      opInPlace(graph, op, clockwiseSrcBuffer, anticlockwiseDstBuffer, prog,
                debugPrefix + "/Step" + std::to_string(step));
    } else {
      replicatedRankSlice(graph, parentGraph, fragments, clockwiseSrcBuffer,
                          prog, [&](unsigned rank) {
        auto clockwiseStepsRemaining = numSteps - 2 - step;
        return ring.getRank(rank, CLOCKWISE, clockwiseStepsRemaining);
      });
      replicatedRankSlice(graph, parentGraph, fragments, anticlockwiseSrcBuffer,
                          prog, [&](unsigned rank) {
        auto anticlockwiseStepsRemaining = numSteps - 1 - step;
        return ring.getRank(rank, ANTICLOCKWISE, anticlockwiseStepsRemaining);
      });
      if (step != 0) {
        prog.add(reduceProg);
      }
    }
  }
  return clockwiseSrcBuffer;
}

static Tensor
reduceScatter(Graph &graph, Graph &parentGraph, const Tensor &toReduce,
              popops::Operation op, Sequence &prog,
              const std::string &debugPrefix,
              const CollectiveOptions &options) {
  CollectiveMethod method = options.method;
  if (method == CollectiveMethod::AUTO) {
    method = pickReduceScatterMethod(graph, parentGraph, toReduce, op);
  }
  switch (method) {
  default: assert(0 && "Unexpected reduce method");
  case CollectiveMethod::CLOCKWISE_RING:
    return unidirectionalRingReduceScatter(graph, parentGraph, toReduce, op,
                                           CLOCKWISE, prog, debugPrefix);
  case CollectiveMethod::ANTICLOCKWISE_RING:
    return unidirectionalRingReduceScatter(graph, parentGraph, toReduce, op,
                                           ANTICLOCKWISE, prog, debugPrefix);
  case CollectiveMethod::BIDIRECTIONAL_RING_PAIR:
    return bidirectionalRingPairReduceScatter(graph, parentGraph, toReduce, op,
                                              prog, debugPrefix);
  case CollectiveMethod::MEET_IN_MIDDLE_RING:
    return ringMeetInMiddleReduceScatter(graph, parentGraph, toReduce, op, prog,
                                         debugPrefix);
  }
}

// Return the tile the last element of a tensor is mapped to.
static unsigned getTileOfLastElement(Graph &graph, const Tensor &t) {
  const auto numElements = t.numElements();
  assert(numElements > 0);
  auto last = t.flatten()[numElements - 1];
  auto tileMapping = graph.getTileMapping(last);
  for (unsigned tile = 0; tile != tileMapping.size(); ++tile) {
    if (!tileMapping[tile].empty())
      return tile;
  }
  POPLIB_UNREACHABLE();
}

// Add padding to the reference tensor so the number of elements on each
// IPU is equal to the number of elements of the fragment that is on that
// IPU times the number of fragments.
static Tensor padReference(Graph &graph, const Tensor &fragment,
                           unsigned numFragments, const Tensor &reference) {
  auto fragmentElementsPerIpu = getNumElementsPerIpu(graph, fragment);
  auto referencePerIpu = getPerIpuTensors(reference, graph);
  const auto numIpus = fragmentElementsPerIpu.size();
  assert(referencePerIpu.size() == numIpus);
  std::vector<Tensor> toConcat = { reference };
  for (unsigned ipu = 0; ipu != numIpus; ++ipu) {
    const auto referenceElements = referencePerIpu[ipu].numElements();
    const auto fragmentElements = fragmentElementsPerIpu[ipu];
    unsigned paddingElements =
        fragmentElements * numFragments - referenceElements;
    if (paddingElements > 0) {
      auto padding = graph.addConstant(reference.elementType(),
                                       {paddingElements}, 0.0f);
      auto tile = getTileOfLastElement(graph, referencePerIpu[ipu]);
      graph.setTileMapping(padding, tile);
      toConcat.push_back(padding);
    }
  }
  return concat(toConcat);
}

static Tensor
unidirectionalRingAllGather(Graph &graph, Graph &parentGraph,
                            const Tensor &toGather,
                            Direction direction,
                            const Tensor &reference,
                            Sequence &prog,
                            const std::string &debugPrefix) {
  const auto replicationFactor = getReplicationFactor(parentGraph, toGather);
  if (replicationFactor == 1) {
    return toGather;
  }
  RingTopology ring(replicationFactor);
  auto numFragments = replicationFactor;
  auto numSteps = replicationFactor;
  auto srcBuffer = graph.clone(toGather);
  auto dstBuffer = graph.clone(toGather);
  auto referencePadded = padReference(graph, toGather, numFragments,
                                      reference);
  auto localData = graph.clone(referencePadded);
  auto fragments = replicatedSplitIntoFragments(localData, numFragments,
                                                graph);
  assert(fragments.dim(1) == toGather.numElements());
  for (unsigned step = 0; step != numSteps; ++step) {
    if (step == 0) {
      prog.add(Copy(toGather, dstBuffer));
    } else {
      crossReplicaCopy(graph, parentGraph, srcBuffer, dstBuffer, prog,
                       [&](unsigned src) {
        return ring.getRank(src, direction, 1);
      });
    }
    replicatedRankUpdate(graph, parentGraph, dstBuffer, fragments, prog,
                        [&](unsigned rank) {
      return ring.getRank(rank, opposite(direction), step);
    });
    std::swap(srcBuffer, dstBuffer);
  }
  return localData.slice(0, reference.numElements());
}

static Tensor
bidirectionalRingPairAllGather(Graph &graph, Graph &parentGraph,
                               const Tensor &toGather,
                               const Tensor &reference,
                               Sequence &prog,
                               const std::string &debugPrefix) {
  const auto replicationFactor = getReplicationFactor(parentGraph, toGather);
  if (replicationFactor == 1) {
    return toGather;
  }
  RingTopology ring(replicationFactor);
  auto numFragments = replicationFactor;
  auto fragmentSize = toGather.numElements();
  auto numSteps = replicationFactor;
  auto srcBuffer = graph.clone(toGather);
  auto dstBuffer = graph.clone(toGather);
  auto referencePadded = padReference(graph, toGather, numFragments,
                                      reference);
  auto localData = graph.clone(referencePadded);
  auto fragments = replicatedSplitIntoFragments(localData, numFragments,
                                                graph);
  auto clockwiseFragments = fragments.slice(0, fragmentSize / 2, 1);
  auto anticlockwiseFragments = fragments.slice(fragmentSize / 2,
                                                fragmentSize, 1);
  for (unsigned step = 0; step != numSteps; ++step) {
    auto clockwiseSrcBuffer = srcBuffer.slice(0, fragmentSize / 2);
    auto anticlockwiseSrcBuffer =
        srcBuffer.slice(fragmentSize / 2, fragmentSize);
    auto clockwiseDstBuffer = dstBuffer.slice(0, fragmentSize / 2);
    auto anticlockwiseDstBuffer =
        dstBuffer.slice(fragmentSize / 2, fragmentSize);
    if (step == 0) {
      prog.add(Copy(toGather, dstBuffer));
    } else {
      crossReplicaCopy(graph, parentGraph, clockwiseSrcBuffer,
                       clockwiseDstBuffer, prog, [&](unsigned src) {
        return ring.getRank(src, CLOCKWISE, 1);
      });
      crossReplicaCopy(graph, parentGraph, anticlockwiseSrcBuffer,
                       anticlockwiseDstBuffer, prog, [&](unsigned src) {
        return ring.getRank(src, ANTICLOCKWISE, 1);
      });
    }
    replicatedRankUpdate(graph, parentGraph, clockwiseDstBuffer,
                         clockwiseFragments, prog, [&](unsigned rank) {
      return ring.getRank(rank, ANTICLOCKWISE, step);
    });
    replicatedRankUpdate(graph, parentGraph, anticlockwiseDstBuffer,
                         anticlockwiseFragments, prog, [&](unsigned rank) {
      return ring.getRank(rank, CLOCKWISE, step);
    });
    std::swap(srcBuffer, dstBuffer);
  }
  return localData.slice(0, reference.numElements());
}

static Tensor
ringMeetInMiddleAllGather(Graph &graph, Graph &parentGraph,
                          const Tensor &toGather,
                          const Tensor &reference,
                          Sequence &prog,
                          const std::string &debugPrefix) {
  const auto replicationFactor = getReplicationFactor(parentGraph, toGather);
  if (replicationFactor == 1) {
    return toGather;
  }
  RingTopology ring(replicationFactor);
  auto numFragments = replicationFactor;
  auto numSteps = 1 + replicationFactor / 2;
  auto clockwiseSrcBuffer = graph.clone(toGather);
  auto anticlockwiseSrcBuffer = graph.clone(toGather);
  auto clockwiseDstBuffer = graph.clone(toGather);
  auto anticlockwiseDstBuffer = graph.clone(toGather);
  auto referencePadded = padReference(graph, toGather, numFragments,
                                      reference);
  auto localData = graph.clone(referencePadded);
  auto fragments = replicatedSplitIntoFragments(localData, numFragments,
                                                graph);
  for (unsigned step = 0; step != numSteps; ++step) {
    if (step == 0) {
      prog.add(Copy(toGather, clockwiseDstBuffer));
      prog.add(Copy(toGather, anticlockwiseDstBuffer));
    } else {
      if (step != numSteps - 1) {
        crossReplicaCopy(graph, parentGraph, clockwiseSrcBuffer,
                         clockwiseDstBuffer, prog, [&](unsigned src) {
          return ring.getRank(src, CLOCKWISE, 1);
        });
      }
      crossReplicaCopy(graph, parentGraph, anticlockwiseSrcBuffer,
                       anticlockwiseDstBuffer, prog, [&](unsigned src) {
        return ring.getRank(src, ANTICLOCKWISE, 1);
      });
    }
    if (step != numSteps - 1) {
      replicatedRankUpdate(graph, parentGraph, clockwiseDstBuffer, fragments,
                           prog, [&](unsigned rank) {
        return ring.getRank(rank, ANTICLOCKWISE, step);
      });
    }
    replicatedRankUpdate(graph, parentGraph, anticlockwiseDstBuffer, fragments,
                         prog, [&](unsigned rank) {
      return ring.getRank(rank, CLOCKWISE, step);
    });
    std::swap(clockwiseSrcBuffer, clockwiseDstBuffer);
    std::swap(anticlockwiseSrcBuffer, anticlockwiseDstBuffer);
  }
  return localData.slice(0, reference.numElements());
}

static Tensor
allGather(Graph &graph, Graph &parentGraph, const Tensor &toGather,
          const Tensor &reference, Sequence &prog,
          const std::string &debugPrefix, const CollectiveOptions &options) {
  CollectiveMethod method = options.method;
  if (method == CollectiveMethod::AUTO) {
    method = pickAllGatherMethod(graph, parentGraph, toGather);
  }
  switch (method) {
  default: assert(0 && "Unexpected reduce method");
  case CollectiveMethod::CLOCKWISE_RING:
    return unidirectionalRingAllGather(graph, parentGraph, toGather, CLOCKWISE,
                                       reference, prog, debugPrefix);
  case CollectiveMethod::ANTICLOCKWISE_RING:
    return unidirectionalRingAllGather(graph, parentGraph, toGather,
                                       ANTICLOCKWISE, reference, prog,
                                       debugPrefix);
  case CollectiveMethod::BIDIRECTIONAL_RING_PAIR:
    return bidirectionalRingPairAllGather(graph, parentGraph, toGather,
                                          reference, prog, debugPrefix);
  case CollectiveMethod::MEET_IN_MIDDLE_RING:
    return ringMeetInMiddleAllGather(graph, parentGraph, toGather, reference,
                                     prog, debugPrefix);
  }
}

Tensor
replicatedAllReduce(Graph &graph, Graph &parentGraph,
                    const poplar::Tensor &data,
                    popops::Operation op,
                    program::Sequence &prog,
                    const std::string &debugPrefix,
                    const poplar::OptionFlags &optionFlags) {
  auto options = parseCollectiveOptions(optionFlags);
  auto result = graph.clone(data);
  auto dataReordered = data.flatten();
  auto resultReordered = result.flatten();
  graph.reorderToSimplify(&dataReordered, {&resultReordered});
  if (options.useReplicatedImplementation) {
    auto reduceScattered =
        reduceScatter(graph, parentGraph, dataReordered, op, prog, debugPrefix,
                      options);
    auto reduceGathered =
        allGather(graph, parentGraph, reduceScattered, dataReordered, prog,
                  debugPrefix, options);
    prog.add(Copy(reduceGathered, resultReordered));
  } else {
    auto reduced =
        allReduce(parentGraph,
                  parentGraph.getNonReplicatedTensor(dataReordered),
                  op, prog, debugPrefix, optionFlags);
    prog.add(Copy(reduced,
                  parentGraph.getNonReplicatedTensor(resultReordered)));
  }
  return result;
}

} // End namespace popops