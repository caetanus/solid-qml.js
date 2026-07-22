#pragma once

#include <QQuickPaintedItem>

// A "press the keys" shortcut recorder (the GNOME/tilix keybinding row control). Shows the
// current sequence; while focused (click it) the next key chord becomes the new binding —
// captured as a portable QKeySequence string ("Ctrl+Shift+E"). Esc cancels, Backspace clears.
// Exposed as `SolidTerm.KeyRecorder`; the config pane binds `sequence` two-way to persisted keys.
class KeyRecorder : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString sequence READ sequence WRITE setSequence NOTIFY sequenceChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY styleChanged)
    Q_PROPERTY(QColor foreground READ foreground WRITE setForeground NOTIFY styleChanged)
    Q_PROPERTY(QColor accent READ accent WRITE setAccent NOTIFY styleChanged)
    Q_PROPERTY(QColor border READ border WRITE setBorder NOTIFY styleChanged)

public:
    explicit KeyRecorder(QQuickItem *parent = nullptr);

    QString sequence() const { return m_sequence; }
    void setSequence(const QString &v);
    bool recording() const { return m_recording; }
    QColor background() const { return m_background; }
    void setBackground(const QColor &c);
    QColor foreground() const { return m_foreground; }
    void setForeground(const QColor &c);
    QColor accent() const { return m_accent; }
    void setAccent(const QColor &c);
    QColor border() const { return m_border; }
    void setBorder(const QColor &c);

    void paint(QPainter *p) override;

signals:
    void sequenceChanged(const QString &sequence);  // user edit only (record/clear), not the WRITE binding
    void recordingChanged();
    void styleChanged();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    void setRecording(bool v);

    QString m_sequence;
    bool m_recording = false;
    QColor m_background = QColor("#1e1e1e");
    QColor m_foreground = QColor("#ffffff");
    QColor m_accent = QColor("#3584e4");
    QColor m_border = QColor("#151515");
};
