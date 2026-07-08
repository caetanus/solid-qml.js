#pragma once

#include <QQmlEngine>

// C++ widget layer for module `solidqml.Widgets 1.0` — the pre-AOT port of the QML components
// (plan: docs/superpowers/plans/2026-07-07-widgets-to-cpp.md). Composition translation, one
// class per former .qml, same type names/property surfaces: the transpiler emit is the contract
// and never changes. C++ registrations coexist with the remaining qmldir composite types under
// the same URI while the port progresses.
namespace SolidWidgets {

void registerTypes();

} // namespace SolidWidgets
