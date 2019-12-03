// Copyright (c) 2018, Graphcore Ltd, All rights reserved.

#ifndef poplin_Convolution_hpp
#define poplin_Convolution_hpp
#include "ConvParams.hpp"
#include <poplar/Graph.hpp>
#include <poplar/OptionFlags.hpp>
#include <poplar/Program.hpp>
#include <set>
#include <tuple>

namespace poplin {

/** Class used to cache the calculation of plans for convolution operations.
 */
class PlanningCache;

uint64_t getFwdFlops(const ConvParams &params);
uint64_t getBwdFlops(const ConvParams &params);
uint64_t getWuFlops(const ConvParams &params);

double getFwdPerfectCycleCount(const poplar::Graph &graph,
                               const ConvParams &params);

double getBwdPerfectCycleCount(const poplar::Graph &graph,
                               const ConvParams &params);

double getWuPerfectCycleCount(const poplar::Graph &graph,
                              const ConvParams &params);

/** Create a weight tensor suitable for use with convolution()
 *
 * The shape of the tensor will be [convGroups x outChansPerConvGroup  x
 * inChansPerConvGroup x H x W]
 *
 * **Convolution options**
 *
 *    * `availableMemoryProportion` Decimal between 0 and 1 (inclusive) [=0.6]
 *
 *      The proportion of tile memory to be made available as temporary memory
 *      for this convolution. This constraint will be ignored (with a warning)
 *      if a conforming plan cannot be found and then the planner will replan
 *      for the smallest memory usage possible. Less temporary memory will
 *      generally result in a convolution that takes more cycles to complete.
 *      However, because always live memory (like code and vertex state) is not
 *      tracked by the planner, a convolution using less temporary memory may
 *      use more memory overall due to an increase of always live memory.
 *
 *      **Note**: We recommend using a value greater than 0.05. Below this value
 *      the volume of always live memory quickly increases and can result in
 *      OOM errors.
 *
 *    * `partialsType` (half, float) [=float]
 *
 *      Data type used for intermediate calculations.
 *
 *    * `pass` (NONE, INFERENCE_FWD, TRAINING_FWD, TRAINING_BWD, TRAINING_WU,
 *      FC_INFERENCE_FWD, FC_TRAINING_FWD, FC_TRAINING_BWD, FC_TRAINING_WU)
 *      [=NONE]
 *
 *    * `use128BitConvUnitLoad` (true, false) [=false]
 *
 *      If true, convolution weights are loaded 128-bits at a time. Otherwise,
 *      they are loaded 64-bits at a time. Not all codelets support 128-bit
 *      loads. This option affects memory usage and cycle count.
 */
/*[INTERNAL]
 *    * `numIPUs` Integer [=target.getNumIPUs()]
 *
 *      Number of IPUs to be used.
 *
 *      Optimize the plan for the specified type of pass. Note the
 *      abbreviations:
 *      FWD (forward), BWD (backward), WU (weight-update), FC (fully-connected).
 *
 *    * `planConstraints` JSON string
 *
 *      Constraints on the chosen convolution plan. Example:
 *
 *          {"0", {"transform": {"swapOperands": true},
 *                 "partition": {"fieldSplit":{"1": 4},
 *                               "inChanSplit": 4,
 *                               "outChanSplit": {"parallel": 4}}
 *                }
 *          }
 *
 *      Where the outer-most index in the plan is an index into the plan
 *      hierarchy, and any multi-dimensional fields are sparsely indexed
 *      objects. Therefore, constraining dimension 1 of fieldSplit to be 4 is
 *      specified as:
 *
 *          {"fieldSplit": {"1": 4}}
 *
 *      This is only implemented for `partitioning` and for the `swapOperands`
 *      transform for now.
 *
 *    * `planConstraintsOutputFilename` String
 *
 *      If set, plan constraints for each plan used by a convolution will be
 *      saved to file. The file path will be the value of this option postpended
 *      with _FWD, _BWD, or _WU (depending on the pass), with a file extension
 *      of .json. The content of these files may be used as input to the
 *      `planConstraints` option (above). The constraints will be complete,
 *      meaning they can only be satisfied by one specific plan - this allows
 *      reliable reproduction regardless of changes to the planner.
 *
 *    * `partialsType.interIPU` (half, float) [=`partialsType`]
 *
 *      Data type of inter-IPU partials.
 *
 *    * `partialsType.interTile` (half, float) [=`partialsType`]
 *
 *      Data type of inter-tile partials.
 *
 *    * `startTileMultiplier` An even integer [=0]
 *
 *      Multiplier used to distribute convolutions across an IPU. If 0,
 *      workload will be distributed across tiles starting from the first tile.
 *      For any other value, distribution will start from a reproducible random
 *      number depending on the chosen value and the convolution parameters.
 *
 *    * `tilesPerIPU` Integer [=target.getTilesPerIPU()]
 *
 *      Number of tiles per IPU to be used.
 *
 *    * `useAggressiveRegrouping` (true, false) [=false]
 *
 *      If true, an attempt will always be made to regroup activations and
 *      weights before the convolution.
 */
/**
 * \param graph   The graph that the tensor will be added to.
 * \param params  The same parameters as used by the convolution().
 * \param name    Debugging name for the tensor.
 * \param options Options controlling the implementation.
 * \param cache   Optional pointer to planning cache to use.
 * \return        The weights tensor suitable for use with convolution().
 */
poplar::Tensor createWeights(poplar::Graph &graph, const ConvParams &params,
                             const std::string &name,
                             const poplar::OptionFlags &options = {},
                             PlanningCache *cache = nullptr);

/** Create a bias tensor suitable for input to addBias() function
 *
 * The tensor will have the shape [outChans]
 *
 * \param graph  The graph that the tensor will be added to.
 * \param acts   The activation tensor which is output from the convolution.
 * \param name   Debugging name for the tensor.
 * \return       The tensor of biases.
 */
poplar::Tensor createBiases(poplar::Graph &graph, const poplar::Tensor &acts,
                            const std::string &name = "biases");

/** Create an input tensor for a convolution.
 *
 * Use this when required to create an input data tensor for a convolution. The
 * same set of parameters which will be passed to the convolution() should also
 * be passed to createInput().
 *
 * The returned tensor has the shape [B x inChans x H x W].
 *
 * \param graph    The tensor will be added to this graph.
 * \param params   Parameters as passed to the target convolution.
 * \param name     Debugging name for the tensor.
 * \param options  Options controlling the implementation. See createWeights().
 * \param cache    Optional pointer to planning cache to use.
 * \return         The allocated input tensor.
 */
poplar::Tensor createInput(poplar::Graph &graph, const ConvParams &params,
                           const std::string &name,
                           const poplar::OptionFlags &options = {},
                           PlanningCache *cache = nullptr);

/** Convolve an input with a set of weights.
 *
 * This is for a 2D convolution.
 *
 * The input tensor is in the form [B x inChans x H x W], and can be allocated
 * using createInput().  The weights tensor is in the form
 * [convGroups x outChansPerConvGroup x inChansPerConvGroup x H x W], and can be
 * allocated using createWeights().
 *
 * The returned tensor has the shape [B x outChans x H x W]
 *
 * Padding and striding are specified in the ConvParams structure.
 *
 * \param graph                   The graph that the operation will be added to.
 * \param in                      Input data tensor.
 * \param weights                 Weights tensor.
 * \param params                  Parameters for the form of the convolution.
 * \param transposeAndFlipWeights For the weight update pass.
 * \param prog                    Poplar program sequence to append the
 *                                operation onto.
 * \param debugPrefix             Name of the operation, for debugging.
 * \param options                 Options that control the implementation. See
 *                                createWeights().
 * \param cache                   Optional pointer to planning cache to use.
 * \return                        The convolved output tensor.
 */
poplar::Tensor convolution(poplar::Graph &graph, const poplar::Tensor &in,
                           const poplar::Tensor &weights,
                           const ConvParams &params,
                           bool transposeAndFlipWeights,
                           poplar::program::Sequence &prog,
                           const std::string &debugPrefix = "",
                           const poplar::OptionFlags &options = {},
                           PlanningCache *cache = nullptr);

using ConvPlanParams = std::tuple<const poplar::Target *, const ConvParams,
                                  const poplar::OptionFlags *>;
/**
 * Plan the specified convolutions.

 * \param convs   A set of tuples of
 *                  - conv-specific target for tile / IPU sizing
 *                  - convolution parameters
 *                  - implementation options. See createWeights().
 *                All entries must have matching machine parameters.
 * \param cache   The planning cache to update.
 */
void preplanConvolutions(const std::set<ConvPlanParams> &convs,
                         PlanningCache &cache);

void weightsTransposeChansFlipXY(poplar::Graph &graph,
                                 const poplar::Tensor &weightsIn,
                                 const poplar::Tensor &WeightsOut,
                                 poplar::program::Sequence &prog,
                                 const std::string &debugPrefix = "");

poplar::Tensor calculateWeightDeltas(
    poplar::Graph &graph, const poplar::Tensor &zDeltas,
    const poplar::Tensor &activations, const ConvParams &params,
    poplar::program::Sequence &prog, const std::string &debugPrefix = "",
    const poplar::OptionFlags &options = {}, PlanningCache *cache = nullptr);

void convolutionWeightUpdate(
    poplar::Graph &graph, const poplar::Tensor &zDeltas,
    const poplar::Tensor &weights, const poplar::Tensor &activations,
    ConvParams params, const poplar::Tensor &scale,
    poplar::program::Sequence &prog, const std::string &debugPrefix = "",
    const poplar::OptionFlags &options = {}, PlanningCache *cache = nullptr);

void convolutionWeightUpdate(
    poplar::Graph &graph, const poplar::Tensor &zDeltas,
    const poplar::Tensor &weights, const poplar::Tensor &activations,
    ConvParams params, float scale, poplar::program::Sequence &prog,
    const std::string &debugPrefix = "",
    const poplar::OptionFlags &options = {}, PlanningCache *cache = nullptr);

void convolutionBiasUpdate(poplar::Graph &graph, const poplar::Tensor &zDeltas,
                           const poplar::Tensor &biases,
                           const poplar::Tensor &scale,
                           const poplar::OptionFlags &options,
                           poplar::program::Sequence &prog,
                           const std::string &debugPrefix = "");

void convolutionBiasUpdate(poplar::Graph &graph, const poplar::Tensor &zDeltas,
                           const poplar::Tensor &biases, float scale,
                           const poplar::OptionFlags &options,
                           poplar::program::Sequence &prog,
                           const std::string &debugPrefix = "");

void addBias(poplar::Graph &graph, const poplar::Tensor &acts,
             const poplar::Tensor &biases, poplar::program::Sequence &prog,
             const std::string &debugPrefix = "");

poplar::Tensor fullyConnectedWeightTranspose(
    poplar::Graph &graph, poplar::Tensor activations, const ConvParams &params,
    poplar::program::Sequence &prog, const std::string &debugPrefix,
    const poplar::OptionFlags &options, PlanningCache *cache = nullptr);

void reportPlanInfo(std::ostream &out, const poplar::Graph &graph,
                    const ConvParams &params,
                    const poplar::OptionFlags &options = {},
                    PlanningCache *cache = nullptr);

void reportWeightUpdatePlanInfo(std::ostream &out, const poplar::Graph &graph,
                                const ConvParams &params,
                                const poplar::OptionFlags &options = {},
                                PlanningCache *cache = nullptr);

struct Plan;

class PlanningCacheImpl;
class PlanningCache {
public:
  PlanningCache();
  ~PlanningCache();
  std::unique_ptr<PlanningCacheImpl> impl;
};

} // namespace poplin

#endif // poplin_Convolution_hpp
