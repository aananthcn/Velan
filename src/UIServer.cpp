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

#include "UIServer.h"
#include "log.h"

#include <chrono>
#include <iostream>

// ---------------------------------------------------------------------------

UIServer::UIServer(const std::string& listen_addr)
    : listen_addr_(listen_addr) {}

UIServer::~UIServer() {
    stop();
}

void UIServer::start() {
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_addr_, grpc::InsecureServerCredentials());
    builder.RegisterService(this);
    server_ = builder.BuildAndStart();
    if (!server_) {
        std::cerr << "[UI] Failed to start gRPC UI server on " << listen_addr_ << "\n";
        return;
    }
    running_ = true;
    std::cout << log_ts() << "[UI] gRPC UI server listening on " << listen_addr_ << "\n";
    server_thread_ = std::thread([this]() { server_->Wait(); });
}

void UIServer::stop() {
    if (!running_.exchange(false)) return;
    if (server_) server_->Shutdown();
    if (server_thread_.joinable()) server_thread_.join();
}

// ---------------------------------------------------------------------------
// WatchState — called once per connecting client; blocks until disconnect.
// The client writer is registered in clients_ so notify() can push to it.
// ---------------------------------------------------------------------------
grpc::Status UIServer::WatchState(grpc::ServerContext*                    ctx,
                                   const velan::WatchRequest*              req,
                                   grpc::ServerWriter<velan::StateUpdate>* writer)
{
    const std::string id = req->client_id().empty() ? ctx->peer() : req->client_id();
    std::cout << log_ts() << "[UI] Client connected: " << id << "\n";

    {
        std::lock_guard<std::mutex> lk(clients_mu_);
        clients_.push_back({writer, false});
    }

    // Send an immediate IDLE update so the client knows the server is live.
    velan::StateUpdate init;
    init.set_state(velan::IDLE);
    init.set_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    writer->Write(init);

    // Block here — notify() will Write() through the stored pointer.
    // The RPC ends when the client disconnects (ctx->IsCancelled()).
    while (!ctx->IsCancelled() && running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Mark this slot dead so notify() skips it on the next broadcast.
    {
        std::lock_guard<std::mutex> lk(clients_mu_);
        for (auto& slot : clients_) {
            if (slot.writer == writer) { slot.dead = true; break; }
        }
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(),
                           [](const ClientSlot& s){ return s.dead; }),
            clients_.end());
    }

    std::cout << log_ts() << "[UI] Client disconnected: " << id << "\n";
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// notify — broadcast state update to all live clients.
// Called from the voice pipeline (any thread). Non-blocking for the caller.
// ---------------------------------------------------------------------------
void UIServer::notify(velan::VoiceAssistantState state,
                       const std::string& transcript,
                       const std::string& response)
{
    std::lock_guard<std::mutex> lk(clients_mu_);
    if (clients_.empty()) return;

    velan::StateUpdate upd;
    upd.set_state(state);
    upd.set_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (!transcript.empty()) upd.set_transcript(transcript);
    if (!response.empty())   upd.set_response(response);

    for (auto& slot : clients_) {
        if (!slot.dead) {
            if (!slot.writer->Write(upd))
                slot.dead = true;   // will be cleaned up on next WatchState exit
        }
    }
}
