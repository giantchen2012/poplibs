#include "popops/Collectives.hpp"

#include "poplibs_support/OptionParsing.hpp"
#include "popops/ElementWise.hpp"
#include "popops/Reduce.hpp"
#include "poputil/TileMapping.hpp"
#include "poputil/Util.hpp"
#include "poputil/exceptions.hpp"
#include <cassert>

using namespace poplar;
using namespace poplar::program;

namespace popops {

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
}

static CollectiveMethod
parseCollectiveOptions(const poplar::OptionFlags &options) {
  CollectiveMethod method = CollectiveMethod::AUTO;
  using poplibs::OptionHandler;
  using poplibs::OptionSpec;
  const OptionSpec spec{
    { "method",
      OptionHandler::createWithEnum(
        method,
        {
          { "auto", CollectiveMethod::AUTO },
          { "clockwise_ring", CollectiveMethod::CLOCKWISE_RING },
          { "anticlockwise_ring", CollectiveMethod::ANTICLOCKWISE_RING },
          { "bidirectional_ring_pair",
            CollectiveMethod::BIDIRECTIONAL_RING_PAIR },
          { "meet_in_middle_ring", CollectiveMethod::MEET_IN_MIDDLE_RING }
        }
      )
    }
  };
  for (const auto &entry : options) {
    spec.parse(entry.first, entry.second);
  }
  return method;
}

static std::vector<Tensor>
splitIntoFragments(const Tensor &t, unsigned numFragments) {
  assert(t.rank() == 1);
  unsigned numElements = t.dim(0);
  // Fragments indexed by ipu.
  std::vector<Tensor> fragments;
  fragments.reserve(numFragments);
  for (unsigned fragment = 0; fragment != numFragments; ++fragment) {
    auto elementBegin = (numElements * fragment) / numFragments;
    auto elementEnd = std::min(numElements,
                               (numElements * (fragment + 1)) / numFragments);
    fragments.push_back(t.slice(elementBegin, elementEnd));
  }
  return fragments;
}

// Return the IPUs in clockwise direction around the ring starting at IPU 0.
static std::vector<unsigned> getIpusInRing(int numIPUs) {
  std::vector<unsigned> ring;
  int ipu = 0;
  for (; ipu < numIPUs; ipu += 2) {
    ring.push_back(ipu);
  }
  ipu -= 1;
  if (ipu == numIPUs) {
    ipu -=2;
    assert(ipu < numIPUs);
  }
  for (; ipu >= 0; ipu -= 2) {
    ring.push_back(ipu);
  }
  return ring;
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

static void
ringReduceScatterStep(Graph &graph,
                      const std::vector<std::vector<Tensor>> &fragments,
                      const std::vector<unsigned> &ipuRing,
                      unsigned step,
                      std::vector<Tensor> &data,
                      std::vector<Tensor> &copySrcs,
                      std::vector<Tensor> &copyDsts,
                      std::vector<Tensor> &addOp0,
                      std::vector<Tensor> &addOp1,
                      bool clockwise) {
  const auto numIpus = graph.getTarget().getNumIPUs();
  const auto numFragments = fragments.size();
  assert(numFragments == numIpus);
  const auto numSteps = numFragments - 1;
  assert(step < numSteps);
  std::vector<Tensor> nextData(numIpus);
  for (unsigned i = 0; i != numIpus; ++i) {
    unsigned fragmentNum;
    unsigned recvIndex;
    if (clockwise) {
      // At each step the IPU at index N in the ring receives data from the
      // previous IPU in the ring and reduces it with a local fragment and
      // sends the data it reduced in the previous step to the next IPU in the
      // ring. We want the IPU at index N to have the reduced result for the
      // N'th fragment after the final step and so working backwards in step
      // (numSteps - 1 - x) the IPU at index (N - x) % numIpus should reduce
      // the data it receives with the N'th fragment. This can be rearranged
      // to give the following expression for the fragment that the IPU at
      // index i should reduce at each step:
      fragmentNum = (i + numSteps - 1 - step) % numIpus;
      recvIndex = (i + numIpus - 1) % numIpus;
    } else {
      // At each step the IPU at index N in the ring receives data from the
      // next IPU in the ring and reduces it with local fragment and sends
      // the data it reduced in the previous step to the previous IPU in the
      // ring. In step (numSteps - 1 - x) the IPU at index (N + x) % numIpus
      // should reduce the data it receives with the N'th fragment. Again this
      // can be rearranged to give the following expression for the fragment
      // that the IPU at index i should reduce at each step:
      fragmentNum = (i - (numSteps - 1 - step)) % numIpus;
      recvIndex = (i + 1) % numIpus;
    }
    auto ipu = ipuRing[i];
    auto recvIpu = ipuRing[recvIndex];
    auto copySrc = step == 0 ? fragments[recvIpu][fragmentNum] :
                               data[recvIndex];
    auto copyDst = poputil::cloneToIpu(graph, copySrc, ipu);
    copySrcs.push_back(copySrc);
    copyDsts.push_back(copyDst);
    addOp0.push_back(copyDst);
    addOp1.push_back(fragments[ipu][fragmentNum]);
    nextData[i] = copyDst;
  }
  data = nextData;
}

static void
meetInMiddleReduceScatterStep(Graph &graph,
                              const std::vector<std::vector<Tensor>> &fragments,
                              const std::vector<unsigned> &ipuRing,
                              unsigned step,
                              std::vector<Tensor> &clockwiseData,
                              std::vector<Tensor> &anticlockwiseData,
                              std::vector<Tensor> &copySrcs,
                              std::vector<Tensor> &copyDsts,
                              std::vector<Tensor> &addOp0,
                              std::vector<Tensor> &addOp1) {
  // The slice that ends up on a single IPU is calculated as follows:
  // In the first step:
  // - The IPU (numIpus - 1) / 2 hops in the anticlockwise direction sends
  //   it's fragment clockwise.
  // - The IPU numIpus / 2 hops in the anticlockwise direction sends
  //   it's fragment clockwise.
  // In each subsequent step each IPU takes the fragment it received in
  // the previous step, adds it to the corresponding local fragment and
  // send the result to the next IPU along the ring until the data reaches
  // the final IPU. After numIPU / 2 steps all the data has reached the
  // final IPU.
  const auto numIpus = graph.getTarget().getNumIPUs();
  const auto numFragments = fragments.size();
  assert(numFragments == numIpus);
  const auto numSteps = numIpus / 2;
  assert(numIpus % 2 == 0);
  assert(numIpus > 2);
  assert(step < numSteps);
  std::vector<Tensor> nextClockwiseData(numIpus);
  std::vector<Tensor> nextAnticlockwiseData(numIpus);
  for (bool clockwise : {true, false}) {
    if (clockwise && step == numSteps - 1)
      continue;
    for (unsigned i = 0; i != numIpus; ++i) {
      unsigned fragmentNum;
      unsigned recvIndex;
      if (clockwise) {
        // At each step the IPU at index N in the ring receives data from the
        // previous IPU in the ring and reduces it with a local fragment and
        // sends the data it reduced in the previous step to the next IPU in the
        // ring. We want the IPU at index N to have the reduced result for the
        // N'th fragments sent clockwise after the penultimate step and so
        // working backwards in step (numSteps - 2 - x) the IPU at index
        // (N - x) % numIpus should reduce the data it receives with the N'th
        // fragment. This can be rearranged to give the following expression for
        // the fragment that the IPU at index i should reduce at each step:
        fragmentNum = (i + numSteps - 2 - step) % numIpus;
        recvIndex = (i + numIpus - 1) % numIpus;
      } else {
        // At each step the IPU at index N in the ring receives data from the
        // next IPU in the ring and reduces it with local fragment and sends
        // the data it reduced in the previous step to the previous IPU in the
        // ring. In step (numSteps - 1 - x) the IPU at index (N + x) % numIpus
        // should reduce the data it receives with the N'th fragment. Again this
        // can be rearranged to give the following expression for the fragment
        // that the IPU at index i should reduce at each step:
        fragmentNum = (i - (numSteps - 1 - step)) % numIpus;
        recvIndex = (i + 1) % numIpus;
      }
      std::vector<Tensor> &data = clockwise ? clockwiseData : anticlockwiseData;
      auto ipu = ipuRing[i];
      auto recvIpu = ipuRing[recvIndex];
      auto copySrc = step == 0 ? fragments[recvIpu][fragmentNum] :
                                 data[recvIndex];
      auto copyDst = poputil::cloneToIpu(graph, copySrc, ipu);
      copySrcs.push_back(copySrc);
      copyDsts.push_back(copyDst);
      addOp0.push_back(copyDst);
      if (step == numSteps - 1) {
        assert(!clockwise);
        addOp1.push_back(clockwiseData[i]);
      } else {
        addOp1.push_back(fragments[ipu][fragmentNum]);
      }
      std::vector<Tensor> &nextData = clockwise ? nextClockwiseData :
                                                  nextAnticlockwiseData;
      nextData[i] = copyDst;
    }
  }
  clockwiseData = nextClockwiseData;
  anticlockwiseData = nextAnticlockwiseData;
}

// Perform a collective reduce scatter operation.
static std::vector<Chunk>
unidirectionalRingReduceScatter(Graph &graph, Tensor toReduce,
                                popops::Operation op, Sequence &prog,
                                bool clockwise,
                                const std::string &debugPrefix) {
  const auto numIpus = graph.getTarget().getNumIPUs();
  assert(toReduce.dim(0) == numIpus);
  if (numIpus == 1) {
    return { {poputil::duplicate(graph, toReduce[0], prog), 0} };
  }
  const auto numFragments = numIpus;
  std::vector<std::vector<Tensor>> fragments;
  for (unsigned i = 0; i != numIpus; ++i) {
    fragments.push_back(splitIntoFragments(toReduce[i], numFragments));
  }

  auto ipuRing = getIpusInRing(numIpus);
  // Temporary data indexed by the position in the ring.
  std::vector<Tensor> data;
  const auto numSteps = ipuRing.size() - 1;
  for (unsigned step = 0; step != numSteps; ++step) {
    std::vector<Tensor> copySrcs;
    std::vector<Tensor> copyDsts;
    std::vector<Tensor> addOp0;
    std::vector<Tensor> addOp1;
    ringReduceScatterStep(graph, fragments, ipuRing, step, data,
                          copySrcs, copyDsts, addOp0, addOp1, clockwise);
    prog.add(Copy(concat(copySrcs), concat(copyDsts)));
    opInPlace(graph, op, concat(addOp0), concat(addOp1), prog,
              debugPrefix + "/Step" + std::to_string(step));
  }
  std::vector<Chunk> chunks(numIpus);
  for (unsigned i = 0; i != numIpus; ++i) {
    const auto ipu = ipuRing[i];
    chunks[ipu] = {data[i], i};
  }
  return chunks;
}

static std::vector<Chunk>
ringMeetInMiddleReduceScatter(Graph &graph, const Tensor &toReduce,
                              popops::Operation op, Sequence &prog,
                              const std::string &debugPrefix) {
  const auto numIpus = graph.getTarget().getNumIPUs();
  assert(toReduce.dim(0) == numIpus);
  if (numIpus == 1) {
    return { {poputil::duplicate(graph, toReduce[0], prog), 0} };
  }
  if (numIpus == 2) {
    return unidirectionalRingReduceScatter(graph, toReduce,
                                           op, prog, true, debugPrefix);
  }
  const auto numFragments = numIpus;
  std::vector<std::vector<Tensor>> fragments;
  for (unsigned i = 0; i != numIpus; ++i) {
    fragments.push_back(splitIntoFragments(toReduce[i], numFragments));
  }

  auto ipuRing = getIpusInRing(numIpus);
  std::vector<Tensor> clockwiseData;
  std::vector<Tensor> anticlockwiseData;
  const auto numSteps = numIpus / 2;
  for (unsigned step = 0; step != numSteps; ++step) {
    std::vector<Tensor> copySrcs;
    std::vector<Tensor> copyDsts;
    std::vector<Tensor> addOp0;
    std::vector<Tensor> addOp1;
    meetInMiddleReduceScatterStep(graph, fragments, ipuRing, step,
                                  clockwiseData, anticlockwiseData, copySrcs,
                                  copyDsts, addOp0, addOp1);
    prog.add(Copy(concat(copySrcs), concat(copyDsts)));
    opInPlace(graph, op, concat(addOp0), concat(addOp1), prog,
              debugPrefix + "/Step" + std::to_string(step));
  }
  std::vector<Chunk> chunks(numIpus);
  for (unsigned i = 0; i != numIpus; ++i) {
    const auto ipu = ipuRing[i];
    chunks[ipu] = {anticlockwiseData[i], i};
  }
  return chunks;
}

static std::vector<Chunk>
bidirectionalRingPairReduceScatter(Graph &graph, const Tensor &toReduce,
                                   popops::Operation op, Sequence &prog,
                                   const std::string &debugPrefix) {
  const auto numIpus = graph.getTarget().getNumIPUs();
  assert(toReduce.dim(0) == numIpus);
  if (numIpus == 1) {
    return { {poputil::duplicate(graph, toReduce[0], prog), 0} };
  }
  const auto numFragments = numIpus * 2;
  std::vector<std::vector<Tensor>> fragments;
  for (unsigned i = 0; i != numIpus; ++i) {
    fragments.push_back(splitIntoFragments(toReduce[i], numFragments));
  }
  // Split the fragments into two sets - even fragments are reduced
  // clockwise around the ring and odd fragments are reduced anticlockwise
  // around the ring.
  std::vector<std::vector<Tensor>> clockwiseFragments(numIpus);
  std::vector<std::vector<Tensor>> anticlockwiseFragments(numIpus);
  for (unsigned i = 0; i != numFragments; i +=2) {
    for (unsigned ipu = 0; ipu != numIpus; ++ipu) {
      clockwiseFragments[ipu].push_back(fragments[ipu][i]);
      anticlockwiseFragments[ipu].push_back(fragments[ipu][i + 1]);
    }
  }

  auto ipuRing = getIpusInRing(numIpus);
  std::vector<Tensor> clockwiseData;
  std::vector<Tensor> anticlockwiseData;
  const auto numSteps = numIpus - 1;
  for (unsigned step = 0; step != numSteps; ++step) {
    std::vector<Tensor> copySrcs;
    std::vector<Tensor> copyDsts;
    std::vector<Tensor> addOp0;
    std::vector<Tensor> addOp1;
    ringReduceScatterStep(graph, clockwiseFragments, ipuRing, step,
                          clockwiseData, copySrcs, copyDsts, addOp0, addOp1,
                          true);
    ringReduceScatterStep(graph, anticlockwiseFragments, ipuRing, step,
                          anticlockwiseData, copySrcs, copyDsts, addOp0, addOp1,
                          false);
    prog.add(Copy(concat(copySrcs), concat(copyDsts)));
    opInPlace(graph, op, concat(addOp0), concat(addOp1), prog,
              debugPrefix + "/Step" + std::to_string(step));
  }
  std::vector<Tensor> data;
  for (unsigned i = 0; i != numIpus; ++i) {
    data.push_back(concat(clockwiseData[i], anticlockwiseData[i]));
  }
  std::vector<Chunk> chunks(numIpus);
  for (unsigned i = 0; i != numIpus; ++i) {
    const auto ipu = ipuRing[i];
    chunks[ipu] = {data[i], i};
  }
  return chunks;
}

static CollectiveMethod
pickReduceScatterMethod(Graph &graph, const Tensor &t,
                        popops::Operation op) {
  const auto numIpus = graph.getTarget().getNumIPUs();
  if (numIpus <= 2)
    return CollectiveMethod::CLOCKWISE_RING;
  const auto &target = graph.getTarget();
  unsigned bytesPerIpu = t.numElements() *
                         target.getTypeSize(t.elementType()) / numIpus;
  // Thresholds where the BIDIRECTIONAL_RING_PAIR method starts to beat the
  // MEET_IN_MIDDLE_RING method determined experimentally.
  if (bytesPerIpu < 1245184 ||
      (numIpus > 4 && bytesPerIpu < 4980736) ||
      (numIpus > 8 && bytesPerIpu < 39845888)) {
    return CollectiveMethod::MEET_IN_MIDDLE_RING;
  }
  return CollectiveMethod::BIDIRECTIONAL_RING_PAIR;
}

static CollectiveMethod
pickAllGatherMethod(Graph &graph, const std::vector<Chunk> &toGather) {
  const auto numIpus = graph.getTarget().getNumIPUs();
  if (numIpus <= 2)
    return CollectiveMethod::CLOCKWISE_RING;
  const auto &target = graph.getTarget();
  const auto numBytes =
    std::accumulate(toGather.begin(), toGather.end(), 0,
                    [&](unsigned n, const Chunk &c) {
      return n + c.tensor.numElements() +
             target.getTypeSize(c.tensor.elementType());
    });
  unsigned bytesPerIpu = numBytes / numIpus;
  // Thresholds where the BIDIRECTIONAL_RING_PAIR method starts to beat the
  // MEET_IN_MIDDLE_RING method determined experimentally.
  if (bytesPerIpu < 622592 ||
      (numIpus > 4 && bytesPerIpu < 2490368) ||
      (numIpus > 8 && bytesPerIpu < 19922944)) {
    return CollectiveMethod::MEET_IN_MIDDLE_RING;
  }
  return CollectiveMethod::BIDIRECTIONAL_RING_PAIR;
}

std::vector<Chunk>
reduceScatter(Graph &graph, const Tensor &toReduce, popops::Operation op,
              Sequence &prog, const std::string &debugPrefix,
              const poplar::OptionFlags &options) {
  if (toReduce.rank() != 2) {
    poputil::poplibs_error("Reduce scatter input tensor does not have rank 2");
  }
  CollectiveMethod method = parseCollectiveOptions(options);
  if (method == CollectiveMethod::AUTO) {
    method = pickReduceScatterMethod(graph, toReduce, op);
  }
  switch (method) {
  default: assert(0 && "Unexpected reduce method");
  case CollectiveMethod::CLOCKWISE_RING:
    return unidirectionalRingReduceScatter(graph, toReduce, op, prog, true,
                                           debugPrefix);
  case CollectiveMethod::ANTICLOCKWISE_RING:
    return unidirectionalRingReduceScatter(graph, toReduce, op, prog, false,
                                           debugPrefix);
  case CollectiveMethod::BIDIRECTIONAL_RING_PAIR:
    return bidirectionalRingPairReduceScatter(graph, toReduce, op, prog,
                                              debugPrefix);
  case CollectiveMethod::MEET_IN_MIDDLE_RING:
    return ringMeetInMiddleReduceScatter(graph, toReduce, op, prog,
                                         debugPrefix);
  }
}

static void
ringAllGatherStep(Graph &graph,
                  const std::vector<Chunk> &toGather,
                  const std::vector<unsigned> &ipuRing,
                  unsigned step,
                  std::vector<Chunk> &data,
                  std::vector<std::vector<Tensor>> &resultChunks,
                  std::vector<Tensor> &copySrcs,
                  std::vector<Tensor> &copyDsts,
                  bool clockwise) {
  const auto numIpus = graph.getTarget().getNumIPUs();
  std::vector<Chunk> nextData(numIpus);
  for (unsigned i = 0; i != numIpus; ++i) {
    auto ipu = ipuRing[i];
    unsigned recvIndex;
    if (clockwise) {
      recvIndex = (i + numIpus - 1) % numIpus;
    } else {
      recvIndex = (i + 1) % numIpus;
    }
    auto &copySrc = step == 0 ? toGather[ipuRing[recvIndex]] :
                                data[recvIndex];
    auto copyDst = poputil::cloneToIpu(graph, copySrc.tensor, ipu);
    copySrcs.push_back(copySrc.tensor);
    copyDsts.push_back(copyDst);
    nextData[i] = { copyDst, copySrc.index };
    resultChunks[ipu][copySrc.index] = copyDst;
  }
  data = nextData;
}

static void
ringMeetInMiddleAllGatherStep(Graph &graph,
                              const std::vector<Chunk> &toGather,
                              const std::vector<unsigned> &ipuRing,
                              unsigned step,
                              std::vector<Chunk> &clockwiseData,
                              std::vector<Chunk> &anticlockwiseData,
                              std::vector<std::vector<Tensor>> &resultChunks,
                              std::vector<Tensor> &copySrcs,
                              std::vector<Tensor> &copyDsts) {
  const auto numIpus = graph.getTarget().getNumIPUs();
  const auto numSteps = numIpus / 2;
  assert(step < numSteps);
  std::vector<Chunk> nextClockwiseData(numIpus);
  std::vector<Chunk> nextAnticlockwiseData(numIpus);
  for (unsigned i = 0; i != numIpus; ++i) {
    for (bool clockwise : {true, false}) {
      if (clockwise && step == numSteps - 1)
        continue;
      auto ipu = ipuRing[i];
      unsigned recvIndex;
      if (clockwise) {
        recvIndex = (i + numIpus - 1) % numIpus;
      } else {
        recvIndex = (i + 1) % numIpus;
      }
      auto &data = clockwise ? clockwiseData : anticlockwiseData;
      auto &copySrc = step == 0 ? toGather[ipuRing[recvIndex]] :
                                  data[recvIndex];
      auto copyDst = poputil::cloneToIpu(graph, copySrc.tensor, ipu);
      copySrcs.push_back(copySrc.tensor);
      copyDsts.push_back(copyDst);
      auto &nextData = clockwise ? nextClockwiseData : nextAnticlockwiseData;
      nextData[i] = { copyDst, copySrc.index };
      resultChunks[ipu][copySrc.index] = copyDst;
    }
  }
  clockwiseData = nextClockwiseData;
  anticlockwiseData = nextAnticlockwiseData;
}

static Tensor
unidirectionalRingAllGather(Graph &graph, const std::vector<Chunk> &toGather,
                            Sequence &prog, bool clockwise) {
  const auto numIpus = graph.getTarget().getNumIPUs();
  assert(toGather.size() == numIpus);
  auto ipuRing = getIpusInRing(numIpus);
  std::vector<std::vector<Tensor>> resultChunks(numIpus,
                                                std::vector<Tensor>(numIpus));
  for (unsigned ipu = 0 ; ipu != numIpus; ++ipu) {
    resultChunks[ipu][toGather[ipu].index] =
        poputil::duplicate(graph, toGather[ipu].tensor, prog);
  }
  const auto numSteps = ipuRing.size() - 1;
  std::vector<Chunk> data;
  for (unsigned step = 0; step != numSteps; ++step) {
    std::vector<Tensor> copySrcs;
    std::vector<Tensor> copyDsts;
    ringAllGatherStep(graph, toGather, ipuRing, step, data, resultChunks,
                      copySrcs, copyDsts, clockwise);
    prog.add(Copy(concat(copySrcs), concat(copyDsts)));
  }
  std::vector<Tensor> result;
  result.reserve(numIpus);
  for (unsigned ipu = 0; ipu != numIpus; ++ipu) {
    result.push_back(concat(resultChunks[ipu]).expand({0}));
  }
  return concat(result);
}

static Tensor
bidirectionalRingPairAllGather(Graph &graph,
                               const std::vector<Chunk> &toGather,
                               Sequence &prog) {
  const auto numIpus = graph.getTarget().getNumIPUs();
  assert(toGather.size() == numIpus);
  auto ipuRing = getIpusInRing(numIpus);
  // Split the each chunk into two parts - one part that is sent clockwise
  // around the ring and one part that is sent anticlockwise around the
  // ring.
  std::vector<Chunk> clockwiseToGather;
  std::vector<Chunk> anticlockwiseToGather;
  std::vector<std::vector<Tensor>>
      clockwiseResultChunks(numIpus, std::vector<Tensor>(numIpus));
  std::vector<std::vector<Tensor>>
      anticlockwiseResultChunks(numIpus, std::vector<Tensor>(numIpus));
  for (unsigned ipu = 0 ; ipu != numIpus; ++ipu) {
    auto fragments = splitIntoFragments(toGather[ipu].tensor, 2);
    const auto index = toGather[ipu].index;
    clockwiseToGather.push_back({fragments[0], index});
    anticlockwiseToGather.push_back({fragments[1], index});
    clockwiseResultChunks[ipu][index] =
        poputil::duplicate(graph, fragments[0], prog);
    anticlockwiseResultChunks[ipu][index] =
        poputil::duplicate(graph, fragments[1], prog);
  }
  const auto numSteps = ipuRing.size() - 1;
  std::vector<Chunk> clockwiseData, anticlockwiseData;
  for (unsigned step = 0; step != numSteps; ++step) {
    std::vector<Tensor> copySrcs;
    std::vector<Tensor> copyDsts;
    ringAllGatherStep(graph, clockwiseToGather, ipuRing, step, clockwiseData,
                      clockwiseResultChunks, copySrcs, copyDsts, true);
    ringAllGatherStep(graph, anticlockwiseToGather, ipuRing, step,
                      anticlockwiseData, anticlockwiseResultChunks, copySrcs,
                      copyDsts, false);
    prog.add(Copy(concat(copySrcs), concat(copyDsts)));
  }
  std::vector<Tensor> result;
  result.reserve(numIpus);
  for (unsigned ipu = 0; ipu != numIpus; ++ipu) {
    std::vector<Tensor> resultChunks;
    for (unsigned chunk = 0 ; chunk != numIpus; ++chunk) {
      resultChunks.push_back(clockwiseResultChunks[ipu][chunk]);
      resultChunks.push_back(anticlockwiseResultChunks[ipu][chunk]);
    }
    result.push_back(concat(resultChunks).expand({0}));
  }
  return concat(result);
}

static Tensor
ringMeetInMiddleAllGather(Graph &graph,
                          const std::vector<Chunk> &toGather,
                          Sequence &prog) {
  const auto numIpus = graph.getTarget().getNumIPUs();
  assert(toGather.size() == numIpus);
  auto ipuRing = getIpusInRing(numIpus);
  std::vector<std::vector<Tensor>> resultChunks(numIpus,
                                                std::vector<Tensor>(numIpus));
  for (unsigned ipu = 0 ; ipu != numIpus; ++ipu) {
    resultChunks[ipu][toGather[ipu].index] =
        poputil::duplicate(graph, toGather[ipu].tensor, prog);
  }
  const auto numSteps = ipuRing.size() / 2;
  std::vector<Chunk> clockwiseData, anticlockwiseData;
  for (unsigned step = 0; step != numSteps; ++step) {
    std::vector<Tensor> copySrcs;
    std::vector<Tensor> copyDsts;
    ringMeetInMiddleAllGatherStep(graph, toGather, ipuRing, step, clockwiseData,
                                  anticlockwiseData, resultChunks, copySrcs,
                                  copyDsts);
    prog.add(Copy(concat(copySrcs), concat(copyDsts)));
  }
  std::vector<Tensor> result;
  result.reserve(numIpus);
  for (unsigned ipu = 0; ipu != numIpus; ++ipu) {
    result.push_back(concat(resultChunks[ipu]).expand({0}));
  }
  return concat(result);
}

Tensor
allGather(Graph &graph, const std::vector<Chunk> &toGather,
          Sequence &prog, const std::string &,
          const poplar::OptionFlags &options) {
  const auto numChunks = toGather.size();
  for (unsigned i = 0; i != numChunks; ++i) {
    if (toGather[i].tensor.rank() != 1) {
      poputil::poplibs_error("All gather input chunk " + std::to_string(i) +
                             " does not have rank 1");
    }
  }
  CollectiveMethod method = parseCollectiveOptions(options);
  if (method == CollectiveMethod::AUTO) {
    method = pickAllGatherMethod(graph, toGather);
  }
  switch (method) {
  default: assert(0 && "Unexpected reduce method");
  case CollectiveMethod::CLOCKWISE_RING:
    return unidirectionalRingAllGather(graph, toGather, prog, true);
  case CollectiveMethod::ANTICLOCKWISE_RING:
    return unidirectionalRingAllGather(graph, toGather, prog, false);
  case CollectiveMethod::BIDIRECTIONAL_RING_PAIR:
    return bidirectionalRingPairAllGather(graph, toGather, prog);
  case CollectiveMethod::MEET_IN_MIDDLE_RING:
    return ringMeetInMiddleAllGather(graph, toGather, prog);
  }
}

poplar::Tensor
allReduce(poplar::Graph &graph, const poplar::Tensor &toReduce,
          popops::Operation op, poplar::program::Sequence &prog,
          const std::string &debugPrefix,
          const poplar::OptionFlags &options) {
  auto flattened = toReduce.flatten(1, toReduce.rank());
  auto scatteredResult = reduceScatter(graph, flattened, op, prog, debugPrefix,
                                       options);
  auto gatheredResult = allGather(graph, scatteredResult, prog, debugPrefix,
                                  options);
  return gatheredResult.reshape(toReduce.shape());
}

poplar::Tensor
replicatedAllReduce(Graph &graph, Graph &parentGraph,
                    const poplar::Tensor &data,
                    popops::Operation op,
                    program::Sequence &prog,
                    const std::string &debugPrefix,
                    const poplar::OptionFlags &options) {
  auto result = graph.clone(data);
  auto dataReordered = data.flatten();
  auto resultReordered = result.flatten();
  graph.reorderToSimplify(&dataReordered, {&resultReordered});
  auto reduced =
      allReduce(parentGraph, parentGraph.getNonReplicatedTensor(dataReordered),
                op, prog, debugPrefix, options);
  prog.add(Copy(reduced, parentGraph.getNonReplicatedTensor(resultReordered)));
  return result;
}

} // End namespace popops
