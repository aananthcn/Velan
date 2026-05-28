// Copyright 2026 Aananth C N
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

#pragma once

#include "velan_ui.grpc.pb.h"
#include "velan_ui.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


// ---------------------------------------------------------------------------
// UIServer — gRPC server that streams AssistantState updates to connected
// velan-ui clients.
//
// Usage:
//   auto ui = std::make_unique<UIServer>("0.0.0.0:50052");
//   ui->start();
//   ...
//   ui->notify(velan::LISTENING);
//   ui->notify(velan::TALKING, transcript, response);
//   ...
//   ui->stop();   // or let destructor handle it
//
// If no client is connected, notify() is a no-op — zero impact on the voice
// pipeline. Thread-safe: notify() may be called from any thread.
// ---------------------------------------------------------------------------
class UIServer final : public velan::VelanUIService::Service {
public:
    explicit UIServer(const std::string& listen_addr);
    ~UIServer();

    // Start the gRPC server on a background thread.
    void start();

    // Stop the server and join the server thread.
    void stop();

    // Broadcast a state change to all connected streaming clients.
    // transcript is set when state == THINKING; response when state == TALKING.
    void notify(velan::AssistantState state,
                const std::string& transcript = "",
                const std::string& response   = "");

private:
    // gRPC server-side streaming RPC implementation.
    grpc::Status WatchState(grpc::ServerContext*                    ctx,
                            const velan::WatchRequest*              req,
                            grpc::ServerWriter<velan::StateUpdate>* writer) override;

    // One entry per connected streaming client.
    struct ClientSlot {
        grpc::ServerWriter<velan::StateUpdate>* writer;
        bool                                    dead = false;
    };

    std::string                 listen_addr_;
    std::unique_ptr<grpc::Server> server_;
    std::thread                 server_thread_;

    std::mutex                  clients_mu_;
    std::vector<ClientSlot>     clients_;    // protected by clients_mu_

    std::atomic<bool>           running_{false};
};
