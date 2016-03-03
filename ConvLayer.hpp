#ifndef _conv_layer_hpp_
#define _conv_layer_hpp_
#include "Net.hpp"



class ConvLayer : public Layer {
public:
  unsigned kernelSize;
  unsigned stride;
  unsigned padding;
  unsigned numInputGroups;
  unsigned numChannels;
  NonLinearityType nonLinearityType;
  NormalizationType normalizationType;

  Tensor weights, biases, z, activations;

  std::string dType;

  unsigned xDim, yDim, prevChannels, xDimOut, yDimOut, weightsPerOutputChannel;

  std::string layerName;

  ConvLayer(unsigned kernelSize,
            unsigned stride,
            unsigned padding,
            unsigned numInputGroups,
            unsigned numChannels,
            NonLinearityType nonLinearityType,
            NormalizationType normalizationType) :
    kernelSize(kernelSize),
    stride(stride),
    padding(padding),
    numInputGroups(numInputGroups),
    numChannels(numChannels),
    nonLinearityType(nonLinearityType),
    normalizationType(normalizationType) {
    layerName = "Conv" + std::to_string(kernelSize) + "x" +
                std::to_string(kernelSize);
  }

  Tensor getFwdActivations() const {
    return activations;
  }

  Tensor getFwdZs() const {
    return z;
  }

  Tensor getBwdErrors() const {
    // TODO
  }

  NonLinearityType getNonLinearityType() const {
    return nonLinearityType;
  }

  void describe(std::ostream &out) {
    unsigned numParams = weights.numElements() + biases.numElements();
    out << "   -- Convolutional layer:\n"
        << "        Size: " << kernelSize << "x" << kernelSize << "\n"
        << "        Stride: " << stride << "\n"
        << "        Padding: " << padding << "\n"
        << "        Input: " << xDim << "x" << yDim
                    <<   "x" << prevChannels << "\n"
        << "        Output: " << xDimOut << "x" << yDimOut
                     <<   "x" << numChannels << "\n"
        << "        Params: " << numParams << "\n";
  }

  void init(Graph &graph, IPUModelEngineBuilder::TileMapping *mapping,
            Layer *prev, Layer *next, NetType netType, float eta,
            unsigned batchSize, unsigned numIPUs, unsigned tilesPerIPU,
            const std::string &dType) {
    Layer::init(numIPUs, tilesPerIPU);
    this->dType = dType;
    Tensor in = prev->getFwdActivations();
    xDim = in.dim(0);
    yDim = in.dim(1);
    prevChannels = in.dim(2);
    xDimOut = (xDim + padding - kernelSize) / stride + 1;
    yDimOut = (yDim + padding - kernelSize) / stride + 1;
    weightsPerOutputChannel = kernelSize * kernelSize * prevChannels + 1;
    z = graph.addTensor(dType, {xDimOut, yDimOut, numChannels});
    activations = graph.addTensor(dType, {xDimOut, yDimOut, numChannels});
    weights = graph.addTensor(dType, {numChannels,
                                      kernelSize,
                                      kernelSize * prevChannels});
    biases = graph.addTensor(dType, {numChannels});
    mapTensor(z, mapping);
    mapTensor(activations, mapping);
    mapTensor(weights, mapping);
    mapTensor(biases, mapping);
  }

  Program initParams(Graph &graph) {
    // TODO
    return Sequence();
  }

  Program startBatch(Graph &graph) {
    // TODO
    return Sequence();
  }

  Program forward(Graph &graph, IPUModelEngineBuilder::TileMapping *mapping,
                  Layer *prev)  {
    Tensor in = prev->getFwdActivations();
    ComputeSet fwd =
      graph.createComputeSet(layerName + ".fwd");
    for (unsigned chan = 0; chan < numChannels; ++chan) {
      for (unsigned i = 0; i < xDimOut; ++i) {
        for (unsigned j = 0; j < yDimOut; ++j) {
          unsigned width = std::min(i * stride + kernelSize, xDim) - i * stride;
          unsigned height = std::min(j * stride + kernelSize, yDim) - j * stride;
          // Create window into previous layer
          Tensor window =
            in.slice({i * stride, j * stride, 0 },
                     {i * stride + width, j * stride + height, prevChannels})
              .reshape({width, height * prevChannels});
          // Get weights that match window size
          Tensor w =
            weights[chan].slice({0, 0}, {width, height * prevChannels});

          auto v = graph.addVertex(fwd, "Convolution",
            { {"activationIn", window},
              {"weights", w},
              {"bias", biases[chan]},
              {"activationOut", activations[i][j][chan]} });
          graph.setInitialValue(v["nonLinearityType"], nonLinearityType);
        }
      }
    }
    mapComputeSet(graph, fwd, mapping);
    return Execute(fwd);
  }

  Program backward(Graph &graph, Layer *prev, Layer *next) {
    // TODO
    return Sequence();
  }

  Program weightSync(Graph &graph) {
    // TODO
    return Sequence();
  }

};



#endif // _conv_layer_hpp_
