# Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
import site
import sys
import unittest

import numpy as np

import paddle
import paddle.static as static
from paddle.fluid.framework import _test_eager_guard, in_dygraph_mode
from paddle.utils.cpp_extension.extension_utils import run_cmd
from paddle.vision.transforms import Compose, Normalize


def custom_relu_dynamic(func, device, dtype, np_x, use_func=True):
    paddle.set_device(device)

    t = paddle.to_tensor(np_x, dtype=dtype)
    t.stop_gradient = False
    sys.stdout.flush()

    out = func(t) if use_func else paddle.nn.functional.relu(t)
    out.stop_gradient = False

    out.backward()

    if t.grad is None:
        return out.numpy(), t.grad
    else:
        return out.numpy(), t.grad.numpy()


def custom_relu_static(func, device, dtype, np_x, use_func=True, test_infer=False):
    paddle.enable_static()
    paddle.set_device(device)

    with static.scope_guard(static.Scope()):
        with static.program_guard(static.Program()):
            x = static.data(name="X", shape=[None, 8], dtype=dtype)
            x.stop_gradient = False
            out = func(x) if use_func else paddle.nn.functional.relu(x)
            static.append_backward(out)

            exe = static.Executor()
            exe.run(static.default_startup_program())
            # in static mode, x data has been covered by out
            out_v = exe.run(
                static.default_main_program(),
                feed={"X": np_x},
                fetch_list=[out.name],
            )

    paddle.disable_static()
    return out_v


def custom_relu_static_pe(func, device, dtype, np_x, use_func=True):
    paddle.enable_static()
    paddle.set_device(device)

    places = paddle.CustomPlace("custom_cpu", 0)

    with static.scope_guard(static.Scope()):
        with static.program_guard(static.Program()):
            x = static.data(name="X", shape=[None, 8], dtype=dtype)
            x.stop_gradient = False
            out = func(x) if use_func else paddle.nn.functional.relu(x)
            static.append_backward(out)

            exe = static.Executor()
            exe.run(static.default_startup_program())

            # in static mode, x data has been covered by out
            compiled_prog = static.CompiledProgram(
                static.default_main_program()
            ).with_data_parallel(loss_name=out.name, places=places)
            out_v = exe.run(compiled_prog, feed={"X": np_x}, fetch_list=[out.name])

    paddle.disable_static()
    return out_v


def custom_relu_double_grad_dynamic(func, device, dtype, np_x, use_func=True):
    paddle.set_device(device)

    t = paddle.to_tensor(np_x, dtype=dtype, stop_gradient=False)

    out = func(t) if use_func else paddle.nn.functional.relu(t)
    out.stop_gradient = False
    dx = paddle.grad(outputs=[out], inputs=[t], create_graph=True, retain_graph=True)
    if in_dygraph_mode():
        dx[0].retain_grads()
    dx[0].backward()

    # Actually, dx[0].grad is an intermediate tensor of double_grad, we cannot obtain out_grad_grad
    assert dx[0].grad is not None
    return dx[0].numpy(), dx[0].grad.numpy()


class TestNewCustomOpSetUpInstall(unittest.TestCase):
    def setUp(self):
        cur_dir = os.path.dirname(os.path.abspath(__file__))
        # compile, install the custom op egg into site-packages under background
        # Currently custom_device op does not support Windows
        if os.name == "nt":
            return
        cmd = "cd {} && {} custom_relu_setup.py install".format(cur_dir, sys.executable)
        run_cmd(cmd)

        site_dir = site.getsitepackages()[0]
        custom_egg_path = [
            x for x in os.listdir(site_dir) if "custom_relu_module_setup" in x
        ]
        assert len(custom_egg_path) == 1, "Matched egg number is %d." % len(
            custom_egg_path
        )
        sys.path.append(os.path.join(site_dir, custom_egg_path[0]))

        # usage: import the package directly
        import custom_relu_module_setup

        self.custom_op = custom_relu_module_setup.custom_relu

        self.dtypes = ["float32", "float64"]
        self.devices = ["custom_cpu"]

        # config seed
        SEED = 2021
        paddle.seed(SEED)
        paddle.framework.random._manual_program_seed(SEED)

    def test_static(self):
        for device in self.devices:
            for dtype in self.dtypes:
                x = np.random.uniform(-1, 1, [4, 8]).astype(dtype)
                out = custom_relu_static(self.custom_op, device, dtype, x)
                pd_out = custom_relu_static(self.custom_op, device, dtype, x, False)
                np.testing.assert_array_equal(
                    out,
                    pd_out,
                    err_msg="custom op out: {},\n paddle api out: {}".format(
                        out, pd_out
                    ),
                )

    def test_static_pe(self):
        for device in self.devices:
            for dtype in self.dtypes:
                x = np.random.uniform(-1, 1, [4, 8]).astype(dtype)
                out = custom_relu_static_pe(self.custom_op, device, dtype, x)
                pd_out = custom_relu_static_pe(self.custom_op, device, dtype, x, False)
                np.testing.assert_array_equal(
                    out,
                    pd_out,
                    err_msg="custom op out: {},\n paddle api out: {}".format(
                        out, pd_out
                    ),
                )

    def func_dynamic(self):
        for device in self.devices:
            for dtype in self.dtypes:
                x = np.random.uniform(-1, 1, [4, 8]).astype(dtype)
                out, x_grad = custom_relu_dynamic(self.custom_op, device, dtype, x)
                pd_out, pd_x_grad = custom_relu_dynamic(
                    self.custom_op, device, dtype, x, False
                )
                np.testing.assert_array_equal(
                    out,
                    pd_out,
                    err_msg="custom op out: {},\n paddle api out: {}".format(
                        out, pd_out
                    ),
                )
                np.testing.assert_array_equal(
                    x_grad,
                    pd_x_grad,
                    err_msg="custom op x grad: {},\n paddle api x grad: {}".format(
                        x_grad, pd_x_grad
                    ),
                )

    def test_dynamic(self):
        with _test_eager_guard():
            self.func_dynamic()
        self.func_dynamic()

    def func_double_grad_dynamic(self):
        for device in self.devices:
            for dtype in self.dtypes:
                x = np.random.uniform(-1, 1, [4, 8]).astype(dtype)
                out, dx_grad = custom_relu_double_grad_dynamic(
                    self.custom_op, device, dtype, x
                )
                pd_out, pd_dx_grad = custom_relu_double_grad_dynamic(
                    self.custom_op, device, dtype, x, False
                )
                np.testing.assert_array_equal(
                    out,
                    pd_out,
                    err_msg="custom op out: {},\n paddle api out: {}".format(
                        out, pd_out
                    ),
                )
                np.testing.assert_array_equal(
                    dx_grad,
                    pd_dx_grad,
                    err_msg="custom op dx grad: {},\n paddle api dx grad: {}".format(
                        dx_grad, pd_dx_grad
                    ),
                )

    def test_double_grad_dynamic(self):
        with _test_eager_guard():
            self.func_double_grad_dynamic()
        self.func_double_grad_dynamic()

    def test_with_dataloader(self):
        for device in self.devices:
            paddle.set_device(device)
            # data loader
            transform = Compose(
                [Normalize(mean=[127.5], std=[127.5], data_format="CHW")]
            )
            train_dataset = paddle.vision.datasets.MNIST(
                mode="train", transform=transform
            )
            train_loader = paddle.io.DataLoader(
                train_dataset,
                batch_size=64,
                shuffle=True,
                drop_last=True,
                num_workers=0,
            )

            for batch_id, (image, _) in enumerate(train_loader()):
                out = self.custom_op(image)
                pd_out = paddle.nn.functional.relu(image)
                np.testing.assert_array_equal(
                    out,
                    pd_out,
                    err_msg="custom op out: {},\n paddle api out: {}".format(
                        out, pd_out
                    ),
                )

                if batch_id == 5:
                    break


if __name__ == "__main__":
    unittest.main()
