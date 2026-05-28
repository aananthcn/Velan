// Copyright 2026 Aananth C N
// Apache License, Version 2.0

#include "GrpcStateWatcher.h"

#include <QDebug>
#include <QMetaObject>

#include <chrono>
#include <thread>


GrpcStateWatcher::GrpcStateWatcher(const QString& server_addr, QObject* parent)
    : QObject(parent), server_addr_(server_addr) {}

GrpcStateWatcher::~GrpcStateWatcher() {
    stop();
}

void GrpcStateWatcher::start() {
    // GrpcStateWatcher itself stays on the main/QML thread.
    // The blocking gRPC stream is driven on a plain std::thread.
    worker_ = std::thread([this]() { run(); });
}

void GrpcStateWatcher::stop() {
    stop_flag_ = true;
    if (worker_.joinable()) worker_.join();
}

// ---------------------------------------------------------------------------
// run() — background thread: connect, stream, reconnect on drop.
// All signal emissions go through QMetaObject::invokeMethod with
// Qt::QueuedConnection so they are delivered on the main thread.
// ---------------------------------------------------------------------------
void GrpcStateWatcher::run() {
    const std::string addr = server_addr_.toStdString();

    auto emit_state = [this](int s, const QString& t, const QString& r) {
        QMetaObject::invokeMethod(this,
            [this, s, t, r]() { emit stateChanged(s, t, r); },
            Qt::QueuedConnection);
    };
    auto emit_connected = [this](bool c) {
        QMetaObject::invokeMethod(this,
            [this, c]() { emit connectedChanged(c); },
            Qt::QueuedConnection);
    };

    while (!stop_flag_.load()) {
        auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
        auto stub    = velan::VelanUIService::NewStub(channel);

        grpc::ClientContext ctx;
        velan::WatchRequest req;
        req.set_client_id("velan-ui");

        auto reader = stub->WatchState(&ctx, req);

        velan::StateUpdate upd;
        bool was_connected = false;

        while (!stop_flag_.load() && reader->Read(&upd)) {
            if (!was_connected) {
                was_connected = true;
                emit_connected(true);
                qDebug() << "[velan-ui] Connected to" << server_addr_;
            }
            emit_state(
                static_cast<int>(upd.state()),
                QString::fromStdString(upd.transcript()),
                QString::fromStdString(upd.response())
            );
        }

        if (was_connected) {
            emit_connected(false);
            qDebug() << "[velan-ui] Disconnected — retrying in 2 s";
        }

        ctx.TryCancel();

        // Wait 2 s before reconnect (100 ms sleep × 20, check stop_flag each).
        for (int i = 0; i < 20 && !stop_flag_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
