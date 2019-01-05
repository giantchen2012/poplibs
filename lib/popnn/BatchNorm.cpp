#include "poputil/VertexTemplates.hpp"
#include "poputil/TileMapping.hpp"
#include "poputil/Util.hpp"
#include "popops/ElementWise.hpp"
#include "popnn/BatchNorm.hpp"
#include "popops/Reduce.hpp"
#include "popops/ScaledAdd.hpp"
#include "poplin/Norms.hpp"
#include "NormsInternal.hpp"
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

std::pair<Tensor, Tensor>
batchNormStatistics(Graph &graph, const Tensor acts,
                    float eps,
                    Sequence &prog,
                    bool unbiasedVarEstimate,
                    const Type &partialsType,
                    const std::string &debugPrefix) {
  checkTensorShape(acts);
  return poplin::normStatistics(graph, acts, eps, prog, unbiasedVarEstimate,
                                partialsType, debugPrefix);
}

Tensor
batchNormWhiten(Graph &graph,
                const Tensor &acts_,
                const Tensor &mean,
                const Tensor &iStdDev,
                Sequence &prog,
                const std::string &debugPrefix) {
  const auto rank = acts_.rank();
  auto acts = preProcessNormActs(acts_);
  auto whitenedActs =
      poplin::normWhiten(graph, acts, mean, iStdDev, prog, debugPrefix);
  return postProcessNormActs(whitenedActs, rank);
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
  checkTensorShape(acts);
  auto preProcessActs = preProcessNormActs(acts);
  auto whitenedActs =
      batchNormWhiten(graph, preProcessActs, mean, iStdDev, prog, debugPrefix);
  auto outputActs =
      poplin::normalise(graph, whitenedActs, gamma, beta, prog, debugPrefix);
  return std::make_pair(postProcessNormActs(outputActs, rank),
                        postProcessNormActs(whitenedActs, rank));
}

Tensor
batchNormalise(Graph &graph,
               const Tensor &acts,
               const Tensor &combinedMultiplicand,
               const Tensor &addend,
               Sequence &prog,
               const std::string &debugPrefix) {
  const auto rank = acts.rank();
  checkTensorShape(acts);
  auto preProcessedActs = preProcessNormActs(acts);
  auto actsNormalised =
      poplin::normalise(graph, preProcessedActs, combinedMultiplicand, addend,
                        prog, debugPrefix);
  return postProcessNormActs(actsNormalised, rank);
}

std::pair<Tensor, Tensor>
batchNormParamGradients(Graph &graph,
                        const Tensor &actsWhitened,
                        const Tensor &gradsIn,
                        Sequence &prog,
                        const Type &partialsType,
                        const std::string &debugPrefix) {
  checkTensorShape(gradsIn);
  checkTensorShape(actsWhitened);
  return poplin::normParamGradients(graph, actsWhitened, gradsIn, prog,
                                    partialsType, debugPrefix);
}

Tensor batchNormGradients(Graph &graph,
                          const Tensor &actsWhitened_,
                          const Tensor &gradsIn_,
                          const Tensor &iStdDev,
                          const Tensor &gamma,
                          Sequence &prog,
                          const Type &partialsType,
                          const std::string &debugPrefix) {
  const auto rank = actsWhitened_.rank();
  checkTensorShape(actsWhitened_);
  checkTensorShape(gradsIn_);
  auto actsWhitened = preProcessNormActs(actsWhitened_);
  auto gradsIn = preProcessNormActs(gradsIn_);
  auto gradsNorm =
      poplin::normGradients(graph, gradsIn, gamma, prog, debugPrefix);
  auto gradsOut =
      poplin::normStatisticsGradients(graph, actsWhitened, gradsNorm,
                                         iStdDev, prog, partialsType,
                                         debugPrefix);
  return postProcessNormActs(gradsOut, rank);
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
