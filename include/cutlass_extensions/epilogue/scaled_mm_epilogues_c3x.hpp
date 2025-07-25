#pragma once

#include "cutlass_extensions/epilogue/broadcast_load_epilogue_c3x.hpp"

/*
   This file defines custom epilogues for fusing channel scales, token scales,
   bias, and activation zero-points onto a GEMM operation using the
   CUTLASS 3.x API, for NVIDIA GPUs with sm90a (Hopper) or later.

   Epilogues must contain a public type named EVTCompute of type Sm90EVT,
   as well as a static prepare_args function that constructs an
   EVTCompute::Arguments struct.
*/

namespace lightllm::c3x {

using namespace cute;

/*
 * This class provides the common load descriptors for the
 * ScaledEpilogue[...] classes
 */
template <typename ElementAcc, typename ElementD, typename EpilogueDescriptor>
struct ScaledEpilogueBase {
 protected:
  using Accum = cutlass::epilogue::fusion::Sm90AccFetch;

  template <typename T>
  using ColOrScalarLoad = cutlass::epilogue::fusion::Sm90ColOrScalarBroadcast<
      0 /*Stages*/, typename EpilogueDescriptor::TileShape, T,
      Stride<Int<1>, Int<0>, Int<0>>>;

  template <typename T>
  using RowOrScalarLoad = cutlass::epilogue::fusion::Sm90RowOrScalarBroadcast<
      0 /*Stages*/, typename EpilogueDescriptor::TileShape, T,
      Stride<Int<0>, Int<1>, Int<0>>>;

  // Don't want to support nullptr by default
  template <typename T, bool EnableNullPtr = false>
  using ColLoad = cutlass::epilogue::fusion::Sm90ColBroadcast<
      0 /*Stages*/, typename EpilogueDescriptor::TileShape, T, T,
      Stride<Int<1>, Int<0>, Int<0>>, 128 / sizeof_bits_v<T>, EnableNullPtr>;

  // Don't want to support nullptr by default
  template <typename T, bool EnableNullPtr = false>
  using RowLoad = cutlass::epilogue::fusion::Sm90RowBroadcast<
      0 /*Stages*/, typename EpilogueDescriptor::TileShape, T, T,
      Stride<Int<0>, Int<1>, Int<0>>, 128 / sizeof_bits_v<T>, EnableNullPtr>;

  // This utility function constructs the arguments for the load descriptors
  // from a tensor. It can handle both row and column, as well as row/column or
  // scalar cases.
  template <typename Descriptor, typename T>
  static auto args_from_tensor(torch::Tensor const& tensor) {
    using Arguments = typename Descriptor::Arguments;
    auto* data_ptr = static_cast<T*>(tensor.data_ptr());
    if constexpr (std::is_same_v<Descriptor, ColOrScalarLoad<T>> ||
                  std::is_same_v<Descriptor, RowOrScalarLoad<T>>) {
      return Arguments{data_ptr, tensor.numel() != 1};
    } else {
      static_assert(!std::is_same_v<Descriptor, ColLoad<T, true>> &&
                    !std::is_same_v<Descriptor, RowLoad<T, true>>);
      return Arguments{data_ptr};
    }
  }

  // This overload handles the case where there might not be a tensor, in which
  // case a nullptr is passed and a constant (0) is used.
  template <typename Descriptor, typename T>
  static auto args_from_tensor(c10::optional<torch::Tensor> const& tensor) {
    using Arguments = typename Descriptor::Arguments;
    auto* data_ptr = tensor ? static_cast<T*>(tensor->data_ptr()) : nullptr;
    static_assert(std::is_same_v<Descriptor, ColLoad<T, true>> ||
                  std::is_same_v<Descriptor, RowLoad<T, true>>);
    return Arguments{data_ptr};
  }
};

/*
   This epilogue function defines a quantized GEMM operation similar to
   torch.scaled_mm_.

   A and B may be both either int8 or fp8_e4m3. A can be
   quantized per-tensor or per-row. B can be quantized per-tensor or per-column.
   Any combination of per-tensor and per-row or column is supported.
   A and B must have symmetric quantization (zero point == 0).

   So the GEMM operation is D = (a_scales * A) (b_scales * B), where the
   scales are applied elementwise with numpy-style broadcasting.

   ScaleA and ScaleB define the epilogue functions that apply the scales for
   the A and B operands respectively. These scales may be either per-tensor or
   per row or column.
*/
template <typename ElementAcc, typename ElementD, typename EpilogueDescriptor>
struct ScaledEpilogue
    : private ScaledEpilogueBase<ElementAcc, ElementD, EpilogueDescriptor> {
 private:
  using SUPER = ScaledEpilogueBase<ElementAcc, ElementD, EpilogueDescriptor>;
  using Accum = typename SUPER::Accum;
  using ScaleA = typename SUPER::template ColOrScalarLoad<float>;
  using ScaleB = typename SUPER::template RowOrScalarLoad<float>;

  using Compute0 = cutlass::epilogue::fusion::Sm90Compute<
      cutlass::multiplies, float, float,
      cutlass::FloatRoundStyle::round_to_nearest>;

  using EVTCompute0 =
      cutlass::epilogue::fusion::Sm90EVT<Compute0, ScaleB, Accum>;

  using Compute1 = cutlass::epilogue::fusion::Sm90Compute<
      cutlass::multiplies, ElementD, float,
      cutlass::FloatRoundStyle::round_to_nearest>;

 public:
  using EVTCompute =
      cutlass::epilogue::fusion::Sm90EVT<Compute1, ScaleA, EVTCompute0>;
  using ArgumentType = typename EVTCompute::Arguments;

  static ArgumentType prepare_args(torch::Tensor const& a_scales,
                                   torch::Tensor const& b_scales) {
    auto a_args = SUPER::template args_from_tensor<ScaleA, float>(a_scales);
    auto b_args = SUPER::template args_from_tensor<ScaleB, float>(b_scales);

    typename EVTCompute0::Arguments evt0_args{b_args};
    return ArgumentType{a_args, evt0_args};
  }
};

/*
 * This epilogue performs the same operation as ScaledEpilogue, but adds a bias.
 * This bias can also be used in the per-tensor azp case, where the activation
 * zero point (azp) is used to compute an azp correction term,
 * which is folded into the bias.
 *
 * The bias tensor must be per-output channel.
 * ScaleA and ScaleB can be per-tensor or per-token/per-channel.
 */
template <typename ElementAcc, typename ElementD, typename EpilogueDescriptor>
struct ScaledEpilogueBias
    : private ScaledEpilogueBase<ElementAcc, ElementD, EpilogueDescriptor> {
 private:
  using SUPER = ScaledEpilogueBase<ElementAcc, ElementD, EpilogueDescriptor>;
  using Accum = typename SUPER::Accum;
  using ScaleA = typename SUPER::template ColOrScalarLoad<float>;
  using ScaleB = typename SUPER::template RowOrScalarLoad<float>;
  using Bias = typename SUPER::template RowLoad<ElementD>;

  using Compute0 = cutlass::epilogue::fusion::Sm90Compute<
      cutlass::multiplies, float, float,
      cutlass::FloatRoundStyle::round_to_nearest>;

  using EVTCompute0 =
      cutlass::epilogue::fusion::Sm90EVT<Compute0, ScaleB, Accum>;

  using Compute1 = cutlass::epilogue::fusion::Sm90Compute<
      cutlass::multiply_add, ElementD, float,
      cutlass::FloatRoundStyle::round_to_nearest>;

 public:
  using EVTCompute =
      cutlass::epilogue::fusion::Sm90EVT<Compute1, ScaleA, EVTCompute0, Bias>;

  using ArgumentType = typename EVTCompute::Arguments;
  static ArgumentType prepare_args(torch::Tensor const& a_scales,
                                   torch::Tensor const& b_scales,
                                   torch::Tensor const& bias) {
    auto a_args = SUPER::template args_from_tensor<ScaleA, float>(a_scales);
    auto b_args = SUPER::template args_from_tensor<ScaleB, float>(b_scales);
    auto bias_args = SUPER::template args_from_tensor<Bias, ElementD>(bias);

    typename EVTCompute0::Arguments evt0_args{b_args};
    return ArgumentType{a_args, evt0_args, bias_args};
  }
};

/*
 * This epilogue performs the same operation as ScaledEpilogue, but multiplies a Ls.
 * The Ls tensor must be per-output channel.
 * ScaleA and ScaleB can be per-tensor or per-token/per-channel.
 */
template <typename ElementAcc, typename ElementD, typename EpilogueDescriptor>
struct ScaledEpilogueLs
    : private ScaledEpilogueBase<ElementAcc, ElementD, EpilogueDescriptor> {
 private:
  using SUPER = ScaledEpilogueBase<ElementAcc, ElementD, EpilogueDescriptor>;
  using Accum = typename SUPER::Accum;
  using ScaleA = typename SUPER::template ColOrScalarLoad<float>;
  using ScaleB = typename SUPER::template RowOrScalarLoad<float>;
  using Ls = typename SUPER::template RowLoad<ElementD>;

  using Compute0 = cutlass::epilogue::fusion::Sm90Compute<
      cutlass::multiplies, float, float,
      cutlass::FloatRoundStyle::round_to_nearest>;

  using EVTCompute0 =
      cutlass::epilogue::fusion::Sm90EVT<Compute0, ScaleB, Accum>;

  using Compute1 = cutlass::epilogue::fusion::Sm90Compute<
      cutlass::multiplies, float, float,
      cutlass::FloatRoundStyle::round_to_nearest>;

  using EVTCompute1 =
      cutlass::epilogue::fusion::Sm90EVT<Compute1, ScaleA, EVTCompute0>;

  using Compute2 = cutlass::epilogue::fusion::Sm90Compute<
      cutlass::multiplies, ElementD, float,
      cutlass::FloatRoundStyle::round_to_nearest>;
    

 public:
  using EVTCompute =
      cutlass::epilogue::fusion::Sm90EVT<Compute2, Ls, EVTCompute1>;

  using ArgumentType = typename EVTCompute::Arguments;
  static ArgumentType prepare_args(torch::Tensor const& a_scales,
                                   torch::Tensor const& b_scales,
                                   torch::Tensor const& ls) {
    auto a_args = SUPER::template args_from_tensor<ScaleA, float>(a_scales);
    auto b_args = SUPER::template args_from_tensor<ScaleB, float>(b_scales);
    auto ls_args = SUPER::template args_from_tensor<Ls, ElementD>(ls);

    typename EVTCompute0::Arguments evt0_args{b_args};
    typename EVTCompute1::Arguments evt1_args{a_args, evt0_args};
    return ArgumentType{ls_args, evt1_args};
  }
};


/*
 * This epilogue performs the same operation as ScaledEpilogue, but adds a bias and multiplies a Ls.
 * The bias tensor must be per-output channel.
 * The Ls tensor must be per-output channel.
 * ScaleA and ScaleB can be per-tensor or per-token/per-channel.
 */
template <typename ElementAcc, typename ElementD, typename EpilogueDescriptor>
struct ScaledEpilogueBiasLs
    : private ScaledEpilogueBase<ElementAcc, ElementD, EpilogueDescriptor> {
 private:
  using SUPER = ScaledEpilogueBase<ElementAcc, ElementD, EpilogueDescriptor>;
  using Accum = typename SUPER::Accum;
  using ScaleA = typename SUPER::template ColOrScalarLoad<float>;
  using ScaleB = typename SUPER::template RowOrScalarLoad<float>;
  using Bias = typename SUPER::template RowLoad<ElementD>;
  using Ls = typename SUPER::template RowLoad<ElementD>;

  using Compute0 = cutlass::epilogue::fusion::Sm90Compute<
      cutlass::multiplies, float, float,
      cutlass::FloatRoundStyle::round_to_nearest>;

  using EVTCompute0 =
      cutlass::epilogue::fusion::Sm90EVT<Compute0, ScaleB, Accum>;

  using Compute1 = cutlass::epilogue::fusion::Sm90Compute<
      cutlass::multiply_add, float, float,
      cutlass::FloatRoundStyle::round_to_nearest>;
    
  using EVTCompute1 =
      cutlass::epilogue::fusion::Sm90EVT<Compute1, ScaleA, EVTCompute0, Bias>;

  using Compute2 = cutlass::epilogue::fusion::Sm90Compute<
      cutlass::multiplies, ElementD, float,
      cutlass::FloatRoundStyle::round_to_nearest>;

 public:
  using EVTCompute =
      cutlass::epilogue::fusion::Sm90EVT<Compute2, Ls, EVTCompute1>;

  using ArgumentType = typename EVTCompute::Arguments;
  static ArgumentType prepare_args(torch::Tensor const& a_scales,
                                   torch::Tensor const& b_scales,
                                   torch::Tensor const& bias,
                                   torch::Tensor const& ls) {
    auto a_args = SUPER::template args_from_tensor<ScaleA, float>(a_scales);
    auto b_args = SUPER::template args_from_tensor<ScaleB, float>(b_scales);
    auto bias_args = SUPER::template args_from_tensor<Bias, ElementD>(bias);
    auto ls_args = SUPER::template args_from_tensor<Ls, ElementD>(ls);

    typename EVTCompute0::Arguments evt0_args{b_args};
    typename EVTCompute1::Arguments evt1_args{a_args, evt0_args, bias_args};
    return ArgumentType{ls_args, evt1_args};
  }
};


} // namespace lightllm::c3x