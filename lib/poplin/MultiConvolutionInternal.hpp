// Copyright (c) 2020 Graphcore Ltd. All rights reserved.

#ifndef poplin_MultiConvolutionInternal_hpp
#define poplin_MultiConvolutionInternal_hpp

#include "CanonicalConvParams.hpp"
#include "poplin/MultiConvolution.hpp"

namespace poplin {
namespace multiconv {
namespace internal {

// These structures mirror the public structures in MultiConvolution.hpp except
// public objects (like OptionFlags) have been replaced with internal objects
// (like ConvOptions).

struct CreateTensorArgs {
  CanonicalConvParams params;
  ConvOptions options;
  std::string name;
};

struct ConvolutionArgs {
  poplar::Tensor inputs;
  poplar::Tensor weights;
  CanonicalConvParams params;
  ConvOptions options;
};

struct CalculateWeightDeltasArgs {
  poplar::Tensor zDeltas;
  poplar::Tensor activations;
  CanonicalConvParams params;
  ConvOptions options;
};

template <typename ScaleType> struct ConvWeightUpdateArgs {
  poplar::Tensor zDeltas;
  poplar::Tensor weights;
  poplar::Tensor activations;
  ScaleType scale;
  CanonicalConvParams params;
  ConvOptions options;
};

} // namespace internal
} // namespace multiconv
} // namespace poplin

#endif // poplin_MultiConvolutionInternal_hpp
