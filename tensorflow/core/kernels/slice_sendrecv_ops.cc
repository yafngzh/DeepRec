/* Copyright 2023 The DeepRec Authors. All Rights Reserved.

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

#include "tensorflow/core/kernels/slice_sendrecv_ops.h"

namespace tensorflow {

//------------------------------------------------------------------------------
// Utils.
static string GetSliceRendezvousKeyPrefix(const string& send_device,
                                          const string& recv_device,
                                          const uint64 send_device_incarnation,
                                          const string& tensor_name) {
  return strings::StrCat(send_device, ";",
                         strings::FpToString(send_device_incarnation), ";",
                         recv_device, ";", tensor_name);
}

static void GetSliceRendezvousKey(const string& key_prefix,
                                  const string& tensor_name_suffix,
                                  const FrameAndIter& frame_iter, string* key) {
  key->clear();
  strings::StrAppend(key, key_prefix, tensor_name_suffix, ";",
                     frame_iter.frame_id, ":", frame_iter.iter_id);
}

static FrameAndIter GetFrameAndIter(OpKernelContext* ctx,
                                    bool hostmem_sendrecv) {
  if (hostmem_sendrecv && ctx->call_frame() != nullptr) {
    // Host memory send/recv pairs are added by
    // common_runtime/memory_types.cc.  When the pair of nodes are
    // added inside a function, we need to use the function call frame
    // to formulate the unique rendezvous key.
    return FrameAndIter(reinterpret_cast<uint64>(ctx->call_frame()), 0);
  } else {
    return ctx->frame_iter();
  }
}

//------------------------------------------------------------------------------
// Functions of SliceSendOp.

SliceSendOp::SliceSendOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
  string send_device;
  OP_REQUIRES_OK(ctx, ctx->GetAttr("send_device", &send_device));
  string recv_device;
  OP_REQUIRES_OK(ctx, ctx->GetAttr("recv_device", &recv_device));
  uint64 send_device_incarnation;
  OP_REQUIRES_OK(
      ctx, ctx->GetAttr("send_device_incarnation",
                        reinterpret_cast<int64*>(&send_device_incarnation)));
  string tensor_name;
  OP_REQUIRES_OK(ctx, ctx->GetAttr("tensor_name", &tensor_name));
  key_prefix_ = \
    GetSliceRendezvousKeyPrefix(send_device, recv_device,
                                send_device_incarnation, tensor_name);
  if (!ctx->GetAttr("_hostmem_sendrecv", &hostmem_sendrecv_).ok()) {
    hostmem_sendrecv_ = false;
  }
  OP_REQUIRES_OK(ctx, ctx->GetAttr("slice_size", &slice_size_));
  OP_REQUIRES_OK(ctx, ctx->GetAttr("T", &dtype_));
}

void SliceSendOp::Compute(OpKernelContext* ctx) {
  OP_REQUIRES(
    ctx, ctx->rendezvous() != nullptr,
    errors::Internal("Op kernel context needs to provide a rendezvous."));

  const Tensor& input_t = ctx->input(0);
  FrameAndIter frame_iter = GetFrameAndIter(ctx, hostmem_sendrecv_);

  // send total_bytes.
  OP_REQUIRES_OK(ctx, SendTotalBytes(ctx, frame_iter, input_t));
  // if input is dead, only send total_bytes dead tensor.
  if (ctx->is_input_dead()) {
    return;
  }

  // if total bytes is smaller than slice size, send directly.
  if (input_t.TotalBytes() <= slice_size_) {
    Rendezvous::Args args;
    args.device_context = ctx->op_device_context();
    args.alloc_attrs = ctx->input_alloc_attr(0);

    Rendezvous::ParsedKey parsed_key;
    GetSliceRendezvousKey(key_prefix_, "_transfer_data", frame_iter,
                          &parsed_key.buf_);
    VLOG(2) << "SliceSend " << parsed_key.buf_;
    OP_REQUIRES_OK(ctx, Rendezvous::ParseKey(parsed_key.buf_, &parsed_key));
    OP_REQUIRES_OK(ctx, ctx->rendezvous()->Send(parsed_key, args, input_t,
                                                ctx->is_input_dead()));
    return;
  }

  // send shape.
  OP_REQUIRES_OK(ctx, SendShape(ctx, frame_iter, input_t));

  // send data.
  if (dtype_ == DT_STRING) {
    OP_REQUIRES_OK(ctx, SendString(ctx, frame_iter, input_t));
  } else {
    OP_REQUIRES_OK(ctx, SendBasicType(ctx, frame_iter, input_t));
  }
}

Status SliceSendOp::SendTotalBytes(OpKernelContext* ctx,
                                   const FrameAndIter& frame_iter,
                                   const Tensor& input_t) {
  Rendezvous::Args args;
  args.device_context = ctx->op_device_context();
  args.alloc_attrs = AllocatorAttributes();

  Rendezvous::ParsedKey parsed_key;
  Tensor total_bytes_t;
  TF_RETURN_IF_ERROR(ctx->allocate_temp(DT_INT64, TensorShape({}),
                                        &total_bytes_t));
  total_bytes_t.scalar<int64>()() = input_t.TotalBytes();
  GetSliceRendezvousKey(key_prefix_, "_slice_transfer_totalbytes", frame_iter,
                        &parsed_key.buf_);
  VLOG(2) << "SliceSend " << parsed_key.buf_;
  TF_RETURN_IF_ERROR(Rendezvous::ParseKey(parsed_key.buf_, &parsed_key));
  return ctx->rendezvous()->Send(parsed_key, args, total_bytes_t,
                                 ctx->is_input_dead());
}

Status SliceSendOp::SendShape(OpKernelContext* ctx,
                              const FrameAndIter& frame_iter,
                              const Tensor& input_t) {
  Rendezvous::Args args;
  args.device_context = ctx->op_device_context();
  args.alloc_attrs = AllocatorAttributes();
  Rendezvous::ParsedKey parsed_key;

  Tensor shape_t;
  TensorShape shape = input_t.shape();
  const int rank = shape.dims();
  TF_RETURN_IF_ERROR(ctx->allocate_temp(DT_INT64, TensorShape({rank}),
                                        &shape_t));
  auto shape_vec = shape_t.vec<int64>();
  for (int i = 0; i < rank; i++) {
    shape_vec(i) = shape.dim_size(i);
  }
  GetSliceRendezvousKey(key_prefix_, "_slice_transfer_shape", frame_iter,
                        &parsed_key.buf_);
  VLOG(2) << "SliceSend " << parsed_key.buf_;
  TF_RETURN_IF_ERROR(Rendezvous::ParseKey(parsed_key.buf_, &parsed_key));
  return ctx->rendezvous()->Send(parsed_key, args, shape_t,
                                 ctx->is_input_dead());
}

Status SliceSendOp::SendString(OpKernelContext* ctx,
                               const FrameAndIter& frame_iter,
                               const Tensor& input_t) {
  Rendezvous::Args args;
  args.device_context = ctx->op_device_context();
  args.alloc_attrs = AllocatorAttributes();
  Rendezvous::ParsedKey parsed_key;

  // send elements size.
  Tensor elements_size_t;
  TF_RETURN_IF_ERROR(ctx->allocate_temp(DT_INT64, input_t.shape(),
                                        &elements_size_t));
  int64 num_elements = input_t.NumElements();
  auto input_flat = input_t.flat<tstring>();
  auto elements_size_flat = elements_size_t.flat<int64>();
  for (int64 i = 0; i < num_elements; i++) {
    elements_size_flat(i) = input_flat(i).size();
  }
  GetSliceRendezvousKey(key_prefix_, "_slice_transfer_elements_size",
                        frame_iter, &parsed_key.buf_);
  VLOG(2) << "SliceSend " << parsed_key.buf_;
  TF_RETURN_IF_ERROR(Rendezvous::ParseKey(parsed_key.buf_, &parsed_key));
  TF_RETURN_IF_ERROR(ctx->rendezvous()->Send(parsed_key, args, elements_size_t,
                                             ctx->is_input_dead()));

  // send data.
  args.alloc_attrs = ctx->input_alloc_attr(0);
  Tensor data_t;
  for (int64 i = 0; i < num_elements; i++) {
    const std::string& elem = input_flat(i);
    if (elem.size() <= slice_size_) {
      TF_RETURN_IF_ERROR(ctx->allocate_temp(DT_STRING, TensorShape({}),
                                            &data_t));
      data_t.scalar<tstring>()() = elem;
      std::string tensor_name_suffix = \
        strings::StrCat("_slice_transfer_data_", std::to_string(i));
      GetSliceRendezvousKey(key_prefix_, tensor_name_suffix, frame_iter,
                            &parsed_key.buf_);
      VLOG(2) << "SliceSend " << parsed_key.buf_;
      TF_RETURN_IF_ERROR(Rendezvous::ParseKey(parsed_key.buf_, &parsed_key));
      TF_RETURN_IF_ERROR(ctx->rendezvous()->Send(parsed_key, args, data_t,
                                                 ctx->is_input_dead()));
    } else {
      TF_RETURN_IF_ERROR(SendStringSlice(ctx, frame_iter, elem, i));
    }
  }

  return Status::OK();
}

Status SliceSendOp::SendStringSlice(OpKernelContext* ctx,
                                    const FrameAndIter& frame_iter,
                                    const std::string& elem, int64 index) {
  Rendezvous::Args args;
  args.device_context = ctx->op_device_context();
  args.alloc_attrs = ctx->input_alloc_attr(0);
  Rendezvous::ParsedKey parsed_key;

  int64 slice_num = (elem.size() + slice_size_ - 1) / slice_size_;
  Tensor data_t;
  for (int64 i = 0; i < slice_num; i++) {
    TF_RETURN_IF_ERROR(ctx->allocate_temp(DT_STRING, TensorShape({}), &data_t));
    size_t start = i * slice_size_;
    size_t copy_size = slice_size_;
    if (start > elem.size() - slice_size_) {
      copy_size = elem.size() - start;
    }
    data_t.scalar<tstring>()() = elem.substr(start, copy_size);
    std::string tensor_name_suffix = \
      strings::StrCat("_slice_transfer_data_", std::to_string(index), "_",
                      std::to_string(i));
    GetSliceRendezvousKey(key_prefix_, tensor_name_suffix, frame_iter,
                          &parsed_key.buf_);
    VLOG(2) << "SliceSend " << parsed_key.buf_;
    TF_RETURN_IF_ERROR(Rendezvous::ParseKey(parsed_key.buf_, &parsed_key));
    TF_RETURN_IF_ERROR(ctx->rendezvous()->Send(parsed_key, args, data_t,
                                               ctx->is_input_dead()));
  }

  return Status::OK();
}

Status SliceSendOp::SendBasicType(OpKernelContext* ctx,
                                  const FrameAndIter& frame_iter,
                                  const Tensor& input_t) {
  Rendezvous::Args args;
  args.device_context = ctx->op_device_context();
  args.alloc_attrs = ctx->input_alloc_attr(0);
  Rendezvous::ParsedKey parsed_key;

  // send data.
  Tensor data_t;
  int64 bytes_num = input_t.TotalBytes();
  int64 slice_num = (bytes_num + slice_size_ - 1) / slice_size_;
  unsigned char* input_base = reinterpret_cast<unsigned char*>(input_t.data());
  for (int64 i = 0; i < slice_num; i++) {
    int64 start = i * slice_size_;
    int64 copy_size = slice_size_;
    if (start > bytes_num - slice_size_) {
      copy_size = bytes_num - start;
    }
    TF_RETURN_IF_ERROR(ctx->allocate_temp(DT_INT8, TensorShape({copy_size}),
                                          &data_t));
    auto data_base = data_t.data();
    std::memcpy(data_base, input_base+start, copy_size);
    std::string tensor_name_suffix = \
      strings::StrCat("_slice_transfer_data_", std::to_string(i));
    GetSliceRendezvousKey(key_prefix_, tensor_name_suffix, frame_iter,
                          &parsed_key.buf_);
    VLOG(2) << "SliceSend " << parsed_key.buf_;
    TF_RETURN_IF_ERROR(Rendezvous::ParseKey(parsed_key.buf_, &parsed_key));
    TF_RETURN_IF_ERROR(ctx->rendezvous()->Send(parsed_key, args, data_t,
                                               ctx->is_input_dead()));
  }

  return Status::OK();
}

REGISTER_KERNEL_BUILDER(Name("_SliceSend").Device(DEVICE_CPU), SliceSendOp);
REGISTER_KERNEL_BUILDER(Name("_SliceSend").Device(DEVICE_DEFAULT), SliceSendOp);

//------------------------------------------------------------------------------
// Functions of SliceRecvOp.

SliceRecvOp::SliceRecvOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
  string send_device;
  OP_REQUIRES_OK(ctx, ctx->GetAttr("send_device", &send_device));
  string recv_device;
  OP_REQUIRES_OK(ctx, ctx->GetAttr("recv_device", &recv_device));
  uint64 send_device_incarnation;
  OP_REQUIRES_OK(
      ctx, ctx->GetAttr("send_device_incarnation",
                        reinterpret_cast<int64*>(&send_device_incarnation)));
  string tensor_name;
  OP_REQUIRES_OK(ctx, ctx->GetAttr("tensor_name", &tensor_name));
  key_prefix_ = \
    GetSliceRendezvousKeyPrefix(send_device, recv_device,
                                send_device_incarnation, tensor_name);
  if (!ctx->GetAttr("_hostmem_sendrecv", &hostmem_sendrecv_).ok()) {
    hostmem_sendrecv_ = false;
  }
  OP_REQUIRES_OK(ctx, ctx->GetAttr("slice_size", &slice_size_));
  OP_REQUIRES_OK(ctx, ctx->GetAttr("tensor_type", &dtype_));
  OP_REQUIRES_OK(ctx, ctx->GetAttr("timeout_ms", &timeout_ms_));
}

void SliceRecvOp::Compute(OpKernelContext* ctx) {
  OP_REQUIRES(
    ctx, ctx->rendezvous() != nullptr,
    errors::Internal("Op kernel context needs to provide a rendezvous."));

  FrameAndIter frame_iter = GetFrameAndIter(ctx, hostmem_sendrecv_);
  bool is_dead;

  // recv total_bytes.
  int64 total_bytes;
  OP_REQUIRES_OK(ctx, RecvTotalBytes(ctx, frame_iter, is_dead, total_bytes));
  if (is_dead) {
    return;
  }

  // if total bytes is smaller than slice size, recv directly.
  if (total_bytes <= slice_size_) {
    Rendezvous::Args args;
    args.device_context = ctx->op_device_context();
    args.alloc_attrs = ctx->output_alloc_attr(0);
    if (ctx->is_eager()) {
      // NOTE(fishx): Only set cancellation_manager in eager mode. Because in
      // Tensorflow 1.x, session (or graph_mgr) will abort the underlying
      // rendezvous if it encounters any error.
      args.cancellation_manager = ctx->cancellation_manager();
    }

    Rendezvous::ParsedKey parsed_key;
    GetSliceRendezvousKey(key_prefix_, "_transfer_data", frame_iter,
                          &parsed_key.buf_);
    VLOG(2) << "SliceRecv " << parsed_key.buf_;
    OP_REQUIRES_OK(ctx, Rendezvous::ParseKey(parsed_key.buf_, &parsed_key));
    Tensor data_t;
    OP_REQUIRES_OK(ctx, ctx->rendezvous()->Recv(parsed_key, args, &data_t,
                                                &is_dead, timeout_ms_));

    // This shouldn't be a dead tensor.
    CHECK_EQ(is_dead, false);
    ctx->set_output(0, data_t);
    return;
  }

  // recv shape.
  TensorShape shape;
  OP_REQUIRES_OK(ctx, RecvShape(ctx, frame_iter, shape));

  // recv data
  Tensor* output_t = nullptr;
  OP_REQUIRES_OK(ctx, ctx->allocate_output(0, shape, &output_t));
  if (dtype_ == DT_STRING) {
    OP_REQUIRES_OK(ctx, RecvString(ctx, frame_iter, shape, output_t));
  } else {
    OP_REQUIRES_OK(ctx, RecvBasicType(ctx, frame_iter, total_bytes, output_t));
  }
}

Status SliceRecvOp::RecvTotalBytes(OpKernelContext* ctx,
                                   const FrameAndIter& frame_iter,
                                   bool& is_dead, int64& total_bytes) {
  Rendezvous::Args args;
  args.device_context = ctx->op_device_context();
  args.alloc_attrs = AllocatorAttributes();
  if (ctx->is_eager()) {
    // NOTE(fishx): Only set cancellation_manager in eager mode. Because in
    // Tensorflow 1.x, session (or graph_mgr) will abort the underlying
    // rendezvous if it encounters any error.
    args.cancellation_manager = ctx->cancellation_manager();
  }

  Rendezvous::ParsedKey parsed_key;
  Tensor total_bytes_t;
  GetSliceRendezvousKey(key_prefix_, "_slice_transfer_totalbytes", frame_iter,
                        &parsed_key.buf_);
  VLOG(2) << "SliceRecv " << parsed_key.buf_;
  TF_RETURN_IF_ERROR(Rendezvous::ParseKey(parsed_key.buf_, &parsed_key));
  TF_RETURN_IF_ERROR(ctx->rendezvous()->Recv(parsed_key, args, &total_bytes_t,
                                             &is_dead, timeout_ms_));
  if (!is_dead) {
    total_bytes = total_bytes_t.scalar<int64>()();
  }

  return Status::OK();
}

Status SliceRecvOp::RecvShape(OpKernelContext* ctx,
                              const FrameAndIter& frame_iter,
                              TensorShape& shape) {
  Rendezvous::Args args;
  args.device_context = ctx->op_device_context();
  args.alloc_attrs = AllocatorAttributes();
  if (ctx->is_eager()) {
    // NOTE(fishx): Only set cancellation_manager in eager mode. Because in
    // Tensorflow 1.x, session (or graph_mgr) will abort the underlying
    // rendezvous if it encounters any error.
    args.cancellation_manager = ctx->cancellation_manager();
  }

  Rendezvous::ParsedKey parsed_key;
  GetSliceRendezvousKey(key_prefix_, "_slice_transfer_shape", frame_iter,
                        &parsed_key.buf_);
  VLOG(2) << "SliceRecv " << parsed_key.buf_;
  TF_RETURN_IF_ERROR(Rendezvous::ParseKey(parsed_key.buf_, &parsed_key));

  Tensor shape_t;
  bool is_dead;
  TF_RETURN_IF_ERROR(ctx->rendezvous()->Recv(parsed_key, args, &shape_t,
                                             &is_dead, timeout_ms_));
  // This shouldn't be a dead tensor.
  CHECK_EQ(is_dead, false);
  auto shape_vec = shape_t.vec<int64>();
  const int64 num_elements = shape_t.NumElements();
  for (int64 i = 0; i < num_elements; i++) {
    shape.AddDim(shape_vec(i));
  }

  return Status::OK();
}

Status SliceRecvOp::RecvString(OpKernelContext* ctx,
                               const FrameAndIter& frame_iter,
                               const TensorShape& shape, Tensor*& output_t) {
  Rendezvous::Args args;
  args.device_context = ctx->op_device_context();
  args.alloc_attrs = AllocatorAttributes();
  if (ctx->is_eager()) {
    // NOTE(fishx): Only set cancellation_manager in eager mode. Because in
    // Tensorflow 1.x, session (or graph_mgr) will abort the underlying
    // rendezvous if it encounters any error.
    args.cancellation_manager = ctx->cancellation_manager();
  }
  Rendezvous::ParsedKey parsed_key;
  bool is_dead;

  // recv elements size.
  GetSliceRendezvousKey(key_prefix_, "_slice_transfer_elements_size",
                        frame_iter, &parsed_key.buf_);
  VLOG(2) << "SliceRecv " << parsed_key.buf_;
  TF_RETURN_IF_ERROR(Rendezvous::ParseKey(parsed_key.buf_, &parsed_key));
  Tensor elements_size_t;
  TF_RETURN_IF_ERROR(ctx->rendezvous()->Recv(parsed_key, args, &elements_size_t,
                                             &is_dead, timeout_ms_));
  // This shouldn't be a dead tensor.
  CHECK_EQ(is_dead, false);
  auto elements_size_flat = elements_size_t.flat<int64>();
  int64 num_elements = shape.num_elements();
  args.alloc_attrs = ctx->output_alloc_attr(0);
  Tensor data_t;
  auto output_flat = output_t->flat<tstring>();
  for (int64 i = 0; i < num_elements; i++) {
    if (elements_size_flat(i) <= slice_size_) {
      std::string tensor_name_suffix = \
        strings::StrCat("_slice_transfer_data_", std::to_string(i));
      GetSliceRendezvousKey(key_prefix_, tensor_name_suffix, frame_iter,
                            &parsed_key.buf_);
      VLOG(2) << "SliceRecv " << parsed_key.buf_;
      TF_RETURN_IF_ERROR(Rendezvous::ParseKey(parsed_key.buf_, &parsed_key));
      TF_RETURN_IF_ERROR(ctx->rendezvous()->Recv(parsed_key, args, &data_t,
                                                 &is_dead, timeout_ms_));
      // This shouldn't be a dead tensor.
      CHECK_EQ(is_dead, false);
      output_flat(i) = data_t.scalar<tstring>()();
    } else {
      TF_RETURN_IF_ERROR(RecvStringSlice(ctx, frame_iter, i,
                                         elements_size_flat(i), output_flat));
    }
  }

  return Status::OK();
}

Status SliceRecvOp::RecvStringSlice(OpKernelContext* ctx,
                                    const FrameAndIter& frame_iter,
                                    const int64 index, const int64 element_size,
                                    TTypes<tstring>::Flat& output_flat) {
  Rendezvous::Args args;
  args.device_context = ctx->op_device_context();
  args.alloc_attrs = ctx->output_alloc_attr(0);
  if (ctx->is_eager()) {
    // NOTE(fishx): Only set cancellation_manager in eager mode. Because in
    // Tensorflow 1.x, session (or graph_mgr) will abort the underlying
    // rendezvous if it encounters any error.
    args.cancellation_manager = ctx->cancellation_manager();
  }
  Rendezvous::ParsedKey parsed_key;

  int64 slice_num = (element_size + slice_size_ - 1) / slice_size_;
  Tensor data_t;
  bool is_dead = false;
  for (int64 i = 0; i < slice_num; i++) {
    std::string tensor_name_suffix = \
      strings::StrCat("_slice_transfer_data_", std::to_string(index), "_",
                      std::to_string(i));
    GetSliceRendezvousKey(key_prefix_, tensor_name_suffix, frame_iter,
                          &parsed_key.buf_);
    VLOG(2) << "SliceRecv " << parsed_key.buf_;
    TF_RETURN_IF_ERROR(Rendezvous::ParseKey(parsed_key.buf_, &parsed_key));
    TF_RETURN_IF_ERROR(ctx->rendezvous()->Recv(parsed_key, args, &data_t,
                                               &is_dead, timeout_ms_));
    // This shouldn't be a dead tensor.
    CHECK_EQ(is_dead, false);
    output_flat(index) += data_t.scalar<tstring>()();
  }

  return Status::OK();
}

Status SliceRecvOp::RecvBasicType(OpKernelContext* ctx,
                                  const FrameAndIter& frame_iter,
                                  const int64 total_bytes,
                                  Tensor*& output_t) {
  Rendezvous::Args args;
  args.device_context = ctx->op_device_context();
  args.alloc_attrs = ctx->output_alloc_attr(0);
  if (ctx->is_eager()) {
    // NOTE(fishx): Only set cancellation_manager in eager mode. Because in
    // Tensorflow 1.x, session (or graph_mgr) will abort the underlying
    // rendezvous if it encounters any error.
    args.cancellation_manager = ctx->cancellation_manager();
  }
  Rendezvous::ParsedKey parsed_key;

  Tensor data_t;
  bool is_dead = false;
  int64 slice_num = (total_bytes + slice_size_ - 1) / slice_size_;
  unsigned char* output_base = \
    reinterpret_cast<unsigned char*>(output_t->data());
  for (int64 i = 0; i < slice_num; i++) {
    int64 start = i * slice_size_;
    int64 copy_size = slice_size_;
    if (start > total_bytes - slice_size_) {
      copy_size = total_bytes - start;
    }
    std::string tensor_name_suffix = \
      strings::StrCat("_slice_transfer_data_", std::to_string(i));
    GetSliceRendezvousKey(key_prefix_, tensor_name_suffix, frame_iter,
                          &parsed_key.buf_);
    VLOG(2) << "SliceSend " << parsed_key.buf_;
    TF_RETURN_IF_ERROR(Rendezvous::ParseKey(parsed_key.buf_, &parsed_key));
    TF_RETURN_IF_ERROR(ctx->rendezvous()->Recv(parsed_key, args, &data_t,
                                               &is_dead, timeout_ms_));
    // This shouldn't be a dead tensor.
    CHECK_EQ(is_dead, false);
    auto data_base = data_t.data();
    std::memcpy(output_base+start, data_base, copy_size);
  }

  return Status::OK();

}

REGISTER_KERNEL_BUILDER(Name("_SliceRecv").Device(DEVICE_CPU), SliceRecvOp);
REGISTER_KERNEL_BUILDER(Name("_SliceRecv").Device(DEVICE_DEFAULT), SliceRecvOp);

} // End of namespace tensorflow
