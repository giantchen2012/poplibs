// Copyright (c) 2018, Graphcore Ltd, All rights reserved.

#ifndef _popops_ExprOp_hpp_
#define _popops_ExprOp_hpp_

namespace popops { namespace expr {

// Enum classes uses for expressions.

enum class TernaryOpType {
  CLAMP,
  SELECT
};

enum class BinaryOpType {
  ADD,
  ATAN2,
  BITWISE_AND,
  BITWISE_OR,
  DIVIDE,
  EQUAL,
  GREATER_THAN_EQUAL,
  GREATER_THAN,
  LESS_THAN_EQUAL,
  LOGICAL_AND,
  LOGICAL_OR,
  LESS_THAN,
  MAXIMUM,
  MINIMUM,
  MULTIPLY,
  NOT_EQUAL,
  POWER,
  REMAINDER,
  SHIFT_LEFT,
  SHIFT_RIGHT,
  SHIFT_RIGHT_SIGN_EXTEND,
  SUBTRACT
};

enum class UnaryOpType {
  ABSOLUTE,
  BITWISE_NOT,
  CEIL,
  COS,
  EXPONENT,
  FLOOR,
  IS_FINITE,
  LOGARITHM,
  LOGICAL_NOT,
  NEGATE,
  SIGNUM,
  SIN,
  TANH,
  ROUND,
  SQRT,
  SQUARE
};

}} // end namespace popops::expr

#endif // _popops_ExprOp_hpp_