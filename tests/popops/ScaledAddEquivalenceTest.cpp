// Copyright (c) 2020 Graphcore Ltd. All rights reserved.
#define BOOST_TEST_MODULE ScaledAddEquivalenceTest

#include <boost/test/unit_test.hpp>
#include <poplar/CSRFunctions.hpp>
#include <poplar/Engine.hpp>
#include <poplibs_support/TestDevice.hpp>
#include <popops/ScaledAdd.hpp>
#include <popops/Zero.hpp>
#include <popops/codelets.hpp>

#include <vector>
using namespace poplibs_support;

BOOST_AUTO_TEST_CASE(ScaledAddEquivalenceTestCheckEqual) {
  auto device = createTestDevice(TEST_TARGET);
  const auto target = device.getTarget();
  poplar::Graph g(target);
  popops::addCodelets(g);

  const unsigned size = 16U;
  auto input = g.addVariable(poplar::HALF, {size}, "input");
  auto outScaledAdd = g.addVariable(poplar::HALF, {size}, "outScaledAdd");
  auto outScaledSub = g.addVariable(poplar::HALF, {size}, "outScaledSub");
  g.setTileMapping(input, 0);
  g.setTileMapping(outScaledAdd, 0);
  g.setTileMapping(outScaledSub, 0);

  // use low enough scale compared to tolerance threshold
  const float scale = 1.33e-7;
  poplar::program::Sequence prog;
  poplar::setStochasticRounding(g, prog, false);
  popops::zero(g, outScaledAdd, prog, "scaledAdd");
  popops::zero(g, outScaledSub, prog, "scaledSub");
  poplar::OptionFlags options = {{"scaleFloatToHalfTolerance", "1e-5"}};
  popops::scaledAddTo(g, outScaledAdd, input, scale, prog, "scaledAdd",
                      options);
  popops::scaledSubtractFrom(g, outScaledSub, input, -scale, prog, "scaledSub",
                             options);

  auto rawBufSize = target.getTypeSize(poplar::HALF) * size;
  std::vector<char> rawIn(rawBufSize);
  std::vector<char> rawOutAdd(rawBufSize);
  std::vector<char> rawOutSub(rawBufSize);

  const std::vector<float> hostIn = {1e3,  2e3,  3e3,  4e3,  5e3,  6e3,
                                     7e3,  8e3,  -1e3, -2e3, -3e3, -4e3,
                                     -5e3, -6e3, -7e3, -8e3};
  poplar::copyFloatToDeviceHalf(target, hostIn.data(), rawIn.data(), size);

  g.createHostWrite("input", input);
  g.createHostRead("outScaledAddRd", outScaledAdd);
  g.createHostRead("outScaledSubRd", outScaledSub);

  poplar::Engine e(g, prog);
  device.bind([&](const poplar::Device &d) {
    e.load(d);
    e.writeTensor("input", rawIn.data(), rawIn.data() + rawIn.size());
    e.run();
    e.readTensor("outScaledAddRd", rawOutAdd.data(),
                 rawOutAdd.data() + rawOutAdd.size());
    e.readTensor("outScaledSubRd", rawOutSub.data(),
                 rawOutSub.data() + rawOutSub.size());
  });

  std::vector<float> hostOutAdd(size);
  poplar::copyDeviceHalfToFloat(target, rawOutAdd.data(), hostOutAdd.data(),
                                size);
  std::vector<float> hostOutSub(size);
  poplar::copyDeviceHalfToFloat(target, rawOutSub.data(), hostOutSub.data(),
                                size);
  BOOST_CHECK_EQUAL_COLLECTIONS(hostOutAdd.begin(), hostOutAdd.end(),
                                hostOutSub.begin(), hostOutSub.end());
}
