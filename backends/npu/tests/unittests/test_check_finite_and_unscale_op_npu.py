#   Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
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

from __future__ import print_function

import numpy as np
import sys
import unittest
from tests.op_test import OpTest

import paddle
import paddle.fluid as fluid
from paddle.fluid import compiler, Program, program_guard
from paddle.fluid.contrib.mixed_precision.amp_nn import check_finite_and_unscale

paddle.enable_static()


class TestCheckFiniteAndUnscaleOp(OpTest):
    def setUp(self):
        self.set_npu()
        self.op_type = "check_finite_and_unscale"
        self.init_dtype()
        x = np.random.random((1024, 1024)).astype(self.dtype)
        scale = np.random.random((1)).astype(self.dtype)

        self.inputs = {'X': [('x0', x)], 'Scale': scale}
        self.outputs = {
            'FoundInfinite': np.array([0]),
            'Out': [('out0', x / scale)],
        }

    def init_dtype(self):
        self.dtype = np.float32

    def test_check_output(self):
        self.check_output_with_place(self.place)

    def set_npu(self):
        self.__class__.use_custom_device = True
        self.place = paddle.CustomPlace('npu', 0)


class TestCheckFiniteAndUnscaleOpWithNan(OpTest):
    def setUp(self):
        self.set_npu()
        self.op_type = "check_finite_and_unscale"
        self.init_dtype()
        x = np.random.random((1024, 1024)).astype(self.dtype)
        x[128][128] = np.nan
        scale = np.random.random((1)).astype(self.dtype)

        self.inputs = {'X': [('x0', x)], 'Scale': scale}
        self.outputs = {
            'FoundInfinite': np.array([1]),
            'Out': [('out0', x)],
        }

    def set_npu(self):
        self.__class__.use_custom_device = True
        self.place = paddle.CustomPlace('npu', 0)

    def init_dtype(self):
        self.dtype = np.float32

    def test_check_output(self):
        # When input contains nan, do not check the output,
        # since the output may be nondeterministic and will be discarded.
        self.check_output_with_place(self.place, no_check_set=['Out'])


class TestCheckFiniteAndUnscaleOpWithInf(OpTest):
    def set_npu(self):
        self.__class__.use_custom_device = True
        self.place = paddle.CustomPlace('npu', 0)

    def setUp(self):
        self.set_npu()
        self.op_type = "check_finite_and_unscale"
        self.init_dtype()
        x = np.random.random((1024, 1024)).astype(self.dtype)
        x[128][128] = np.inf
        scale = np.random.random((1)).astype(self.dtype)

        self.inputs = {'X': [('x0', x)], 'Scale': scale}
        self.outputs = {
            'FoundInfinite': np.array([1]),
            'Out': [('out0', x)],
        }

    def init_dtype(self):
        self.dtype = np.float32

    def test_check_output(self):
        # When input contains inf, do not check the output,
        # since the output may be nondeterministic and will be discarded.
        self.check_output_with_place(self.place, no_check_set=['Out'])


if __name__ == '__main__':
    unittest.main()
