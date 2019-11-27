#include "NonLinearity.hpp"

namespace popnn {

template <typename FPType, NonLinearityType nlType>
class WORKER_ALIGN NonLinearityGradSupervisor : public SupervisorVertex {
public:
  NonLinearityGradSupervisor();

  Input<Vector<FPType, SCALED_PTR64, 8>> outGrad;
  Input<Vector<FPType, SCALED_PTR64, 8>> out;
  Output<Vector<FPType, SCALED_PTR64, 8>> inGrad;
  const unsigned short n;

  IS_EXTERNAL_CODELET(true);
  bool compute() {
    for (unsigned i = 0; i < n; ++i) {
      const auto derivative = nonlinearity_derivative(nlType, float(out[i]));
      inGrad[i] = outGrad[i] * FPType(derivative);
    }
    return true;
  }
};

INSTANTIATE_NL(NonLinearityGradSupervisor)

} // namespace popnn