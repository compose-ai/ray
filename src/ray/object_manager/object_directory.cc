// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/object_manager/object_directory.h"

#include "ray/stats/stats.h"

namespace ray {

ObjectDirectory::ObjectDirectory(instrumented_io_context &io_service,
                                 std::shared_ptr<gcs::GcsClient> &gcs_client)
    : io_service_(io_service), gcs_client_(gcs_client) {}

namespace {

using ray::rpc::GcsChangeMode;
using ray::rpc::GcsNodeInfo;
using ray::rpc::ObjectTableData;

/// Process a notification of the object table entries and store the result in
/// node_ids. This assumes that node_ids already contains the result of the
/// object table entries up to but not including this notification.
bool UpdateObjectLocations(const std::vector<rpc::ObjectLocationChange> &location_updates,
                           std::shared_ptr<gcs::GcsClient> gcs_client,
                           std::unordered_set<NodeID> *node_ids, std::string *spilled_url,
                           NodeID *spilled_node_id, size_t *object_size) {
  // location_updates contains the updates of locations of the object.
  // with GcsChangeMode, we can determine whether the update mode is
  // addition or deletion.
  bool isUpdated = false;
  for (const auto &update : location_updates) {
    // The size can be 0 if the update was a deletion. This assumes that an
    // object's size is always greater than 0.
    // TODO(swang): If that's not the case, we should use a flag to check
    // whether the size is set instead.
    if (update.size() > 0) {
      *object_size = update.size();
    }

    if (!update.node_id().empty()) {
      NodeID node_id = NodeID::FromBinary(update.node_id());
      if (update.is_add() && 0 == node_ids->count(node_id)) {
        node_ids->insert(node_id);
        isUpdated = true;
      } else if (!update.is_add() && 1 == node_ids->count(node_id)) {
        node_ids->erase(node_id);
        isUpdated = true;
      }
    } else {
      RAY_CHECK(!update.spilled_url().empty());
      const auto received_spilled_node_id = NodeID::FromBinary(update.spilled_node_id());
      RAY_LOG(DEBUG) << "Received object spilled at " << update.spilled_url()
                     << " spilled at " << NodeID::FromBinary(update.spilled_node_id());
      if (update.spilled_url() != *spilled_url) {
        *spilled_url = update.spilled_url();
        *spilled_node_id = received_spilled_node_id;
        isUpdated = true;
      }
    }
  }
  // Filter out the removed nodes from the object locations.
  for (auto it = node_ids->begin(); it != node_ids->end();) {
    if (gcs_client->Nodes().IsRemoved(*it)) {
      it = node_ids->erase(it);
    } else {
      it++;
    }
  }

  return isUpdated;
}

}  // namespace

void ObjectDirectory::LookupRemoteConnectionInfo(
    RemoteConnectionInfo &connection_info) const {
  auto node_info = gcs_client_->Nodes().Get(connection_info.node_id);
  if (node_info) {
    NodeID result_node_id = NodeID::FromBinary(node_info->node_id());
    RAY_CHECK(result_node_id == connection_info.node_id);
    connection_info.ip = node_info->node_manager_address();
    connection_info.port = static_cast<uint16_t>(node_info->object_manager_port());
  }
}

std::vector<RemoteConnectionInfo> ObjectDirectory::LookupAllRemoteConnections() const {
  std::vector<RemoteConnectionInfo> remote_connections;
  const auto &node_map = gcs_client_->Nodes().GetAll();
  for (const auto &item : node_map) {
    RemoteConnectionInfo info(item.first);
    LookupRemoteConnectionInfo(info);
    if (info.Connected() && info.node_id != gcs_client_->Nodes().GetSelfId()) {
      remote_connections.push_back(info);
    }
  }
  return remote_connections;
}

void ObjectDirectory::HandleNodeRemoved(const NodeID &node_id) {
  for (auto &listener : listeners_) {
    const ObjectID &object_id = listener.first;
    if (listener.second.current_object_locations.count(node_id) > 0) {
      // If the subscribed object has the removed node as a location, update
      // its locations with an empty update so that the location will be removed.
      UpdateObjectLocations({}, gcs_client_, &listener.second.current_object_locations,
                            &listener.second.spilled_url,
                            &listener.second.spilled_node_id,
                            &listener.second.object_size);
      // Re-call all the subscribed callbacks for the object, since its
      // locations have changed.
      for (const auto &callback_pair : listener.second.callbacks) {
        // It is safe to call the callback directly since this is already running
        // in the subscription callback stack.
        callback_pair.second(object_id, listener.second.current_object_locations,
                             listener.second.spilled_url, listener.second.spilled_node_id,
                             listener.second.object_size);
      }
    }
  }
}

}  // namespace ray
