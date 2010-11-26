/* Copyright (c) 2009 maidsafe.net limited
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.
    * Neither the name of the maidsafe.net limited nor the names of its
    contributors may be used to endorse or promote products derived from this
    software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <boost/tokenizer.hpp>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/message.h>

#include <typeinfo>
#include <vector>

#include "maidsafe/base/log.h"
#include "maidsafe/protobuf/transport_message.pb.h"
#include "maidsafe/protobuf/rpcmessage.pb.h"
#include "maidsafe/protobuf/testservices.pb.h"
#include "maidsafe/protobuf/kademlia_service_messages.pb.h"
#include "maidsafe/rpcprotocol/channel-api.h"
#include "maidsafe/rpcprotocol/channelimpl.h"
#include "maidsafe/rpcprotocol/channelmanager-api.h"
#include "maidsafe/rpcprotocol/channelmanagerimpl.h"
#include "maidsafe/rpcprotocol/rpcstructs.h"
#include "maidsafe/transport/transport.h"

namespace rpcprotocol {

void ControllerImpl::Reset() {
  timeout_ = -1;
  time_sent_ = 0;
  time_received_ = 0;
  rtt_ = 0.0;
  failure_.clear();
  transport_.reset();
}

ChannelImpl::ChannelImpl(
    boost::shared_ptr<ChannelManager> channel_manager)
    : channel_manager_(channel_manager),
    transport_(channel_manager_->transport()),
      service_(0), ip_(),
      rendezvous_ip_(), port_(0), rendezvous_port_(0),
      id_(0), local_transport_(false) {
  channel_manager_->AddChannelId(&id_);
}

ChannelImpl::ChannelImpl(boost::shared_ptr<ChannelManager> channel_manager,
                         const IP &ip, const Port &port,
                         const IP &rendezvous_ip, const Port &rendezvous_port)
    : channel_manager_(channel_manager),
      transport_(channel_manager_->transport()),
      service_(0), ip_(), rendezvous_ip_(),
      port_(port),
      rendezvous_port_(rendezvous_port), id_(0), local_transport_(true) {
  channel_manager_->AddChannelId(&id_);

  // To send we need ip in decimal dotted format
  if (ip.size() == 4) {
    ip_ = base::IpBytesToAscii(ip);
  } else {
    ip_ = ip;
  }

 
  if (rendezvous_ip.size() == 4) {
    rendezvous_ip_ = base::IpBytesToAscii(rendezvous_ip);
  } else {
    rendezvous_ip_ = rendezvous_ip;
  }
}



ChannelImpl::~ChannelImpl() {
  channel_manager_->RemoveChannelId(id_);
}

std::string ChannelImpl::GetServiceName(const std::string &full_name) {
  std::string service_name;
  try {
    boost::char_separator<char> sep(".");
    boost::tokenizer< boost::char_separator<char> > tok(full_name, sep);
    boost::tokenizer< boost::char_separator<char> >::iterator beg = tok.begin();
    int no_tokens = -1;
    while (beg != tok.end()) {
      ++beg;
      ++no_tokens;
    }
    beg = tok.begin();
    advance(beg, no_tokens - 1);
    service_name = *beg;
  } catch(const std::exception &e) {
    DLOG(ERROR) << "ChannelImpl::GetServiceName - full method name format: "
                << e.what() << std::endl;
  }
  return service_name;
}

void ChannelImpl::CallMethod(const google::protobuf::MethodDescriptor *method,
                             google::protobuf::RpcController *rpc_controller,
                             const google::protobuf::Message *request,
                             google::protobuf::Message *response,
                             google::protobuf::Closure *done) {
  if ((ip_.empty()) || (port_ == 0)) {
    DLOG(ERROR) << "ChannelImpl::CallMethod. No remote_ip or remote_port: "
                << method->full_name() << std::endl;
    done->Run();
    return;
  }

  // Wrap request in TransportMessage
  transport::TransportMessage transport_message;
  transport_message.set_type(transport::TransportMessage::kRequest);
  rpcprotocol::RpcMessage *rpc_message =
      transport_message.mutable_data()->mutable_rpc_message();
  rpc_message->set_method(method->name());
  rpc_message->set_service(GetServiceName(method->full_name()));

  // Get field descriptor for RPC payload
  const google::protobuf::FieldDescriptor *field_descriptor =
      request->GetDescriptor()->extension(0);
  if (!field_descriptor) {
    DLOG(ERROR) << "ChannelImpl::CallMethod - No such RPC extension exists: "
                << request->GetTypeName() << std::endl;
    done->Run();
    return;
  }

  // Get mutable payload field
  rpcprotocol::RpcMessage::Detail *rpc_message_detail =
      rpc_message->mutable_detail();

  // Copy payload into RpcMessage
  const google::protobuf::Message::Reflection *reflection =
      rpc_message_detail->GetReflection();
  google::protobuf::Message *mutable_message =
      reflection->MutableMessage(rpc_message_detail, field_descriptor);
  mutable_message->CopyFrom(*request);

  PendingMessage pending_request;
  pending_request.status = kAwaitingRequestSend;
  pending_request.args = response;
  pending_request.callback = done;
  pending_request.controller = static_cast<Controller*>(rpc_controller);
  pending_request.controller->set_method(method->name());
  if (local_transport_) {
    pending_request.controller->set_connection(transport_);
    pending_request.local_transport = true;
  }

  SocketId socket_id;
  // TODO (dirvine)
//   if (local_transport_)
//     socket_id = transport_->socket_id();
//   else
    socket_id = transport_->PrepareToSend(ip_, port_,
                                              rendezvous_ip_, rendezvous_port_);
  pending_request.controller->set_socket_id(socket_id);

  if (socket_id < 0 ||
      !channel_manager_->AddPendingRequest(socket_id, pending_request)) {
    DLOG(INFO) << "Failed to send the RPC request(" << rpc_message->method()
               << ") - " << socket_id << " to " << ip_ << ":"
               << port_ << std::endl;
    done->Run();
    return;
  }
  rpc_message->set_rpc_id(socket_id);
// TODO (dirvine)
//   if (local_transport_) {
//     transport_->Send(transport_message,
//                           pending_request.controller->timeout());
//  // } else {
    transport_->Send(transport_message, socket_id,
                         pending_request.controller->timeout());
  //}

//  DLOG(INFO) << "Sent RPC request(" << rpc_message->method() << ") - "
//             << socket_id << " to " << remote_ip_ << ":" << remote_port_
//             << std::endl;
}

void ChannelImpl::HandleRequest(const rpcprotocol::RpcMessage &rpc_message,
                                const SocketId &socket_id,
                                const float &rtt) {
  if (!service_) {
    DLOG(ERROR) << "ChannelImpl::HandleRequest - no service." << std::endl;
    return;
  }
  if (!rpc_message.IsInitialized()) {
    DLOG(ERROR) << "ChannelImpl::HandleRequest - uninitialised." << std::endl;
    return;
  }
  if (!rpc_message.has_method()) {
    DLOG(ERROR) << "ChannelImpl::HandleRequest - no method." << std::endl;
    return;
  }

//  DLOG(ERROR) << "ChannelImpl::HandleRequest - " << rpc_message.method()
//              << std::endl;
  const google::protobuf::MethodDescriptor* method =
      service_->GetDescriptor()->FindMethodByName(rpc_message.method());
  google::protobuf::Message *response  =
      service_->GetResponsePrototype(method).New();

  // Extract the optional field which is the actual RPC payload.  The field must
  // be a proto message itself and is an extension.
  std::vector<const google::protobuf::FieldDescriptor*> field_descriptors;
  rpc_message.detail().GetReflection()->ListFields(rpc_message.detail(),
                                                   &field_descriptors);

  // Check only one field exists
  if (field_descriptors.size() != size_t(1) || !field_descriptors.at(0) ||
      field_descriptors.at(0)->type() !=
          google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
    DLOG(ERROR) << "ChannelImpl::HandleRequest - invalid request." << std::endl;
    return;
  }

  // Copy the payload to a new message (DescriptorProto inherits from Message)
  const google::protobuf::Message &args =
      rpc_message.detail().GetReflection()->GetMessage(rpc_message.detail(),
                                                       field_descriptors.at(0));

  boost::shared_ptr<Controller> controller(new Controller);
  controller->set_rtt(rtt);
  controller->set_socket_id(socket_id);
  google::protobuf::Closure *done =
      google::protobuf::NewCallback<ChannelImpl,
                                    const google::protobuf::Message*,
                                    boost::shared_ptr<Controller> >
      (this, &ChannelImpl::SendResponse, response, controller);
  service_->CallMethod(method, controller.get(), &args, response, done);
}

void ChannelImpl::SendResponse(const google::protobuf::Message *response,
                               boost::shared_ptr<Controller> controller) {
  if (!response->IsInitialized()) {
    DLOG(ERROR) << "ChannelImpl::SendResponse - response uninitialised - "
                << controller->Failed() << "." << std::endl;
    return;
  }

  // Wrap request in TransportMessage
  transport::TransportMessage transport_message;
  transport_message.set_type(transport::TransportMessage::kResponse);
  rpcprotocol::RpcMessage *rpc_message =
      transport_message.mutable_data()->mutable_rpc_message();
  rpc_message->set_rpc_id(controller->socket_id());
  rpc_message->set_method(controller->method());
  rpc_message->set_service("done");

  // Get field descriptor for RPC payload
  const google::protobuf::FieldDescriptor *field_descriptor =
      response->GetDescriptor()->extension(0);
  if (!field_descriptor) {
    DLOG(ERROR) << "ChannelImpl::CallMethod - No such RPC extension exists."
                << std::endl;
    return;
  }

  // Get mutable payload field
  rpcprotocol::RpcMessage::Detail *rpc_message_detail =
      rpc_message->mutable_detail();
  // Copy payload into RpcMessage
  google::protobuf::Message *mutable_message =
      rpc_message_detail->GetReflection()->MutableMessage(
          rpc_message_detail, field_descriptor);
  try {
    mutable_message->CopyFrom(*response);
  }
  catch(const std::exception &e) {
    DLOG(ERROR) << "ChannelImpl::CallMethod - Error merging message:"
                << e.what() << std::endl;
    delete response;
    return;
  }

  transport_->Send(transport_message, controller->socket_id(), 0);
  delete response;
//  DLOG(ERROR) << "ChannelImpl::CallMethod - Response sent - "
//              << controller->socket_id() << std::endl;
}

}  // namespace rpcprotocol
