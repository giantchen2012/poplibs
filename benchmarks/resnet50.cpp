#include <initializer_list>
#include "popnn/Net.hpp"

/** This model is derived from the paper:

    Deep Residual Learning for Image Recognition
    Kaiming He, Xiangyu Zhang, Shaoqing Ren, Jian Sun
    http://arxiv.org/abs/1512.03385

    The details are summarized in a internal spreadsheet comparing different
    imagenet models (congidox document XM-010286-UN).
*/

int main(int argc, char **argv) {
  DataSet IMAGENET;
  IMAGENET.dataSize = 224*224*4;
  IMAGENET.dim = std::vector<std::size_t>{224,224,4};
  IMAGENET.numTraining = 1;
  IMAGENET.numTest = 1;
  IMAGENET.testLabels =
    std::unique_ptr<unsigned[]>(new unsigned[IMAGENET.numTest]);
  IMAGENET.testData =
    std::unique_ptr<float[]>(new float[IMAGENET.dataSize * IMAGENET.numTest]);
  IMAGENET.trainingLabels =
    std::unique_ptr<unsigned[]>(new unsigned[IMAGENET.numTraining]);
  IMAGENET.trainingData =
    std::unique_ptr<float[]>(new float[IMAGENET.dataSize *
                                       IMAGENET.numTraining]);
  NetOptions options;
  options.doComputation = true;
  options.useIPUModel = true;
  options.doTestsDuringTraining = false;
  options.ignoreData = true;
  bool doTraining = false;
  if (!parseCommandLine(argc, argv, options, doTraining))
    return 1;
  NetType netType = doTraining ? TrainingNet : TestOnlyNet;

  auto resMethod = RESIDUAL_PAD;

  Net net(IMAGENET,
          options.batchSize,
          makeLayers({

            // 7x7,64, stride 2
            new ConvLayer(7, 2, 3, 64, NON_LINEARITY_RELU),
            // 3x3 max pool, stride 2
            new MaxPoolLayer(3, 2, 1),

            // 1x1,64 -> 3x3, 64-> 1x1, 256 (residual) X 3
            new ConvLayer(1, 1, 0, 64, NON_LINEARITY_RELU),
            new ConvLayer(3, 1, 1, 64, NON_LINEARITY_RELU),
            new ConvResLayer(1, 1, 0, 256, NON_LINEARITY_RELU, 3, resMethod),
            new ConvLayer(1, 1, 0, 64, NON_LINEARITY_RELU),
            new ConvLayer(3, 1, 1, 64, NON_LINEARITY_RELU),
            new ConvResLayer(1, 1, 0, 256, NON_LINEARITY_RELU, 3, resMethod),
            new ConvLayer(1, 1, 0, 64, NON_LINEARITY_RELU),
            new ConvLayer(3, 1, 1, 64, NON_LINEARITY_RELU),
            new ConvResLayer(1, 1, 0, 256, NON_LINEARITY_RELU, 3, resMethod),

            // 1x1,128 -> 3x3, 128-> 1x1, 512 (residual) X 4
            new ConvLayer(1, 2, 0, 128, NON_LINEARITY_RELU),  // DOWN SAMPLE
            new ConvLayer(3, 1, 1, 128, NON_LINEARITY_RELU),
            new ConvResLayer(1, 1, 0, 512, NON_LINEARITY_RELU, 3, resMethod),
            new ConvLayer(1, 1, 0, 128, NON_LINEARITY_RELU),
            new ConvLayer(3, 1, 1, 128, NON_LINEARITY_RELU),
            new ConvResLayer(1, 1, 0, 512, NON_LINEARITY_RELU, 3, resMethod),
            new ConvLayer(1, 1, 0, 128, NON_LINEARITY_RELU),
            new ConvLayer(3, 1, 1, 128, NON_LINEARITY_RELU),
            new ConvResLayer(1, 1, 0, 512, NON_LINEARITY_RELU, 3, resMethod),
            new ConvLayer(1, 1, 0, 128, NON_LINEARITY_RELU),
            new ConvLayer(3, 1, 1, 128, NON_LINEARITY_RELU),
            new ConvResLayer(1, 1, 0, 512, NON_LINEARITY_RELU, 3, resMethod),

            // 1x1,256 -> 3x3, 256-> 1x1, 1024 (residual) X 6
            new ConvLayer(1, 2, 0, 256, NON_LINEARITY_RELU),  // DOWN SAMPLE
            new ConvLayer(3, 1, 1, 256, NON_LINEARITY_RELU),
            new ConvResLayer(1, 1, 0, 1024, NON_LINEARITY_RELU, 3, resMethod),
            new ConvLayer(1, 1, 0, 256, NON_LINEARITY_RELU),
            new ConvLayer(3, 1, 1, 256, NON_LINEARITY_RELU),
            new ConvResLayer(1, 1, 0, 1024, NON_LINEARITY_RELU, 3, resMethod),
            new ConvLayer(1, 1, 0, 256, NON_LINEARITY_RELU),
            new ConvLayer(3, 1, 1, 256, NON_LINEARITY_RELU),
            new ConvResLayer(1, 1, 0, 1024, NON_LINEARITY_RELU, 3, resMethod),
            new ConvLayer(1, 1, 0, 256, NON_LINEARITY_RELU),
            new ConvLayer(3, 1, 1, 256, NON_LINEARITY_RELU),
            new ConvResLayer(1, 1, 0, 1024, NON_LINEARITY_RELU, 3, resMethod),
            new ConvLayer(1, 1, 0, 256, NON_LINEARITY_RELU),
            new ConvLayer(3, 1, 1, 256, NON_LINEARITY_RELU),
            new ConvResLayer(1, 1, 0, 1024, NON_LINEARITY_RELU, 3, resMethod),
            new ConvLayer(1, 1, 0, 256, NON_LINEARITY_RELU),
            new ConvLayer(3, 1, 1, 256, NON_LINEARITY_RELU),
            new ConvResLayer(1, 1, 0, 1024, NON_LINEARITY_RELU, 3, resMethod),

            // 1x1,512 -> 3x3, 512 -> 1x1, 2048 (residual) X 3
            new ConvLayer(1, 2, 0, 512, NON_LINEARITY_RELU),  // DOWN SAMPLE
            new ConvLayer(3, 1, 1, 512, NON_LINEARITY_RELU),
            new ConvResLayer(1, 1, 0, 2048, NON_LINEARITY_RELU, 3, resMethod),
            new ConvLayer(1, 1, 0, 512, NON_LINEARITY_RELU),
            new ConvLayer(3, 1, 1, 512, NON_LINEARITY_RELU),
            new ConvResLayer(1, 1, 0, 2048, NON_LINEARITY_RELU, 3, resMethod),
            new ConvLayer(1, 1, 0, 512, NON_LINEARITY_RELU),
            new ConvLayer(3, 1, 1, 512, NON_LINEARITY_RELU),
            new ConvResLayer(1, 1, 0, 2048, NON_LINEARITY_RELU, 3, resMethod),

            new MaxPoolLayer(7, 7),
            new FullyConnectedLayer(1000, NON_LINEARITY_RELU),
          }),
          SOFTMAX_CROSS_ENTROPY_LOSS,
          0.9, // learning rate
          netType,
          FP16,
          options
          );

  net.run(1);
  return 0;
}
