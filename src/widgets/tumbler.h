#pragma once

#include "qmlcss/cssfill.h"

#include <QPointer>
#include <QVariant>

// Tumbler (port of Tumbler.qml). The C++ wrapper owns the controlled `currentIndex` (RestoreNone
// Binding forwards root→control, onCurrentIndexChanged mirrors back so `__inputN.currentIndex`
// reads stay live) plus `model` and the focus/disabled cssState; the T.Tumbler with its minimal
// working ListView contentItem (SnapToItem + StrictlyEnforceRange, wrap:false — the exact
// non-wrap configuration the private TumblerView generates) and the displacement-driven delegate
// rides the original QML body as a `root`-bound snippet.
namespace SolidWidgets {

class Tumbler : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(QVariant model READ model WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(bool disabled READ disabled WRITE setDisabled NOTIFY disabledChanged)

public:
    explicit Tumbler(QQuickItem *parent = nullptr);

    QVariant model() const { return m_model; }
    void setModel(const QVariant &v);
    int currentIndex() const { return m_currentIndex; }
    void setCurrentIndex(int v);
    bool disabled() const { return m_disabled; }
    void setDisabled(bool v);

signals:
    void modelChanged();
    void currentIndexChanged();
    void disabledChanged();

protected:
    void componentComplete() override;

private:
    Q_SLOT void syncState();

    QVariant m_model;
    int m_currentIndex = 0;
    bool m_disabled = false;
    QPointer<QQuickItem> m_control;
};

} // namespace SolidWidgets
