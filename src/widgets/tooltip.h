#pragma once

#include <QQuickItem>
#include <QString>

namespace SolidWidgets {

// The HTML `title` attribute: a hover tooltip on the element that carries it. The transpiler
// instantiates W.ToolTip as a child of the titled element, with `shown` bound to that element's
// hover. Behaviour (show delay, popup, positioning) is QtQuick.Templates' T.ToolTip; the look is
// CSS — `.tooltip` (the box) and `.tooltip-text`, re-anchored at the titled element so scoped
// rules (`.toolbar .tooltip`) match. A plain QQuickItem, NOT a CSS box: it takes no part in the
// layout of the element it annotates.
class ToolTip : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(bool shown READ shown WRITE setShown NOTIFY shownChanged)

public:
    explicit ToolTip(QQuickItem *parent = nullptr);

    QString text() const { return m_text; }
    void setText(const QString &v);
    bool shown() const { return m_shown; }
    void setShown(bool v);

signals:
    void textChanged();
    void shownChanged();

protected:
    void componentComplete() override;

private:
    QString m_text;
    bool m_shown = false;
};

} // namespace SolidWidgets
