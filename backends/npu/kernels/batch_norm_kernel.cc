// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "kernels/funcs/npu_funcs.h"
#include "kernels/funcs/npu_op_runner.h"

namespace custom_kernel {

template <typename T, typename Context>
void BatchNormKernel(const Context& dev_ctx,
                     const phi::DenseTensor& x,
                     const phi::DenseTensor& running_mean,
                     const phi::DenseTensor& running_var,
                     const phi::DenseTensor& scale,
                     const phi::DenseTensor& bias,
                     bool is_test,
                     float momentum,
                     float epsilon,
                     const std::string& data_layout_str,
                     bool use_global_stats,
                     bool trainable_stats,
                     phi::DenseTensor* y,
                     phi::DenseTensor* mean_out,
                     phi::DenseTensor* variance_out,
                     phi::DenseTensor* saved_mean,
                     phi::DenseTensor* saved_variance,
                     phi::DenseTensor* reserve_space) {
  bool test_mode = is_test && (!trainable_stats);
  bool training = !test_mode && !use_global_stats;

  phi::DataLayout data_layout = phi::StringToDataLayout(data_layout_str);

  dev_ctx.template Alloc<T>(y);
  phi::DenseTensor x_tensor(x), y_tensor(*y);
  const auto& x_dims = x.dims();

  PADDLE_ENFORCE_EQ(
      x_dims.size() >= 2 && x_dims.size() <= 5,
      true,
      phi::errors::InvalidArgument(
          "The size of input's dimensions should be between 2 and 5"
          "But received: the size of input's dimensions is [%d]",
          x_dims.size()));
  if (x_dims.size() == 2 && data_layout == phi::DataLayout::kNHWC) {
    data_layout = phi::DataLayout::kNCHW;
  }
  // transform 3d tensor to 4d tensor to satisfy the format
  if (x.dims().size() == 3) {
    auto x_shape_vec = phi::vectorize(x.dims());
    if (data_layout == phi::DataLayout::kNCHW) {
      x_shape_vec.push_back(1);  // expand NCL -> NCL1
    } else {
      x_shape_vec.insert(x_shape_vec.begin() + 2, 1);  // expand NLC -> NL1C
    }
    auto x_new_shape = phi::make_ddim(x_shape_vec);
    x_tensor.Resize(x_new_shape);
  }
  if (data_layout == phi::DataLayout::kNHWC) {
    phi::DenseTensorMeta x_meta = {
        x.dtype(), x_tensor.dims(), phi::DataLayout::kNHWC};
    phi::DenseTensorMeta y_meta = {
        y->dtype(), y->dims(), phi::DataLayout::kNHWC};
    x_tensor.set_meta(x_meta);
    y_tensor.set_meta(y_meta);
  }

  auto stream = dev_ctx.stream();
  if (!training) {
    const auto& runner_infer =
        NpuOpRunner("BNInfer",
                    {x_tensor, scale, bias, running_mean, running_var},
                    {y_tensor},
                    {{"epsilon", epsilon}});
    runner_infer.Run(stream);
  } else {
    dev_ctx.template Alloc<float>(mean_out);
    dev_ctx.template Alloc<float>(variance_out);
    dev_ctx.template Alloc<float>(saved_mean);
    dev_ctx.template Alloc<float>(saved_variance);

    phi::DenseTensor sum, square_sum;
    sum.Resize(running_mean.dims());
    square_sum.Resize(running_mean.dims());
    dev_ctx.template Alloc<float>(&sum);
    dev_ctx.template Alloc<float>(&square_sum);

    const auto& runner_reduce = NpuOpRunner("BNTrainingReduce",
                                            {x_tensor},
                                            {sum, square_sum},
                                            {{"epsilon", epsilon}});
    runner_reduce.Run(stream);

    const auto& runner_update = NpuOpRunner(
        "BNTrainingUpdate",
        {x_tensor, sum, square_sum, scale, bias, running_mean, running_var},
        {y_tensor, *mean_out, *variance_out, *saved_mean, *saved_variance},
        {{"factor", momentum}, {"epsilon", epsilon}});
    runner_update.Run(stream);
  }
}

template <typename T, typename Context>
void BatchNormGradKernel(
    const Context& dev_ctx,
    const phi::DenseTensor& x,
    const phi::DenseTensor& scale,
    const phi::DenseTensor& bias,
    const paddle::optional<phi::DenseTensor>& mean,
    const paddle::optional<phi::DenseTensor>& variance,
    const phi::DenseTensor& saved_mean,
    const phi::DenseTensor& saved_inv_variance,
    const paddle::optional<phi::DenseTensor>& reserve_space,
    const phi::DenseTensor& d_y,
    float momentum,
    float epsilon,
    const std::string& data_layout_str,
    bool is_test,
    bool use_global_stats,
    bool trainable_statistics,
    phi::DenseTensor* d_x,
    phi::DenseTensor* d_scale,
    phi::DenseTensor* d_bias) {
  phi::DataLayout data_layout = phi::StringToDataLayout(data_layout_str);

  use_global_stats = is_test || use_global_stats;

  phi::DenseTensor x_tensor(x), dy_tensor(d_y);

  if (x.dims().size() == 3) {
    auto x_shape_vec = phi::vectorize(x.dims());
    if (data_layout == phi::DataLayout::kNCHW) {
      x_shape_vec.push_back(1);  // expand NCL -> NCL1
    } else {
      x_shape_vec.insert(x_shape_vec.begin() + 2, 1);  // expand NLC -> NL1C
    }
    auto x_new_shape = phi::make_ddim(x_shape_vec);
    x_tensor.Resize(x_new_shape);
    dy_tensor.Resize(x_new_shape);
  }
  if (data_layout == phi::DataLayout::kNHWC) {
    phi::DenseTensorMeta x_meta = {
        x.dtype(), x_tensor.dims(), phi::DataLayout::kNHWC};
    phi::DenseTensorMeta dy_meta = {
        d_y.dtype(), dy_tensor.dims(), phi::DataLayout::kNHWC};
    x_tensor.set_meta(x_meta);
    dy_tensor.set_meta(dy_meta);
  }

  phi::DenseTensor scale_grad_tmp, bias_grad_tmp;
  scale_grad_tmp.Resize(scale.dims());
  bias_grad_tmp.Resize(bias.dims());
  dev_ctx.template Alloc<float>(&scale_grad_tmp);
  dev_ctx.template Alloc<float>(&bias_grad_tmp);

  if (d_scale == nullptr) {
    d_scale = &scale_grad_tmp;
  }
  if (d_bias == nullptr) {
    d_bias = &bias_grad_tmp;
  }

  auto stream = dev_ctx.stream();
  if (d_scale && d_bias) {
    dev_ctx.template Alloc<float>(d_scale);
    dev_ctx.template Alloc<float>(d_bias);

    if (use_global_stats) {
      const auto* running_mean = mean.get_ptr();
      const auto* running_variance = variance.get_ptr();
      const auto& runner_update =
          NpuOpRunner("BNTrainingUpdateGrad",
                      {dy_tensor, x_tensor, *running_mean, *running_variance},
                      {*d_scale, *d_bias},
                      {{"epsilon", epsilon}});
      runner_update.Run(stream);
    } else {
      const auto& runner_update =
          NpuOpRunner("BNTrainingUpdateGrad",
                      {dy_tensor, x_tensor, saved_mean, saved_inv_variance},
                      {*d_scale, *d_bias},
                      {{"epsilon", epsilon}});
      runner_update.Run(stream);
    }
  }
  if (d_x) {
    dev_ctx.template Alloc<T>(d_x);
    phi::DenseTensor dx_tensor(*d_x);
    if (data_layout == phi::DataLayout::kNHWC) {
      phi::DenseTensorMeta dx_meta = {
          d_x->dtype(), d_x->dims(), phi::DataLayout::kNHWC};
      dx_tensor.set_meta(dx_meta);
    }
    if (use_global_stats) {
      const auto* running_variance = variance.get_ptr();
      const auto& runner_infer =
          NpuOpRunner("BNInferGrad",
                      {dy_tensor, scale, *running_variance},
                      {dx_tensor},
                      {{"epsilon", epsilon}});
      runner_infer.Run(stream);
    } else {
      const auto& runner_reduce = NpuOpRunner("BNTrainingReduceGrad",
                                              {dy_tensor,
                                               x_tensor,
                                               *d_scale,
                                               *d_bias,
                                               scale,
                                               saved_mean,
                                               saved_inv_variance},
                                              {dx_tensor},
                                              {{"epsilon", epsilon}});
      runner_reduce.Run(stream);
    }
  }
}

template <typename T, typename Context>
void BatchNormInferKernel(const Context& dev_ctx,
                          const phi::DenseTensor& x,
                          const phi::DenseTensor& mean,
                          const phi::DenseTensor& variance,
                          const phi::DenseTensor& scale,
                          const phi::DenseTensor& bias,
                          float momentum,
                          float epsilon,
                          const std::string& data_layout_str,
                          phi::DenseTensor* y,
                          phi::DenseTensor* mean_out,
                          phi::DenseTensor* variance_out) {
  phi::DataLayout data_layout = StringToDataLayout(data_layout_str);

  const auto& x_dims = x.dims();

  dev_ctx.template Alloc<T>(y);

  phi::DenseTensor x_tensor(x);
  phi::DenseTensor y_tensor(*y);

  PADDLE_ENFORCE_EQ(
      x_dims.size() >= 2 && x_dims.size() <= 5,
      true,
      phi::errors::InvalidArgument(
          "The size of input's dimensions should be between 2 and 5"
          "But received: the size of input's dimensions is [%d]",
          x_dims.size()));

  if (x_dims.size() == 2 && data_layout == phi::DataLayout::kNHWC) {
    data_layout = phi::DataLayout::kNCHW;
  }
  if (x_dims.size() == 3) {
    auto x_shape_vec = phi::vectorize(x_dims);
    if (data_layout == phi::DataLayout::kNCHW) {
      x_shape_vec.push_back(1);  // expand NCL -> NCL1
    } else {
      x_shape_vec.insert(x_shape_vec.begin() + 2, 1);  // expand NLC -> NL1C
    }
    auto x_new_shape = phi::make_ddim(x_shape_vec);
    x_tensor.Resize(x_new_shape);
  }
  if (data_layout == phi::DataLayout::kNHWC) {
    phi::DenseTensorMeta x_meta = {
        x.dtype(), x_tensor.dims(), phi::DataLayout::kNHWC};
    phi::DenseTensorMeta y_meta = {
        y->dtype(), y->dims(), phi::DataLayout::kNHWC};
    x_tensor.set_meta(x_meta);
    y_tensor.set_meta(y_meta);
  }

  auto stream = dev_ctx.stream();
  const auto& runner_infer =
      NpuOpRunner("BNInfer",
                  {x_tensor, scale, bias, mean, variance},
                  {y_tensor},
                  {{"epsilon", epsilon}});
  runner_infer.Run(stream);
}

}  // namespace custom_kernel

PD_REGISTER_PLUGIN_KERNEL(batch_norm,
                          npu,
                          ALL_LAYOUT,
                          custom_kernel::BatchNormKernel,
                          phi::dtype::float16,
                          float,
                          double) {}

PD_REGISTER_PLUGIN_KERNEL(batch_norm_grad,
                          npu,
                          ALL_LAYOUT,
                          custom_kernel::BatchNormGradKernel,
                          phi::dtype::float16,
                          float,
                          double) {}

PD_REGISTER_PLUGIN_KERNEL(batch_norm_infer,
                          npu,
                          ALL_LAYOUT,
                          custom_kernel::BatchNormInferKernel,
                          phi::dtype::float16,
                          float,
                          double) {}
