// Copyright (c) 2020 Graphcore Ltd. All rights reserved.
//
// Performs sparse matrix multiplication Q = R * S Where
// Q and S are dense matrices and R is a sparse matrix
// with block size of 8x8. Uses AMP to divide the
// 8x16 output matrix computation among workers. 
//
// Each worker processes a set of columns and the split is 
// [2, 2, 2, 2, 0, 0]. The imbalance between the max and minimum
// is deliberate to allow supervisor processing if possible.


#if defined(__IPU__)
#include "BlockSparseMatMulAmpGradW.h.S"
#include "poplar/AvailableVTypes.h"

// =============================================================================

#define CODELET_NAME __runCodelet_popsparse__SparseDenseMatMulBlockAmpGradW___half_float_8_8

// =============================================================================

#define w_id                   m1
#define w_lt4                  m2
#define w_zOff                 m5
#define w_qWorkerOff           m6
#define w_rWorkerOff           m7
#define w_qGrad                m9
#define w_tripack              m8:9
#define w_rGradRetained        m10
#define w_qGradRetained        m11

#define w_fp_clr_reg           a0
#define w_xin                  a0:1
#define w_pin                  a2:3
#define w_xinpin               a0:3
#define w_pout1                a4:5

#define SIZEOF_INPUT_TYPE      2
#define SIZEOF_PARTIALS_TYPE   4
#define Z_PER_PASS             16
#define BLOCK_SIZE             8
#define WORKER_STACK_SIZE      0

DEF_STACK_USAGE WORKER_STACK_SIZE blockSparseDenseMultiplyGradWAmp_hf8x8
.section ".text.blockSparseDenseMultiplyGradWAmp_hf8x8", FUNCTION_IS_WORKER
.type blockSparseDenseMultiplyGradWAmp_hf8x8, @function
.align 4
.worker
blockSparseDenseMultiplyGradWAmp_hf8x8:
get               $w_id, $WSR
and               $w_id, $w_id, CSR_W_WSR__CTXTID_M1__MASK
cmpult            $w_lt4, $w_id, 4
mul               $w_qWorkerOff, $w_id, 2 * SIZEOF_INPUT_TYPE * Z_PER_PASS
mul               $w_rWorkerOff, $w_id, SIZEOF_PARTIALS_TYPE * BLOCK_SIZE * 2

blockSparseDenseMultiplyGradWAmp_hf8x8_retained:
ld32              $w_rGradRetained, $mvertex_base, W_AMP_RGRAD_BASE_BLOCK/4
ld32              $w_qGradRetained, $mvertex_base, W_AMP_QGRAD_BASE/4
add               $w_rGradRetained, $w_rGradRetained, $w_rWorkerOff
add               $w_qGradRetained, $w_qGradRetained, $w_qWorkerOff

blockSparseDenseMultiplyGradWAmp_hf8x8_retained_zOff:
brz               $w_lt4, LExit
ld32              $w_zOff, $mvertex_base, W_AMP_OFFSET_Z/4

// move pointers to correct position
add               $w_qGrad, $w_qGradRetained, $w_zOff
{ 
  tapack            $w_tripack, $w_qGrad, $w_rGradRetained, $w_rGradRetained
  setzi             $w_fp_clr_reg, 1 << CSR_W_FP_CLR__ZAACC__SHIFT 
}
{ 
  ld2x64pace        $azeros, $w_pin, $w_tripack+=, $mzero, 0b0001
  uput              $FP_CLR, $w_fp_clr_reg 
}
// Begin feeding partials
{ 
  ld2x64pace        $azeros, $w_pin, $w_tripack+=, $mzero, 0b0001
  f16v4sisoamp      $w_pout1, $azeros, $w_pin, TAMP_F16V4_E4_P0 
}

{ 
  ld2x64pace        $azeros, $w_pin, $w_tripack+=, $mzero, 0b0001
  f16v4sisoamp      $w_pout1, $azeros, $w_pin, TAMP_F16V4_E4_P1 
}

{ 
  ld2x64pace        $azeros, $w_pin, $w_tripack+=, $mzero, 0b0001
  f16v4sisoamp      $w_pout1, $azeros, $w_pin, TAMP_F16V4_E4_P2 
}
  
{ 
  ld2x64pace        $w_xin, $w_pin, $w_tripack+=, $mzero, 0b0000
  f16v4sisoamp      $w_pout1, $azeros, $w_pin, TAMP_F16V4_E4_P3 
}

// Begin feeding inputs
{ 
  ld2x64pace        $w_xin, $w_pin, $w_tripack+=, $mzero, 0b0000
  f16v4sisoamp      $w_pout1, $w_xin, $w_pin, TAMP_F16V4_E4_P0 
}

{ 
  ld2x64pace        $w_xin, $w_pin, $w_tripack+=, $mzero, 0b0000
  f16v4sisoamp      $w_pout1, $w_xin, $w_pin, TAMP_F16V4_E4_P1 
}

{ 
  ld2x64pace        $w_xin, $w_pin, $w_tripack+=, $mzero, 0b1100
  f16v4sisoamp      $w_pout1, $w_xin, $w_pin, TAMP_F16V4_E4_P2 
}

{ 
  ld2x64pace        $w_xin, $azeros, $w_tripack+=, $mzero, 0b0100
  f16v4sisoamp      $w_pout1, $w_xin, $w_pin, TAMP_F16V4_E4_P3 
}

// Outputs are now available
{ 
  ld2x64pace        $w_xin, $azeros, $w_tripack+=, $mzero, 0b0100
  f16v4sisoamp      $w_pout1, $w_xin, $azeros, TAMP_F16V4_E4_P0 
}
{ 
  ldst64pace        $w_xin, $w_pout1, $w_tripack+=, $mzero, 0b0000
  f16v4sisoamp      $w_pout1, $w_xin, $azeros, TAMP_F16V4_E4_P1 
}

{ 
  ldst64pace        $w_xin, $w_pout1, $w_tripack+=, $mzero, 0b0000
  f16v4sisoamp      $w_pout1, $w_xin, $azeros, TAMP_F16V4_E4_P2 
}

// Stop loading inputs
{ 
  st64pace          $w_pout1, $w_tripack+=, $mzero, 0b00
  f16v4sisoamp      $w_pout1, $w_xin, $azeros, TAMP_F16V4_E4_P3 
}

// Stop feeding inputs
{ 
  st64pace          $w_pout1, $w_tripack+=, $mzero, 0b00
  f16v4sisoamp      $w_pout1, $azeros, $azeros, TAMP_F16V4_E4_P0 
}

{ 
  st64pace           $w_pout1, $w_tripack+=, $mzero, 0b00
  f16v4sisoamp       $w_pout1, $azeros, $azeros, TAMP_F16V4_E4_P1 
}
{ 
  st64pace          $w_pout1, $w_tripack+=, $mzero, 0b00
  f16v4sisoamp      $w_pout1, $azeros, $azeros, TAMP_F16V4_E4_P2 
}

{ 
  st64pace          $w_pout1, $w_tripack+=, $mzero, 0b00
  f16v4sisoamp      $w_pout1, $azeros, $azeros, TAMP_F16V4_E4_P3 
}

st64pace          $w_pout1, $w_tripack+=, $mzero, 0b00
LExit:
exitz             $mzero
.size blockSparseDenseMultiplyGradWAmp_hf8x8, . - blockSparseDenseMultiplyGradWAmp_hf8x8

// =============================================================================

// Instantiate supervisor codelet
BLOCK_SPARSE_MATMUL_GRADW_AMP_SUP CODELET_NAME half float 8 blockSparseDenseMultiplyGradWAmp_hf8x8

#endif // defined(__IPU__)
