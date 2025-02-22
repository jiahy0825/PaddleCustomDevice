/* Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "kernels/funcs/mlu_baseop.h"

namespace custom_kernel {

template <typename T, typename Context>
void ArgMaxKernel(const Context& dev_ctx,
                  const phi::DenseTensor& x,
                  const phi::Scalar& axis,
                  bool keepdims,
                  bool flatten,
                  int dtype,
                  phi::DenseTensor* out) {
  auto arg_max_axis = axis.to<int>();
  if (x.numel() == 0) return;
  PADDLE_ENFORCE_EQ(
      (dtype == 2 || dtype == 3),
      true,
      phi::errors::InvalidArgument(
          "The attribute of dtype in argmax op must be [%s] or [%s], "
          "but received [%s]",
          DataType::INT64,
          DataType::INT32,
          dtype));

  if (arg_max_axis < 0) {
    phi::DDim x_dims;
    x_dims = x.dims();
    arg_max_axis += x_dims.size();
  }

  Tensor flatten_x;
  flatten_x = x;
  if (flatten) {
    flatten_x.Resize(phi::make_ddim({x.numel()}));
    // if flatten, the arg_max_axis just as 0
    arg_max_axis = 0;
  }
  std::vector<int> reduce_dims;
  reduce_dims.push_back(arg_max_axis);

  auto out_dims = out->dims();
  int out_count = out_dims[0];
  for (int i = 1; i < out_dims.size(); i++) {
    out_count = out_count * out_dims[i];
  }
  size_t indices_size_inbytes = out_count * sizeof(int32_t);
  Tensor value_out;
  value_out.Resize(out->dims());
  dev_ctx.template Alloc<T>(&value_out);
  MLUCnnlTensorDesc value_out_desc(value_out);
  MLUCnnlTensorDesc input_desc(
      flatten_x, CNNL_LAYOUT_ARRAY, ToCnnlDataType(flatten_x.dtype()));
  MLUCnnlReduceDesc reduction_desc(reduce_dims,
                                   CNNL_REDUCE_MAX,
                                   ToCnnlDataType<T>(),
                                   CNNL_NOT_PROPAGATE_NAN,
                                   CNNL_REDUCE_ONLY_INDICES,
                                   CNNL_32BIT_INDICES);

  if (dtype == 2) {
    dev_ctx.template Alloc<int32_t>(out);
    MLUCnnl::Reduce(dev_ctx,
                    true /*need_workspace*/,
                    reduction_desc.get(),
                    nullptr,
                    input_desc.get(),
                    GetBasePtr(&flatten_x),
                    indices_size_inbytes /*indices_size*/,
                    GetBasePtr(out),
                    nullptr,
                    value_out_desc.get(),
                    GetBasePtr(&value_out));
  } else {
    dev_ctx.template Alloc<int64_t>(out);
    Tensor out_int32;
    out_int32.Resize(out->dims());
    dev_ctx.template Alloc<int32_t>(&out_int32);
    MLUCnnl::Reduce(dev_ctx,
                    true /*need_workspace*/,
                    reduction_desc.get(),
                    nullptr,
                    input_desc.get(),
                    GetBasePtr(&flatten_x),
                    indices_size_inbytes /*indices_size*/,
                    GetBasePtr(&out_int32),
                    nullptr,
                    value_out_desc.get(),
                    GetBasePtr(&value_out));

    // cast indices type to int64
    MLUCnnlTensorDesc out_int32_desc(out_int32);
    MLUCnnlTensorDesc cast_output_desc(*out);
    cnnlCastDataType_t cast_type =
        GetCastDataType(DataType::INT32, DataType::INT64);
    MLUCnnl::Cast(dev_ctx,
                  cast_type,
                  out_int32_desc.get(),
                  GetBasePtr(&out_int32),
                  cast_output_desc.get(),
                  GetBasePtr(out));
  }
}

}  // namespace custom_kernel

PD_REGISTER_PLUGIN_KERNEL(arg_max,
                          CustomMLU,
                          ALL_LAYOUT,
                          custom_kernel::ArgMaxKernel,
                          float,
                          phi::dtype::float16) {}
