// Copyright (c) 2020 Graphcore Ltd. All rights reserved.
#ifndef _ConvVertexType_hpp_
#define _ConvVertexType_hpp_

#include "ConvPlan.hpp"
#include <poplar/Type.hpp>

namespace poplar {
class Target;
}

namespace poplin {

struct ConvParams;
class ConvOptions;

struct ConvVertexType {
  Plan::Method method;
  poplar::Type inputType;
  poplar::Type partialType;

  unsigned convGroupsPerGroup;
  unsigned inChansPerGroup;
  unsigned partialChansPerGroup;

  // TODO: these variables are only valid for certain methods, it might be
  // better to use a variant here instead.
  //
  // The width of the kernel that slides over the input. Only 4 is currently
  // supported in the software but the SLIC engine also supports 3.
  unsigned slicWindowWidth;
  // Number of engines enabled. Allowed options: 4 or 8
  unsigned numConvUnitsRequired;

  // If TRUE convolution library will use unsigned short type for vertex
  // states, otherwise will fallback into unsigned type
  bool useLimitedVersion;

  ConvVertexType(Plan::Method method, poplar::Type inputType,
                 poplar::Type partialType, unsigned convGroupsPerGroup,
                 unsigned inChansPerGroup, unsigned partialChansPerGroup,
                 unsigned slicWindowWidth, unsigned numConvUnitsRequired,
                 bool useLimitedVersion)
      : method(method), inputType(inputType), partialType(partialType),
        convGroupsPerGroup(convGroupsPerGroup),
        inChansPerGroup(inChansPerGroup),
        partialChansPerGroup(partialChansPerGroup),
        slicWindowWidth(slicWindowWidth),
        numConvUnitsRequired(numConvUnitsRequired),
        useLimitedVersion(useLimitedVersion) {}
};

bool canUseConvolutionInstruction(bool floatActivations, bool floatPartials,
                                  unsigned inChansPerGroup,
                                  unsigned numConvUnitsRequired,
                                  unsigned outChansPerGroup,
                                  const poplar::Target &target);

std::vector<ConvVertexType>
getConvVertexTypeCandidates(const poplar::Target &target,
                            poplar::Type inputType, poplar::Type outputType,
                            poplar::Type partialType, const ConvParams &params,
                            const ConvOptions &options, bool isJointPlan);

} // End namespace poplin

#endif // _ConvVertexType_hpp_
