#pragma once

#include "qmlcss/cssrect.h"

#include <QPointer>
#include <QQuickWindow>
#include <QTimer>

// Tabstop — the keyboard tab-focus ring (port of Tabstop.qml). Drop one into any Window (the
// app root AND each dialog Window) with `window:` set; it follows the window's activeFocusItem
// and frames whatever holds keyboard focus. Styled via the `::tab-stop` pseudo-element
// (cssPart), thin dotted by default. No mouse interaction — clicks pass through.
//
// The 16ms frame timer tracks the focused item's screen rect (mapToItem isn't reactive to
// scroll on its own) — same cadence the QML had, now without a Timer object per ring.
namespace SolidWidgets {

class Tabstop : public QmlCss::CssRect {
    Q_OBJECT
    Q_PROPERTY(QQuickWindow *window READ window WRITE setWindow NOTIFY windowChanged)

public:
    explicit Tabstop(QQuickItem *parent = nullptr);

    QQuickWindow *window() const { return m_window; }
    void setWindow(QQuickWindow *w);

signals:
    void windowChanged();

protected:
    void componentComplete() override;

private:
    Q_SLOT void refreshVisible();
    void track();

    QPointer<QQuickWindow> m_window;
    QObject *m_tabstop = nullptr; // the loader's solidTabstop context property
    QTimer m_timer;
};

} // namespace SolidWidgets
