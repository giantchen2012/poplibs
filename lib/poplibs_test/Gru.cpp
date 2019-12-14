// Copyright (c) Graphcore Ltd, All rights reserved.
#include <boost/multi_array.hpp>
#include <cassert>
#include <poplibs_test/GeneralMatrixAdd.hpp>
#include <poplibs_test/GeneralMatrixMultiply.hpp>
#include <poplibs_test/Gru.hpp>
#include <poplibs_test/NonLinearity.hpp>

//#define DEBUG_TENSOR

using IndexRange = boost::multi_array_types::index_range;
using Array1dRef = boost::multi_array_ref<double, 1>;
using Array2dRef = boost::multi_array_ref<double, 2>;
using Array2d = boost::multi_array<double, 2>;
using Array3dRef = boost::multi_array_ref<double, 3>;
using Array4dRef = boost::multi_array_ref<double, 4>;
using Array3d = boost::multi_array<double, 3>;

using namespace poplibs_test;

// Fwd state array indices
#define GRU_FWD_STATE_RESET_GATE 0
#define GRU_FWD_STATE_UPDATE_GATE 1
#define GRU_FWD_STATE_CANDIDATE 2
#define GRU_FWD_STATE_OUTPUT 3

#define GRU_BWD_STATE_RESET_GATE 0
#define GRU_BWD_STATE_UPDATE_GATE 1
#define GRU_BWD_STATE_CANDIDATE 2

static void matrixOne(boost::multi_array_ref<double, 2> matA) {
  std::fill(matA.data(), matA.data() + matA.num_elements(), 1.0);
}

static void matrixZero(boost::multi_array_ref<double, 2> matA) {
  std::fill(matA.data(), matA.data() + matA.num_elements(), 0.0);
}

static void matrixZero(boost::multi_array_ref<double, 3> matA) {
  std::fill(matA.data(), matA.data() + matA.num_elements(), 0.0);
}

/**
 * Process a given unit type within an GRU given its weights and biases.
 * The non-linearity is also specified although it may be derived from the unit
 */
static void processBasicGruUnit(const Array2dRef prevOutput,
                                const Array2dRef input,
                                const Array3dRef weightsInput,
                                const Array3dRef weightsOutput,
                                const Array2dRef biases, Array2dRef output,
                                enum BasicGruCellUnit gruUnit,
                                popnn::NonLinearityType nonLinearityType) {
  const auto batchSize = prevOutput.shape()[0];
  const auto outputSize = prevOutput.shape()[1];

  /* split weight into two parts:
   * 1) part which weighs only the previous output
   * 2) part which weighs only the input
   */
  Array2d weightsOutputUnit = weightsOutput[gruUnit];
  Array2d weightsInputUnit = weightsInput[gruUnit];

  gemm::generalMatrixMultiply(prevOutput, weightsOutputUnit, output, false,
                              false);
  gemm::generalMatrixMultiply(input, weightsInputUnit, output, output, 1.0, 1.0,
                              false, false);
  /* add bias */
  for (auto b = 0U; b != batchSize; ++b) {
    for (auto i = 0U; i != outputSize; ++i) {
      output[b][i] += biases[gruUnit][i];
    }
  }

  /* apply non-linearity */
  nonLinearity(nonLinearityType, output);
}

static void printMatrix2d(FILE *fp, std::string msg, Array2dRef in) {
  if (!fp)
    return;

  fprintf(fp, "%s: {\n", msg.c_str());
  unsigned matRows = in.shape()[0];
  unsigned matCols = in.shape()[1];

  for (auto r = 0U; r != matRows; ++r) {
    fprintf(fp, " {");
    for (auto c = 0U; c != matCols; ++c) {
      if (c != matCols - 1)
        fprintf(fp, "%f,", in[r][c]);
      else
        fprintf(fp, "%f}\n", in[r][c]);
    }
  }
  fprintf(fp, "}\n");
}

static void printMatrix3d(FILE *fp, std::string msg, Array3dRef in) {
  if (!fp)
    return;

  fprintf(fp, "%s: {\n", msg.c_str());
  unsigned matRows = in.shape()[0];
  unsigned matCols = in.shape()[1];
  unsigned matInner = in.shape()[2];

  for (auto r = 0U; r != matRows; ++r) {
    fprintf(fp, " {\n");
    for (auto c = 0U; c != matCols; ++c) {
      fprintf(fp, "  {");
      for (auto i = 0U; i != matInner; ++i) {
        if (i != matInner - 1)
          fprintf(fp, "%f,", in[r][c][i]);
        else
          fprintf(fp, "%f}\n", in[r][c][i]);
      }
    }
    fprintf(fp, " }\n");
  }
  fprintf(fp, "}\n");
}

void poplibs_test::gru::basicGruCellForwardPass(const Array3dRef input,
                                                const Array2dRef biases,
                                                const Array2dRef prevOutput,
                                                const Array3dRef weightsInput,
                                                const Array3dRef weightsOutput,
                                                Array4dRef state) {
  const auto sequenceSize = state.shape()[1];
  const auto batchSize = state.shape()[2];
  const auto outputSize = state.shape()[3];
#ifndef NDEBUG
  const auto inputSize = input.shape()[2];
#endif
  assert(state.shape()[0] == GRU_NUM_FWD_STATES);
  assert(weightsInput.shape()[0] == BASIC_GRU_CELL_NUM_UNITS);
  assert(weightsInput.shape()[1] == inputSize);
  assert(weightsInput.shape()[2] == outputSize);
  assert(weightsOutput.shape()[0] == BASIC_GRU_CELL_NUM_UNITS);
  assert(weightsOutput.shape()[1] == outputSize);
  assert(weightsOutput.shape()[2] == outputSize);
  assert(biases.shape()[0] == BASIC_GRU_CELL_NUM_UNITS);
  assert(biases.shape()[1] == outputSize);
  assert(prevOutput.shape()[0] == batchSize);
  assert(prevOutput.shape()[1] == outputSize);

  FILE *fp = NULL;
#ifdef DEBUG_TENSOR
  fp = fopen("fwd.txt", "w");
#endif
  printMatrix3d(fp, "fwd weightsInput", weightsInput);
  printMatrix3d(fp, "fwd weightsOutput", weightsOutput);
  printMatrix2d(fp, "fwd bias", biases);
  for (auto s = 0U; s != sequenceSize; ++s) {
    if (fp)
      fprintf(fp, "fwd Loop: {%d}\n", s);
    Array2d ysm1 = s == 0 ? state[GRU_FWD_STATE_ACTS_IDX][s]
                          : state[GRU_FWD_STATE_ACTS_IDX][s - 1];
    Array2d prevOutputThisStep = s == 0 ? prevOutput : ysm1;
    Array2d inputThisStep = input[s];

    printMatrix2d(fp, "fwd h_prev", prevOutputThisStep);
    printMatrix2d(fp, "fwd input", inputThisStep);

    /* update gate */
    Array2d updateGate(boost::extents[batchSize][outputSize]);
    processBasicGruUnit(prevOutputThisStep, inputThisStep, weightsInput,
                        weightsOutput, biases, updateGate,
                        BASIC_GRU_CELL_UPDATE_GATE,
                        popnn::NonLinearityType::SIGMOID);
    state[GRU_FWD_STATE_UPDATE_GATE_IDX][s] = updateGate;

    /* reset gate */
    Array2d resetGate(boost::extents[batchSize][outputSize]);
    processBasicGruUnit(
        prevOutputThisStep, inputThisStep, weightsInput, weightsOutput, biases,
        resetGate, BASIC_GRU_CELL_RESET_GATE, popnn::NonLinearityType::SIGMOID);
    state[GRU_FWD_STATE_RESET_GATE_IDX][s] = resetGate;

    /* candidate */
    Array2d candidate(boost::extents[batchSize][outputSize]);
    Array2d tmp1(boost::extents[batchSize][outputSize]);
    poplibs_test::gemm::hadamardProduct(resetGate, prevOutputThisStep, tmp1);
    processBasicGruUnit(tmp1, inputThisStep, weightsInput, weightsOutput,
                        biases, candidate, BASIC_GRU_CELL_CANDIDATE,
                        popnn::NonLinearityType::TANH);
    state[GRU_FWD_STATE_CANDIDATE_IDX][s] = candidate;

    printMatrix2d(fp, "fwd resetGate", resetGate);
    printMatrix2d(fp, "fwd updateGate", updateGate);
    printMatrix2d(fp, "fwd candidate", candidate);

    /* output */
    Array2d matOne(boost::extents[batchSize][outputSize]);
    matrixOne(matOne);

    Array2d updateGateComp(boost::extents[batchSize][outputSize]);
    poplibs_test::axpby::add(matOne, updateGate, updateGateComp, 1.0, -1.0);
    Array2d s1(boost::extents[batchSize][outputSize]);
    Array2d s2(boost::extents[batchSize][outputSize]);
    poplibs_test::gemm::hadamardProduct(updateGate, prevOutputThisStep, s1);
    poplibs_test::gemm::hadamardProduct(updateGateComp, candidate, s2);

    Array2d outputThisStep(boost::extents[batchSize][outputSize]);
    poplibs_test::axpby::add(s1, s2, outputThisStep);

    state[GRU_FWD_STATE_ACTS_IDX][s] = outputThisStep;
    printMatrix2d(fp, "fwd output", outputThisStep);
  }
  if (fp)
    fclose(fp);
}

static Array2d getSlice(Array2d &in, int offset, int size) {
  int batchSize = in.shape()[0];

  Array2d out(boost::extents[batchSize][size]);
  for (int i = 0; i < batchSize; i++) {
    for (int j = 0; j < size; j++) {
      out[i][j] = in[i][j + offset];
    }
  }

  return out;
}

static Array2d concatMatrix2D(const Array2d matA, const Array2d matB,
                              int dimension) {
  const auto matARows = matA.shape()[0];
  const auto matACols = matA.shape()[1];

  const auto matBRows = matB.shape()[0];
  const auto matBCols = matB.shape()[1];

  if (dimension == 0) {
    Array2d matC(boost::extents[matARows + matBRows][matACols]);
    if (matACols != matBCols)
      return matC;
    for (unsigned int i = 0; i < matARows; i++) {
      for (unsigned int j = 0; j < matACols; j++) {
        matC[i][j] = matA[i][j];
      }
    }
    for (unsigned int i = 0; i < matBRows; i++) {
      for (unsigned int j = 0; j < matBCols; j++) {
        matC[i + matARows][j] = matB[i][j];
      }
    }
    return matC;
  } else if (dimension == 1) {
    Array2d matC(boost::extents[matARows][matACols + matBCols]);
    if (matARows != matBRows)
      return matC;
    for (unsigned int i = 0; i < matARows; i++) {
      for (unsigned int j = 0; j < matACols; j++) {
        matC[i][j] = matA[i][j];
      }
    }
    for (unsigned int i = 0; i < matBRows; i++) {
      for (unsigned int j = 0; j < matBCols; j++) {
        matC[i][j + matACols] = matB[i][j];
      }
    }

    return matC;
  } else {
    // not implemented
    assert(0);
    Array2d matC(boost::extents[matARows][matACols + matBCols]);
    return matC;
  }
}

void poplibs_test::gru::basicGruCellBackwardPass(
    bool outputFullSequence, const Array3dRef weightsInput,
    const Array3dRef weightsOutput, const Array3dRef gradsNextLayer,
    const Array4dRef fwdState, const Array2dRef outputActsInit,
    Array4dRef bwdState, Array3dRef gradsPrevLayer) {
  const auto sequenceSize = fwdState.shape()[1];
  const auto batchSize = fwdState.shape()[2];
  const auto outputSize = fwdState.shape()[3];
  const auto inputSize = gradsPrevLayer.shape()[2];

  assert(fwdState.shape()[0] == GRU_NUM_FWD_STATES);
  assert(bwdState.shape()[0] == GRU_NUM_BWD_STATES);
  assert(weightsInput.shape()[0] == BASIC_GRU_CELL_NUM_UNITS);
  assert(weightsInput.shape()[1] == inputSize);
  assert(weightsInput.shape()[2] == outputSize);
  assert(weightsOutput.shape()[0] == BASIC_GRU_CELL_NUM_UNITS);
  assert(weightsOutput.shape()[1] == outputSize);
  assert(weightsOutput.shape()[2] == outputSize);
  assert(fwdState.shape()[1] == sequenceSize);
  assert(fwdState.shape()[2] == batchSize);
  assert(fwdState.shape()[3] == outputSize);
  assert(bwdState.shape()[1] == sequenceSize);
  assert(bwdState.shape()[2] == batchSize);
  assert(bwdState.shape()[3] == outputSize);
  assert(gradsNextLayer.shape()[0] == sequenceSize);
  assert(gradsNextLayer.shape()[1] == batchSize);
  assert(gradsNextLayer.shape()[2] == outputSize);
  assert(gradsPrevLayer.shape()[0] == sequenceSize);
  assert(gradsPrevLayer.shape()[1] == batchSize);

  // gradient of output of this step
  Array2d gradOutput(boost::extents[batchSize][outputSize]);
  matrixZero(gradOutput);

  Array2d matOne(boost::extents[batchSize][outputSize]);
  matrixOne(matOne);

  Array2d w_c = concatMatrix2D(weightsInput[GRU_FWD_STATE_CANDIDATE_IDX],
                               weightsOutput[GRU_FWD_STATE_CANDIDATE_IDX], 0);
  Array2d w_ru = concatMatrix2D(
      concatMatrix2D(weightsInput[GRU_FWD_STATE_RESET_GATE_IDX],
                     weightsOutput[GRU_FWD_STATE_RESET_GATE_IDX], 0),
      concatMatrix2D(weightsInput[GRU_FWD_STATE_UPDATE_GATE_IDX],
                     weightsOutput[GRU_FWD_STATE_UPDATE_GATE_IDX], 0),
      1);
  FILE *fp = NULL;
#ifdef DEBUG_TENSOR
  fp = fopen("bwd.txt", "w");
#endif
  for (auto i = sequenceSize; i != 0; --i) {
    const auto s = i - 1;
    if (fp)
      fprintf(fp, "bwd Loop: {%ld}\n", s);

    Array2d d_h(boost::extents[batchSize][outputSize]);
    Array2d gradOut(boost::extents[batchSize][outputSize]);
    ;
    if (outputFullSequence)
      gradOut = gradsNextLayer[s];
    else {
      // Only the last layer receive the gradient
      if (s == sequenceSize - 1)
        gradOut = gradsNextLayer[0];
      else {
        matrixZero(gradOut);
      }
    }
    axpby::add(gradOut, gradOutput, d_h);
    printMatrix2d(fp, "bwd outGrad", gradOutput);

    Array2d u = fwdState[GRU_FWD_STATE_UPDATE_GATE][s];
    Array2d r = fwdState[GRU_FWD_STATE_RESET_GATE][s];
    Array2d c = fwdState[GRU_FWD_STATE_CANDIDATE][s];

    printMatrix2d(fp, "bwd d_h", d_h);
    printMatrix2d(fp, "bwd r", r);
    printMatrix2d(fp, "bwd u", u);
    printMatrix2d(fp, "bwd c", c);

    Array2d u_comp(boost::extents[batchSize][outputSize]);
    poplibs_test::axpby::add(matOne, u, u_comp, 1.0, -1.0);
    Array2d d_c(boost::extents[batchSize][outputSize]);
    gemm::hadamardProduct(u_comp, d_h, d_c);
    bwdNonLinearity(popnn::NonLinearityType::TANH, c, d_c);

    Array2d h_prev(boost::extents[batchSize][outputSize]);
    if (s == 0) {
      h_prev = outputActsInit;
    } else {
      h_prev = fwdState[GRU_FWD_STATE_ACTS_IDX][s - 1];
    }

    printMatrix2d(fp, "bwd h_prev", h_prev);

    Array2d h_prev_c(boost::extents[batchSize][outputSize]);
    poplibs_test::axpby::add(h_prev, c, h_prev_c, 1.0, -1.0);
    Array2d d_u(boost::extents[batchSize][outputSize]);
    gemm::hadamardProduct(d_h, h_prev_c, d_u);
    bwdNonLinearity(popnn::NonLinearityType::SIGMOID, u, d_u);

    Array2d d_x2_h_prevr(boost::extents[batchSize][inputSize + outputSize]);
    // [2nd_component_of_d_x d_h_prevr] = d_c X w_c^T
    gemm::generalMatrixMultiply(d_c, w_c, d_x2_h_prevr, false, true);

    Array2d d_hr = getSlice(d_x2_h_prevr, inputSize, outputSize);
    Array2d d_r(boost::extents[batchSize][outputSize]);
    gemm::hadamardProduct(d_hr, h_prev, d_r);
    bwdNonLinearity(popnn::NonLinearityType::SIGMOID, r, d_r);

    // [1st_component_of_d_x 1st_component_of_d_h_prev] = [d_r d_u] X w_ru^T
    Array2d d_r_d_u = concatMatrix2D(d_r, d_u, 1);
    Array2d d_x1_h_prev1(boost::extents[batchSize][inputSize + outputSize]);
    gemm::generalMatrixMultiply(d_r_d_u, w_ru, d_x1_h_prev1, false, true);
    Array2d d_x_h_prev(boost::extents[batchSize][inputSize + outputSize]);
    poplibs_test::axpby::add(d_x1_h_prev1, d_x2_h_prevr, d_x_h_prev, 1.0, 1.0);
    Array2d d_x(boost::extents[batchSize][inputSize]);
    d_x = getSlice(d_x_h_prev, 0, inputSize);

    Array2d d_h_prev(boost::extents[batchSize][outputSize]);
    {
      Array2d t1(boost::extents[batchSize][outputSize]);
      Array2d t2(boost::extents[batchSize][outputSize]);
      Array2d t3(boost::extents[batchSize][outputSize]);
      gemm::hadamardProduct(d_hr, r, t1);
      gemm::hadamardProduct(d_h, u, t2);
      poplibs_test::axpby::add(t1, t2, t3, 1.0, 1.0);
      poplibs_test::axpby::add(t3,
                               getSlice(d_x1_h_prev1, inputSize, outputSize),
                               d_h_prev, 1.0, 1.0);
    }
    gradOutput = d_h_prev;
    gradsPrevLayer[s] = d_x;

    // save bwd state for weight update
    bwdState[BASIC_GRU_CELL_UPDATE_GATE][s] = d_u;
    bwdState[BASIC_GRU_CELL_RESET_GATE][s] = d_r;
    bwdState[BASIC_GRU_CELL_CANDIDATE][s] = d_c;
  }
  if (fp)
    fclose(fp);
}

void poplibs_test::gru::basicGruCellParamUpdate(const Array3dRef prevLayerActs,
                                                const Array4dRef fwdState,
                                                const Array2dRef outputActsInit,
                                                const Array4dRef bwdState,
                                                Array3dRef weightsInputDeltas,
                                                Array3dRef weightsOutputDeltas,
                                                Array2dRef biasDeltas) {
  const auto sequenceSize = prevLayerActs.shape()[0];
  const auto batchSize = prevLayerActs.shape()[1];
  const auto inputSize = prevLayerActs.shape()[2];
  const auto outputSize = fwdState.shape()[3];

  assert(fwdState.shape()[0] == GRU_NUM_FWD_STATES);
  assert(fwdState.shape()[1] == sequenceSize);
  assert(fwdState.shape()[2] == batchSize);
  assert(outputActsInit.shape()[0] == batchSize);
  assert(outputActsInit.shape()[1] == outputSize);
  assert(bwdState.shape()[0] == GRU_NUM_BWD_STATES);
  assert(bwdState.shape()[1] == sequenceSize);
  assert(bwdState.shape()[2] == batchSize);
  assert(bwdState.shape()[3] == outputSize);
  assert(weightsInputDeltas.shape()[0] == BASIC_GRU_CELL_NUM_UNITS);
  assert(weightsInputDeltas.shape()[1] == inputSize);
  assert(weightsInputDeltas.shape()[2] == outputSize);
  assert(weightsOutputDeltas.shape()[0] == BASIC_GRU_CELL_NUM_UNITS);
  assert(weightsOutputDeltas.shape()[1] == outputSize);
  assert(weightsOutputDeltas.shape()[2] == outputSize);
  assert(biasDeltas.shape()[0] == BASIC_GRU_CELL_NUM_UNITS);
  assert(biasDeltas.shape()[1] == outputSize);

  matrixZero(weightsInputDeltas);
  matrixZero(weightsOutputDeltas);
  matrixZero(biasDeltas);
  /*
    d_w_r = x_h_prev^T * d_r

    d_w_u = x_h_prev^T * d_u

    d_w_c = x_h_prevr^T * d_c_bar

    d_b_ru = sum of d_r_bar_u_bar along axis = 0

    d_b_c = sum of d_c_bar along axis = 0
  */
  for (auto i = sequenceSize; i != 0; --i) {
    const auto s = i - 1;
    Array2d h_prev(boost::extents[batchSize][outputSize]);
    if (s == 0) {
      h_prev = outputActsInit;
    } else {
      h_prev = fwdState[GRU_FWD_STATE_ACTS_IDX][s - 1];
    }
    Array2d x = prevLayerActs[s];
    Array2d x_h_prev = concatMatrix2D(x, h_prev, 1);
    Array2d h_prevr(boost::extents[batchSize][outputSize]);
    Array2d r = fwdState[GRU_FWD_STATE_RESET_GATE_IDX][s];
    gemm::hadamardProduct(h_prev, r, h_prevr);
    Array2d x_h_prevr = concatMatrix2D(x, h_prevr, 1);

    Array2d d_r = bwdState[BASIC_GRU_CELL_RESET_GATE][s];
    Array2d d_u = bwdState[BASIC_GRU_CELL_UPDATE_GATE][s];
    Array2d d_c = bwdState[BASIC_GRU_CELL_CANDIDATE][s];
    Array2d d_w_r(boost::extents[inputSize + outputSize][outputSize]);
    Array2d d_w_u(boost::extents[inputSize + outputSize][outputSize]);
    Array2d d_w_c(boost::extents[inputSize + outputSize][outputSize]);
    gemm::generalMatrixMultiply(x_h_prev, d_r, d_w_r, true, false);
    gemm::generalMatrixMultiply(x_h_prev, d_u, d_w_u, true, false);
    gemm::generalMatrixMultiply(x_h_prevr, d_c, d_w_c, true, false);
    for (unsigned int m = 0; m < inputSize; m++) {
      for (unsigned int n = 0; n < outputSize; n++) {
        weightsInputDeltas[BASIC_GRU_CELL_RESET_GATE][m][n] += d_w_r[m][n];
        weightsInputDeltas[BASIC_GRU_CELL_UPDATE_GATE][m][n] += d_w_u[m][n];
        weightsInputDeltas[BASIC_GRU_CELL_CANDIDATE][m][n] += d_w_c[m][n];
      }
    }
    for (unsigned int m = 0; m < outputSize; m++) {
      for (unsigned int n = 0; n < outputSize; n++) {
        weightsOutputDeltas[BASIC_GRU_CELL_RESET_GATE][m][n] +=
            d_w_r[m + inputSize][n];
        weightsOutputDeltas[BASIC_GRU_CELL_UPDATE_GATE][m][n] +=
            d_w_u[m + inputSize][n];
        weightsOutputDeltas[BASIC_GRU_CELL_CANDIDATE][m][n] +=
            d_w_c[m + inputSize][n];
      }
    }
    for (unsigned int m = 0; m < outputSize; m++) {
      for (unsigned int n = 0; n < batchSize; n++) {
        biasDeltas[BASIC_GRU_CELL_RESET_GATE][m] += d_r[n][m];
        biasDeltas[BASIC_GRU_CELL_UPDATE_GATE][m] += d_u[n][m];
        biasDeltas[BASIC_GRU_CELL_CANDIDATE][m] += d_c[n][m];
      }
    }
  }
}
