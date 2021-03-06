// Copyright (c) 2018 Graphcore Ltd. All rights reserved.
// Test for the transpose2d vertex

#define BOOST_TEST_MODULE TransposeTest
#include "poputil/VertexTemplates.hpp"
#include <poplar/Engine.hpp>
#include <poplibs_support/TestDevice.hpp>
#include <poplibs_test/Util.hpp>
#include <poplin/codelets.hpp>
#include <popops/Rearrange.hpp>
#include <popops/codelets.hpp>
#include <poputil/TileMapping.hpp>

using namespace poplar;
using namespace poplar::program;
using namespace popops::rearrange;
using namespace poputil;
using namespace poplibs_test::util;
using namespace poplin;
using namespace poplibs_support;

// Define a number of tests to run:
struct TestParams {
  unsigned rows;
  unsigned cols;
  unsigned matrices;
  bool force2d;
};

std::vector<TestParams> SmallTestList = {
    {1, 10, 1, false},  {7, 1, 2, false},   {8, 4, 1, false},
    {24, 4, 2, false},  {4, 4, 3, false},   {4, 4, 1, false},
    {5, 7, 2, false},   {16, 16, 3, true},  {16, 16, 3, false},
    {12, 16, 2, true},  {12, 16, 2, false}, {8, 8, 1, false},
    {8, 9, 1, false},   {9, 4, 1, false},   {4, 4, 1, true},
    {8, 4, 1, true},    {16, 4, 2, true},   {16, 4, 5, false},
    {16, 4, 6, false},  {16, 4, 15, false}, {16, 4, 18, false},
    {16, 4, 31, false},
};

std::vector<TestParams> T19548TestList = {
    {512, 4, 1, true},
};

//*************************************************
// Main Test function for Transpose 2d
//
// Overview:
// define max_matrices of size max_rows,MAX_COLUMNS
// Run a series of tests that transpose a varying number
// of matrices, but also select various small subsections/slices
// of data to transpose.
// The results are put into a memory area large enough to
// hold max_matrices of max_rowsxMAX_COLUMNS but often much of the data
// is expected to be zero.  This is checked as well as the "wanted" data.
//*************************************************
void TransposeTest(const Type &dataType, bool useSupervisorVertex,
                   const std::vector<TestParams> &testList) {

  // determine the sizes of arrays required
  auto test_count = testList.size();

  auto max_rows =
      std::max_element(testList.begin(), testList.end(),
                       [](const TestParams &a, const TestParams &b) {
                         return (a.rows < b.rows);
                       })
          ->rows;
  auto max_cols =
      std::max_element(testList.begin(), testList.end(),
                       [](const TestParams &a, const TestParams &b) {
                         return (a.cols < b.cols);
                       })
          ->cols;
  auto max_matrices =
      std::max_element(testList.begin(), testList.end(),
                       [](const TestParams &a, const TestParams &b) {
                         return (a.matrices < b.matrices);
                       })
          ->matrices;

  // Whole data array size
  auto test_size = max_rows * max_cols * max_matrices;
  auto total_size = test_count * test_size;

  // Program generated test data
  std::vector<double> outTest(total_size);
  std::vector<double> inTest(total_size);

  bool signedType = (dataType == HALF || dataType == FLOAT || dataType == INT ||
                     dataType == SHORT);

  // Initialise input pattern.
  std::generate_n(inTest.data(), inTest.size(), [i = 0, signedType]() mutable {
    // We don't want numbers that are outside the 'half'
    // precision (for integers):  -2048 <= HALF <= +2048
    return (int(i++) % 4096) - (signedType ? 2048 : 0);
  });

  auto device = createTestDevice(TEST_TARGET, 1, test_count);
  Target target = device.getTarget();

  // Create Graph object
  Graph graph(target);
  popops::addCodelets(graph);
  poplin::addCodelets(graph);

  // Input data
  Tensor in = graph.addVariable(
      dataType, {test_count, max_matrices, max_rows * max_cols}, "Input Data");

  // Result data
  Tensor out = graph.addVariable(
      dataType, {test_count, max_matrices, max_rows * max_cols}, "Output");

  // allocateHostMemoryForTensor
  Sequence uploadProg, downloadProg;
  std::vector<std::pair<std::string, char *>> tmap;
  auto input = allocateHostMemoryForTensor(in, "in", graph, uploadProg,
                                           downloadProg, tmap);

  auto output = allocateHostMemoryForTensor(out, "out", graph, uploadProg,
                                            downloadProg, tmap);

  Sequence prog;
  ComputeSet cs = graph.addComputeSet("testTranpose");

  for (std::size_t test = 0; test < test_count; test++) {
    // put each test on a different tile
    graph.setTileMapping(in[test], test);
    graph.setTileMapping(out[test], test);

    auto matrices = testList[test].matrices;
    auto rows = testList[test].rows;
    auto cols = testList[test].cols;

    // Zero output
    const auto zero =
        graph.addConstant(out.elementType(), out[test].shape(), 0);
    graph.setTileMapping(zero, test);
    prog.add(Copy(zero, out[test]));

    const auto fastVariant =
        canUseFastTranspose(target, dataType, rows, cols, matrices) &&
        !testList[test].force2d;

    std::string vertexName = "popops::Transpose2d";
    if (fastVariant) {
      vertexName = useSupervisorVertex ? "popops::TransposeSupervisor"
                                       : "popops::Transpose";
    }

    const auto vertexClass = templateVertex(vertexName, dataType);

    auto transVertex = graph.addVertex(cs, vertexClass);
    graph.setTileMapping(transVertex, test);

    // Different slices of the same input data to test looping decisions
    auto sliceIn = in[test].slice({0, 0}, {matrices, rows * cols});
    auto sliceOut = out[test].slice({0, 0}, {matrices, rows * cols});

    if (fastVariant) {
      graph.connect(transVertex["src"], sliceIn.flatten());
      graph.connect(transVertex["dst"], sliceOut.flatten());
      graph.setInitialValue(transVertex["numSrcColumnsD4"], cols / 4);
      graph.setInitialValue(transVertex["numSrcRowsD4"], rows / 4);
      if (!useSupervisorVertex) {
        graph.setInitialValue(transVertex["numTranspositionsM1"], matrices - 1);
      } else {
        // We will run one supervisor vertex, starting the 6 workers.
        // The first 'workerCount' workers (1<=workerCount<=6) will
        // transpose 'numTranspositions' matrices and (6-workerCount)
        // workers transposing (numTranspositions-1) matrices.
        // Note that (6-workerCount) and/or (numTranspositions-1) might
        // be zero.
        unsigned numWorkerContexts = target.getNumWorkerContexts();
        unsigned workerCount = numWorkerContexts, numTranspositions = 1;
        if (matrices <= numWorkerContexts) {
          workerCount = matrices;
        } else {
          numTranspositions = matrices / workerCount;
          unsigned rem = matrices % workerCount;
          if (rem > 0) {
            workerCount = rem;
            numTranspositions += 1;
          }
        }
        graph.setInitialValue(transVertex["numTranspositions"],
                              numTranspositions);
        graph.setInitialValue(transVertex["workerCount"], workerCount);
      }
    } else {
      graph.connect(transVertex["src"], sliceIn);
      graph.connect(transVertex["dst"], sliceOut);
      graph.setInitialValue(transVertex["numSrcColumns"], cols);
      graph.setInitialValue(transVertex["numSrcRows"], rows);
    }
  }

  prog.add(Execute(cs));

  std::vector<Program> programs;
  const auto testProgIndex = programs.size();
  programs.push_back(prog);
  const auto uploadProgIndex = programs.size();
  programs.push_back(uploadProg);
  const auto downloadProgIndex = programs.size();
  programs.push_back(downloadProg);

  // Run each program and compare host and IPU result
  Engine engine(graph, programs);
  attachStreams(engine, tmap);

  // Put test inputs into an array of the correct type ready to use
  std::vector<double> outHost(total_size);

  copy(target, inTest.data(), inTest.size(), dataType, input.get());

  device.bind([&](const Device &d) {
    engine.load(d);
    engine.run(uploadProgIndex);
    engine.run(testProgIndex);
    engine.run(downloadProgIndex);
  });

  copy(target, dataType, output.get(), outHost.data(), outHost.size());

  // Host generated result, start with zeros
  std::fill_n(outTest.data(), outTest.size(), 0);

  for (std::size_t test = 0; test < test_count; test++) {
    auto matrices = testList[test].matrices;
    auto rows = testList[test].rows;
    auto cols = testList[test].cols;

    const int testIndex = test * test_size;

    // Then transpose the same portion of the input as the code under test
    for (unsigned k = 0; k < matrices; k++) {
      int inIndex = k * max_rows * max_cols;
      for (unsigned i = 0; i < rows; i++) {
        for (unsigned j = 0; j < cols; j++) {
          const int outIndex = i + (j * rows) + (k * max_rows * max_cols);
          outTest[testIndex + outIndex] = inTest[testIndex + inIndex++];
        }
      }
    }
  }

  // Check the result, in the outTest array
  // Always check the whole output memory to catch any overwrites
  bool check = checkIsClose("TestTranspose", outHost.data(), {outHost.size()},
                            outTest.data(), outTest.size(), 0.0, 0.0);
  BOOST_CHECK(check);
}

BOOST_AUTO_TEST_SUITE(Transpose2d)

BOOST_AUTO_TEST_CASE(TransposeTest_half_true) {
  TransposeTest(HALF, true, SmallTestList);
}
BOOST_AUTO_TEST_CASE(TransposeTest_unsigned_short_true) {
  TransposeTest(UNSIGNED_SHORT, true, SmallTestList);
}
BOOST_AUTO_TEST_CASE(TransposeTest_short_true) {
  TransposeTest(SHORT, true, SmallTestList);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(TransposeFast_16bit)

BOOST_AUTO_TEST_CASE(TransposeTest_half_false) {
  TransposeTest(HALF, false, SmallTestList);
}
BOOST_AUTO_TEST_CASE(TransposeTest_unsigned_short_false) {
  TransposeTest(UNSIGNED_SHORT, false, SmallTestList);
}
BOOST_AUTO_TEST_CASE(TransposeTest_short_false) {
  TransposeTest(SHORT, false, SmallTestList);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(TransposeFast_Float)

BOOST_AUTO_TEST_CASE(TransposeTest_float_false) {
  TransposeTest(FLOAT, false, SmallTestList);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(TransposeFast_Integral)

BOOST_AUTO_TEST_CASE(TransposeTest_unsigned_int_false) {
  TransposeTest(UNSIGNED_INT, false, SmallTestList);
}
BOOST_AUTO_TEST_CASE(TransposeTest_int_false) {
  TransposeTest(INT, false, SmallTestList);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(T19548)

BOOST_AUTO_TEST_CASE(TransposeTest_float_false_T19548) {
  TransposeTest(FLOAT, false, T19548TestList);
}
BOOST_AUTO_TEST_CASE(TransposeTest_unsigned_int_false_T19548) {
  TransposeTest(UNSIGNED_INT, false, T19548TestList);
}
BOOST_AUTO_TEST_CASE(TransposeTest_int_false_T19548) {
  TransposeTest(INT, false, T19548TestList);
}

BOOST_AUTO_TEST_SUITE_END()
