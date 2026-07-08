#pragma once

#include "qmlcss/cssfill.h"

// Progress + BusyIndicator (ports of Progress.qml / BusyIndicator.qml). The C++ class carries
// the property surface and state; the visual internals (track/bar, the spoke ring) compose from
// the original QML bodies as cached snippets bound to `root` (see snippetwidget.h).
namespace SolidWidgets {

// Progress — <progress>: value < 0 (the default) means indeterminate (a 30% segment sliding).
class Progress : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(qreal value READ value WRITE setValue NOTIFY valueChanged)
    Q_PROPERTY(qreal max READ max WRITE setMax NOTIFY maxChanged)

public:
    explicit Progress(QQuickItem *parent = nullptr);

    qreal value() const { return m_value; }
    void setValue(qreal v);
    qreal max() const { return m_max; }
    void setMax(qreal v);

signals:
    void valueChanged();
    void maxChanged();

protected:
    void componentComplete() override;

private:
    void syncState();

    qreal m_value = -1;
    qreal m_max = 1;
};

// BusyIndicator — the eight-spoke spinner; `running` gates the rotation (idle costs no frames).
class BusyIndicator : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)

public:
    explicit BusyIndicator(QQuickItem *parent = nullptr);

    bool running() const { return m_running; }
    void setRunning(bool v);

signals:
    void runningChanged();

protected:
    void componentComplete() override;

private:
    bool m_running = true;
};

} // namespace SolidWidgets
