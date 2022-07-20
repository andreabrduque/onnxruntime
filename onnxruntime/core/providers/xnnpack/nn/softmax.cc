// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/xnnpack/nn/softmax.h"

#include "core/framework/op_kernel.h"
#include "core/providers/cpu/math/softmax_shared.h"

namespace onnxruntime {
namespace xnnpack {

namespace {
static bool IsQuantSoftmaxSupported(const NodeUnit& node_unit, const GraphViewer& graph) {
  bool supported = false;
  do {
    TensorQuantType x_input_type, output_type;
    x_input_type = GetTensorQuantType(node_unit, 0, false, graph);
    output_type = GetTensorQuantType(node_unit, 0, true, graph);
    if (x_input_type != TensorTypeUint8 ||
        output_type != TensorTypeUint8) {
      break;
    }
    supported = true;
  } while (false);

  return supported;
}

static bool IsQuantizedSoftmax(QuantizedOpType quant_op_type) {
  return (quant_op_type == QuantizedOpType::QDQSoftmax);
}
}  // namespace

bool Softmax::IsSoftmaxOnnxNodeSupported(const NodeUnit& node_unit,
                                         const GraphViewer& graph) {
  bool supported = false;
  if (IsQuantizedSoftmax(GetQuantizedOpType(node_unit)) &&
      IsQuantSoftmaxSupported(node_unit, graph) == false) {
    return false;
  }
  // use do {} while(false) so it's easier to set a breakpoint on the return
  do {
    // SoftMax has 1 input.
    const auto& inputs = node_unit.Inputs();
    const auto& x_arg = inputs[0].node_arg;

    // we only support float and u8 currently
    const auto* x_type = x_arg.TypeAsProto();
    if (x_type == nullptr ||
        (x_type->tensor_type().elem_type() != ONNX_NAMESPACE::TensorProto_DataType_FLOAT &&
         x_type->tensor_type().elem_type() != ONNX_NAMESPACE::TensorProto_DataType_UINT8)) {
      break;
    }
    ProtoHelperNodeContext nc(node_unit.GetNode());
    OpNodeProtoHelper info(&nc);

    // axis could be any dim, but we want it to be the last one right now.
    // otherwise, just leave it to CPU_EP
    int64_t axis = 1;
    info.GetAttrOrDefault<int64_t>("axis", &axis, -1);  // Opset 13 has default value -1
    if (node_unit.SinceVersion() <= 12 && axis == -1) {
      axis = 1;  // default 1 for op-version less than 12
    }

    const auto* x_shape = x_arg.Shape();
    if (!x_shape || x_shape->dim_size() == 0) {
      break;
    }

    if (axis != -1 && axis != x_shape->dim_size() - 1 && node_unit.SinceVersion() >= 13) {
      break;
    }

    // require the performed axises by Softmax to be known so we can construct the xnnpack kernel prior to Compute
    if (node_unit.SinceVersion() <= 12) {
      for (int axis_s = gsl::narrow_cast<int>(axis); axis_s < x_shape->dim_size(); ++axis_s) {
        if (!x_shape->dim(axis_s).has_dim_value()) {
          break;
        }
      }
    } else {
      // opset version >=13
      if (!x_shape->dim(x_shape->dim_size() - 1).has_dim_value()) {
        break;
      }
    }

    supported = true;
  } while (false);

  return supported;
}

Softmax::Softmax(const OpKernelInfo& info) : OpKernel{info} {
  const auto& node = info.node();
  int64_t opset = -1;
  Status status = info.GetAttr<int64_t>("opset", &opset);
  ORT_ENFORCE(status.IsOK(), "opset must be existed in attributes of QlinearSoftmax");
  opset_ = gsl::narrow_cast<int>(opset);

  int64_t axis = -1;
  status = info.GetAttr<int64_t>("axis", &axis);
  // our op checker function has ensured that axis must be the last dim
  // The "semantic" meaning of axis has changed in opset-13.
  // Please compare: https://github.com/onnx/onnx/blob/master/docs/Operators.md#Softmax
  // with https://github.com/onnx/onnx/blob/master/docs/Changelog.md#Softmax-11 for detailed explanations
  if (status.IsOK()) {
    axis_ = gsl::narrow_cast<int>(axis);
  } else {
    if (opset_ < 13) {
      axis_ = 1;  // opset-12 and below, the default axis value is 1
    } else {
      axis_ = -1;  // opset-13, the default axis value is -1
    }
  }

  // we have check it in GetCapability
  auto input_defs = node.InputDefs();
  const auto& x_shape = input_defs[0]->Shape();
  size_t rank = x_shape->dim_size();
  if (rank == 0) {
    return;
  }
  if (axis_ < 0) {
    axis_ = gsl::narrow<int>(HandleNegativeAxis(axis_, int64_t(rank)));
  }

  int kernel_dtype = 0;
  ORT_ENFORCE(GetType(*input_defs[0], kernel_dtype));
  if (kernel_dtype == ONNX_NAMESPACE::TensorProto_DataType_FLOAT) {
    op_type_ = OpComputeType::op_compute_type_fp32;
  } else if (kernel_dtype == ONNX_NAMESPACE::TensorProto_DataType_UINT8) {
    op_type_ = OpComputeType::op_compute_type_qu8;
  } else {
    ORT_THROW("error kernel type input, expected uint8|float, but got `", kernel_dtype, "`");
  }

  uint32_t channels = gsl::narrow_cast<uint32_t>(x_shape->dim(axis_).dim_value());
  if (opset_ < 13) {
    for (int i = axis_ + 1; i < gsl::narrow_cast<int>(rank); i++) {
      channels *= gsl::narrow_cast<uint32_t>(x_shape->dim(i).dim_value());
    }
  }

  xnn_status xstatus = xnn_status_invalid_state;
  struct xnn_operator* p = nullptr;
  if (op_type_ == OpComputeType::op_compute_type_qu8) {
    // the order of input tensor, x,x_scale, x_zp, y_scale, y_zp
    InputTensorOrder tensor_index = {-1, 1, 2, -1, -1, -1, 3, 4, -1};
    ParseQuantParamFromInfoByOrder(info, tensor_index, quant_param_);
    xstatus = xnn_create_softmax_nc_qu8(
        channels,
        channels,
        channels,
        quant_param_.X_scale_value,
        gsl::narrow_cast<uint8_t>(quant_param_.Y_zero_point_value),
        quant_param_.Y_scale_value,
        0,  // flags,
        &p);
  } else if (op_type_ == OpComputeType::op_compute_type_fp32) {
    xstatus = xnn_create_softmax_nc_f32(
        channels,
        channels,
        channels,
        0,  // flags,
        &p);
  }
  ORT_ENFORCE(xstatus == xnn_status_success, "xnn_create_softmax_nc failed. Status:", xstatus);
  op0_.reset(p);
}

// compute method of Softmax
Status Softmax::Compute(OpKernelContext* ctx) const {
  const auto* X = ctx->Input<Tensor>(0);
  const auto& X_shape = X->Shape();
  auto* Y = ctx->Output(0, X_shape);

  // edge case. one or more dims with value of 0. nothing to do
  if (X_shape.Size() == 0) {
    return Status::OK();
  }
  const size_t N = X_shape.SizeToDimension(axis_);
  // const size_t D = X_shape.SizeFromDimension(axis_); // the step D is 1
  xnn_status status = xnn_status_invalid_state;
  if (op_type_ == OpComputeType::op_compute_type_qu8) {
    status = xnn_setup_softmax_nc_qu8(
        op0_.get(),
        N,
        X->Data<uint8_t>(),
        Y->MutableData<uint8_t>(),
        nullptr);
  } else {
    status = xnn_setup_softmax_nc_f32(
        op0_.get(),
        N,
        X->Data<float>(),
        Y->MutableData<float>(),
        nullptr);
  }
  if (status != xnn_status_success) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "xnn_setup_softmax_nc_type returned ", status);
  }
  status = xnn_run_operator(op0_.get(), nullptr);
  if (status != xnn_status_success) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "xnn_run_operator returned ", status);
  }
  return Status::OK();
}

ONNX_OPERATOR_VERSIONED_KERNEL_EX(Softmax, kOnnxDomain, 1, 12, kXnnpackExecutionProvider,
                                  KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<float>()),
                                  Softmax);
ONNX_OPERATOR_KERNEL_EX(Softmax, kOnnxDomain, 13, kXnnpackExecutionProvider,
                        KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<float>()),
                        Softmax);
ONNX_OPERATOR_VERSIONED_KERNEL_EX(QLinearSoftmax, kMSInternalNHWCDomain, 1, 12, kXnnpackExecutionProvider,
                                  KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<uint8_t>()),
                                  Softmax);
ONNX_OPERATOR_KERNEL_EX(QLinearSoftmax, kMSInternalNHWCDomain, 13, kXnnpackExecutionProvider,
                        KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<uint8_t>()),
                        Softmax);

}  // namespace xnnpack
}  // namespace onnxruntime
