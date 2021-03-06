# Copyright (c) 2016-present, Facebook, Inc.
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
##############################################################################

## @package cnn
# Module caffe2.python.cnn
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

from caffe2.python import brew, workspace
from caffe2.python.model_helper import ModelHelper
from caffe2.proto import caffe2_pb2
import logging


class CNNModelHelper(ModelHelper):
    """A helper model so we can write CNN models more easily, without having to
    manually define parameter initializations and operators separately.
    """

    def __init__(self, order="NCHW", name=None,
                 use_gpu_engine=True, gpu_engine_exhaustive_search=False,
                 ws_nbytes_limit=None, init_params=True,
                 skip_sparse_optim=False,
                 param_model=None):
        logging.warning(
            "[====DEPRECATE WARNING====]: you are creating an "
            "object from CNNModelHelper class which will be deprecated soon. "
            "Please use ModelHelper object with brew module. For more "
            "information, please refer to caffe2.ai and python/brew.py, "
            "python/brew_test.py for more information."
        )

        cnn_arg_scope = {
            'order': order,
            'use_gpu_engine': use_gpu_engine,
            'gpu_engine_exhaustive_search': gpu_engine_exhaustive_search,
        }
        if ws_nbytes_limit:
            cnn_arg_scope['ws_nbytes_limit'] = ws_nbytes_limit
        super(CNNModelHelper, self).__init__(
            skip_sparse_optim=skip_sparse_optim,
            name="CNN" if name is None else name,
            init_params=init_params,
            param_model=param_model,
            arg_scope=cnn_arg_scope,
        )

        self.order = order
        self.use_gpu_engine = use_gpu_engine
        self.gpu_engine_exhaustive_search = gpu_engine_exhaustive_search
        self.ws_nbytes_limit = ws_nbytes_limit
        if self.order != "NHWC" and self.order != "NCHW":
            raise ValueError(
                "Cannot understand the CNN storage order %s." % self.order
            )

    def ImageInput(self, blob_in, blob_out, use_gpu_transform=False, **kwargs):
        return brew.image_input(
            self,
            blob_in,
            blob_out,
            order=self.order,
            use_gpu_transform=use_gpu_transform,
            **kwargs
        )

    def VideoInput(self, blob_in, blob_out, **kwargs):
        return brew.video_input(
            self,
            blob_in,
            blob_out,
            **kwargs
        )

    def PadImage(self, blob_in, blob_out, **kwargs):
        # TODO(wyiming): remove this dummy helper later
        self.net.PadImage(blob_in, blob_out, **kwargs)

    def ConvNd(self, *args, **kwargs):
        return brew.conv_nd(
            self,
            *args,
            use_gpu_engine=self.use_gpu_engine,
            order=self.order,
            gpu_engine_exhaustive_search=self.gpu_engine_exhaustive_search,
            ws_nbytes_limit=self.ws_nbytes_limit,
            **kwargs
        )

    def Conv(self, *args, **kwargs):
        return brew.conv(
            self,
            *args,
            use_gpu_engine=self.use_gpu_engine,
            order=self.order,
            gpu_engine_exhaustive_search=self.gpu_engine_exhaustive_search,
            ws_nbytes_limit=self.ws_nbytes_limit,
            **kwargs
        )

    def ConvTranspose(self, *args, **kwargs):
        return brew.conv_transpose(
            self,
            *args,
            use_gpu_engine=self.use_gpu_engine,
            order=self.order,
            gpu_engine_exhaustive_search=self.gpu_engine_exhaustive_search,
            ws_nbytes_limit=self.ws_nbytes_limit,
            **kwargs
        )

    def GroupConv(self, *args, **kwargs):
        return brew.group_conv(
            self,
            *args,
            use_gpu_engine=self.use_gpu_engine,
            order=self.order,
            gpu_engine_exhaustive_search=self.gpu_engine_exhaustive_search,
            ws_nbytes_limit=self.ws_nbytes_limit,
            **kwargs
        )

    def GroupConv_Deprecated(self, *args, **kwargs):
        return brew.group_conv_deprecated(
            self,
            *args,
            use_gpu_engine=self.use_gpu_engine,
            order=self.order,
            gpu_engine_exhaustive_search=self.gpu_engine_exhaustive_search,
            ws_nbytes_limit=self.ws_nbytes_limit,
            **kwargs
        )

    def FC(self, *args, **kwargs):
        return brew.fc(self, *args, **kwargs)

    def PackedFC(self, *args, **kwargs):
        return brew.packed_fc(self, *args, **kwargs)

    def FC_Prune(self, *args, **kwargs):
        return brew.fc_prune(self, *args, **kwargs)

    def FC_Decomp(self, *args, **kwargs):
        return brew.fc_decomp(self, *args, **kwargs)

    def FC_Sparse(self, *args, **kwargs):
        return brew.fc_sparse(self, *args, **kwargs)

    def Dropout(self, *args, **kwargs):
        return brew.dropout(
            self, *args, order=self.order, use_gpu_engine=self.use_gpu_engine, **kwargs
        )

    def LRN(self, *args, **kwargs):
        return brew.lrn(
            self, *args, order=self.order, use_gpu_engine=self.use_gpu_engine, **kwargs
        )

    def Softmax(self, *args, **kwargs):
        return brew.softmax(self, *args, use_gpu_engine=self.use_gpu_engine, **kwargs)

    def SpatialBN(self, *args, **kwargs):
        return brew.spatial_bn(self, *args, order=self.order, **kwargs)

    def InstanceNorm(self, *args, **kwargs):
        return brew.instance_norm(self, *args, order=self.order, **kwargs)

    def Relu(self, *args, **kwargs):
        return brew.relu(
            self, *args, order=self.order, use_gpu_engine=self.use_gpu_engine, **kwargs
        )

    def PRelu(self, *args, **kwargs):
        return brew.prelu(self, *args, **kwargs)

    def Concat(self, *args, **kwargs):
        return brew.concat(self, *args, order=self.order, **kwargs)

    def DepthConcat(self, *args, **kwargs):
        """The old depth concat function - we should move to use concat."""
        print("DepthConcat is deprecated. use Concat instead.")
        return self.Concat(*args, **kwargs)

    def Sum(self, *args, **kwargs):
        return brew.sum(self, *args, **kwargs)

    def Transpose(self, *args, **kwargs):
        return brew.transpose(self, *args, use_gpu_engine=self.use_gpu_engine, **kwargs)

    def Iter(self, *args, **kwargs):
        return brew.iter(self, *args, **kwargs)

    def Accuracy(self, *args, **kwargs):
        return brew.accuracy(self, *args, **kwargs)

    def MaxPool(self, *args, **kwargs):
        return brew.max_pool(
            self, *args, use_gpu_engine=self.use_gpu_engine, order=self.order, **kwargs
        )

    def MaxPoolWithIndex(self, *args, **kwargs):
        return brew.max_pool_with_index(self, *args, order=self.order, **kwargs)

    def AveragePool(self, *args, **kwargs):
        return brew.average_pool(
            self, *args, use_gpu_engine=self.use_gpu_engine, order=self.order, **kwargs
        )

    @property
    def XavierInit(self):
        return ('XavierFill', {})

    def ConstantInit(self, value):
        return ('ConstantFill', dict(value=value))

    @property
    def MSRAInit(self):
        return ('MSRAFill', {})

    @property
    def ZeroInit(self):
        return ('ConstantFill', {})

    def AddWeightDecay(self, weight_decay):
        return brew.add_weight_decay(self, weight_decay)

    @property
    def CPU(self):
        device_option = caffe2_pb2.DeviceOption()
        device_option.device_type = caffe2_pb2.CPU
        return device_option

    @property
    def GPU(self, gpu_id=0):

        device_option = caffe2_pb2.DeviceOption()
        if workspace.has_hip:
            device_option.device_type = caffe2_pb2.HIP
            device_option.hip_gpu_id = gpu_id
        else:
            device_option.device_type = caffe2_pb2.CUDA
            device_option.cuda_gpu_id = gpu_id
        return device_option
