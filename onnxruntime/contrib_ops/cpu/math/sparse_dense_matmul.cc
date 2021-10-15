// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#if !defined(DISABLE_SPARSE_TENSORS)

#include "core/framework/op_kernel.h"
#include "core/framework/element_type_lists.h"
#include "core/framework/sparse_tensor.h"
#include "core/framework/sparse_utils.h"
#include "core/util/math.h"
#include "core/util/math_cpuonly.h"

namespace onnxruntime {
namespace contrib {

namespace {
using SparseToDenseSupportedTypes = TypeList<float, double, int32_t, int64_t, uint32_t, uint64_t>;
}

// Currently supports batching only for the dense argument
class SparseToDenseMatMul final : public OpKernel {
 public:
  explicit SparseToDenseMatMul(const OpKernelInfo& info) : OpKernel(info) {
    info.GetAttrOrDefault<float>("alpha", &alpha_attr_, 1.0);
    info.GetAttrOrDefault<int64_t>("transA", &trans_a_attr_, 0);
    info.GetAttrOrDefault<int64_t>("transB", &trans_b_attr_, 0);
  }

  Status Compute(OpKernelContext*) const override;

 private:
  float alpha_attr_;
  int64_t trans_a_attr_;
  int64_t trans_b_attr_;
};

ONNX_OPERATOR_KERNEL_EX(
    SparseToDenseMatMul,
    kMSDomain,
    1,
    kCpuExecutionProvider,
    KernelDefBuilder()
        .TypeConstraint("T", BuildKernelDefSparseConstraintsFromTypeList<SparseToDenseSupportedTypes>())
        .TypeConstraint("T1", BuildKernelDefConstraintsFromTypeList<SparseToDenseSupportedTypes>()),
    SparseToDenseMatMul);

namespace {
struct ComputeCtx {
  bool trans_A;
  bool trans_B;
  float alpha;
};

#if !defined(__i386__) && !defined(_M_IX86) && !defined(__wasm__) && !defined(__ANDROID__)
template <typename T>
inline void SparseDenseMatMulImpl(const ComputeCtx& ctx, const ConstSparseMatrixMap<T>& map_A,
                                  const ConstEigenMatrixMapRowMajor<T>& map_B, EigenMatrixMapRowMajor<T>& output_map) {
  if (ctx.trans_A && ctx.trans_B) {
    output_map = map_A.transpose() * map_B.transpose();
  } else if (ctx.trans_A && !ctx.trans_B) {
    output_map = map_A.transpose() * map_B;
  } else if (!ctx.trans_A && ctx.trans_B) {
    output_map = map_A * map_B.transpose();
  } else {
    output_map = map_A * map_B;
  }
}

template <>
inline void SparseDenseMatMulImpl<float>(const ComputeCtx& ctx, const ConstSparseMatrixMap<float>& map_A,
                                         const ConstEigenMatrixMapRowMajor<float>& map_B, EigenMatrixMapRowMajor<float>& output_map) {
  if (ctx.trans_A && ctx.trans_B) {
    output_map = map_A.transpose() * ctx.alpha * map_B.transpose();
  } else if (ctx.trans_A && !ctx.trans_B) {
    output_map = map_A.transpose() * ctx.alpha * map_B;
  } else if (!ctx.trans_A && ctx.trans_B) {
    output_map = map_A * ctx.alpha * map_B.transpose();
  } else {
    output_map = map_A * ctx.alpha * map_B;
  }
}

// Handle CSR sparse format using Eigen
template <class T>
struct SparseToDenseCsr {
  void operator()(const ComputeCtx& ctx, const SparseTensor& A, const Tensor& B, Tensor& output) const {
    const auto& a_dims = A.DenseShape().GetDims();
    const auto& b_dims = B.Shape().GetDims();
    const auto& out_dims = output.Shape().GetDims();
    auto csr_view = A.AsCsr();

    ConstSparseMatrixMap<T> map_A(a_dims[0], a_dims[1], A.NumValues(),
                                  csr_view.Outer().Data<int64_t>(),
                                  csr_view.Inner().Data<int64_t>(),
                                  A.Values().Data<T>());
    ConstEigenMatrixMapRowMajor<T> map_B(B.Data<T>(), b_dims[0], b_dims[1]);
    EigenMatrixMapRowMajor<T> output_map(output.MutableData<T>(), out_dims[0], out_dims[1]);
    // XXX: Consider re-writing it as a parallel loop as Eigen requires it to use OpenMP
    // XXX: Consider vectorization
    SparseDenseMatMulImpl(ctx, map_A, map_B, output_map);
  }
};

#endif  //!defined(__i386__) && !defined(_M_IX86) && !defined(__wasm__) && !defined(__ANDROID__)

template <typename T>
inline T Mul(T a_value, float, T b_value) {
  return a_value * b_value;
}

template <>
inline float Mul<float>(float a_value, float alpha, float b_value) {
  return a_value * alpha * b_value;
}

// Inspired by TensorFlow SparseTensorDenseMatmul
template <typename T>
struct SparseToDenseCoo {
  Status operator()(const ComputeCtx& ctx, const SparseTensor& A, const Tensor& B, Tensor& output) const {
    const auto& b_dims = B.Shape().GetDims();
    const auto& out_dims = output.Shape().GetDims();
    const auto nnz = A.NumValues();

    auto a_values = A.Values().DataAsSpan<T>();
    auto coo_view = A.AsCoo();
    const auto& ind_dims = coo_view.Indices().Shape().GetDims();
    ORT_RETURN_IF_NOT(ind_dims.size() == 2, "COO indices must be 2-D, got: ", ind_dims.size());

    ConstEigenMatrixMapRowMajor<int64_t> a_indicies_map(coo_view.Indices().Data<int64_t>(), ind_dims[0], ind_dims[1]);
    ConstEigenMatrixMapRowMajor<T> map_b(B.Data<T>(), b_dims[0], b_dims[1]);
    EigenMatrixMapRowMajor<T> output_map(output.MutableData<T>(), out_dims[0], out_dims[1]);
    output_map.setZero();

    const auto rhs_right = (ctx.trans_B) ? b_dims[0] : b_dims[1];
    const auto lhs_right = (ctx.trans_B) ? b_dims[1] : b_dims[0];
    const int lhs_index_a = (ctx.trans_A) ? 1 : 0;
    const int rhs_index_a = (ctx.trans_A) ? 0 : 1;
    const auto out_left = out_dims[0];

    // XXX: Make this parallel
    for (size_t i = 0; i < nnz; ++i) {
      const auto m = a_indicies_map(i, lhs_index_a);
      const auto k = a_indicies_map(i, rhs_index_a);
      ORT_RETURN_IF_NOT(k < lhs_right, "COO k index: ", k, " is out of bounds of lhs_right: ", lhs_right);
      ORT_RETURN_IF_NOT(m < out_left, "COO m index: ", m, " is out of bounds of out_left: ", out_left);
      const T a_value = a_values[i];
      for (int64_t n = 0; n < rhs_right; ++n) {
        const T b_value = (ctx.trans_B) ? map_b(n, k) : map_b(k, n);
        output_map(m, n) += Mul(a_value, ctx.alpha, b_value);
      }
    }

    return Status::OK();
  }
};

}  // namespace

Status SparseToDenseMatMul::Compute(OpKernelContext* ctx) const {
  // We currently do not support batching, but may do so in the future
  const auto* A = ctx->Input<SparseTensor>(0);
  const auto* B = ctx->Input<Tensor>(1);

  const auto& A_shape = A->DenseShape();
  const auto& B_shape = B->Shape();

  ORT_RETURN_IF_NOT(A_shape.NumDimensions() == 2, "Currently supporting only 2-D matrices");
  ORT_RETURN_IF_NOT(B_shape.NumDimensions() == 2, "Currently supporting only 2-D matrices");

  const auto& a_dims = A_shape.GetDims();
  const auto& b_dims = B_shape.GetDims();

  const auto outer_A = (trans_a_attr_) ? a_dims[1] : a_dims[0];
  const auto inner_A = (trans_a_attr_) ? a_dims[0] : a_dims[1];
  const auto inner_B = (trans_b_attr_) ? b_dims[1] : b_dims[0];
  const auto outer_B = (trans_b_attr_) ? b_dims[0] : b_dims[1];

  ORT_RETURN_IF_NOT(inner_A == inner_B, "Can not multiply A and B as inner dimension does not match. inner_A: ",
                    inner_A, " vs inner_B: ", inner_B);

  TensorShape output_shape{outer_A, outer_B};
  auto* output = ctx->Output(0, output_shape);

  utils::MLTypeCallDispatcherFromTypeList<SparseToDenseSupportedTypes> t_disp(A->GetElementType());
  // I am not expecting to do the below in every kernel but this is a reference
  // implementation to show the expectations.
  ComputeCtx compute_ctx{trans_a_attr_ != 0, trans_b_attr_ != 0, alpha_attr_};
  if (A->Format() == SparseFormat::kCoo) {
    auto coo_view = A->AsCoo();
    const auto num_dims = coo_view.Indices().Shape().NumDimensions();
    ORT_RETURN_IF_NOT(num_dims == 2, "Expecting COO 2-D indices shape");
    ORT_RETURN_IF_NOT(A->Values().Shape().Size() * 2 == coo_view.Indices().Shape().Size(), "Expecting 2xValues == indices");
    auto status = t_disp.InvokeRet<Status, SparseToDenseCoo>(compute_ctx, *A, *B, *output);
    ORT_RETURN_IF_ERROR(status);
// Eigen has a bug in x86 where it calculates reallocation size as -1
// and throws bad_alloc
#if !defined(__i386__) && !defined(_M_IX86) && !defined(__wasm__) && !defined(__ANDROID__)
  } else if (A->Format() == SparseFormat::kCsrc) {
    auto csr_view = A->AsCsr();
    ORT_RETURN_IF_NOT(A->Values().Shape().Size() == csr_view.Inner().Shape().Size(),
                      "Expecting the same number NNZ == size of Inner indices");
    ORT_RETURN_IF_NOT((A_shape.GetDims()[0] + 1) == csr_view.Outer().Shape().Size(), "Outer size must be M + 1");
    t_disp.Invoke<SparseToDenseCsr>(compute_ctx, *A, *B, *output);
  } else {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Currently support only COO and CSR(x64) formats");
  }
#else
  } else {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "WASM and 32-bit builds support only COO format");
  }
#endif  //!defined(__i386__) && !defined(_M_IX86) && !defined(__wasm__) && !defined(__ANDROID__)

  return Status::OK();
}

namespace {
using SparseGemmSupportedTypes = TypeList<float, int64_t>;
}

class SparseToSparseMatMul final : public OpKernel {
 public:
  explicit SparseToSparseMatMul(const OpKernelInfo& info) : OpKernel(info) {
    info.GetAttrOrDefault<float>("alpha", &alpha_attr_, 1.0);
    int64_t trans_a_attr, trans_b_attr;
    info.GetAttrOrDefault<int64_t>("transA", &trans_a_attr, 0);
    info.GetAttrOrDefault<int64_t>("transB", &trans_b_attr, 0);
    trans_a_attr_ = trans_a_attr != 0;
    trans_b_attr_ = trans_b_attr != 0;
  }

  Status Compute(OpKernelContext*) const override;

 private:
  Status EigenCompute(const SparseTensor& input_A,
                      const SparseTensor& input_B,
                      std::vector<int64_t> a_dims,
                      std::vector<int64_t> b_dims,
                      SparseTensor& output_tensor) const;

  Status ComputeImpl(const SparseTensor& input_A,
                     const SparseTensor& input_B,
                     std::vector<int64_t> a_dims,
                     std::vector<int64_t> b_dims,
                     SparseTensor& output_tensor) const;

  float alpha_attr_;
  bool trans_a_attr_;
  bool trans_b_attr_;
};

ONNX_OPERATOR_KERNEL_EX(
    Gemm,
    kMSDomain,
    1,
    kCpuExecutionProvider,
    KernelDefBuilder()
        .TypeConstraint("T", BuildKernelDefSparseConstraintsFromTypeList<SparseGemmSupportedTypes>())
        .TypeConstraint("T1", BuildKernelDefConstraintsFromTypeList<SparseGemmSupportedTypes>()),
    SparseToSparseMatMul);

namespace {
Status CheckOutputDims(int64_t result_rows, int64_t result_cols, SparseTensor& output_tensor) {
  const auto output_rows = (output_tensor.DenseShape().NumDimensions() == 1)
                               ? 1
                               : output_tensor.DenseShape().GetDims()[0];

  ORT_RETURN_IF_NOT(output_rows == result_rows, "Result rows does not match output tensor rows");

  const auto output_cols = (output_tensor.DenseShape().NumDimensions() == 1)
                               ? output_tensor.DenseShape().GetDims()[0]
                               : output_tensor.DenseShape().GetDims()[1];

  ORT_RETURN_IF_NOT(output_cols == result_cols, "Result cols does not match output tensor cols");

  return Status::OK();
}

Status CheckDimsAndCopyResult(size_t nnz, int64_t result_rows, int64_t result_cols,
                              void* result_values_ptr, gsl::span<const int64_t> inner_span,
                              gsl::span<const int64_t> outer_span,
                              SparseTensor& output_tensor) {
  if (nnz == 0) {
    ORT_IGNORE_RETURN_VALUE(output_tensor.MakeCooData(0, 0));
    return Status::OK();
  }

  ORT_RETURN_IF_ERROR(CheckOutputDims(result_rows, result_cols, output_tensor));

  TensorShape result_values_shape{static_cast<int64_t>(nnz)};
  Tensor result_values(output_tensor.DataType(), result_values_shape, result_values_ptr, output_tensor.Location());
  auto coo_mutator = output_tensor.MakeCooData(nnz, nnz);
  sparse_utils::CopyCpuTensor(result_values, coo_mutator.Values());

  ORT_RETURN_IF_ERROR(sparse_utils::ConvertCsrIndicesToCooIndices(result_cols, inner_span, outer_span,
                                                                  coo_mutator.Indices().template MutableDataAsSpan<int64_t>()));

  return Status::OK();
}

inline bool IsVector(const std::vector<int64_t>& dims) {
  return (dims.size() == 1 || (dims.size() == 2 && (dims[0] == 1 || dims[1] == 1)));
}

inline bool IsRowVector(const std::vector<int64_t>& computed_dims, bool transpose) {
  return (computed_dims.size() == 2 && (transpose) ? computed_dims[1] == 1 : computed_dims[0] == 1);
}

inline bool IsColVector(const std::vector<int64_t>& computed_dims, bool transpose) {
  return (computed_dims.size() == 2 && (transpose) ? computed_dims[0] == 1 : computed_dims[1] == 1);
}

}  // namespace

#if !defined(__i386__) && !defined(_M_IX86) && !defined(__wasm__) && !defined(__ANDROID__)
namespace {

struct SparseToSparseComputeCtx {
  bool transA_;
  bool transB_;
  float alpha_;
  const Tensor& values_A_;
  gsl::span<const int64_t> inner_a_;
  gsl::span<const int64_t> outer_a_;
  const Tensor& values_B_;
  gsl::span<const int64_t> inner_b_;
  gsl::span<const int64_t> outer_b_;
  SparseTensor& output_;
};

template <typename T>
inline SparseMatrix<T> SparseToSparseMatMulImpl(const SparseToSparseComputeCtx& ctx,
                                                const ConstSparseMatrixMap<T>& map_A,
                                                const ConstSparseMatrixMap<T>& map_B) {
  if (ctx.transA_ && ctx.transB_) {
    return map_A.transpose() * map_B.transpose();
  } else if (ctx.transA_ && !ctx.transB_) {
    return map_A.transpose() * map_B;
  } else if (!ctx.transA_ && ctx.transB_) {
    return map_A * map_B.transpose();
  }
  return map_A * map_B;
}

template <>
inline SparseMatrix<float> SparseToSparseMatMulImpl<float>(const SparseToSparseComputeCtx& ctx,
                                                           const ConstSparseMatrixMap<float>& map_A,
                                                           const ConstSparseMatrixMap<float>& map_B) {
  if (ctx.transA_ && ctx.transB_) {
    return map_A.transpose() * ctx.alpha_ * map_B.transpose();
  } else if (ctx.transA_ && !ctx.transB_) {
    return map_A.transpose() * ctx.alpha_ * map_B;
  } else if (!ctx.transA_ && ctx.transB_) {
    return map_A * ctx.alpha_ * map_B.transpose();
  }
  return map_A * ctx.alpha_ * map_B;
}

template <class T>
struct SparseToSparseEigenMatrixB {
  Status operator()(const SparseToSparseComputeCtx& ctx,
                    const std::vector<int64_t>& computed_a_dims,
                    const std::vector<int64_t>& computed_b_dims) const {
    ConstSparseMatrixMap<T> map_A(computed_a_dims[0], computed_a_dims[1],
                                  ctx.values_A_.Shape().Size(),
                                  ctx.outer_a_.data(),
                                  ctx.inner_a_.data(),
                                  ctx.values_A_.template Data<T>());

    ConstSparseMatrixMap<T> map_B(computed_b_dims[0], computed_b_dims[1],
                                  ctx.values_B_.Shape().Size(),
                                  ctx.outer_b_.data(),
                                  ctx.inner_b_.data(),
                                  ctx.values_B_.template Data<T>());

    SparseMatrix<T> result = SparseToSparseMatMulImpl(ctx, map_A, map_B);
    const auto nnz = gsl::narrow<size_t>(result.nonZeros());
    const auto rows = gsl::narrow<size_t>(result.rows());
    gsl::span<const int64_t> inner_span = gsl::make_span(result.innerIndexPtr(), nnz);
    gsl::span<const int64_t> outer_span = gsl::make_span(result.outerIndexPtr(), rows + 1);
    return CheckDimsAndCopyResult(nnz, result.rows(), result.cols(), result.valuePtr(), inner_span, outer_span, ctx.output_);
  }
};

}  // namespace

Status SparseToSparseMatMul::EigenCompute(const SparseTensor& input_A,
                                          const SparseTensor& input_B,
                                          std::vector<int64_t> a_dims,
                                          std::vector<int64_t> b_dims,
                                          SparseTensor& output_tensor) const {
  const bool a_is_vector = IsVector(a_dims);

  nonstd::optional<std::vector<int64_t>> inner_indices_A;
  nonstd::optional<std::vector<int64_t>> outer_indices_A;

  const auto& coo_indices_a = input_A.AsCoo().Indices();
  ORT_RETURN_IF_ERROR(sparse_utils::ConvertCooIndicesToCsrIndices(a_dims, coo_indices_a.Shape().NumDimensions(),
                                                                  coo_indices_a.DataAsSpan<int64_t>(),
                                                                  inner_indices_A, outer_indices_A));

  bool a_transpose = trans_a_attr_;
  if (a_is_vector) {
    // For vectors conversion to CSR takes place as if it was a row vector
    // therefore, we need to flip transpose flag and swap dims
    if (IsColVector(a_dims, false)) {
      a_transpose = !a_transpose;
      std::swap(a_dims[0], a_dims[1]);
    }
  }

  const bool b_is_vector = IsVector(b_dims);
  nonstd::optional<std::vector<int64_t>> inner_indices_B;
  nonstd::optional<std::vector<int64_t>> outer_indices_B;
  const auto& coo_indices_b = input_B.AsCoo().Indices();
  ORT_RETURN_IF_ERROR(sparse_utils::ConvertCooIndicesToCsrIndices(b_dims, coo_indices_b.Shape().NumDimensions(),
                                                                  coo_indices_b.DataAsSpan<int64_t>(),
                                                                  inner_indices_B, outer_indices_B));
  bool b_transpose = trans_b_attr_;
  if (b_is_vector) {
    // For vectors conversion to CSR takes place as if it was a row vector
    // therefore, we need to flip transpose flag and swap dims
    if (IsColVector(b_dims, false)) {
      b_transpose = !b_transpose;
      std::swap(b_dims[0], b_dims[1]);
    }
  }

  utils::MLTypeCallDispatcherFromTypeList<SparseGemmSupportedTypes> t_disp(input_A.GetElementType());

  SparseToSparseComputeCtx compute_ctx{a_transpose, b_transpose, alpha_attr_,
                                       input_A.Values(), gsl::make_span(*inner_indices_A), gsl::make_span(*outer_indices_A),
                                       input_B.Values(), gsl::make_span(*inner_indices_B), gsl::make_span(*outer_indices_B),
                                       output_tensor};

  return t_disp.InvokeRet<Status, SparseToSparseEigenMatrixB>(compute_ctx, a_dims, b_dims);
}

#else

Status SparseToSparseMatMul::EigenCompute(const SparseTensor&,
                                          const SparseTensor&,
                                          std::vector<int64_t>,
                                          std::vector<int64_t>,
                                          SparseTensor&) const {
  return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Eigen Gemm sparse not supported on 32-bit builds");
}

#endif  // !defined(__i386__) && !defined(_M_IX86) && !defined(__wasm__) && !defined(__ANDROID__)

namespace {

// Used for RowVector to ColVector Mul
// The functor will multiply all non-zeros in A to all non-zeros in B
// and will produce appropriate indices for each of the products.
template <typename T>
struct ScalarAccumulate {
  void operator()(const Tensor& A_values, const Tensor& B_values, float alpha,
                  const gsl::span<const int64_t>& a_indices, const gsl::span<const int64_t>& b_indices,
                  Tensor& output_values) const {
    const T* a_data = A_values.Data<T>();
    const T* b_data = B_values.Data<T>();
    T accum = 0;

    auto match_cb = [&accum, a_data, alpha, b_data](size_t a_offset, size_t b_offset) {
      accum += Mul(a_data[a_offset], alpha, b_data[b_offset]);
    };

    sparse_utils::ScanForSparseMatches(a_indices, b_indices, match_cb);
    *output_values.MutableData<T>() = accum;
  }
};

size_t CountNonEmptyRows(const gsl::span<const int64_t>& outer_indices) {
  const size_t outer_limit = outer_indices.size() - 1;
  size_t non_empty_rows = 0;
  for (size_t row = 0; row < outer_limit; ++row) {
    const auto row_indices_start = gsl::narrow<gsl::index>(outer_indices[row]);
    const auto row_nnz_len = gsl::narrow<gsl::index>(outer_indices[row + 1]) - row_indices_start;
    if (row_nnz_len > 0) {
      ++non_empty_rows;
    }
  }
  return non_empty_rows;
}

// Used for ColVector to RowVector Mul
struct VectorOuterProductCtx {
  const SparseTensor& input_A_;
  const SparseTensor& input_B_;
  // 1d coo indices
  const gsl::span<const int64_t>& a_indices_;
  const gsl::span<const int64_t>& b_indices_;
  SparseTensor& output_tensor_;
};

template <typename T>
struct VectorOuterProduct {
  Status operator()(const VectorOuterProductCtx& ctx, float alpha) const {
    gsl::span<const T> a_values = ctx.input_A_.Values().DataAsSpan<T>();
    gsl::span<const T> b_values = ctx.input_B_.Values().DataAsSpan<T>();
    ORT_RETURN_IF_NOT(a_values.size() == ctx.a_indices_.size(), "A values size does not match A indices size");
    ORT_RETURN_IF_NOT(b_values.size() == ctx.b_indices_.size(), "B values size does not match B indices size");

    const auto cols = ctx.output_tensor_.DenseShape().GetDims()[1];
    // We know output is going to be m*n products of non-zeros (including app defined zeros)
    const size_t output_size = a_values.size() * b_values.size();
    auto coo_mutator = ctx.output_tensor_.MakeCooData(output_size, output_size);
    T* output_data = coo_mutator.Values().MutableData<T>();
    int64_t* output_indices = coo_mutator.Indices().MutableData<int64_t>();
    const size_t b_limit = ctx.b_indices_.size();
    for (size_t a_i = 0, a_limit = ctx.a_indices_.size(); a_i < a_limit; ++a_i) {
      const T a_val = a_values[a_i];
      const auto dest_row = ctx.a_indices_[a_i];
      const auto dest_row_offset = dest_row * cols;
      for (size_t b_i = 0; b_i < b_limit; ++b_i) {
        const auto dest_col = ctx.b_indices_[b_i];
        *output_indices++ = dest_row_offset + dest_col;
        *output_data++ = Mul(a_val, alpha, b_values[b_i]);
      }
    }
    return Status::OK();
  }
};

struct MatrixToVectorCtx {
  const Tensor& matrix_values_;
  const gsl::span<const int64_t>& mat_inner_;
  const gsl::span<const int64_t>& mat_outer_;
  const Tensor& vector_values_;
  const gsl::span<const int64_t>& b_1d_coo_;
  SparseTensor& output_tensor_;
};

// Performs multiplication of matrix to a column vector (or row vector to a matrix)
// A(m, n) B(n, 1) = C(m, 1) output is a column vector
// Providing the matrix is properly transposed, we can also do the reverse
// with the same code
// A(1, m) B(m, n) = C(1, n) => row vector
template <typename T>
struct MatrixToVector {
  Status operator()(const MatrixToVectorCtx& ctx, float alpha) const {
    // We know that A is not a fully sparse matrix at this point
    // we must have at least 1 row (actually more since it is not a vector and we handle vectors separately)
    // and outer indices must be at least 2 (for 1 row) or more in size.
    ORT_RETURN_IF_NOT(ctx.mat_outer_.size() > 1, "A outer indices must have at least 2 in size");

    // Scan all the rows and count those that contain any nnz
    const size_t output_size = CountNonEmptyRows(ctx.mat_outer_);

    auto matrix_values_span = ctx.matrix_values_.DataAsSpan<T>();
    const T* vector_values_data = ctx.vector_values_.Data<T>();
    auto coo_mutator = ctx.output_tensor_.MakeCooData(output_size, output_size);
    T* output_data = coo_mutator.Values().MutableData<T>();
    int64_t* output_indices = coo_mutator.Indices().MutableData<int64_t>();

    // XXX: This can be parallelized
    // We multiply each row nnz of the of the A matrix to vector B nnz.
    const size_t outer_limit = ctx.mat_outer_.size() - 1;
    for (size_t row = 0; row < outer_limit; ++row) {
      const auto row_indices_start = gsl::narrow<gsl::index>(ctx.mat_outer_[row]);
      const auto row_nnz_len = gsl::narrow<gsl::index>(ctx.mat_outer_[row + 1]) - row_indices_start;
      // We skip rows with no nnz, assuming that application defined zeros still show up in the nnz
      if (row_nnz_len > 0) {
        // Each row of indices has row relative offsets (column number)
        auto a_row_indices_span = ctx.mat_inner_.subspan(row_indices_start, row_nnz_len);
        auto a_row_values_span = matrix_values_span.subspan(row_indices_start, row_nnz_len);
        T accum = 0;
        auto accum_cb = [&accum, &a_row_values_span, alpha, vector_values_data](size_t a_offset, size_t b_offset) {
          accum += Mul(a_row_values_span[a_offset], alpha, vector_values_data[b_offset]);
        };
        sparse_utils::ScanForSparseMatches(a_row_indices_span, ctx.b_1d_coo_, accum_cb);
        *output_data++ = accum;
        *output_indices++ = gsl::narrow<int64_t>(row);
      }
    }
    return Status::OK();
  }
};

}  // namespace

Status SparseToSparseMatMul::ComputeImpl(const SparseTensor& input_A, const SparseTensor& input_B,
                                         std::vector<int64_t> a_dims, std::vector<int64_t> b_dims,
                                         SparseTensor& output_tensor) const {
  const bool a_is_vector = IsVector(a_dims);
  const bool b_is_vector = IsVector(b_dims);

  if (!a_is_vector && !b_is_vector) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Non-Eigen impl does not support this combination yet");
  }

  nonstd::optional<std::vector<int64_t>> inner_indices_A;
  nonstd::optional<std::vector<int64_t>> outer_indices_A;
  nonstd::optional<std::vector<size_t>> value_mapping_A; // only for transpose cases
  nonstd::optional<std::vector<int64_t>> a_1d_coo;
  gsl::span<const int64_t> a_1d_coo_span;
  if (a_is_vector) {
    ORT_RETURN_IF_ERROR(sparse_utils::GetCoo1DIndicesAndMaybeConvert(input_A, a_1d_coo, a_1d_coo_span));
  } else {
    if (b_is_vector) {
      // A Matrix to B vector
      const auto& coo_indices_a = input_A.AsCoo().Indices();
      if (!trans_a_attr_) {
        ORT_RETURN_IF_ERROR(sparse_utils::ConvertCooIndicesToCsrIndices(a_dims, coo_indices_a.Shape().NumDimensions(),
                                                                        coo_indices_a.DataAsSpan<int64_t>(),
                                                                        inner_indices_A, outer_indices_A));
      } else {
        // Transpose indices
        ORT_RETURN_IF_ERROR(sparse_utils::ConvertCooIndicesToCsrIndicesAndTranspose(a_dims, coo_indices_a.Shape().NumDimensions(),
                                                                                    coo_indices_a.DataAsSpan<int64_t>(),
                                                                                    inner_indices_A, outer_indices_A, value_mapping_A));
      }
    }
  }

  nonstd::optional<std::vector<int64_t>> inner_indices_B;
  nonstd::optional<std::vector<int64_t>> outer_indices_B;
  nonstd::optional<std::vector<size_t>> value_mapping_B;  // only for transpose cases
  nonstd::optional<std::vector<int64_t>> b_1d_coo;
  gsl::span<const int64_t> b_1d_coo_span;
  if (b_is_vector) {
    ORT_RETURN_IF_ERROR(sparse_utils::GetCoo1DIndicesAndMaybeConvert(input_B, b_1d_coo, b_1d_coo_span));
  } else {
    if (a_is_vector) {
      // We do reverse of the transpose flag bc we switch operands places, no need to transpose the vector
      // or the product since the product is a vector
      const auto& coo_indices_b = input_B.AsCoo().Indices();
      if (trans_b_attr_) {
        ORT_RETURN_IF_ERROR(sparse_utils::ConvertCooIndicesToCsrIndices(b_dims, coo_indices_b.Shape().NumDimensions(),
                                                                        coo_indices_b.DataAsSpan<int64_t>(),
                                                                        inner_indices_B, outer_indices_B));
      } else {
        // Transpose indices
        ORT_RETURN_IF_ERROR(sparse_utils::ConvertCooIndicesToCsrIndicesAndTranspose(b_dims, coo_indices_b.Shape().NumDimensions(),
                                                                                    coo_indices_b.DataAsSpan<int64_t>(),
                                                                                    inner_indices_B, outer_indices_B, value_mapping_B));
      }
    }
  }

  utils::MLTypeCallDispatcherFromTypeList<SparseGemmSupportedTypes> t_disp(input_A.GetElementType());
  if (a_is_vector && b_is_vector) {
    if (IsRowVector(a_dims, trans_a_attr_) && IsColVector(b_dims, trans_b_attr_)) {
      // Result is a scalar with dense shape [1, 1] and coordinates (0, 0)
      // need to multiply corresponding elements of the vectors
      assert(!a_1d_coo_span.empty());
      assert(!b_1d_coo_span.empty());
      auto coo_mutator = output_tensor.MakeCooData(1, 1);
      int64_t* output_indices = coo_mutator.Indices().MutableData<int64_t>();
      *output_indices = 0;
      t_disp.Invoke<ScalarAccumulate>(input_A.Values(), input_B.Values(), alpha_attr_,
                                      a_1d_coo_span, b_1d_coo_span,
                                      coo_mutator.Values());
      return Status::OK();
    } else if (IsColVector(a_dims, trans_a_attr_) && IsRowVector(b_dims, trans_b_attr_)) {
      // Result is a matrix [m, n] of everything non-zero in A to everything non-zero in B
      assert(!a_1d_coo_span.empty());
      assert(!b_1d_coo_span.empty());
      VectorOuterProductCtx compute_ctx{input_A, input_B, a_1d_coo_span, b_1d_coo_span, output_tensor};
      return t_disp.InvokeRet<Status, VectorOuterProduct>(compute_ctx, alpha_attr_);
    }
  } else if (!a_is_vector && b_is_vector) {
    // Result is a column vector
    assert(!inner_indices_A->empty());
    assert(!outer_indices_A->empty());
    assert(!b_1d_coo_span.empty());
    MatrixToVectorCtx compute_ctx{input_A.Values(), *inner_indices_A, *outer_indices_A,
                                  input_B.Values(), b_1d_coo_span,
                                  output_tensor};
    return t_disp.InvokeRet<Status, MatrixToVector>(compute_ctx, alpha_attr_);
  } else if (a_is_vector && !b_is_vector) {
    assert(!inner_indices_B->empty());
    assert(!outer_indices_B->empty());
    assert(!a_1d_coo_span.empty());
    MatrixToVectorCtx compute_ctx{input_B.Values(), *inner_indices_B, *outer_indices_B,
                                  input_A.Values(), a_1d_coo_span,
                                  output_tensor};
    return t_disp.InvokeRet<Status, MatrixToVector>(compute_ctx, alpha_attr_);
  }

  return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Unsupported operand combination");
}

Status SparseToSparseMatMul::Compute(OpKernelContext* ctx) const {
  const SparseTensor& input_A = *ctx->Input<SparseTensor>(0);
  const SparseTensor& input_B = *ctx->Input<SparseTensor>(1);

  ORT_RETURN_IF_NOT(input_A.Format() == SparseFormat::kCoo && input_B.Format() == SparseFormat::kCoo,
                    "Currently support only COO format");

  const auto& A_shape = input_A.DenseShape();
  const auto& B_shape = input_B.DenseShape();

  ORT_RETURN_IF_NOT(A_shape.NumDimensions() > 0 && A_shape.NumDimensions() <= 2, "Currently supporting only 1 and 2-D matrices");
  ORT_RETURN_IF_NOT(B_shape.NumDimensions() > 0 && B_shape.NumDimensions() <= 2, "Currently supporting only 1 and 2-D matrices");

  auto a_dims = A_shape.GetDims();
  auto b_dims = B_shape.GetDims();
  if (a_dims.size() == 1) {
    a_dims.insert(a_dims.begin(), 1);
  }

  if (b_dims.size() == 1) {
    b_dims.insert(b_dims.end(), 1);
  }

  const auto outer_A = (trans_a_attr_) ? a_dims[1] : a_dims[0];
  const auto inner_A = (trans_a_attr_) ? a_dims[0] : a_dims[1];
  const auto inner_B = (trans_b_attr_) ? b_dims[1] : b_dims[0];
  const auto outer_B = (trans_b_attr_) ? b_dims[0] : b_dims[1];

  ORT_RETURN_IF_NOT(inner_A == inner_B, "Can not multiply A and B as inner dimension does not match. inner_A: ",
                    inner_A, " vs inner_B: ", inner_B);

  std::vector<int64_t> output_dims{outer_A, outer_B};
  SparseTensor& output_tensor = *ctx->OutputSparse(0, output_dims);
  if (input_A.NumValues() == 0 || input_B.NumValues() == 0) {
    // All zeros as a result.
    ORT_IGNORE_RETURN_VALUE(output_tensor.MakeCooData(0, 0));
    return Status::OK();
  }

  //return EigenCompute(input_A, input_B, a_dims, b_dims, output_tensor);
  return ComputeImpl(input_A, input_B, a_dims, b_dims, output_tensor);
}

}  // namespace contrib
}  // namespace onnxruntime

#endif  //!defined(DISABLE_SPARSE_TENSORS)