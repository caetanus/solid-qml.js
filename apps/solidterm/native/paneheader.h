#pragma once

#include <QQuickPaintedItem>

// The per-pane title strip (owner: "cada split precisa de seu proprio title"). tilix draws a
// small header on each pane with its OSC title and a focused indicator; this is that. Clicking it
// focuses the pane (signal to TerminalPanes). Painted natively — a thin bar, accent underline on
// the focused pane.
class PaneHeader : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY titleChanged)
    Q_PROPERTY(bool focused READ focused WRITE setFocused NOTIFY focusedChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY styleChanged)
    Q_PROPERTY(QColor foreground READ foreground WRITE setForeground NOTIFY styleChanged)
    Q_PROPERTY(QColor accent READ accent WRITE setAccent NOTIFY styleChanged)

public:
    explicit PaneHeader(QQuickItem *parent = nullptr);

    QString title() const { return m_title; }
    void setTitle(const QString &v);
    bool focused() const { return m_focused; }
    void setFocused(bool v);
    QColor background() const { return m_background; }
    void setBackground(const QColor &v);
    QColor foreground() const { return m_foreground; }
    void setForeground(const QColor &v);
    QColor accent() const { return m_accent; }
    void setAccent(const QColor &v);

    void paint(QPainter *p) override;

signals:
    void titleChanged();
    void focusedChanged();
    void styleChanged();
    void clicked();
    void closeRequested();

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    QRectF closeRect() const;

    QString m_title;
    bool m_focused = false;
    QColor m_background = QColor("#2a2a2a");
    QColor m_foreground = QColor("#cccccc");
    QColor m_accent = QColor("#3584e4");
};
