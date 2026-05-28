# Copyright 2026 Aananth C N
# Apache License, Version 2.0
#
# This file is included by the root CMakeLists only when Qt6 is found.
# CMAKE_CURRENT_SOURCE_DIR here = src/ui/, so resource PREFIX paths resolve
# relative to this directory — qrc:/VelanUI/main.qml, qrc:/VelanUI/SiriOrb.qml
#
# We use qt_add_resources (plain file embedding) rather than qt_add_qml_module
# to avoid the known Qt 6 qmlcachegen / AUTOMOC dependency cycle (QTBUG-103497).
# velan-ui has no registered C++ QML types, so we don't need the full module
# machinery — plain resource embedding is sufficient.

qt_add_executable(velan-ui
    main.cpp
    GrpcStateWatcher.cpp
)

set_target_properties(velan-ui PROPERTIES AUTOMOC ON)

# Embed QML at qrc:/VelanUI/<filename> — matches QUrl in main.cpp.
qt_add_resources(velan-ui "velan_qml"
    PREFIX "/VelanUI"
    FILES
        main.qml
        SiriOrb.qml
)

# velan_ui_proto and UI_PROTO_GEN_DIR are defined in the parent scope.
target_include_directories(velan-ui PRIVATE "${UI_PROTO_GEN_DIR}")
target_link_libraries(velan-ui PRIVATE
    Qt6::Core
    Qt6::Quick
    Qt6::QuickControls2
    velan_ui_proto
)

install(TARGETS velan-ui DESTINATION bin)
message(STATUS "velan-ui: building with Qt ${Qt6_VERSION}")
