#include "TestDevice.hpp"
#include <poplar/Engine.hpp>
#include "popops/codelets.hpp"
#include "poplibs_test/Util.hpp"

#define BOOST_TEST_MODULE ScaledAddSupervisor_fp
#include <boost/test/included/unit_test.hpp>


using namespace poplar;
using namespace poplar::program;
using namespace poplibs_test::util;

#define TOL 0.1 //tolerance of 0.1%

#define N 80

// Test data, generated by:
// python -c "import random; print [round(random.uniform(0, 100), 4)
//    for _ in range(N)]"

const float data[N] = {
  0.8534, 2.9833, 38.2024, 87.3113, 8.3774, 27.5261, 97.3378, 63.7722,
    52.5539, 30.8552, 78.9132, 11.1624, 61.1376, 42.0059, 20.9077, 10.4159,
    47.8163, 10.6081, 19.2055, 48.1933, 42.9815, 73.1804, 65.6732, 56.2054,
    83.5201, 54.353, 27.3245, 6.1426, 84.6202, 59.0347, 52.5354, 0.3793,
    65.2122, 18.2263, 32.5403, 13.1368, 65.8324, 97.6239, 31.3668, 26.183,
    16.7465, 88.9421, 82.422, 31.6807, 89.8731, 64.4955, 59.5105, 18.8455,
    62.2198, 7.2624, 24.9691, 15.5876, 79.9009, 20.5059, 13.2128, 38.238,
    76.9248, 99.4896, 15.4235, 89.3595, 71.5428, 62.7379, 45.6806, 6.0773,
    81.0174, 33.8174, 1.0395, 57.2691, 67.4487, 78.0565, 60.1302, 39.5229,
    39.6528, 37.9882, 45.9843, 50.885, 37.9814, 26.9937, 0.727, 89.451
};

const float deltas[N] = {
  5.8678, 87.3042, 27.6216, 61.4568, 65.8711, 93.0195, 34.1048, 74.3848,
  36.9936, 48.9242, 80.4252, 82.9536, 8.6372, 96.0092, 41.1759, 86.8282,
  52.3811, 76.1267, 27.2576, 19.4517, 17.4603, 84.3021, 98.6319, 48.2396,
  90.1868, 28.2355, 62.9416, 93.7382, 74.413, 7.4225, 48.916, 96.0203,
  98.1374, 33.2734, 94.4999, 38.9091, 31.4119, 42.8233, 43.049, 82.7856,
  56.9155, 5.2595, 65.9839, 1.8433, 58.3097, 41.7467, 43.9233, 33.41,
  81.7994, 87.5566, 63.5808, 57.3755, 9.3762, 78.467, 84.4506, 89.7413,
  18.6427, 45.7754, 7.3802, 8.0999, 73.7112, 42.8081, 15.3092, 63.5193,
  3.4319, 48.2729, 71.1376, 47.277, 7.7794, 77.9405, 89.8753, 70.6456,
  24.8513, 70.6801, 17.3198, 55.721, 41.1727, 33.0615, 0.3736, 49.9985
};

float expected[N];

double atol(const Type &type) {
  return type == HALF ? 1e-7 : 1e-20;
}

void testScaledAddSupervisor(const char *vertex, const Type &dataType,
                          const Type &deltaType, const bool &constantFactor) {
  auto device = createTestDevice(TEST_TARGET);
  Graph graph(device.getTarget());

  popops::addCodelets(graph);
  const float factor = 1.8424;

  // Generate the expected result
  for(unsigned i = 0; i < N; i++) {
    expected[i] = data[i] + deltas[i] * factor;
  }

  const auto &target = device.getTarget();
  Sequence prog;
  // create a ComputeSet for each test case of size = 1...N
  for (unsigned i = 1; i <= N; ++i) {
    auto cs = graph.addComputeSet("cs" + std::to_string(i));
    auto v = graph.addVertex(cs, vertex);
    graph.setTileMapping(v, 0);

    auto dataTensor = graph.addVariable(dataType, {i});
    graph.setTileMapping(dataTensor, 0);
    graph.connect(v["data"], dataTensor);

    graph.createHostWrite("data" + std::to_string(i), dataTensor);
    graph.createHostRead("data" + std::to_string(i), dataTensor);

    auto deltasTensor = graph.addVariable(deltaType, {i});
    graph.setTileMapping(deltasTensor, 0);
    graph.connect(v["deltas"], deltasTensor);
    graph.createHostWrite("deltas" + std::to_string(i), deltasTensor);

    if(constantFactor) {
      graph.setInitialValue(v["K"], factor);
    }
    else {
      auto factorTensor = graph.addVariable(dataType, {});
      graph.setTileMapping(factorTensor,0);
      graph.connect(v["factor"], factorTensor);
      graph.setInitialValue(factorTensor, factor);
    }
    prog.add(Execute(cs));
  }

  Engine e(graph, prog);
  device.bind([&](const Device &d) {
    e.load(d);

    std::unique_ptr<char[]> dataBuffer(
      new char[N * target.getTypeSize(dataType)]);
    std::unique_ptr<char[]> deltaBuffer(
      new char[N * target.getTypeSize(deltaType)]);

    for (unsigned i = 1; i <= N; ++i) {
      copy(target, data, i, dataType, dataBuffer.get());
      e.writeTensor("data" + std::to_string(i), dataBuffer.get());
      copy(target, deltas, i, deltaType, deltaBuffer.get());
      e.writeTensor("deltas" + std::to_string(i), deltaBuffer.get());
    }

    e.run();

    std::array<float, N> actual;
    for (unsigned i = 1; i <= N; ++i) {
      e.readTensor("data" + std::to_string(i), dataBuffer.get());
      copy(target, dataType, dataBuffer.get(), actual.data(), i);

      auto test = "n=" + std::to_string(i);
      BOOST_CHECK(checkIsClose(test, actual.data(), {i}, expected, i,
                               TOL, atol(dataType)));
    }
  });
}

BOOST_AUTO_TEST_CASE(ScaledAddSupervisorHalfConst) {
  testScaledAddSupervisor("popops::ScaledAddSupervisor<half,half,true>",
                                                              HALF, HALF, true);
}

BOOST_AUTO_TEST_CASE(ScaledAddSupervisorFloatConst) {
  testScaledAddSupervisor("popops::ScaledAddSupervisor<float,float,true>",
                                                            FLOAT, FLOAT, true);
}

BOOST_AUTO_TEST_CASE(ScaledAddSupervisorFloatHalfConst) {
  testScaledAddSupervisor("popops::ScaledAddSupervisor<half,float,true>",
                                                          HALF, FLOAT, true);
}

BOOST_AUTO_TEST_CASE(ScaledAddSupervisorHalfTensor) {
  testScaledAddSupervisor("popops::ScaledAddSupervisor<half,half,false>",
                                                            HALF, HALF, false);
}

BOOST_AUTO_TEST_CASE(ScaledAddSupervisorFloatTensor) {
  testScaledAddSupervisor("popops::ScaledAddSupervisor<float,float,false>",
                                                          FLOAT, FLOAT, false);
}

BOOST_AUTO_TEST_CASE(ScaledAddSupervisorFloatHalfTensor) {
  testScaledAddSupervisor("popops::ScaledAddSupervisor<half,float,false>",
                                                          HALF, FLOAT, false);
}
