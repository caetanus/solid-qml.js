#pragma once

#include <QtQuickTemplates2/private/qquicktabbutton_p.h>

// TabButton (port of TabButton.qml) — subclasses the PRIVATE QQuickTabButton because QQuickTabBar
// manages checked/currentIndex through qobject_cast to that exact type (owner-approved private
// dependency, same precedent as QZipReader). Background and contentItem are SIBLING slots, so
// ancestor-state scoping cannot reach the label through the background — both carry the same
// cssState (checked → "selected", hovered → "hover") via `root`-bound snippets. Only the checked
// tab is a tab stop (desktop model: a tab bar is ONE stop; arrows switch, handled by the C++
// TabBar's snippet); ClickFocus keeps a clicked tab focusable without stealing the tab chain.
namespace SolidWidgets {

class TabButton : public QQuickTabButton {
    Q_OBJECT

public:
    explicit TabButton(QQuickItem *parent = nullptr);

protected:
    void componentComplete() override;

private:
    Q_SLOT void syncTabstop();
    Q_SLOT void syncImplicit();

    QObject *m_tabstop = nullptr; // the loader's solidTabstop switch
};

} // namespace SolidWidgets
