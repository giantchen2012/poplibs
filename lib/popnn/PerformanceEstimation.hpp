#ifndef _performance_estimation_h_
#define _performance_estimation_h_

#include "popnn/NonLinearity.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <numeric>
#include <vector>

inline uint64_t getNonLinearityCycles(std::vector<unsigned> regionSizes,
                                      popnn::NonLinearityType nonLinearityType,
                                      bool isFloat,
                                      bool is2D,
                                      bool supervisorVertex,
                                      unsigned dataPathWidth,
                                      unsigned numWorkers) {
  uint64_t cycles = 0;
  if (!is2D)
    assert(regionSizes.size() == 1);
  for (const auto numItems : regionSizes) {
    const auto floatVectorWidth = dataPathWidth / 32;
    const auto halfVectorWidth =  dataPathWidth / 16;
    const auto transHalfVectorWidth = 2;
    cycles += 10;
    switch (nonLinearityType) {
    case popnn::NonLinearityType::RELU:
      {
        const unsigned numBlocks = isFloat ?
                  (numItems + floatVectorWidth - 1) / floatVectorWidth :
                  (numItems+ halfVectorWidth - 1) / halfVectorWidth;
        cycles += (numBlocks / 2) * 3 + (numBlocks & 1);
      }
      break;
    case popnn::NonLinearityType::SIGMOID:
      // scalar operation for floats, vector operation for halves
      // sigm is ~5 cycles for float, ~2 cycles for half
      if (isFloat) {
        cycles += numItems * 7;
      } else {
        cycles += 2 * (numItems + transHalfVectorWidth - 1)
                      / transHalfVectorWidth;
      }
      break;
    case popnn::NonLinearityType::TANH:
      // scalar operation for floats, vector operation for halves
      // tanh is ~5 cycles for float, always 1 cycle for half.
      if (isFloat) {
        cycles += numItems * 5;
      } else {
        cycles += (numItems + transHalfVectorWidth - 1) / transHalfVectorWidth;
      }
      break;
    case popnn::NonLinearityType::SOFTMAX:
      throw std::runtime_error("Nonlinearity not implemented as a "
                               "single vertex");
    default:
      throw std::runtime_error("Invalid nonlinearity type");
    }
  }
  if (!is2D) {
    // no outer loop
    cycles -= 2;
    // scaled32 pointer
    cycles += 1+2; // form base constant, add+shift
  }

  if (supervisorVertex) {
    // We don't account for the possible future existence of a 2D
    // supervisor vertex.
    assert(!is2D);
    cycles = (cycles + numWorkers - 1) / numWorkers;
    cycles += 9;
  } else {
    cycles += 5;
  }

  return cycles;
}

inline uint64_t getBwdNonlinearityDerivativeCycles(
                  std::vector<unsigned> regionSizes,
                  popnn::NonLinearityType nonLinearityType,
                  bool isFloat,
                  bool is2D,
                  bool supervisorVertex,
                  unsigned dataPathWidth,
                  unsigned numWorkers) {
  uint64_t cycles = supervisorVertex ? 9 : 5; // vertex overhead;
  if (!is2D)
    assert(regionSizes.size() == 1);
  for (const auto numItems : regionSizes) {
    const unsigned vectorWidth = dataPathWidth / (isFloat ? 32 : 16);
    const unsigned numVectors = (numItems + vectorWidth - 1) / vectorWidth;
    // scaled32 pointers for out/outGrad
    switch (nonLinearityType) {
    case popnn::NonLinearityType::SIGMOID:
      cycles += 5 + numVectors * 3;
      break;
    case popnn::NonLinearityType::RELU:
      {
        const unsigned vertexOverhead =
                                       // run instruction
                                       (supervisorVertex ? 0 : 2)
                                       + 7; // remaining vertex overhead
        cycles += vertexOverhead + numVectors * 3;
      }
      break;
    case popnn::NonLinearityType::TANH:
      cycles += 5 + numVectors * 3;
      break;
    case popnn::NonLinearityType::SOFTMAX:
      throw std::runtime_error("Nonlinearity not implemented");
    default:
      throw std::runtime_error("Invalid nonlinearity type");
    }
  }
  if (!is2D) {
    // no outer loop
    cycles -= 4;
    // scaled32 pointer for inGrad
    cycles += 1+3*2; // 3pointers*add+shift
  }
  if (supervisorVertex)
    cycles = numWorkers * cycles + 9;
  return cycles;
}

inline uint64_t getLossTransformCycles(const bool isFloat,
                                       const bool isSoftmax,
                                       const std::size_t size) {
  uint64_t cycles =
        5 // vertex overhead;
      + 5 // 5 loads of pointers
      + 5 // get base and pointer shifts
      + (isFloat ? 0 : 1) // shift size for halves
      + 2 // 2 load aheads
      + 1 // repeat instruction
      + (isSoftmax ? 5 : 4) * (isFloat ? size : size / 2) // loop
      + (isFloat ? 0 : (2 + (size & 0x1 ? (isSoftmax ? 7 : 6) : 0))) // RMW
      + 1; // exit instruction
  return cycles;
}

#endif // _performance_estimation_h_
