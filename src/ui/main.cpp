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

#include "GrpcStateWatcher.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QCommandLineParser>
#include <QDebug>


int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("velan-ui");
    app.setApplicationVersion("1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("Velan voice assistant UI");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption serverOpt(
        {"s", "server"},
        "Velan gRPC UI server address (default: localhost:50052)",
        "host:port", "localhost:50052");
    parser.addOption(serverOpt);
    parser.process(app);

    const QString serverAddr = parser.value(serverOpt);
    qDebug() << "[velan-ui] Connecting to Velan at" << serverAddr;

    GrpcStateWatcher watcher(serverAddr);
    watcher.start();

    QQmlApplicationEngine engine;
    // Expose the watcher to QML so SiriOrb can connect to stateChanged.
    engine.rootContext()->setContextProperty("stateWatcher", &watcher);
    // Resource path: RESOURCE_PREFIX "/" + URI "VelanUI" + filename
    // Matches qt_add_qml_module(URI VelanUI, RESOURCE_PREFIX "/") in CMakeLists.txt
    engine.load(QUrl(QStringLiteral("qrc:/VelanUI/main.qml")));

    if (engine.rootObjects().isEmpty()) return -1;

    int rc = app.exec();
    watcher.stop();
    return rc;
}
