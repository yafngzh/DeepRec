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

#ifndef TENSORFLOW_FRAMEWORK_RENDEZVOUS_H_
#define TENSORFLOW_FRAMEWORK_RENDEZVOUS_H_

#include <string>

#include "tensorflow/core/framework/cancellation.h"
#include "tensorflow/core/framework/control_flow.h"
#include "tensorflow/core/framework/device_base.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/lib/core/refcount.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/util/device_name_utils.h"

namespace tensorflow {

// A Rendezvous is an abstraction for passing tensors from producers
// to consumers. A rendezvous is a table of channels. Each channel is
// keyed by a rendezvous key. The key encodes a pair of <producer,
// consumer>, where the producer and the consumer are tensorflow
// devices.
//
// The producer calls the Send() method to send one tensor over one
// named channel. The consumer calls the Recv() method to receive one
// tensor from a named channel. A sequence of tensors can be passed
// from the producer to the consumer.  The consumer receives them in
// the order as the producer sends them.
//
// A consumer may safely request the tensor before or after it has
// been produced.  A consumer has the choice of making a blocking call
// or providing a callback: in either case, the consumer receives the
// Tensor as soon as it is available.  A producer never blocks.
class Rendezvous : public core::RefCounted {
 public:
  struct Args {
    DeviceContext* device_context = nullptr;
    AllocatorAttributes alloc_attrs;
    CancellationManager* cancellation_manager = nullptr;  // not owned.
  };

  // Constructs a rendezvous key for the tensor of "name" sent from
  // "src_device" to "dst_device". The tensor is generated in the frame
  // and iteration specified by "frame_iter".
  static string CreateKey(const string& src_device, uint64 src_incarnation,
                          const string& dst_device, const string& name,
                          const FrameAndIter& frame_iter);

  // Parses the key constructed by CreateKey and parse src/dst device
  // names into structures respectively.
  struct ParsedKey {
    StringPiece src_device;
    DeviceNameUtils::ParsedName src;
    uint64 src_incarnation = 0;
    StringPiece dst_device;
    DeviceNameUtils::ParsedName dst;
    StringPiece edge_name;

    ParsedKey() {}
    ParsedKey(const ParsedKey& b) { *this = b; }

    ParsedKey& operator=(const ParsedKey& b);
    StringPiece FullKey() const { return buf_; }

   private:
    friend class Rendezvous;
    friend class SendOp;
    friend class RecvOp;
    friend class FuseRecvOp;
    friend class SliceSendOp;
    friend class SliceRecvOp;
    friend class RefSendOp;
    friend class RefRecvOp;
    string buf_;
  };
  static Status ParseKey(StringPiece key, ParsedKey* out);

  // The caller is a tensor producer and it sends a message (a tensor
  // "val" and a bool "is_dead") under the given "key".
  //
  // {val, is_dead} is bundled as a message sent and received.
  // Typically, is_dead is set by some control flow nodes
  // (e.g., a not-taken branch).  args is passed by Send to the
  // Recv function to communicate any information that the Recv
  // function might need.  This is typically only necessary for
  // Send/Recv on the same worker.
  //
  // Send() never blocks.
  virtual Status Send(const ParsedKey& key, const Args& args, const Tensor& val,
                      const bool is_dead) = 0;

  // Send ref tensor
  virtual Status Send(const ParsedKey& key, const Args& args, Tensor* ref_val,
                      mutex* ref_mu, const bool is_dead) { return Status::OK(); }

  // Callback provided by a tensor consumer waiting on the rendezvous.
  // It will be invoked when the tensor is available, or when a non-OK
  // status arises in the production of that tensor.  It also gets
  // two Rendezvous::Args, one provided by the sender, the other by the
  // receiver, which may be needed when a non-CPU device is in use
  // by either side.
  typedef std::function<void(const Status&, const Args&, const Args&,
                             const Tensor&, const bool)>
      DoneCallback;

  typedef std::function<void(const Status&, const Args&, const Args&,
                             Tensor*, mutex*, const bool)>
      RefDoneCallback;

  virtual void RecvAsync(const ParsedKey& key, const Args& args,
                         DoneCallback done) = 0;

  // Recv ref tensor
  virtual void RecvAsync(const ParsedKey& key, const Args& args,
                         RefDoneCallback done) {};

  typedef std::function<void(const Status&, const std::vector<Args>&,
                             const Args&,
                             const std::vector<Tensor>&,
                             const std::vector<bool>&)>
      FuseDoneCallback;

  // Local rendezvous does not need this.
  virtual void FuseRecvAsync(const std::vector<ParsedKey>& parsed_keys,
                             const Args& args, FuseDoneCallback done) {}

  // Synchronous wrapper for RecvAsync.
  Status Recv(const ParsedKey& key, const Args& args, Tensor* val,
              bool* is_dead, int64 timeout_ms);
  Status Recv(const ParsedKey& key, const Args& args, Tensor* val,
              bool* is_dead);

  // Aborts all pending and future Send/Recv with the given "status".
  //
  // StartAbort() does not wait for ongoing calls to finish.
  // REQUIRES: !status.ok()
  virtual void StartAbort(const Status& status) = 0;

 protected:
  ~Rendezvous() override;
};

// Returns a Rendezvous instance that is limited to use only by
// producers and consumers in the local process.  The caller assumes
// ownership of one Ref() on the returned object.
Rendezvous* NewLocalRendezvous();

}  // end namespace tensorflow

#endif  // TENSORFLOW_FRAMEWORK_RENDEZVOUS_H_
