/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#if (defined(GOOGLE_CUDA) && GOOGLE_CUDA) || \
    (defined(TENSORFLOW_USE_ROCM) && TENSORFLOW_USE_ROCM)

#define EIGEN_USE_GPU

#include <stdio.h>

#include "tensorflow/core/kernels/softplus_op.h"

#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor_types.h"

namespace tensorflow {

typedef Eigen::GpuDevice GPUDevice;

// Definition of the GPU implementations declared in softplus_op.cc.
#define DEFINE_SOFTPLUS_GPU_KERNELS(T) \
  template struct functor::Softplus<GPUDevice, T>;

#define DEFINE_SOFTPLUS_GRAD_GPU_KERNELS(T) \
  template struct functor::SoftplusGrad<GPUDevice, T>;

#if !defined(MLIR_GENERATED_GPU_KERNELS_ENABLED)
TF_CALL_GPU_NUMBER_TYPES(DEFINE_SOFTPLUS_GPU_KERNELS);
TF_CALL_bfloat16(DEFINE_SOFTPLUS_GPU_KERNELS);
#endif

TF_CALL_GPU_NUMBER_TYPES(DEFINE_SOFTPLUS_GRAD_GPU_KERNELS);
TF_CALL_bfloat16(DEFINE_SOFTPLUS_GRAD_GPU_KERNELS);

}  // end namespace tensorflow

#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM
