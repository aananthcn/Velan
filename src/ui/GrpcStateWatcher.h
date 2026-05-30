// Copyright 2026 Aananth C N
// Apache License, Version 2.0

#pragma once

#include "velan_ui.grpc.pb.h"
#include "velan_ui.pb.h"

#include <grpcpp/grpcpp.h>

#include <QObject>
#include <QString>

#include <atomic>
#include <thread>


// ---------------------------------------------------------------------------
// GrpcStateWatcher — lives on the QML/main thread; streams StateUpdates from
// the Velan gRPC server on a background std::thread.
//
// Signals are emitted via QMetaObject::invokeMethod(Qt::QueuedConnection) so
// they are always delivered on the main thread — safe for QML Connections.
//
// Reconnects automatically if the connection drops (Velan not yet started,
// network blip, etc.) — retries every 2 seconds.
// ---------------------------------------------------------------------------
class GrpcStateWatcher : public QObject {
    Q_OBJECT

public:
    explicit GrpcStateWatcher(const QString& server_addr, QObject* parent = nullptr);
    ~GrpcStateWatcher() override;

    // Start the background watcher thread. Call once after construction.
    void start();

    // Stop the watcher and join the background thread.
    void stop();

signals:
    // Emitted on every StateUpdate received from Velan.
    // state maps to velan::VoiceAssistantState (0=IDLE … 4=TALKING).
    void stateChanged(int state, const QString& transcript, const QString& response);

    // Emitted when the gRPC connection is established or lost.
    void connectedChanged(bool connected);

private:
    void run();   // background thread body

    QString            server_addr_;
    std::thread        worker_;
    std::atomic<bool>  stop_flag_{false};
};
