// Copyright (c) 2018, Graphcore Ltd, All rights reserved.
#ifndef poplin_internal_ConvOptions_hpp
#define poplin_internal_ConvOptions_hpp

namespace poplin {

enum class WeightUpdateMethod {
  AMP,
  AUTO
};

const char *asString(const WeightUpdateMethod &method);
std::ostream &operator<<(std::ostream &os, const WeightUpdateMethod &method);
std::istream &operator>>(std::istream &is, WeightUpdateMethod &method);

enum class Pass {
  NONE,
  INFERENCE_FWD,
  TRAINING_FWD,
  TRAINING_BWD,
  TRAINING_WU,
  FC_INFERENCE_FWD,
  FC_TRAINING_FWD,
  FC_TRAINING_BWD,
  FC_TRAINING_WU
};

/** Options to control the implementation of a convolution */
struct ConvOptions {
  WeightUpdateMethod weightUpdateMethod = WeightUpdateMethod::AUTO;
  bool useWinograd = false;
  unsigned winogradPatchSize = 4;
  unsigned tempMemoryBudget = 0;
  /// The pass this layer corresponds to.
  Pass pass = Pass::NONE;
  poplar::Type partialsType = poplar::FLOAT;
  poplar::Type interTilePartialsType = poplar::FLOAT;
  poplar::Type interIpuPartialsType = poplar::FLOAT;
  bool use128BitConvUnitLoad = false;
};

inline bool operator<(const ConvOptions &a, const ConvOptions &b) {
  return std::tie(a.weightUpdateMethod,
                  a.useWinograd,
                  a.winogradPatchSize,
                  a.tempMemoryBudget,
                  a.pass,
                  a.partialsType,
                  a.interTilePartialsType,
                  a.interIpuPartialsType,
                  a.use128BitConvUnitLoad) <
           std::tie(b.weightUpdateMethod,
                    b.useWinograd,
                    b.winogradPatchSize,
                    b.tempMemoryBudget,
                    b.pass,
                    b.partialsType,
                    b.interTilePartialsType,
                    b.interIpuPartialsType,
                    b.use128BitConvUnitLoad);
}

} // end namespace poplin

#endif // poplin_internal_ConvOptions_hpp
