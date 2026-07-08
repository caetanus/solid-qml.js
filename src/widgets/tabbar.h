#pragma once

#include "containerbase.h"

// TabBar (port of TabBar.qml). The C++ wrapper owns the controlled `currentIndex` (RestoreNone
// Binding forwards root→control, onCurrentIndexChanged mirrors back) and forwards its DEFAULT
// property into the control's contentData (SlotContainer). The T.TabBar with the Basic-style
// horizontal ListView contentItem, the stretch-to-CSS contentHeight and the wrap-around arrow
// stepping rides the original QML body as a `root`-bound snippet.
//
// NOTE: TabButton stays a .qml composite — its ROOT is a T.TabButton, and QQuickTabBar manages
// checked/currentIndex through qobject_cast<QQuickTabButton*> (a PRIVATE Qt class). Subclassing
// it means a QtQuickTemplates2-private dependency — owner's call, parked.
namespace SolidWidgets {

class TabBar : public SlotContainer {
    Q_OBJECT
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)

public:
    explicit TabBar(QQuickItem *parent = nullptr);

    int currentIndex() const { return m_currentIndex; }
    void setCurrentIndex(int v);

signals:
    void currentIndexChanged();

protected:
    void componentComplete() override;

private:
    int m_currentIndex = 0;
};

} // namespace SolidWidgets
