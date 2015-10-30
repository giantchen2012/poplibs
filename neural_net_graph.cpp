#include <poplar/Vertex.hpp>
#include <poplar/ComputeSet.hpp>
#include <poplar/DataElement.hpp>
#include <poplar/DataArray.hpp>
#include <iostream>
#include <iomanip>
#include <cmath>
#include "neural_net_common.h"

using namespace poplar;

static uint64_t dense_dotproduct_cycles(unsigned size) {
  return (size+1)/2+2;
}


/****************************************************************************/
/*            Auxiliary math functions                                      */
/****************************************************************************/

static float sigmoid(float x)
{
  return (1. / (1. + exp(-x)));
}

static float sigmoid_derivative(float x)
{
  return sigmoid(x) * (1. - sigmoid(x));
}

static float relu(float x)
{
  if (x > 0)
    return x;
  return 0;
}

static float relu_derivative(float x)
{
  if (x > 0)
    return 1;
  return 0;
}

static float nonlinearity(NonLinearityType t, float x) {
  switch (t) {
  case NON_LINEARITY_SIGMOID:
    return sigmoid(x);
  case NON_LINEARITY_RELU:
    return relu(x);
  case NON_LINEARITY_NONE:
    return x;
  }
}

static float nonlinearity_derivative(NonLinearityType t, float x) {
  switch (t) {
  case NON_LINEARITY_SIGMOID:
    return sigmoid_derivative(x);
  case NON_LINEARITY_RELU:
    return relu_derivative(x);
  case NON_LINEARITY_NONE:
    return 1;
  }
}

/****************************************************************************/
/*            Vertices                                                      */
/****************************************************************************/

/* To run as much in parallel as possible, batches of data are fed through
   the net in a pipelined fashion. During each compute step, each layer of
   the net will be processing a different item from the batch.

   To implement this, each vertex after the input layer
   takes an indexIn input from the previous layer telling it which item of the
   batch to process. It will process this data and pass it onto the next layer
   via an indexOut output.

   As well as the pipelining, there is a global state value fed to all
   vertices which controls which phase of the algorithm is currently being
   performed. This state is then set by the control program. */

/* The following enum provides the possible values of the state variable. */
typedef enum nn_state_t {
  INIT,
  TRAIN,
  TEST,
  WEIGHT_SYNC
} nn_state_t;


/* The following defines are special control codes for the indexIn/indexOut
   values which control the pipelining of compute. */
#define NOTHING_TO_PROCESS (-2)
#define DONE_PROCESSING    (-1)
#define START_WEIGHT_UPDATE (-3)

/* An input layer holds a a batch worth of one input to the net.
   Each iteration it will output a new item of the batch
   (feeding it into the net) and at the end will feed the DONE_PROCESSING
   control signal */
class InputVertex : public Vertex {
public:
  Vector<FPType> data;
  FPType activationOut;
  FPType z = 0;
  int indexOut;

  Input<nn_state_t> state;
  unsigned batchSize;

  bool compute() {
    if (state == INIT) {
      /* On initialization the indexOut variable is set to
         NOTHING_TO_PROCESS so the next layer will not do anything until
         this layer starts outputting */
      indexOut = NOTHING_TO_PROCESS;
      return true;
    }

    if (indexOut == DONE_PROCESSING)
      return true;

    if (indexOut == NOTHING_TO_PROCESS) {
      /* First compute step after init, start outputting data. */
      indexOut = 0;
    } else {
      indexOut++;
    }

    if (indexOut == batchSize) {
      /* The whole batch has been output. */
      indexOut = DONE_PROCESSING;
      return true;
    }

    /* The input layer will simple output data values until a whole
       batch has been output. */
    activationOut = data[indexOut];

    return false;
  }

  uint64_t getCycleEstimate() const {
    return 10;
  }
};


class LayeredInputVertex : public Vertex {
public:
  Vector<FPType> data;
  Vector<FPType> activationOut;
  FPType z = 0;
  int indexOut;

  Input<nn_state_t> state;
  unsigned batchSize;

  bool compute() {
    if (state == INIT) {
      /* On initialization the indexOut variable is set to
         NOTHING_TO_PROCESS so the next layer will not do anything until
         this layer starts outputting */
      indexOut = NOTHING_TO_PROCESS;
      return true;
    }

    if (indexOut == DONE_PROCESSING)
      return true;

    if (indexOut == NOTHING_TO_PROCESS) {
      /* First compute step after init, start outputting data. */
      indexOut = 0;
    } else {
      indexOut++;
    }

    if (indexOut == batchSize) {
      /* The whole batch has been output. */
      indexOut = DONE_PROCESSING;
      return true;
    }

    unsigned numOutputLayers = activationOut.size();
    /* The input layer will simple output data values until a whole
       batch has been output. */
    for (unsigned i = 0; i < numOutputLayers; ++i)
      activationOut[i] = data[indexOut * numOutputLayers + i];

    return false;
  }

  uint64_t getCycleEstimate() const {
    return 10;
  }
};

/** This vertex gathers together a vector of inputs into
    a dense vector.
    This will gather all the activations from a previous layer
    to pass on to all the vertices in the next layer. This is used as
    an optimization for fully connected layers.
 */
class InnerProductFwdGatherVertex : public Vertex {
public:
  Vector<Input<FPType>> activationIn;
  Vector<FPType> activationOut;

  Input<nn_state_t> state;
  Input<int> indexIn;

  bool compute() {
    if (state == INIT)
      return true;

    if (indexIn == NOTHING_TO_PROCESS ||
        indexIn == DONE_PROCESSING)
      return true;

    for (unsigned i = 0; i < activationOut.size(); ++i) {
      activationOut[i] = activationIn[i];
    }
    return true;
  }

  uint64_t getCycleEstimate() const  {
    if (state == INIT ||
        indexIn == NOTHING_TO_PROCESS ||
        indexIn == DONE_PROCESSING)
      return 5;

    return (activationIn.size()*2 + 10);
  }
};


class InnerProductFwdLayeredGatherVertex : public Vertex {
public:
  Vector<Input<Vector<FPType>>> activationIn;
  Vector<FPType> activationOut;

  Input<nn_state_t> state;
  Input<int> indexIn;
  int indexOut;

  bool compute() {
    if (state == INIT) {
      indexOut = NOTHING_TO_PROCESS;
      return true;
    }

    if (indexIn == NOTHING_TO_PROCESS ||
        indexIn == DONE_PROCESSING) {
      indexOut = indexIn;
      return true;
    }

    unsigned i = 0;
    for (unsigned j = 0; j < activationIn.size(); ++j) {
      for (unsigned k = 0; k < activationIn[j].size(); ++k) {
        activationOut[i++] = activationIn[j][k];
      }
    }

    indexOut = indexIn;
    return true;
  }

  uint64_t getCycleEstimate() const  {
    if (state == INIT ||
        indexIn == NOTHING_TO_PROCESS ||
        indexIn == DONE_PROCESSING)
      return 5;

    return (activationIn.size()*activationIn[0].size()*2 + 10);
  }
};

/* This vertex calculates the forward pass of the network.
   It implements one artificial neuron that performs a weighted sum
   of its inputs followed by a non-linear function (e.g. sigmoid or reLU). */
class InnerProductFwdVertex : public Vertex {
public:
  NonLinearityType nonLinearityType;

  Input<Vector<FPType>> activationIn;
  Input<int> indexIn;
  int indexOut;

  /* Both the weighted sum (z) and the activation are stored since the
     backward pass needs both these items of data. */
  FPType z;
  FPType activationOut;

  /* The weights aren't stored in this vertex but in a separate
     vertex that manages the weights.
     This is still efficient but allows the other vertex to sync
     weights with the backward pass after weight update has occurred.  */
  Input<Vector<FPType>> weights;
  Input<FPType> bias;

  Input<nn_state_t> state;

  bool compute() {

    /* Handle the pipeline control */
    if (state == INIT) {
      indexOut = NOTHING_TO_PROCESS;
      return true;
    }

    if (indexIn == NOTHING_TO_PROCESS)
      return false;

    if (indexIn == DONE_PROCESSING) {
      indexOut = DONE_PROCESSING;
      return true;
    }

    /* Perform a weighted sum of the inputs. */
    FPType sum = 0;
    for (unsigned i = 0;  i < activationIn.size(); ++i) {
      sum += activationIn[i] * weights[i];
    }

    /* Add the bias. */
    z = sum + bias;

    /* Apply the non-linearity to get the activation. */
    activationOut = nonlinearity(nonLinearityType, z);
    indexOut = indexIn;
    return false;
  }

  uint64_t getCycleEstimate() const {
    if (state == INIT ||
        indexIn == NOTHING_TO_PROCESS ||
        indexIn == DONE_PROCESSING)
      return 10;

    return dense_dotproduct_cycles(activationIn.size()) + 20;
  }

};



/** This vertex gathers together a vector of delta terms into a dense vector.
    This will gather all the deltas from a layer
    to pass on to all the vertices in the previous layer. This is used as
    an optimization for fully connected layers. */
class InnerProductBwdGatherVertex : public Vertex {
public:
  Vector<Input<FPType>> deltaIn;
  Vector<FPType> deltaOut;

  Input<nn_state_t> state;
  Input<int> indexIn;

  bool compute() {
    if (state == INIT || state == TEST)
      return true;

    if (indexIn == NOTHING_TO_PROCESS ||
        indexIn == DONE_PROCESSING ||
        indexIn == START_WEIGHT_UPDATE)
      return true;

    for (unsigned i = 0; i < deltaOut.size(); ++i) {
      deltaOut[i] = deltaIn[i];
    }
    return true;
  }

  uint64_t getCycleEstimate() const {
    return (deltaIn.size()*2 + 10);
  }
};

/* This vertex implements the back propagation pass of the algorithm.
   There are three phases: first, the deltas are calculated
   and stored for the batch, then a second phase calculates the gradients
   and updates the weights, finally a third phase syncs the new weights back
   with the forward pass vertices. */
class InnerProductBwdVertex : public Vertex {
public:
  /* This is the non-linearity of the *previous* layer */
  NonLinearityType nonLinearityType;

  Input<int> indexIn;
  /* The input delta is the partial derivative of the error with respect to
   *  the z-term (the sum before the non-linearity) of this vertex. */
  Input<Vector<FPType>> deltaIn;

  /* The output delta is the partical derivative of the error with respect to
   * the z-term of the connected vertex from the previous layer */
  FPType deltaOut;
  int indexOut;

  /* These weights are duplicated from the forward pass. These vectors
     across the backward vertices form the transpose matrix of the weight
     vectors across the forward vertices */
  Vector<FPType> weights;

  /* The backward layer needs to record the activations and z-terms from the
     forward layer to be able to calculate the gradients. */
  Input<FPType> activationIn;
  Input<FPType> zIn;
  Input<int> actIndexIn;
  Vector<FPType> zRecord, actRecord, bwdRecord;

  /* The learning rate is passed to this vertex from a central control
     vertex. */
  Input<FPType> eta;
  Input<nn_state_t> state;

  /* The following state is used to control the weight sync phase */
  Vector<FPType> weightSyncOutput;
  unsigned currentRank;
  bool doingWeightUpdate;

  bool compute() {

    if (state == INIT) {
      indexOut = NOTHING_TO_PROCESS;
      doingWeightUpdate = false;
      /* For weight sync */
      currentRank = 0;
      return true;
    }

    if (state == WEIGHT_SYNC) {
      unsigned N = weightSyncOutput.size();
      unsigned stride = weights.size() / N;
      /*  During weight sync, one weight is output each compute
          step to be transferred to the forward layers. */
      if (currentRank >= stride)
        return true;

      for (unsigned i = 0; i < N; ++i) {
        unsigned j = stride * i + currentRank;
        if (j < weights.size()) {
          weightSyncOutput[i] = weights[j];
        }
      }
      ++currentRank;
      return false;
    }

    if (state == TEST)
      return true;

    /* During the forward pass the backwards vertex needs to record
       the activations and z-terms going through the network. */
    if (actIndexIn != NOTHING_TO_PROCESS &&
	actIndexIn != DONE_PROCESSING) {
      actRecord[actIndexIn] = activationIn;
      zRecord[actIndexIn] = zIn;
    }

    if (doingWeightUpdate) {
      if (indexIn == DONE_PROCESSING) {
        indexOut = DONE_PROCESSING;
        return true;
      }

      /* Calculate the weight gradients and update the weights. */
      unsigned batchSize = actRecord.size();
      for (unsigned i = 0;  i < deltaIn.size(); ++i) {
        FPType gradient = actRecord[indexIn] * deltaIn[i];
        weights[i] += eta * gradient  / batchSize;
      }

     /* Make sure the next layer gets the correct delta for
        its weight update. */
      deltaOut = bwdRecord[indexIn];
      indexOut = indexIn;

    } else {

      if (indexIn == NOTHING_TO_PROCESS)
        return false;

      if (indexIn == START_WEIGHT_UPDATE) {
        doingWeightUpdate = true;
        indexOut = START_WEIGHT_UPDATE;
        return false;
      }

      /* This is the core of the back-propagation algorithm.
         The output delta is formed from the weighted sum of the
         input deltas. */
      FPType sum = 0;
      for (unsigned i = 0;  i < deltaIn.size(); ++i) {
        sum += deltaIn[i] * weights[i];
      }

      /* Apply the chain-rule on the non-linear function of the previous
         layer. */
      FPType nlGradient = nonlinearity_derivative(nonLinearityType,
                                                 zRecord[indexIn]);

      /* Pass on the error to the previous layer. */
      deltaOut = nlGradient * sum;

      /* Record the output for the weight update phase. */
      bwdRecord[indexIn] = deltaOut;

      /* Pass on the batch index to process to the previous layer. */
      indexOut = indexIn;
    }
    return false;
  }

  uint64_t getCycleEstimate() const {
    if (state == INIT)
      return 10;

    if (state == WEIGHT_SYNC)
      return 15;

    if (state == TEST)
      return 0;

    uint64_t cycles = 0;

    if (actIndexIn != NOTHING_TO_PROCESS &&
        actIndexIn != DONE_PROCESSING) {
      cycles += 5;
    }

    if (doingWeightUpdate) {
      if (indexIn == DONE_PROCESSING)
        return cycles + 5;

      return cycles + deltaIn.size() * 3 + 5;
    } else {
      if (indexIn == NOTHING_TO_PROCESS ||
          indexIn == START_WEIGHT_UPDATE)
        return cycles + 5;

      // weighted sum
      cycles += deltaIn.size() * 2;

      // non linearity
      cycles += 5;

      // other stuff
      cycles += 5;

      return cycles;
    }
  }
};

/* This vertex handles the gradient descent update for the bias terms.
 * During the backward pass the bias term is updated based on the delta
 * input coming from the next layer. */
class InnerProductBwdBiasVertex : public Vertex {
public:
  Input<FPType> deltaIn;
  Input<int> indexIn;

  FPType bias;
  
  Input<FPType> eta;
  Input<nn_state_t> state;
  unsigned batchSize;

  FPType update;
  bool updated;

  bool compute() {

    if (state == INIT) {
      update = 0;
      updated = false;
      return true;
    }

    if (state == TEST)
      return true;

    if (updated)
      return true;

    if (indexIn == NOTHING_TO_PROCESS)
      return false;

    if (indexIn == START_WEIGHT_UPDATE) {
      bias += eta * update / (FPType) batchSize;
      updated = true;
      return true;
    }

    if (indexIn == DONE_PROCESSING)
      return true;

    FPType gradient = deltaIn * 1;
    update += gradient * 1;
    return false;
  }

  uint64_t getCycleEstimate() const {
    if (state == INIT)
      return 5;
    if (state == TEST)
      return 0;
    if (indexIn == START_WEIGHT_UPDATE)
      return 15;
    return 10;
  }
};

/* This vertex supplies the weights and bias to the forward pass vertex for
   a test-only network. */
class InnerProductParamsFwdOnlyVertex : public Vertex {
public:
  Vector<FPType> weights;
  FPType bias;

  bool compute() {
    return true;
  }

  uint64_t getCycleEstimate() const {
    return 0;
  }
};




/* One of these vertices are created per layer.
   The vertex gathers weights from the backward pass vertices
   and passes on the vector to the forward pass vertices.

   Each compute step the backward layer passes on the weights
   for a different vertex in the forward layer. The relevant
   forward layer param vertex will copy out the weights on the
   correct iteration. */
class InnerProductParamsGatherVertex : public Vertex {
public:
  Vector<Input<FPType>> weightsIn;
  Vector<FPType> weightsOut;

  bool compute() {
    for (unsigned i = 0; i < weightsOut.size(); ++i) {
      weightsOut[i] = weightsIn[i];
    }
    return true;
  }

  uint64_t getCycleEstimate() const {
    return (weightsIn.size()*2 + 10);
  }

};

/* This vertex supplies the weights and bias to the forward pass vertex.
   On the weight sync pass it will copy the weights from the param gathering
   vertex on the correct iteration. */
class InnerProductParamsVertex : public Vertex {
public:
  Input<Vector<FPType>> weightsIn;
  Vector<FPType> weightsOut;
  Input<FPType> biasIn;
  FPType biasOut;

  unsigned currentRank;
  unsigned myRank;
  bool updated;

  Input<nn_state_t> state;

  bool compute() {
    if (state == INIT) {
      currentRank = 0;
      updated = false;
      return true;
    }

    if (state != WEIGHT_SYNC)
      return true;

    if (myRank == currentRank) {
      for (unsigned i = 0; i < weightsOut.size(); ++i) {
        weightsOut[i] = weightsIn[i];
      }
      biasOut = biasIn;
      updated = true;
    }
    currentRank += 1;
    return updated;
  }

  uint64_t getCycleEstimate() const {
    if (state == INIT)
      return 5;

    if (myRank == currentRank)
      return weightsIn.size()*2 + 10;

    return 10;
  }
};


class ConvLayerFwdVertex : public Vertex {
public:
  Vector<Input<Vector<FPType>>> activationIn;
  Input<int> indexIn;
  int indexOut;

  Vector<FPType> zOut, top, bottom;

  /* The weights aren't stored in this vertex but in a separate
     vertex that manages the weights.
     This is still efficient but allows the other vertex to sync
     weights with the backward pass after weight update has occurred.  */
  Input<Vector<FPType>> weights;

  Input<nn_state_t> state;

  bool compute() {

    /* Handle the pipeline control */
    if (state == INIT) {
      indexOut = NOTHING_TO_PROCESS;
      return true;
    }

    if (indexIn == NOTHING_TO_PROCESS)
      return false;

    if (indexIn == DONE_PROCESSING) {
      indexOut = DONE_PROCESSING;
      return true;
    }

    unsigned numInputLayers = activationIn[0].size();
    unsigned numOutputLayers = zOut.size();
    unsigned fmapSize = activationIn.size();

    unsigned wIndex = 0;


    for (unsigned i = 0; i < numOutputLayers; ++i) {
      FPType sum = 0;
      /* Perform a weighted sum of the inputs. */
      for (unsigned j = 0; j < fmapSize; j++) {
        for (unsigned k = 0;  k < numInputLayers; ++k) {
          FPType w = weights[wIndex++];
          sum += activationIn[j][k] * w;
        }
      }
      zOut[i] = sum;
    }

    indexOut = indexIn;
    return false;
  }

  uint64_t getCycleEstimate() const {
    if (state == INIT ||
        indexIn == NOTHING_TO_PROCESS ||
        indexIn == DONE_PROCESSING)
      return 5;

    // Sample ranges for this loop (from AlexNet, second conv: 16x25x12)
    // 16 * (zero + 25 * (ldptr + 12 * ( ldinc, ldinc, mac)) + storeinc)
    unsigned numInputLayers = activationIn[0].size();
    unsigned numOutputLayers = zOut.size();
    unsigned fmapSize = activationIn.size();

    return 10 + numOutputLayers * (2 + fmapSize * (1 + dense_dotproduct_cycles(numInputLayers)));

  }

};

class ConvReductionVertex : public Vertex {
public:
  NonLinearityType nonLinearityType;
  NormalizationType normalizationType;

  Vector<Input<Vector<FPType>>> zIn;
  Input<Vector<FPType>> bias;

  // These input the extra elements on the top and bottom of the
  // chunk - required for noramlization.
  Vector<Input<Vector<FPType>>> bottomIn;
  Input<Vector<FPType>> bottomBias;
  Vector<Input<Vector<FPType>>> topIn;
  Input<Vector<FPType>> topBias;

  Vector<FPType> activationOut, top, bottom;

  Input<nn_state_t> state;
  Input<int> indexIn;
  int indexOut;

  bool compute() {
    if (state == INIT) {
      indexOut = NOTHING_TO_PROCESS;
      return true;
    }

    if (indexIn == NOTHING_TO_PROCESS ||
        indexIn == DONE_PROCESSING) {
      indexOut = indexIn;
      return true;
    }

    for (unsigned i = 0; i < activationOut.size(); ++i) {
      FPType sum = 0;
      for (unsigned j = 0; j < zIn.size(); ++j) {
        sum += zIn[j][i];
      }
      sum += bias[i];
      activationOut[i] = nonlinearity(nonLinearityType, sum);
    }

    for (unsigned i = 0; i < top.size(); ++i) {
      FPType sum = 0;
      for (unsigned j = 0; j < topIn.size(); ++j) {
        sum += topIn[j][i];
      }
      sum += topBias[i];
      top[i] = nonlinearity(nonLinearityType, sum);
    }

    for (unsigned i = 0; i < bottom.size(); ++i) {
      FPType sum = 0;
      for (unsigned j = 0; j < bottomIn.size(); ++j) {
        sum += bottomIn[j][i];
      }
      sum += bottomBias[i];
      bottom[i] = nonlinearity(nonLinearityType, sum);
    }

    //Normalization
    switch (normalizationType) {
    case NORMALIZATION_NONE:
      break;
    case NORMALIZATION_LR:
      //TODO - make this configurable
      unsigned n = 5;
      unsigned N = activationOut.size();
      for (int i = 0; i < N; ++i) {
        FPType sum = 0;
        for (int j = -n/2; j < n/2; ++j) {
          if (i + j < 0) {
            sum += bottom[i + j + n/2] *
                   bottom[i + j + n/2];
          } else if (i + j > N) {
            sum += top[i + j - N] *
                   top[i + j - N];
          } else {
            sum += activationOut[i + j] * activationOut[i + j];
          }
        }
        // TODO - make these configurable
        FPType alpha = 0.0001, beta = 0.75, k = 2;
        activationOut[i] = pow(k + alpha * sum, beta);
      }
      break;
    }

    indexOut = indexIn;
    return true;
  }

  uint64_t getCycleEstimate() const  {
    if (state == INIT ||
        indexIn == NOTHING_TO_PROCESS ||
        indexIn == DONE_PROCESSING)
      return 5;

    uint64_t cycles = 0;

    cycles += activationOut.size() * (4 + zIn.size()*2);
    cycles += top.size() * (4 + topIn.size()*2);
    cycles += bottom.size() * (4 + bottomIn.size()*2);
    cycles += activationOut.size() * (5 * 2 + 10);
    return cycles;
  }
};


class ConvPaddingVertex : public Vertex {
public:
  Vector<FPType> activationOut;
  bool compute() {
    return true;
  }
  uint64_t getCycleEstimate() const {
    return 0;
  }
};

class ConvWeightsFwdOnlyVertex : public Vertex {
public:
  Vector<FPType> weights;

  bool compute() {
    return true;
  }

  uint64_t getCycleEstimate() const {
    return 0;
  }
};

class ConvBiasFwdOnlyVertex : public Vertex {
public:
  Vector<FPType> bias;
  Vector<FPType> topBias, bottomBias;

  bool compute() {
    return true;
  }

  uint64_t getCycleEstimate() const {
    return 0;
  }
};


class MaxPoolFwdVertex : public Vertex {
public:
  Vector<Input<Vector<FPType>>> activationIn;
  Input<int> indexIn;
  int indexOut;
  Vector<FPType> activationOut;
  Input<nn_state_t> state;

  bool compute() {

    /* Handle the pipeline control */
    if (state == INIT) {
      indexOut = NOTHING_TO_PROCESS;
      return true;
    }

    if (indexIn == NOTHING_TO_PROCESS)
      return false;

    if (indexIn == DONE_PROCESSING) {
      indexOut = DONE_PROCESSING;
      return true;
    }

    unsigned numLayers = activationOut.size();

    for (unsigned i = 0; i < numLayers; i++) {
      FPType m = activationIn[0][i];
      for (unsigned j = 1; j < activationIn.size(); ++j) {
        if (activationIn[j][i] > m)
          m = activationIn[j][i];
      }
      activationOut[i] = m;
    }
    indexOut = indexIn;
    return false;
  }

  uint64_t getCycleEstimate() const {
    if (state == INIT ||
        indexIn == NOTHING_TO_PROCESS ||
        indexIn == DONE_PROCESSING)
      return 5;

    return 10 + activationOut.size() * (2 + activationIn.size() * 2);
  }

};


/* The error vertex calculates the classification error (or loss) of the output
   of the last hidden layer of the network.
   It also calculates the partial derivative of that loss with respect its
   inputs and kicks off the backward pass. */
class ErrorVertex : public Vertex {
public:
  LossType lossType;

  Vector<Input<FPType>> zIn;
  Input<int> indexIn;

  /* The non-linearity type of the *previous* layer */
  NonLinearityType nonLinearityType;


  /* The delta out is the partial derivative of the loss with respect to the
     z term of the previous layer.  */
  Vector<FPType> deltaOut;
  int indexOut;

  /* The expected data */
  Vector<unsigned char> labels;

  Input<nn_state_t> state;
  unsigned batchSize;
  bool doingWeightUpdate;

  /* Probability vector - used for softmax */
  Vector<FPType> probs;

  FPType error;
  unsigned numCorrect;

  bool compute() {
    if (state == INIT) {
      error = 0;
      indexOut = NOTHING_TO_PROCESS;
      doingWeightUpdate = false;
      return true;
    }

    if (doingWeightUpdate) {
      if (indexOut == DONE_PROCESSING)
        return true;

      if (indexOut == START_WEIGHT_UPDATE) {
        indexOut = 0;
      } else {
        indexOut++;
        if (indexOut == batchSize) {
          indexOut = DONE_PROCESSING;
          return true;
        }
      }
    } else {
      if (indexIn == NOTHING_TO_PROCESS)
        return false;

      if (indexIn == DONE_PROCESSING) {
        /* When every element of the batch is processed, the
           weight update pass is kicked off. */
        indexOut = START_WEIGHT_UPDATE;
        doingWeightUpdate = true;
        return true;
      }
    }

    unsigned index = doingWeightUpdate ? indexOut : indexIn;
    unsigned E = labels[index];

    switch (lossType) {
    case SUM_SQUARED_LOSS:
      {
      /* Calculate the sum-squared error and the partial derivative
         to pass back. */
      FPType sum = 0;
      for (unsigned i = 0;  i < zIn.size(); ++i) {
        FPType expected = (i == E ? 1 : 0);
        FPType actual = nonlinearity(nonLinearityType, zIn[i]);
        FPType nlGradient = nonlinearity_derivative(nonLinearityType, zIn[i]);
        deltaOut[i] = (expected - actual) * nlGradient;
        sum += (expected - actual) *  (expected - actual);
      }
      error = sum;
      break;
      }
    case SOFTMAX_CROSS_ENTROPY_LOSS:
      /* Calculate the softmax probability distribution */
      for (unsigned i = 0;  i < zIn.size(); ++i) {
        FPType act = nonlinearity(nonLinearityType, zIn[i]);
        probs[i] = exp(act);
      }
      FPType sum = 0;
      for (FPType p : probs)
        sum += p;
      for (unsigned i = 0;  i < zIn.size(); ++i)
        probs[i] /= sum;

      /* Calculate the cross-entropy error and the partial derivative
         to pass back. */
      error = 0;
      for (unsigned i = 0;  i < probs.size(); ++i) {
        FPType expected = (i == E ? 1 : 0);
        FPType nlGradient = nonlinearity_derivative(nonLinearityType, zIn[i]);
        deltaOut[i] = -(probs[i] - expected) * nlGradient;
        error += expected * log(probs[i]);
      }
      break;
    }

    // Calculate the classification error for reporting test results
    // This assumes that the
    // non-linearity is monotonic, so the max output of the previous
    // layer is the max z-term of the previous layer.
    FPType max = zIn[0];
    unsigned maxIndex = 0;
    for (unsigned i = 0;  i < zIn.size(); ++i) {
      if (zIn[i] > max) {
        max = zIn[i];
        maxIndex = i;
      }
    }
    bool correct = (maxIndex == E);
    if (correct)
      numCorrect++;

    indexOut = index;
    return false;
  }

  uint64_t getCycleEstimate() const {
    if (state == INIT)
      return 5;

    if (doingWeightUpdate) {
      if (indexOut == DONE_PROCESSING)
        return 5;

      return 10;
    } else {
      if (indexIn == NOTHING_TO_PROCESS)
        return 5;

      if (indexIn == DONE_PROCESSING)
        return 5;
    }

    uint64_t cycles = 5;
    switch (lossType) {
    case SUM_SQUARED_LOSS:
      cycles += zIn.size() * 30;
      break;
    case SOFTMAX_CROSS_ENTROPY_LOSS:
      cycles += zIn.size() * 50;
      break;
    }

    cycles += zIn.size() * 10;

    cycles += 5;

    return cycles;
  }
};

/****************************************************************************/
/*            Vertices                                                      */
/****************************************************************************/

/* Run the weight sync pass over the graph. */
void weightSync(DataElement<nn_state_t> state,
                ComputeSet weightSyncCS)
{
  state = INIT;
  weightSyncCS.compute();
  state = WEIGHT_SYNC;
  bool complete = false;
  while (!complete) {
    complete = weightSyncCS.compute();
  }
}

/* Train on a single batch */
void trainOnBatch(DataElement<nn_state_t> state,
                  ComputeSet trainCS, ComputeSet weightSyncCS,
                  DataArray trainingData,  DataArray trainingLabels)
{
  trainingData.copyIn();
  trainingLabels.copyIn();
  state = INIT;
  trainCS.compute();
  state = TRAIN;
  bool complete = false;
  while (!complete) {
    complete = trainCS.compute();
  }
  weightSync(state, weightSyncCS);
}

/* Test on a single batch */
void testOnBatch(DataElement<nn_state_t> state,
                 ComputeSet testCS,
                 DataArray testData,  DataArray testLabels)
{
  testData.copyIn();
  testLabels.copyIn();
  state = INIT;
  testCS.compute();
  state = TEST;
  bool complete = false;
  while (!complete) {
    complete = testCS.compute();
  }
}

/* The main algorithm, repeatedly train on batches from the training
   set whilst occasionally testing on the test set and reporting the error. */
__control__
void doTraining(ComputeSet trainCS, ComputeSet testCS, ComputeSet weightSyncCS,
                DataElement<nn_state_t> state,
                DataArray initialParams,
                DataArray trainingData,  DataArray trainingLabels,
                DataArray testData,  DataArray testLabels,
                unsigned batchSize,
                DataElement<unsigned> numBatches,
                DataElement<unsigned> numCorrect,
                unsigned numTestBatches,
                unsigned numBatchesBetweenTests,
                bool singleBatchProfile) {
  if (singleBatchProfile) {
    trainOnBatch(state, trainCS, weightSyncCS, trainingData, trainingLabels);
    return;
  }

  std::cout << "-- Initializing params.\n";
  initialParams.copyIn();
  weightSync(state, weightSyncCS);

  std::cout << "-- Training with batch size " << batchSize << ".\n";
  for (unsigned i = 0; i < numBatches; i++) {
    trainOnBatch(state, trainCS, weightSyncCS, trainingData, trainingLabels);
    if (i % numBatchesBetweenTests == 0) {
      numCorrect = 0;
      for (unsigned j = 0; j < numTestBatches; j++) {
        testOnBatch(state, trainCS, testData, testLabels);
      }
      unsigned numTests = (numTestBatches * batchSize);
      float percentCorrect = 100 * ((float) numCorrect) / numTests;
      std::cout << "--- Accuracy after " << i << " batches = "
                << percentCorrect << "%\n";
    }
  }
}

__control__
void doTest(ComputeSet testCS,
            DataElement<nn_state_t> state,
            DataArray initialParams,
            DataArray testData,  DataArray testLabels,
            unsigned batchSize,
            DataElement<unsigned> numBatches,
            DataElement<unsigned> numCorrect,
            unsigned numTestBatches,
            bool singleBatchProfile) {
  if (singleBatchProfile) {
    testOnBatch(state, testCS, testData, testLabels);
    return;
  }

  std::cout << "-- Initializing params.\n";
  initialParams.copyIn();

  std::cout << "-- Testing.\n";
  numCorrect = 0;
  for (unsigned j = 0; j < numTestBatches; j++) {
    testOnBatch(state, testCS, testData, testLabels);
  }
  unsigned numTests = (numTestBatches * batchSize);
  float percentCorrect = 100 * ((float) numCorrect) / numTests;
  std::cout << "--- Accuracy = " << percentCorrect << "%\n";
}
