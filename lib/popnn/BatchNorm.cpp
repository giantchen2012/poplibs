#include "poputil/VertexTemplates.hpp"
#include "poputil/TileMapping.hpp"
#include "poputil/Util.hpp"
#include "popops/ElementWise.hpp"
#include "popnn/BatchNorm.hpp"
#include "popops/Reduce.hpp"
#include "popops/ScaledAdd.hpp"
#include "poplin/Convolution.hpp"
#include "poputil/exceptions.hpp"
#include <poplar/Program.hpp>
#include <poplar/Graph.hpp>
#include <poplar/Tensor.hpp>
#include <cassert>
#include <numeric>
#include <functional>
#include <map>

using namespace poplar;
using namespace poplar::program;
using namespace poputil;
using namespace popops;

namespace popnn {
namespace bn {

static void check(Tensor acts) {
  const auto rank = acts.rank();
  if (rank != 2 && rank != 4 ) {
    throw poputil::poplibs_error(
      "Batch norm supported for tensors of rank 2 or 4");
  }
}

std::size_t numChannels(Tensor acts) {
  return acts.dim(1);
}

std::size_t numActsPerChannel(Tensor acts) {
  return acts.numElements() / numChannels(acts);
}


static Tensor
createFullyConnectedBatchNormParam(Graph &graph,
                     const Tensor& acts,
                     const std::string &name) {
  const unsigned numActs = acts.shape()[1];
  const auto dType = acts.elementType();
  auto param = graph.addVariable(dType, {numActs}, name);
  mapTensorLinearly(graph, param);
  return param;
}

Tensor
createBatchNormGamma(Graph &graph, const Tensor &acts) {
  const auto rank = acts.rank();
  check(acts);
  if (rank == 4) {
    return poplin::createBatchNormGamma(graph, acts);
  } else {
    return createFullyConnectedBatchNormParam(graph, acts, "gamma");
  }
}

Tensor
createBatchNormBeta(Graph &graph, const Tensor &acts) {
  const auto rank = acts.rank();
  check(acts);
  if (rank == 4) {
    return poplin::createBatchNormBeta(graph, acts);
  } else {
    return createFullyConnectedBatchNormParam(graph, acts, "beta");
  }
}

std::pair<Tensor, Tensor>
createBatchNormParams(Graph &graph, const Tensor acts) {
  Tensor gamma = createBatchNormGamma(graph, acts);
  Tensor beta = createBatchNormBeta(graph, acts);
  return std::make_pair(gamma, beta);
}

std::pair<Tensor, Tensor>
batchNormEstimates(Graph &graph, const Tensor acts,
                   float eps,
                   Sequence &prog,
                   bool unbiasedVarEstimate,
                   const Type &partialsType,
                   const std::string &debugPrefix) {

  const auto rank = acts.rank();
  check(acts);
  if (rank == 4) {
    return poplin::batchNormEstimates(graph, acts, eps, prog,
                                      unbiasedVarEstimate, partialsType,
                                      debugPrefix);
  } else {

    const auto fnPrefix = debugPrefix + "/BN/Estimates";
    ComputeSet cs = graph.addComputeSet(fnPrefix);

    const auto &target = graph.getTarget();

    assert(acts.rank() == 2);
    const unsigned numChans = numChannels(acts);
    const auto dType = acts.elementType();

    auto mean = graph.addVariable(dType, {numChans}, "mean");
    mapTensorLinearly(graph, mean);
    auto iStdDev = graph.addVariable(dType, {numChans}, "stdDev");
    mapTensorLinearly(graph, iStdDev);

    auto actsShuf = acts.dimShuffle({1, 0});

    // Both mean and stdDev share the same mapping, so use mapping of one
    const auto mapping = graph.getTileMapping(mean);
    const auto grainSize = target.getVectorWidth(dType);

    for (auto tile = 0U; tile != mapping.size(); ++tile) {
      const auto vertexRegions =
          splitRegionsBetweenWorkers(target, mapping[tile], grainSize);

      for (const auto &regions : vertexRegions) {
        unsigned inpIdx = 0;
        unsigned num = 0;
        const auto unbiasedEstStr = unbiasedVarEstimate ? "true" : "false";
        auto v = graph.addVertex(cs,
                                 templateVertex("popnn::BatchNormEstimates",
                                                dType, partialsType,
                                                unbiasedEstStr));

        for (const auto &interval : regions) {
          const auto begin = interval.begin();
          const auto end = interval.end();

          for (auto i = begin; i != end; ++i) {
            graph.connect(v["acts"][inpIdx++], actsShuf[i].flatten());
          }
          graph.connect(v["mean"][num], mean.slice(interval));
          graph.connect(v["iStdDev"][num], iStdDev.slice(interval));
          ++num;
        }
        graph.setFieldSize(v["acts"], inpIdx);
        graph.setFieldSize(v["mean"], num);
        graph.setFieldSize(v["iStdDev"], num);
        graph.setInitialValue(v["eps"], eps);
        graph.setTileMapping(v, tile);
      }
    }
    prog.add(Execute(cs));
    return std::make_pair(mean, iStdDev);
  }
}

Tensor
batchNormWhiten(Graph &graph,
                const Tensor &acts,
                const Tensor &mean,
                const Tensor &iStdDev,
                Sequence &prog,
                const std::string &debugPrefix) {
  const auto rank = acts.rank();
  if (rank == 4) {
    return poplin::batchNormWhiten(graph, acts, mean, iStdDev, prog,
                                   debugPrefix);
  } else {
    const auto fnPrefix = debugPrefix + "/BN/whiten";
    const auto numChans = numChannels(acts);
    const auto actsPerChan = numActsPerChannel(acts);

    auto bMean =
        mean.broadcast(actsPerChan, 0).reshape({actsPerChan, numChans});
    auto bIStdDev =
        iStdDev.broadcast(actsPerChan, 0).reshape({actsPerChan, numChans});
    auto actsZeroMean = popops::sub(graph, acts, bMean, prog, fnPrefix);
    auto actsWhitened = popops::mul(graph, actsZeroMean, bIStdDev, prog,
                                    fnPrefix);
    return actsWhitened;
  }
}

std::pair<Tensor, Tensor>
batchNormalise(Graph &graph,
               const Tensor &acts,
               const Tensor &gamma,
               const Tensor &beta,
               const Tensor &mean,
               const Tensor &iStdDev,
               Sequence &prog,
               const std::string &debugPrefix) {
  const auto rank = acts.rank();
  check(acts);
  if (rank == 4) {
    return poplin::batchNormalise(graph, acts, gamma, beta, mean, iStdDev,
                                   prog, debugPrefix);
  } else {
    const auto fnPrefix = debugPrefix + "/BN/batchNormalise";
    const auto actsShape = acts.shape();
    const auto actsPerChan = numActsPerChannel(acts);

    auto actsWhitened = batchNormWhiten(graph, acts, mean, iStdDev, prog,
                                        fnPrefix);

    auto actsOut = mul(graph, actsWhitened,
                       gamma.broadcast(actsPerChan, 0).reshape(actsShape),
                       prog, fnPrefix);
    addInPlace(graph, actsOut,
               beta.broadcast(actsPerChan, 0).reshape(actsShape),
               prog, fnPrefix);
    return std::make_pair(actsOut, actsWhitened);
  }
}

Tensor
batchNormalise(Graph &graph,
               const Tensor &acts,
               const Tensor &combinedMultiplicand,
               const Tensor &addend,
               Sequence &prog,
               const std::string &debugPrefix) {
  const auto rank = acts.rank();
  check(acts);
  if (rank == 4) {
    return poplin::batchNormalise(graph, acts, combinedMultiplicand, addend,
                                   prog, debugPrefix);
  } else {
    const auto fnPrefix = debugPrefix + "/BN/batchNormaliseInference";
    const auto actsShape = acts.shape();
    const auto numChans = numChannels(acts);
    const auto actsPerChan = numActsPerChannel(acts);

    auto bAddend =
        addend.broadcast(actsPerChan, 0).reshape({actsPerChan, numChans});
    auto bCombinedMultiplicand =
        combinedMultiplicand.broadcast(actsPerChan, 0)
                            .reshape({actsPerChan, numChans});
    auto actsBN = mul(graph, acts, bCombinedMultiplicand, prog, fnPrefix);

    addInPlace(graph, actsBN, bAddend, prog, fnPrefix);
    return actsBN;
  }
}

std::pair<Tensor, Tensor>
batchNormDeltas(Graph &graph,
                const Tensor &actsWhitened,
                const Tensor &gradsIn,
                Sequence &prog,
                const Type &partialsType,
                const std::string &debugPrefix) {
  check(actsWhitened);
  return poplin::batchNormDeltas(graph, actsWhitened, gradsIn, prog,
                                    partialsType, debugPrefix);
}

Tensor batchNormGradients(Graph &graph,
                          const Tensor &actsWhitened,
                          const Tensor &gradsIn,
                          const Tensor &gammaDelta,
                          const Tensor &betaDelta,
                          const Tensor &iStdDev,
                          const Tensor &gamma,
                          Sequence &prog,
                          const Type &partialsType,
                          const std::string &debugPrefix) {
  const auto rank = actsWhitened.rank();
  check(actsWhitened);
  if (rank == 4) {
    return poplin::batchNormGradients(graph, actsWhitened, gradsIn, gammaDelta,
                                       betaDelta, iStdDev, gamma, prog,
                                       partialsType, debugPrefix);
  } else {
    const auto fnPrefix = debugPrefix + "/BN/gradients";
    const auto actsShape = actsWhitened.shape();
    const auto numElements = numActsPerChannel(actsWhitened);
    const float rScale = 1.0 / numElements;

    const auto gammaDeltaMulAct =
        mul(graph, actsWhitened,
            gammaDelta.broadcast(numElements, 0).reshape(actsShape), prog,
            fnPrefix);

    auto gradient = graph.clone(actsWhitened, debugPrefix + "/gradient");
    prog.add(Copy(gradsIn, gradient));
    scaledAddTo(graph, gradient, gammaDeltaMulAct, -rScale, prog, fnPrefix);
    scaledAddTo(graph, gradient,
          betaDelta.broadcast(numElements, 0).reshape(actsShape),
          -rScale, prog, fnPrefix);

    const auto scale = mul(graph, gamma, iStdDev, prog, fnPrefix);

    return mul(graph, gradient,
               scale.broadcast(numElements, 0).reshape(actsShape),
               prog, fnPrefix);
  }
}

void batchNormParamUpdate(Graph &graph,
                          const Tensor &gammaDelta,
                          const Tensor &betaDelta,
                          float learningRate,
                          Tensor &gamma,
                          Tensor &beta,
                          Sequence &prog,
                          const std::string &debugPrefix) {
  const std::string fnPrefix = debugPrefix + "/BN/paramUpdate";
  // Do update of beta and gamma together
  scaledAddTo(graph, concat(beta, gamma), concat(betaDelta, gammaDelta),
              -learningRate, prog, fnPrefix);
}

uint64_t getFwdFlops(uint64_t numChannels, uint64_t actsPerChannel,
                     bool computeEstimates) {
  // Acts per channel:
  // - for fc layers is the total number of batches.
  // - for conv layers it is the field size per channel * batch size
  //
  // Number of channels:
  // - for fc layers is the total number of activations in a batch
  // - for conv layers is the total number of channels

  uint64_t flopsForEstimates =
      (actsPerChannel - 1) * numChannels   // sum for mean
      + numChannels                        // divide by actsPerChannel
      + actsPerChannel * numChannels       // square
      + (actsPerChannel - 1) * numChannels // sum of squares
      + numChannels                        // divide by actsPerChannel
      + numChannels                        // mean square
      + numChannels                        // sub
      + numChannels                        // add eps
      + numChannels;                       // sqrt: revisit this
  uint64_t flopsForActs =
      + actsPerChannel * numChannels       // sub mean
      + actsPerChannel * numChannels       // divide by std dev
      + actsPerChannel * numChannels       // multiply by gamma
      + actsPerChannel * numChannels;      // add beta
  return (computeEstimates ? flopsForEstimates : 0) + flopsForActs;
}


uint64_t getBwdFlops(uint64_t numChannels, uint64_t actsPerChannel) {
  // assumes whitened activations are available
  uint64_t flopsReduceGrads =
      (actsPerChannel - 1) * numChannels   // Reduce
      + numChannels;                       // Divide by actsPerChannel
  uint64_t flopsReduceProd =
      actsPerChannel * numChannels         // product of whitenedActs * grads
      + (actsPerChannel - 1) * numChannels // reduce
      + numChannels                        // divide by actsPerChannel
      + actsPerChannel * numChannels;      // reduced multiply by whitened acts

  uint64_t finalComp =
      actsPerChannel * numChannels         // add the two parts above
      + numChannels                        // gamma divide by standard dev
      + actsPerChannel * numChannels;      // scale by (gamma/stdDev
  return flopsReduceGrads + flopsReduceProd + finalComp;
}


uint64_t getWuFlops(uint64_t numChannels, uint64_t actsPerChannel) {
  uint64_t flopsBeta =
    (actsPerChannel - 1) * numChannels     // Reduce
    + numChannels                          // multiply learning rate
    + numChannels;                         // update beta

  uint64_t flopsGamma =
    actsPerChannel * numChannels           // product of grads and activations
    + (actsPerChannel - 1) * numChannels   // reduce
    + numChannels                          // multiply learning rate
    + numChannels;                         // update gamma
  return flopsBeta + flopsGamma;
}

} // namespace bn
} // namespace popnn
