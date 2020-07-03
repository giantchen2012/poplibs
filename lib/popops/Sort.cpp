// Copyright (c) 2018 Graphcore Ltd. All rights reserved.

#include "popops/Sort.hpp"

#include <poplibs_support/Algorithms.hpp>
#include <popops/ElementWise.hpp>
#include <popops/Reduce.hpp>
#include <poputil/VertexTemplates.hpp>
#include <poputil/exceptions.hpp>

#include <algorithm>

namespace popops {
namespace {

poplar::program::Program predicatedSwap(poplar::Graph &graph,
                                        poplar::Tensor pred, poplar::Tensor a,
                                        poplar::Tensor b,
                                        const std::string &debugPrefix) {
  poplar::program::Sequence result;

  poplar::Tensor tmp =
      popops::select(graph, b, a, pred, result, debugPrefix + "/select_left");
  poplar::Tensor not_pred =
      popops::logicalNot(graph, pred, result, debugPrefix + "invert_pred");
  popops::selectInPlace(graph, b, a, not_pred, result,
                        debugPrefix + "/select_right");

  result.add(poplar::program::Copy(tmp, a));

  return std::move(result);
}

// Turn the ND input tensor into a 2D tensor, preserving the sort dimension.
poplar::Tensor flattenDimension(poplar::Tensor input, unsigned dimension) {
  std::vector<unsigned> permutation(input.rank());
  std::iota(permutation.begin(), permutation.end(), 0);
  std::swap(permutation.back(), permutation[dimension]);
  poplar::Tensor inputView = input.dimShuffle(permutation);

  return inputView.reshape(
      {input.numElements() / input.dim(dimension), input.dim(dimension)});
}

std::string heapSortVertex(poplar::Type a) {
  return poputil::templateVertex("popops::HeapSortVertex", a);
}

std::string heapSortVertex(poplar::Type a, poplar::Type b) {
  return poputil::templateVertex("popops::HeapSortVertexKV", a, b);
}

poplar::ComputeSet sortSlice(poplar::Graph &graph, poplar::Tensor in,
                             const std::string &debugPrefix) {
  auto sortCS = graph.addComputeSet(debugPrefix + "/sortCS");
  const std::string vertexType = heapSortVertex(in.elementType());

  for (std::size_t i = 0; i < in.dim(0); ++i) {
    poplar::Tensor inputSlice = in[i];

    const auto tileIntervals = graph.getTileMapping(inputSlice);
    for (std::size_t tile = 0; tile < tileIntervals.size(); ++tile) {
      for (const auto &interval : tileIntervals[tile]) {
        auto v = graph.addVertex(sortCS, vertexType);

        graph.setTileMapping(v, tile);
        graph.connect(v["out"], inputSlice.slice(interval));
      }
    }
  }

  return sortCS;
}

poplar::ComputeSet sortSlice(poplar::Graph &graph, poplar::Tensor key,
                             poplar::Tensor value,
                             const std::string &debugPrefix) {
  auto sortCS = graph.addComputeSet(debugPrefix + "/sortCS");
  const std::string vertexType =
      heapSortVertex(key.elementType(), value.elementType());

  for (std::size_t i = 0; i < key.dim(0); ++i) {
    poplar::Tensor keySlice = key[i];
    poplar::Tensor valueSlice = value[i];

    const auto tileIntervals = graph.getTileMapping(keySlice);
    for (std::size_t tile = 0; tile < tileIntervals.size(); ++tile) {
      for (const auto &interval : tileIntervals[tile]) {
        auto v = graph.addVertex(sortCS, vertexType);

        graph.setTileMapping(v, tile);
        graph.connect(v["key"], keySlice.slice(interval));
        graph.connect(v["value"], valueSlice.slice(interval));
      }
    }
  }

  return sortCS;
}

bool intervalComp(const poplar::Interval &a, const poplar::Interval &b) {
  return a.begin() < b.begin();
}

bool intervalNotEmpty(const poplar::Interval &a) { return a.size() != 0; }

poplar::Tensor isNotSortedPredicate(poplar::Graph &graph,
                                    poplar::program::Sequence &prog,
                                    poplar::Tensor input,
                                    const std::string &debugPrefix) {
  std::vector<poplar::Tensor> lhss;
  std::vector<poplar::Tensor> rhss;
  for (std::size_t i = 0; i < input.dim(0); ++i) {
    poplar::Tensor inputSlice = input[i];

    // Find the adjacent non-empty intervals
    auto intervals = poplibs::flatten(graph.getTileMapping(inputSlice));
    const auto newEnd =
        std::partition(intervals.begin(), intervals.end(), intervalNotEmpty);
    intervals.erase(newEnd, intervals.end());
    std::sort(intervals.begin(), intervals.end(), intervalComp);

    if (intervals.size() > 0) {
      // For each neighbouring pair of intervals, check that the neighbouring
      // elements are in order.
      for (std::size_t k = 0; k < intervals.size() - 1; ++k) {
        lhss.push_back(inputSlice[intervals[k].end() - 1].expand({0}));
        rhss.push_back(inputSlice[intervals[k + 1].begin()].expand({0}));
      }
    }
  }

  if (lhss.empty()) {
    auto c = graph.addConstant(poplar::BOOL, {}, false, debugPrefix + "/false");
    graph.setTileMapping(c, 0);
    return c;
  } else {
    poplar::Tensor lhs = poplar::concat(lhss);
    poplar::Tensor rhs = poplar::concat(rhss);
    poplar::Tensor edges = popops::lt(graph, rhs, lhs, prog,
                                      debugPrefix + "/isNotSortedPredicate");

    std::vector<std::size_t> dims(edges.rank());
    std::iota(std::begin(dims), std::end(dims), 0);

    return reduce(graph, edges, poplar::BOOL, dims, {Operation::LOGICAL_OR},
                  prog, debugPrefix + "/reduction");
  }
}

poplar::program::Sequence createExchange(poplar::Graph &graph,
                                         poplar::Tensor input,
                                         const std::size_t startIndex,
                                         const std::string &debugPrefix) {
  poplar::program::Sequence result;

  for (std::size_t i = 0; i < input.dim(0); ++i) {
    poplar::Tensor inputSlice = input[i];

    // Find the adjacent non-empty intervals
    auto intervals = poplibs::flatten(graph.getTileMapping(inputSlice));
    const auto newEnd =
        std::partition(intervals.begin(), intervals.end(), intervalNotEmpty);
    intervals.erase(newEnd, intervals.end());

    std::sort(intervals.begin(), intervals.end(), intervalComp);

    if (!intervals.empty()) {
      // For each neighbouring pair of intervals, conditionally exchange the
      // neighbouring elements.
      std::vector<poplar::Tensor> lhss;
      std::vector<poplar::Tensor> rhss;
      for (std::size_t k = startIndex; k < intervals.size() - 1; k += 2) {
        lhss.push_back(inputSlice[intervals[k].end() - 1].expand({0}));
        rhss.push_back(inputSlice[intervals[k + 1].begin()].expand({0}));
      }

      if (!lhss.empty()) {
        poplar::Tensor lhs = poplar::concat(lhss);
        poplar::Tensor rhs = poplar::concat(rhss);
        poplar::Tensor pred = popops::lt(graph, rhs, lhs, result);
        result.add(predicatedSwap(graph, pred, lhs, rhs, debugPrefix));
      }
    }
  }

  return result;
}

poplar::program::Sequence
createExchange(poplar::Graph &graph, poplar::Tensor key, poplar::Tensor value,
               const std::size_t startIndex, const std::string &debugPrefix) {
  poplar::program::Sequence result;

  for (std::size_t i = 0; i < key.dim(0); ++i) {
    poplar::Tensor keySlice = key[i];
    poplar::Tensor valueSlice = value[i];

    // Find the adjacent non-empty intervals
    auto intervals = poplibs::flatten(graph.getTileMapping(keySlice));
    const auto newEnd =
        std::partition(intervals.begin(), intervals.end(), intervalNotEmpty);
    intervals.erase(newEnd, intervals.end());
    std::sort(intervals.begin(), intervals.end(), intervalComp);

    if (!intervals.empty()) {
      // For each neighbouring pair of intervals, conditionally exchange the
      // neighbouring elements.
      std::vector<poplar::Tensor> key_lhss;
      std::vector<poplar::Tensor> key_rhss;
      std::vector<poplar::Tensor> value_lhss;
      std::vector<poplar::Tensor> value_rhss;
      for (std::size_t k = startIndex; k < intervals.size() - 1; k += 2) {
        key_lhss.push_back(keySlice[intervals[k].end() - 1].expand({0}));
        key_rhss.push_back(keySlice[intervals[k + 1].begin()].expand({0}));

        value_lhss.push_back(valueSlice[intervals[k].end() - 1].expand({0}));
        value_rhss.push_back(valueSlice[intervals[k + 1].begin()].expand({0}));
      }

      if (!key_lhss.empty()) {
        poplar::Tensor key_lhs = poplar::concat(key_lhss);
        poplar::Tensor key_rhs = poplar::concat(key_rhss);
        poplar::Tensor value_lhs = poplar::concat(value_lhss);
        poplar::Tensor value_rhs = poplar::concat(value_rhss);
        poplar::Tensor pred = popops::lt(graph, key_rhs, key_lhs, result);
        result.add(predicatedSwap(graph, pred, key_lhs, key_rhs, debugPrefix));
        result.add(
            predicatedSwap(graph, pred, value_lhs, value_rhs, debugPrefix));
      }
    }
  }

  return result;
}

poplar::program::Sequence createEvenExchange(poplar::Graph &graph,
                                             poplar::Tensor input,
                                             const std::string &debugPrefix) {
  return createExchange(graph, input, 0, debugPrefix);
}

poplar::program::Sequence createOddExchange(poplar::Graph &graph,
                                            poplar::Tensor input,
                                            const std::string &debugPrefix) {
  return createExchange(graph, input, 1, debugPrefix);
}

poplar::program::Sequence createEvenExchange(poplar::Graph &graph,
                                             poplar::Tensor key,
                                             poplar::Tensor value,
                                             const std::string &debugPrefix) {
  return createExchange(graph, key, value, 0, debugPrefix);
}

poplar::program::Sequence createOddExchange(poplar::Graph &graph,
                                            poplar::Tensor key,
                                            poplar::Tensor value,
                                            const std::string &debugPrefix) {
  return createExchange(graph, key, value, 1, debugPrefix);
}

} // namespace

poplar::Tensor sort(poplar::Graph &graph, const poplar::Tensor &t, unsigned dim,
                    poplar::program::Sequence &prog,
                    const std::string &debugPrefix) {
  poplar::Tensor result = graph.clone(t);
  prog.add(poplar::program::Copy(t, result));

  sortInPlace(graph, result, dim, prog, debugPrefix);

  return result;
}

void sortInPlace(poplar::Graph &graph, const poplar::Tensor &t, unsigned dim,
                 poplar::program::Sequence &prog,
                 const std::string &debugPrefix) {
  if (dim >= t.rank()) {
    throw poputil::poplibs_error(
        "Chosen sort dimension does not refer to a valid "
        "dimension in the input tensor");
  }

  poplar::Tensor tView = flattenDimension(t, dim);
  poplar::ComputeSet sortCS = sortSlice(graph, tView, debugPrefix);

  poplar::program::Sequence sortStep;

  // swap the even interval edges
  sortStep.add(createEvenExchange(graph, tView, debugPrefix));

  // swap the odd interval edges
  sortStep.add(createOddExchange(graph, tView, debugPrefix));

  // Sort each interval
  sortStep.add(poplar::program::Execute(sortCS));

  // Perform an initial sort of each interval
  prog.add(poplar::program::Execute(sortCS));

  // Repeat the sort step until all edges are in order
  poplar::program::Sequence cond;
  poplar::Tensor pred = isNotSortedPredicate(graph, cond, tView, debugPrefix);
  prog.add(poplar::program::RepeatWhileTrue(cond, pred, sortStep));
}

poplar::Tensor sortKeyValue(poplar::Graph &graph, const poplar::Tensor &k,
                            const poplar::Tensor &v, unsigned dim,
                            poplar::program::Sequence &prog,
                            const std::string &debugPrefix) {
  poplar::Tensor key = graph.clone(k);
  poplar::Tensor value = graph.clone(v);

  prog.add(poplar::program::Copy(k, key));
  prog.add(poplar::program::Copy(v, value));

  sortKeyValueInPlace(graph, key, value, dim, prog, debugPrefix);

  return value;
}

void sortKeyValueInPlace(poplar::Graph &graph, const poplar::Tensor &k,
                         const poplar::Tensor &v, unsigned dim,
                         poplar::program::Sequence &prog,
                         const std::string &debugPrefix) {
  if (k.shape() != v.shape()) {
    throw poputil::poplibs_error(
        "Key and Value arguments to sortKeyValue must be the same shape");
  }

  if (dim >= k.rank()) {
    throw poputil::poplibs_error(
        "Chosen sort dimension does not refer to a valid "
        "dimension in the input tensor");
  }

  poplar::Tensor keyView = flattenDimension(k, dim);
  poplar::Tensor valueView = flattenDimension(v, dim);

  poplar::ComputeSet sortCS = sortSlice(graph, keyView, valueView, debugPrefix);

  poplar::program::Sequence sortStep;

  // swap the even interval edges
  sortStep.add(createEvenExchange(graph, keyView, valueView, debugPrefix));

  // swap the odd interval edges
  sortStep.add(createOddExchange(graph, keyView, valueView, debugPrefix));

  // Sort each interval
  sortStep.add(poplar::program::Execute(sortCS));

  // Perform an initial sort of each interval
  prog.add(poplar::program::Execute(sortCS));

  // Repeat the sort step until all edges are in order
  poplar::program::Sequence cond;
  poplar::Tensor pred = isNotSortedPredicate(graph, cond, keyView, debugPrefix);
  poplar::program::RepeatWhileTrue repeat(cond, pred, sortStep);
  prog.add(repeat);
}

} // namespace popops
