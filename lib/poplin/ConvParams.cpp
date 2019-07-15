#include "poplin/ConvParams.hpp"
#include "poplibs_support/VectorUtils.hpp"
#include "poplin/ConvUtil.hpp"
#include <algorithm>
#include "poplibs_support/print.hpp"
#include "poputil/exceptions.hpp"
#include <boost/functional/hash.hpp>
#include <boost/functional/hash.hpp>

namespace poplin {

namespace {
// Return a convolution where the same input, kernel and output size match the
// specified convolution and where the output is all zero.
static ConvParams getZeroConv(const ConvParams &params) {
  // We represent the zero convolution as follows:
  // - truncate the input and the kernel to size zero.
  // - zero pad the input and the kernel to size one.
  // - convolve the input and kernel resulting in an output of size one.
  // - truncate the output to size zero.
  // - pad the output to match the expected output size.
  ConvParams zeroConv = params;
  const auto numFieldDims = params.getNumFieldDims();
  std::vector<unsigned> allZeros(numFieldDims, 0);
  std::vector<unsigned> allOnes(numFieldDims, 1);
  std::vector<bool> allFalse(numFieldDims, false);
  zeroConv.inputTransform.truncationLower = allZeros;
  zeroConv.inputTransform.truncationUpper =
      vectorConvert<unsigned>(params.inputFieldShape);
  zeroConv.inputTransform.dilation = allOnes;
  zeroConv.inputTransform.paddingLower = allOnes;
  zeroConv.inputTransform.paddingUpper = allZeros;
  zeroConv.inputTransform.flip = allFalse;
  zeroConv.kernelTransform.truncationLower = allZeros;
  zeroConv.kernelTransform.truncationUpper =
      vectorConvert<unsigned>(params.kernelShape);
  zeroConv.kernelTransform.dilation = allOnes;
  zeroConv.kernelTransform.paddingLower = allOnes;
  zeroConv.kernelTransform.paddingUpper = allZeros;
  zeroConv.kernelTransform.flip = allFalse;
  zeroConv.outputTransform.truncationLower = allZeros;
  zeroConv.outputTransform.truncationUpper = allOnes;
  zeroConv.outputTransform.stride = allOnes;
  zeroConv.outputTransform.paddingLower = allZeros;
  zeroConv.outputTransform.paddingUpper =
      vectorConvert<unsigned>(params.getOutputFieldShape());
  assert(zeroConv.getOutputFieldShape() == params.getOutputFieldShape());
  return zeroConv;
}
} // Anonymous namespace

ConvParams::InputTransform::
InputTransform(std::vector<unsigned> truncationLower_,
               std::vector<unsigned> truncationUpper_,
               std::vector<unsigned> dilation_,
               std::vector<unsigned> paddingLower_,
               std::vector<unsigned> paddingUpper_,
               std::vector<bool> flip_) :
    truncationLower(std::move(truncationLower_)),
    truncationUpper(std::move(truncationUpper_)),
    dilation(std::move(dilation_)),
    paddingLower(std::move(paddingLower_)),
    paddingUpper(std::move(paddingUpper_)),
    flip(flip_) {}

ConvParams::InputTransform::InputTransform(const std::size_t size) :
  InputTransform(std::vector<unsigned>(size, 0),
                 std::vector<unsigned>(size, 0),
                 std::vector<unsigned>(size, 1),
                 std::vector<unsigned>(size, 0),
                 std::vector<unsigned>(size, 0),
                 std::vector<bool>(size, false)) {}

ConvParams::OutputTransform::
OutputTransform(std::vector<unsigned> truncationLower_,
                std::vector<unsigned> truncationUpper_,
                std::vector<unsigned> stride_,
                std::vector<unsigned> paddingLower_,
                std::vector<unsigned> paddingUpper_) :
    truncationLower(std::move(truncationLower_)),
    truncationUpper(std::move(truncationUpper_)),
    stride(std::move(stride_)),
    paddingLower(std::move(paddingLower_)),
    paddingUpper(std::move(paddingUpper_))
{}

ConvParams::OutputTransform::OutputTransform(const std::size_t size) :
  OutputTransform(std::vector<unsigned>(size, 0),
                  std::vector<unsigned>(size, 0),
                  std::vector<unsigned>(size, 1),
                  std::vector<unsigned>(size, 0),
                  std::vector<unsigned>(size, 0)) {}

ConvParams::
ConvParams(poplar::Type inputType_,
           poplar::Type outputType_,
           std::size_t batchSize_,
           std::vector<std::size_t> inputFieldShape_,
           std::vector<std::size_t> kernelShape_,
           std::size_t inputChannels_,
           std::size_t outputChannels_,
           std::size_t numConvGroups_,
           InputTransform inputTransform_,
           InputTransform kernelTransform_,
           OutputTransform outputTransform_) :
    inputType(std::move(inputType_)),
    outputType(std::move(outputType_)),
    batchSize(batchSize_),
    inputFieldShape(std::move(inputFieldShape_)),
    kernelShape(std::move(kernelShape_)),
    inputChannels(inputChannels_),
    outputChannels(outputChannels_),
    numConvGroups(numConvGroups_),
    inputTransform(inputTransform_),
    kernelTransform(kernelTransform_),
    outputTransform(outputTransform_) {}

void ConvParams::validate() const {
  const auto numFieldDims = inputFieldShape.size();
  if (kernelShape.size() != numFieldDims) {
    throw poputil::poplibs_error("Number of kernel field dimensions does not"
                               "match the number of input field dimensions");
  }
  for(const auto stride : outputTransform.stride) {
    if(stride == 0) {
      throw poputil::poplibs_error("Stride must be non zero");
    }
  }
  for(const auto dilation : inputTransform.dilation) {
    if(dilation == 0) {
      throw poputil::poplibs_error("Input dilation must be non zero."
                                   " Dilation = 1 results in no dilation");
    }
  }
  for(const auto dilation : kernelTransform.dilation) {
    if(dilation == 0) {
      throw poputil::poplibs_error("Kernel dilation must be non zero."
                                   " Dilation = 1 results in no dilation");
    }
  }
  const std::pair<std::size_t, const char *> sizes[] = {
    {inputTransform.truncationLower.size(), "input truncation (lower)"},
    {inputTransform.truncationUpper.size(), "input truncation (upper)"},
    {inputTransform.dilation.size(), "input dilation"},
    {inputTransform.paddingLower.size(), "input padding (lower)"},
    {inputTransform.paddingUpper.size(), "input padding (upper)"},
    {inputTransform.flip.size(), "input flip"},
    {kernelTransform.truncationLower.size(), "kernel truncation (lower)"},
    {kernelTransform.truncationUpper.size(), "kernel truncation (upper)"},
    {kernelTransform.dilation.size(), "kernel dilation"},
    {kernelTransform.paddingLower.size(), "kernel padding (lower)"},
    {kernelTransform.paddingUpper.size(), "kernel padding (upper)"},
    {kernelTransform.flip.size(), "kernel flip"},
    {outputTransform.truncationLower.size(), "output truncation (lower)"},
    {outputTransform.truncationUpper.size(), "output truncation (upper)"},
    {outputTransform.stride.size(), "stride"},
    {outputTransform.paddingLower.size(), "output padding (lower)"},
    {outputTransform.paddingUpper.size(), "output padding (upper)"},
  };
  for (const auto &entry : sizes) {
    if (entry.first != numFieldDims) {
      throw poputil::poplibs_error(std::string("Number of ") + entry.second +
                                 " dimensions does not match the number of "
                                 "field dimensions");
    }
  }
  for (unsigned dim = 0; dim != numFieldDims; ++dim) {
    if (inputTransform.truncationLower[dim] +
        inputTransform.truncationUpper[dim] >
        inputFieldShape[dim]) {
      throw poputil::poplibs_error("Truncation for dimension " +
                                 std::to_string(dim) +
                                 " truncates by more than the size of the "
                                 "field");
    }
    if (kernelTransform.truncationLower[dim] +
        kernelTransform.truncationUpper[dim] >
        kernelShape[dim]) {
      throw poputil::poplibs_error("Truncation for dimension " +
                                 std::to_string(dim) +
                                 " truncates by more than the size of the "
                                 "kernel");
    }
    const auto transformedInputSize = getTransformedInputSize(dim);
    const auto transformedKernelSize = getTransformedKernelSize(dim);
    if (transformedKernelSize == 0) {
      throw poputil::poplibs_error("Transformed kernel for dimension " +
                                  std::to_string(dim) +
                                  " has zero size");
    }

    if (transformedInputSize < transformedKernelSize) {
      throw poputil::poplibs_error("Transformed input size for dimension " +
                                  std::to_string(dim) +
                                  " is less than the transformed kernel size");
    }
    const auto convOutSize = getUntransformedOutputSize(dim);
    if (outputTransform.truncationLower[dim] +
        outputTransform.truncationUpper[dim] >
        convOutSize) {
      throw poputil::poplibs_error("Output truncation for dimension " +
                                 std::to_string(dim) +
                                 " truncates by more than the size of the "
                                 "convolution output");
    }
  }
}

ConvParams::ConvParams(
  poplar::Type inputType_,
  poplar::Type outputType_,
  std::size_t batchSize_,
  std::vector<std::size_t> inputFieldShape_,
  std::vector<std::size_t> kernelShape_,
  std::size_t inputChannels_,
  std::size_t outputChannels_,
  std::size_t numConvGroups_) : ConvParams(
    inputType_,
    outputType_,
    batchSize_,
    inputFieldShape_,
    kernelShape_,
    inputChannels_,
    outputChannels_,
    numConvGroups_,
    InputTransform(inputFieldShape_.size()),
    InputTransform(inputFieldShape_.size()),
    OutputTransform(inputFieldShape_.size())) {}

ConvParams::ConvParams(
  poplar::Type dataType_,
  std::size_t batchSize_,
  std::vector<std::size_t> inputFieldShape_,
  std::vector<std::size_t> kernelShape_,
  std::size_t inputChannels_,
  std::size_t outputChannels_,
  std::size_t numConvGroups_) : ConvParams(
    dataType_,
    dataType_,
    batchSize_,
    inputFieldShape_,
    kernelShape_,
    inputChannels_,
    outputChannels_,
    numConvGroups_) {}

std::ostream& operator<<(std::ostream &os, const ConvParams &p) {
  os << "Params: inputType                  " << p.inputType << "\n";
  os << "        outputType                 " << p.outputType << "\n";
  os << "        batchSize                  " << p.batchSize << "\n";
  os << "        numConvGroups              " << p.numConvGroups << "\n";
  os << "        inputFieldShape            ";
  printContainer(p.inputFieldShape, os);
  os << "\n";
  os << "        kernelShape                ";
  printContainer(p.kernelShape, os);
  os << "\n";
  os << "        inputChannelsPerConvGroup  ";
  os << p.getNumInputChansPerConvGroup() << "\n";
  os << "        outputChannelsPerConvGroup ";
  os << p.getNumOutputChansPerConvGroup() << "\n";
  os << "        inputTruncationLower       ";
  printContainer(p.inputTransform.truncationLower, os);
  os << "\n";
  os << "        inputTruncationUpper       ";
  printContainer(p.inputTransform.truncationUpper, os);
  os << "\n";
  os << "        inputDilation              ";
  printContainer(p.inputTransform.dilation, os);
  os << "\n";
  os << "        inputPaddingLower          ";
  printContainer(p.inputTransform.paddingLower, os);
  os << "\n";
  os << "        inputPaddingUpper          ";
  printContainer(p.inputTransform.paddingUpper, os);
  os << "\n";
  os << "        flipInput                  ";
  printContainer(p.inputTransform.flip, os);
  os << "\n";
  os << "        kernelTruncationLower      ";
  printContainer(p.kernelTransform.truncationLower, os);
  os << "\n";
  os << "        kernelTruncationUpper      ";
  printContainer(p.kernelTransform.truncationUpper, os);
  os << "\n";
  os << "        kernelDilation             ";
  printContainer(p.kernelTransform.dilation, os);
  os << "\n";
  os << "        kernelPaddingLower         ";
  printContainer(p.kernelTransform.paddingLower, os);
  os << "\n";
  os << "        kernelPaddingUpper         ";
  printContainer(p.kernelTransform.paddingUpper, os);
  os << "\n";
  os << "        flipKernel                 ";
  printContainer(p.kernelTransform.flip, os);
  os << "\n";
  os << "        outputTruncationLower      ";
  printContainer(p.outputTransform.truncationLower, os);
  os << "\n";
  os << "        outputTruncationUpper      ";
  printContainer(p.outputTransform.truncationUpper, os);
  os << "\n";
  os << "        stride                     ";
  printContainer(p.outputTransform.stride, os);
  os << "\n";
  os << "        outputPaddingLower         ";
  printContainer(p.outputTransform.paddingLower, os);
  os << "\n";
  os << "        outputPaddingUpper         ";
  printContainer(p.outputTransform.paddingUpper, os);
  os << "\n";
  os << "        outputFieldShape           ";
  printContainer(p.getOutputFieldShape(), os);
  os << "\n";
  return os;
}

std::size_t hash_value(const ConvParams::InputTransform &it) {
  return std::hash<ConvParams::InputTransform>()(it);
}

std::size_t hash_value(const ConvParams::OutputTransform &ot) {
  return std::hash<ConvParams::OutputTransform>()(ot);
}

ConvParams ConvParams::canonicalize() const {
  validate();
  ConvParams newParams = *this;
  const auto numFieldDims = getNumFieldDims();
  for (unsigned dim = 0; dim != numFieldDims; ++dim) {
    const auto outSize = newParams.getOutputSize(dim);
    auto &inputTruncationLower = newParams.inputTransform.truncationLower[dim];
    auto &inputTruncationUpper = newParams.inputTransform.truncationUpper[dim];
    auto &inputPaddingLower = newParams.inputTransform.paddingLower[dim];
    auto &inputPaddingUpper = newParams.inputTransform.paddingUpper[dim];
    auto &kernelTruncationLower =
        newParams.kernelTransform.truncationLower[dim];
    auto &kernelTruncationUpper =
        newParams.kernelTransform.truncationUpper[dim];
    auto &kernelPaddingLower = newParams.kernelTransform.paddingLower[dim];
    auto &kernelPaddingUpper = newParams.kernelTransform.paddingUpper[dim];
    auto &outputTruncationLower =
        newParams.outputTransform.truncationLower[dim];
    auto &outputTruncationUpper =
        newParams.outputTransform.truncationUpper[dim];
    auto &outputPaddingLower = newParams.outputTransform.paddingLower[dim];
    auto &outputPaddingUpper = newParams.outputTransform.paddingUpper[dim];

    // Compute output elements that are known to be zero.
    auto nonZeroRange =
        getOutputRangeForKernelRange(dim, {0, newParams.getOutputSize(dim)},
                                     {0, newParams.kernelShape[dim]},
                                     newParams);
    // Truncate and pad the output so the number zero elements can be
    // determined directly from the output padding.
    if (nonZeroRange.first == nonZeroRange.second) {
      return getZeroConv(newParams);
    }
    const auto outputZerosLower = nonZeroRange.first;
    const auto outputZerosUpper = outSize - nonZeroRange.second;
    if (outputZerosLower > outputPaddingLower) {
      outputTruncationLower += (outputZerosLower - outputPaddingLower) *
                               newParams.outputTransform.stride[dim];
      outputPaddingLower = outputZerosLower;
    }
    if (outputZerosUpper > outputPaddingUpper) {
      outputTruncationUpper += (outputZerosUpper - outputPaddingUpper) *
                               newParams.outputTransform.stride[dim];
      outputPaddingUpper = outputZerosUpper;
    }
    // Truncate the output of the convolution so there are no excess elements
    // at the end that are ignored. If there are no ignored elements backprop
    // of the striding operation is input dilation with no padding.
    auto truncatedConvOutSize =
        newParams.getUntransformedOutputSize(dim) - (outputTruncationLower +
                                                     outputTruncationUpper);
    const auto ignored = (truncatedConvOutSize - 1) %
                         newParams.outputTransform.stride[dim];
    outputTruncationUpper += ignored;
    truncatedConvOutSize -= ignored;
    // Avoid unnecessary striding.
    if (truncatedConvOutSize == 1) {
      newParams.outputTransform.stride[dim] = 1;
    }
    // Compute input elements that are ignored.
    auto inputUsedRange =
        getInputRange(dim, {0, outSize},
                      {0, newParams.kernelShape[dim]}, newParams);
    // Truncate and pad the input so the number of ignored elements can
    // be determined directly from the input truncation.
    assert(inputUsedRange.first != inputUsedRange.second);
    const auto inputIgnoredLower = inputUsedRange.first;
    const auto inputIgnoredUpper = newParams.getInputSize(dim) -
                                   inputUsedRange.second;
    if (inputIgnoredLower > inputTruncationLower) {
      inputPaddingLower += (inputIgnoredLower - inputTruncationLower) *
                           newParams.inputTransform.dilation[dim];
      inputTruncationLower = inputIgnoredLower;
    }
    if (inputIgnoredUpper > inputTruncationUpper) {
      inputPaddingUpper += (inputIgnoredUpper - inputTruncationUpper) *
                           newParams.inputTransform.dilation[dim];
      inputTruncationUpper = inputIgnoredUpper;
    }

    // Compute kernel elements that are ignored.
    auto kernelUsedRange =
        getKernelRange(dim, {0, outSize},
                       {0, newParams.getInputSize(dim)}, newParams);
    // Truncate and pad the kernel so the number of ignored elements can
    // be determined directly from the kernel truncation.
    assert(kernelUsedRange.first != kernelUsedRange.second);
    const auto kernelIgnoredLower = kernelUsedRange.first;
    const auto kernelIgnoredUpper = newParams.kernelShape[dim] -
                                   kernelUsedRange.second;
    if (kernelIgnoredLower > kernelTruncationLower) {
      kernelPaddingLower += (kernelIgnoredLower - kernelTruncationLower) *
                           newParams.kernelTransform.dilation[dim];
      kernelTruncationLower = kernelIgnoredLower;
    }
    if (kernelIgnoredUpper > kernelTruncationUpper) {
      kernelPaddingUpper += (kernelIgnoredUpper - kernelTruncationUpper) *
                           newParams.kernelTransform.dilation[dim];
      kernelTruncationUpper = kernelIgnoredUpper;
    }

    // Remove padding if both the input and the kernel are padded.
    auto &flippedKernelPaddingLower =
        newParams.kernelTransform.flip[dim] ?
          newParams.kernelTransform.paddingUpper[dim] :
          newParams.kernelTransform.paddingLower[dim];
    auto &flippedKernelPaddingUpper =
        newParams.kernelTransform.flip[dim] ?
          newParams.kernelTransform.paddingLower[dim] :
          newParams.kernelTransform.paddingUpper[dim];
    auto &flippedPaddingLower =
        newParams.inputTransform.flip[dim] ?
          newParams.inputTransform.paddingUpper[dim] :
          newParams.inputTransform.paddingLower[dim];
    auto &flippedPaddingUpper =
        newParams.inputTransform.flip[dim] ?
          newParams.inputTransform.paddingLower[dim] :
          newParams.inputTransform.paddingUpper[dim];
    auto excessPaddingLower =
        std::min({flippedPaddingLower, flippedKernelPaddingLower,
                  newParams.getTransformedKernelSize(dim) - 1});
    flippedPaddingLower -= excessPaddingLower;
    flippedKernelPaddingLower -= excessPaddingLower;
    auto excessPaddingUpper =
        std::min({flippedPaddingUpper, flippedKernelPaddingUpper,
                  newParams.getTransformedKernelSize(dim) - 1});
    flippedPaddingUpper -= excessPaddingUpper;
    flippedKernelPaddingUpper -= excessPaddingUpper;

    // Remove padding if the input is padded and the output is truncated.
    excessPaddingLower =
        std::min({flippedPaddingLower, outputTruncationLower,
                  static_cast<unsigned>(
                    newParams.getUntransformedOutputSize(dim) - 1
                  )});
    flippedPaddingLower -= excessPaddingLower;
    outputTruncationLower -= excessPaddingLower;
    excessPaddingUpper =
        std::min({flippedPaddingUpper, outputTruncationUpper,
                  static_cast<unsigned>(
                    newParams.getUntransformedOutputSize(dim) - 1
                  )});
    flippedPaddingUpper -= excessPaddingUpper;
    outputTruncationUpper -= excessPaddingUpper;

    // Avoid unnecessary flipping / dilation.
    if (newParams.inputFieldShape[dim] <=
        newParams.inputTransform.truncationLower[dim] +
        1 + newParams.inputTransform.truncationUpper[dim]) {
      newParams.inputTransform.dilation[dim] = 1;
      if (newParams.inputTransform.flip[dim]) {
        newParams.inputTransform.flip[dim] = false;
        std::swap(newParams.inputTransform.paddingLower[dim],
                  newParams.inputTransform.paddingUpper[dim]);
      }
    }
    if (newParams.kernelShape[dim] <=
        newParams.kernelTransform.truncationLower[dim] + 1 +
        newParams.kernelTransform.truncationUpper[dim]) {
      newParams.kernelTransform.dilation[dim] = 1;
      if (newParams.kernelTransform.flip[dim]) {
        newParams.kernelTransform.flip[dim] = false;
        std::swap(newParams.kernelTransform.paddingLower[dim],
                  newParams.kernelTransform.paddingUpper[dim]);
      }
    }
    assert(newParams.getOutputSize(dim) == outSize);
  }
  return newParams;
}

} // namespace poplin

namespace std {

std::size_t
hash<poplin::ConvParams::InputTransform>::operator()(
  const poplin::ConvParams::InputTransform &it) const {
  std::size_t seed = 0;
  boost::hash_range(seed, std::begin(it.truncationLower),
                    std::end(it.truncationLower));
  boost::hash_range(seed, std::begin(it.truncationUpper),
                    std::end(it.truncationUpper));
  boost::hash_range(seed, std::begin(it.dilation), std::end(it.dilation));
  boost::hash_range(seed, std::begin(it.paddingLower),
                    std::end(it.paddingLower));
  boost::hash_range(seed, std::begin(it.paddingUpper),
                    std::end(it.paddingUpper));
  return seed;
}

std::size_t
hash<poplin::ConvParams::OutputTransform>::operator()(
  const poplin::ConvParams::OutputTransform &ot) const {
  std::size_t seed = 0;
  boost::hash_range(seed, std::begin(ot.truncationLower),
                    std::end(ot.truncationLower));
  boost::hash_range(seed, std::begin(ot.truncationUpper),
                    std::end(ot.truncationUpper));
  boost::hash_range(seed, std::begin(ot.stride), std::end(ot.stride));
  boost::hash_range(seed, std::begin(ot.paddingLower),
                    std::end(ot.paddingLower));
  boost::hash_range(seed, std::begin(ot.paddingUpper),
                    std::end(ot.paddingUpper));
  return seed;
}

std::size_t
hash<poplin::ConvParams>::operator()(const poplin::ConvParams &p) const {
  std::size_t seed = 0;
  // TODO: specialise std::hash for poplar::Type
  boost::hash_combine(seed, std::string(p.inputType.toString()));
  boost::hash_combine(seed, std::string(p.outputType.toString()));
  boost::hash_combine(seed, p.batchSize);
  boost::hash_range(seed, std::begin(p.inputFieldShape),
                    std::end(p.inputFieldShape));
  boost::hash_range(seed, std::begin(p.kernelShape), std::end(p.kernelShape));
  boost::hash_combine(seed, p.inputChannels);
  boost::hash_combine(seed, p.outputChannels);
  boost::hash_combine(seed, p.numConvGroups);
  boost::hash_combine(seed, p.inputTransform);
  boost::hash_combine(seed, p.kernelTransform);
  boost::hash_combine(seed, p.outputTransform);
  return seed;
}
} // namespace std