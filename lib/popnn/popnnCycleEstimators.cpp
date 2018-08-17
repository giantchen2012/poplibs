#include "popnnCycleEstimators.hpp"

#include "popnn/NonLinearity.hpp"
#include "popnn/NonLinearityDefUtil.hpp"
#include "popnn/PoolingDef.hpp"
#include "PoolingDefUtil.hpp"
#include "PerformanceEstimation.hpp"

using namespace poplar;

// Macro to create entries in cycle estimator table
#define INSTANTIATE_NL_CYCLE_ESTIMATOR(v, ...) \
        CYCLE_ESTIMATOR_ENTRY(popnn, v, FLOAT, \
                              popnn::NonLinearityType::SIGMOID), \
        CYCLE_ESTIMATOR_ENTRY(popnn, v, HALF, \
                              popnn::NonLinearityType::SIGMOID), \
        CYCLE_ESTIMATOR_ENTRY(popnn, v, FLOAT, \
                              popnn::NonLinearityType::RELU), \
        CYCLE_ESTIMATOR_ENTRY(popnn, v, HALF, \
                              popnn::NonLinearityType::RELU), \
        CYCLE_ESTIMATOR_ENTRY(popnn, v, FLOAT, \
                              popnn::NonLinearityType::TANH), \
        CYCLE_ESTIMATOR_ENTRY(popnn, v, HALF, \
                              popnn::NonLinearityType::TANH)

namespace popnn {

std::uint64_t
MAKE_CYCLE_ESTIMATOR_NAME(NonLinearitySupervisor)(
                              const VertexIntrospector &vertex,
                              const Target &target,
                              const Type &type,
                              const NonLinearityType &nlType) {
  bool isFloat = type == FLOAT;
  std::vector<unsigned> regionSizes;
  const auto data = vertex.getFieldInfo("data");
  regionSizes.push_back(data.size());
  return getNonLinearityCycles(regionSizes, nlType, isFloat, false, true,
                               target.getDataPathWidth(),
                               target.getNumWorkerContexts());
}

std::uint64_t
MAKE_CYCLE_ESTIMATOR_NAME(NonLinearityGradSupervisor)(
                              const VertexIntrospector &vertex,
                              const Target &target,
                              const Type &type,
                              const NonLinearityType &nlType) {
  bool isFloat = type == FLOAT;
  std::vector<unsigned> regionSizes;
  const auto inGrad = vertex.getFieldInfo("inGrad");
  CODELET_FIELD(outGrad);
  CODELET_FIELD(out);
  assert(outGrad.size() == inGrad.size());
  assert(outGrad.size() == out.size());
  regionSizes.push_back(inGrad.size());
  return getBwdNonlinearityDerivativeCycles(regionSizes, nlType, isFloat, false,
                                            true, target.getDataPathWidth(),
                                            target.getNumWorkerContexts());
}

std::uint64_t
MAKE_CYCLE_ESTIMATOR_NAME(NonLinearity2D)(const VertexIntrospector &vertex,
                                          const Target &target,
                                          const Type &type,
                                          const NonLinearityType &nlType) {
  bool isFloat = type == FLOAT;
  std::vector<unsigned> regionSizes;
  const auto data = vertex.getFieldInfo("data");
  for (unsigned i=0;i<data.size(); ++i)
    regionSizes.push_back(data[i].size());
  return getNonLinearityCycles(regionSizes, nlType, isFloat, true, false,
                               target.getDataPathWidth(),
                               target.getNumWorkerContexts());
}

std::uint64_t
MAKE_CYCLE_ESTIMATOR_NAME(NonLinearityGrad2D)(const VertexIntrospector &vertex,
                                              const Target &target,
                                              const Type &type,
                                              const NonLinearityType &nlType) {
  bool isFloat = type == FLOAT;
  std::vector<unsigned> regionSizes;
  const auto inGrad = vertex.getFieldInfo("inGrad");
  CODELET_FIELD(outGrad);
  CODELET_FIELD(out);
  assert(outGrad.size() == inGrad.size());
  for (unsigned i = 0; i < inGrad.size(); ++i) {
    assert(outGrad[i].size() == inGrad[i].size());
    assert(outGrad[i].size() == out[i].size());
    regionSizes.push_back(inGrad[i].size());
  }
  return getBwdNonlinearityDerivativeCycles(regionSizes, nlType, isFloat, true,
                                            false, target.getDataPathWidth(),
                                            target.getNumWorkerContexts());
}

std::uint64_t
MAKE_CYCLE_ESTIMATOR_NAME(MaxPooling)(const VertexIntrospector &vertex,
                                      const Target &target,
                                      const Type &type) {
  unsigned numCycles = 10;
  bool isFloat = type == FLOAT;
  const auto out = vertex.getFieldInfo("out");
  const auto windowSizes = vertex.getFieldInfo("windowSizes");
  const auto windowSizeValues =
    windowSizes.getInitialValues<unsigned short>(target);
  const auto vectorWidth = target.getDataPathWidth() / (isFloat ? 32 : 16);
  unsigned inIndex = 0;
  CODELET_FIELD(in);
  assert(windowSizes.size() == out.size());
  for (unsigned i = 0; i < out.size(); ++i) {
    for (unsigned w = 0; w < windowSizeValues[i]; ++w) {
      assert(out[i].size() == in[inIndex].size());
    }
    inIndex += windowSizeValues[i];
    auto numVectors = (out[i].size() + vectorWidth - 1) / vectorWidth;
    auto windowSize = windowSizeValues[i];
    // TODO: This is too optimistic
    numCycles += 1 + numVectors * (1 + windowSize);
  }
  return numCycles;
}

std::uint64_t
MAKE_CYCLE_ESTIMATOR_NAME(ScaledSumPooling)(const VertexIntrospector &vertex,
                                            const Target &target,
                                            const Type &type,
                                            const popnn::PoolingType pType) {
  unsigned numCycles = 10;
  bool isFloat = type == FLOAT;
  const auto out = vertex.getFieldInfo("out");
  const auto windowSizes = vertex.getFieldInfo("windowSizes");
  const auto windowSizeValues =
    windowSizes.getInitialValues<unsigned short>(target);
  const auto vectorWidth = target.getDataPathWidth() / (isFloat ? 32 : 16);
  const unsigned scaleCycles = pType == popnn::PoolingType::AVG ? 1 : 0;
  CODELET_FIELD(in);
  unsigned inIndex = 0;
  for (unsigned i = 0; i < out.size(); ++i) {
    for (unsigned w = 0; w < windowSizeValues[i]; ++w) {
      assert(out[i].size() == in[inIndex].size());
      inIndex++;
    }
    auto numVectors = (out[i].size() + vectorWidth - 1) / vectorWidth;
    auto windowSize = windowSizeValues[i];
    // load ptr/vec/ load data/add for windowSize
    // axpby and store
    numCycles += 2 + scaleCycles + numVectors * (3 + 2 * windowSize);
  }
  return numCycles;
}

std::uint64_t
MAKE_CYCLE_ESTIMATOR_NAME(MaxPoolingGrad)(const VertexIntrospector &vertex,
                                          const Target &target,
                                          const Type &type) {
  unsigned numCycles = 10;
  bool isFloat = type == FLOAT;
  const auto inGrad = vertex.getFieldInfo("inGrad");
  const auto windowSizes = vertex.getFieldInfo("windowSizes");
  const auto windowSizeValues =
    windowSizes.getInitialValues<unsigned short>(target);
  const auto vectorWidth = target.getDataPathWidth() / (isFloat ? 32 : 16);
  // Expected implementation per group:
  // load group of actIn
  // for windowsize:
  // load actOut
  //  compare
  //  res<<=14 (covert to 0.5/0)
  //  mac
  // getacc
  // double
  // store
  CODELET_FIELD(in);
  CODELET_FIELD(out);
  CODELET_FIELD(outGrad);
  unsigned inIndex = 0;
  for (unsigned i = 0; i < inGrad.size(); ++i) {
    assert(inGrad[i].size() == in[i].size());
    for (unsigned w = 0; w < windowSizeValues[i]; ++w) {
      assert(inGrad[i].size() == outGrad[inIndex].size());
      assert(inGrad[i].size() == out[inIndex].size());
      inIndex++;
    }
    auto numVectors = (inGrad[i].size() + vectorWidth - 1) / vectorWidth;
    auto windowSize = windowSizeValues[i];
    // TODO: This is too optimistic
    numCycles += 5 + numVectors * (5 + windowSize * 3);
  }
  return numCycles;
}

std::uint64_t
MAKE_CYCLE_ESTIMATOR_NAME(SumPoolingGrad)(const VertexIntrospector &vertex,
                                          const Target &target,
                                          const Type &type) {
  unsigned numCycles = 10;
  bool isFloat = type == FLOAT;
  const auto inGrad = vertex.getFieldInfo("inGrad");
  const auto windowSizes = vertex.getFieldInfo("windowSizes");
  const auto windowSizeValues =
    windowSizes.getInitialValues<unsigned short>(target);
  const auto vectorWidth = target.getDataPathWidth() / (isFloat ? 32 : 16);
  // Expected implementation per group:
  // for windowsize:
  // load deltaIn
  //  acc
  // getacc
  // axpby
  // double
  // store
#ifndef NDEBUG
  CODELET_FIELD(outGrad);
  unsigned inIndex = 0;
#endif
  for (unsigned i = 0; i < inGrad.size(); ++i) {
    for (unsigned w = 0; w < windowSizeValues[i]; ++w)
      assert(inGrad[i].size() == outGrad[inIndex++].size());
    auto numVectors = (inGrad[i].size() + vectorWidth - 1) / vectorWidth;
    auto windowSize = windowSizeValues[i];
    // TODO: This is too optimistic
    numCycles += 2 + numVectors * (4 + windowSize * 1);
  }
  return numCycles;
}

std::uint64_t
MAKE_CYCLE_ESTIMATOR_NAME(LossSumSquaredTransform)
  (const VertexIntrospector &vertex,
   const Target &target,
   const Type &fpType) {
  const bool isFloat = fpType == FLOAT;
  const auto size = vertex.getFieldInfo("probs").size();
  const auto isSoftmax = false;
  return getLossTransformCycles(isFloat, isSoftmax, size);
}

std::uint64_t
MAKE_CYCLE_ESTIMATOR_NAME(LossSoftmaxTransform)
  (const VertexIntrospector &vertex,
   const Target &target,
   const Type &fpType) {
  const bool isFloat = fpType == FLOAT;
  const auto size = vertex.getFieldInfo("probs").size();
  const auto isSoftmax = true;
  return getLossTransformCycles(isFloat, isSoftmax, size);
}

std::uint64_t
MAKE_CYCLE_ESTIMATOR_NAME(ReduceMaxClassGather)
  (const VertexIntrospector &vertex,
   const Target &target,
   const Type &fpType,
   const Type &labelType) {
  std::uint64_t cycles = 5 + // Vertex overhead
                         4;  // Supervisor call + sync
  CODELET_FIELD(activations);
  CODELET_SCALAR_VAL(size, unsigned);
  CODELET_SCALAR_VAL(divisorLog2, unsigned short);
  const auto numWorkers = target.getNumWorkerContexts();
  const auto maxValueGrainSize =
    std::max<std::size_t>(1, target.getAtomicStoreGranularity() /
                             target.getTypeSize(fpType));
  const auto divisor = (1u << divisorLog2);
  // Check the divisor chosen is large enough to process all inputs
  // with the target number of workers and the grain size.
  assert(divisor * maxValueGrainSize * numWorkers >= size);
  const auto isFloat = (fpType == FLOAT);

  // NOTE: Cycles per act in inner loop are conservative in that we assume
  // we always take the branch. i.e. activations are sorted in ascending
  // order when passed to the vertex.
  if (isFloat) {
    cycles += 3 + // Load acts ptr, size, divisor
              2 + // Get worker ID
              7 + // Calculate the worker region, branch if nothing
              1 + // Offset pointer
              2 + // Initialise maxValue/maxIndex
              2 + // sub for brnzdec, skip loop if single element
              (divisor - 1) * 6 +
              4 + // Recover maxIndex from loop
              2 + // Load maxValue/maxIndex pointers
              2;  // Store maxValue/maxIndex
  } else {
    cycles += 3 + // Load vertex state
              2 + // Get worker ID
              4 + // Setup outer loop, branch if nothing to do
              2 * (4 + // Calculate region
                   3 + // Setup loop counter, branch if empty
                   2 + // Load and offset pointer for worker
                   2 + // Initialise maxValue/maxIndex
                   2 + // sub for brnzdec, skip loop if single element
                   (divisor - 1) * 6 +
                   4 + // Recover maxIndex from loop
                   1 + // Store maxIndex
                   6)+ // Increment outer loop counter, branch
              ((size & 1) ? 6 : 4); // RMW/store for maxValue
  }

  return cycles;
}

std::uint64_t
MAKE_CYCLE_ESTIMATOR_NAME(ReduceMaxClassSparse)
  (const VertexIntrospector &vertex,
   const Target &target,
   const Type &fpType,
   const Type &labelType) {
  std::uint64_t cycles = 5; // Vertex overhead
  CODELET_FIELD(activations);
  CODELET_FIELD(labels);
  const auto numActs = activations.size();
  assert(numActs == labels.size());
  const auto isFloat = (fpType == FLOAT);

  cycles += 3 + // Load acts pointer start/end, labels pointer
            2 + // sub + shr acts pointers for element count
            4 + // Load first act as maxValue, initialise maxIndex
            2;  // sub 2 for first element and brnzdec, branch if done

  // NOTE: Cycles per act are conservative in that we assume we always
  // take the branch. i.e. activations are sorted in ascending order
  // when passed to the vertex.
  cycles += (numActs - 1) *
              (2 + // [M] load act pointer, load act
               1 + // [A] compare to current max
               1 + // [M] atom
               1 + // [M] branch
               1 + // [M/A] set current max value/index
               1); // [M] brnzdec

  cycles += 3 + // Recover max index from loop above, load actual label
            2 + // Load maxValue/maxIndex pointers
            1 + // Store maxIndex
            (isFloat ? 1 : 6); // Store maxValue

  return cycles;
}

std::uint64_t
MAKE_CYCLE_ESTIMATOR_NAME(CalcAccuracy)(const VertexIntrospector &vertex,
                                        const Target &target,
                                        const Type &labelType) {
  std::uint64_t cycles = 5; // Vertex overhead
  CODELET_FIELD(maxPerBatch);
  CODELET_FIELD(expected);
  const auto batchSize = maxPerBatch.size();
  assert(batchSize == expected.size());

  cycles += 4 + // Load maxPerBatch start/end, sub, shift for num elements
            2 + // Load expected and numCorrect pointer
            1 + // Load initial numCorrect value
            1;  // rpt

  cycles += batchSize *
              (2 + // Load maxPerBatch/expected
               1 + // cmpeq
               1); // add

  cycles += 1; // Store final numCorrect

  return cycles;
}

std::uint64_t
MAKE_CYCLE_ESTIMATOR_NAME(BatchNormEstimates)(const VertexIntrospector &vertex,
                                              const Target &target,
                                              const Type &inType,
                                              const Type &partialsType) {
  unsigned numCycles = 5;
  const auto mean = vertex.getFieldInfo("mean");
  const auto acts = vertex.getFieldInfo("acts");
  const unsigned n = mean.size();
  const auto batchSize = acts[0].size();
#ifndef NDEBUG
  unsigned actsIdx = 0;
#endif
  for (unsigned i = 0; i != n; ++i) {
    const unsigned numActs = mean[i].size();
    numCycles += (batchSize + 7) * numActs;
    for (unsigned a = 0; a != numActs; ++a) {
      assert(acts[actsIdx++].size() == batchSize);
    }
  }
  return numCycles;
}

poplibs::CycleEstimatorTable makeCyclesFunctionTable() {
  return
  {
    CYCLE_ESTIMATOR_ENTRY(popnn, BatchNormEstimates, FLOAT, FLOAT),
    CYCLE_ESTIMATOR_ENTRY(popnn, BatchNormEstimates, HALF, FLOAT),

    CYCLE_ESTIMATOR_ENTRY(popnn, LossSumSquaredTransform, FLOAT),
    CYCLE_ESTIMATOR_ENTRY(popnn, LossSumSquaredTransform, HALF),

    CYCLE_ESTIMATOR_ENTRY(popnn, LossSoftmaxTransform, FLOAT),
    CYCLE_ESTIMATOR_ENTRY(popnn, LossSoftmaxTransform, HALF),

    CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassGather, FLOAT, UNSIGNED_INT),
    CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassGather, HALF, UNSIGNED_INT),
    CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassGather, FLOAT, INT),
    CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassGather, HALF, INT),

    CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassSparse, FLOAT, UNSIGNED_INT),
    CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassSparse, HALF, UNSIGNED_INT),
    CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassSparse, FLOAT, INT),
    CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassSparse, HALF, INT),

    CYCLE_ESTIMATOR_ENTRY(popnn, CalcAccuracy, UNSIGNED_INT),
    CYCLE_ESTIMATOR_ENTRY(popnn, CalcAccuracy, INT),

    CYCLE_ESTIMATOR_ENTRY(popnn, SumPoolingGrad, FLOAT),
    CYCLE_ESTIMATOR_ENTRY(popnn, SumPoolingGrad, HALF),

    CYCLE_ESTIMATOR_ENTRY(popnn, MaxPoolingGrad, FLOAT),
    CYCLE_ESTIMATOR_ENTRY(popnn, MaxPoolingGrad, HALF),

    CYCLE_ESTIMATOR_ENTRY(popnn, ScaledSumPooling, FLOAT, PoolingType::AVG),
    CYCLE_ESTIMATOR_ENTRY(popnn, ScaledSumPooling, FLOAT, PoolingType::SUM),
    CYCLE_ESTIMATOR_ENTRY(popnn, ScaledSumPooling, HALF, PoolingType::AVG),
    CYCLE_ESTIMATOR_ENTRY(popnn, ScaledSumPooling, HALF, PoolingType::SUM),

    CYCLE_ESTIMATOR_ENTRY(popnn, MaxPooling, FLOAT),
    CYCLE_ESTIMATOR_ENTRY(popnn, MaxPooling, HALF),

    INSTANTIATE_NL_CYCLE_ESTIMATOR(NonLinearityGradSupervisor),
    INSTANTIATE_NL_CYCLE_ESTIMATOR(NonLinearitySupervisor),
    INSTANTIATE_NL_CYCLE_ESTIMATOR(NonLinearityGrad2D),
    INSTANTIATE_NL_CYCLE_ESTIMATOR(NonLinearity2D)
  };
};

} // end namespace popnn
