#pragma once

#include <QQuickItem>
#include <QVector>

class TerminalView;

// The split container (owner: "split screen … parecido com tilix"). Splitting a terminal into
// side-by-side / stacked panes is terminal-domain layout, and For→SplitView doesn't work through
// the transpiler subset (our SplitView doesn't adopt Repeater items), so the pane management is
// native C++ here — created/destroyed TerminalViews, fraction layout, draggable dividers, focus
// cycling, auto-close on shell exit. Exposed to the solid TSX as `SolidTerm.TerminalPanes` and
// driven by keyboard shortcuts (Ctrl+Shift+E/O split, Ctrl+Shift+W close, Alt+arrows focus).
//
// v1 is a single-level N-way split with one orientation (set by the first split); nested trees
// (mixed h/v like tilix's arbitrary layout) are the follow-up.
class TerminalPanes : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(int orientation READ orientation WRITE setOrientation NOTIFY orientationChanged)
    Q_PROPERTY(int count READ count NOTIFY panesChanged)
    // Forwarded to every pane so preferences apply live across splits.
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY styleChanged)
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY styleChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY styleChanged)
    Q_PROPERTY(QColor foreground READ foreground WRITE setForeground NOTIFY styleChanged)
    Q_PROPERTY(int scrollbackLimit READ scrollbackLimit WRITE setScrollbackLimit NOTIFY styleChanged)
    Q_PROPERTY(QColor handleColor READ handleColor WRITE setHandleColor NOTIFY styleChanged)

public:
    explicit TerminalPanes(QQuickItem *parent = nullptr);

    int orientation() const { return m_orientation; }
    void setOrientation(int v);
    int count() const { return m_panes.size(); }

    QString fontFamily() const { return m_fontFamily; }
    void setFontFamily(const QString &v);
    int fontSize() const { return m_fontSize; }
    void setFontSize(int v);
    QColor background() const { return m_background; }
    void setBackground(const QColor &v);
    QColor foreground() const { return m_foreground; }
    void setForeground(const QColor &v);
    int scrollbackLimit() const { return m_scrollbackLimit; }
    void setScrollbackLimit(int v);
    QColor handleColor() const { return m_handleColor; }
    void setHandleColor(const QColor &v);

    // Split the FOCUSED pane along `orient` (Qt::Horizontal = side by side, Qt::Vertical =
    // stacked). The first split fixes the container orientation for v1.
    Q_INVOKABLE void split(int orient);
    Q_INVOKABLE void closeFocused();
    Q_INVOKABLE void focusNext();
    Q_INVOKABLE void focusPrev();
    // Paste/copy proxied to the focused pane (menu actions).
    Q_INVOKABLE void copyFocused();
    Q_INVOKABLE void pasteFocused();
    Q_INVOKABLE void clearFocused();

signals:
    void orientationChanged();
    void panesChanged();
    void styleChanged();
    void allClosed();           // last pane's shell exited → the app can quit/close the tab
    void titleChanged(const QString &title); // focused pane's OSC title

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    struct Handle;
    TerminalView *makePane();
    void addPaneAfterFocused();
    void removePane(int index);
    void relayout();
    void applyStyle(TerminalView *v);
    void setFocusedIndex(int i);
    int focusedIndex() const;

    QVector<TerminalView *> m_panes;
    QVector<qreal> m_fractions;     // per-pane share of the main axis (sums to 1)
    QVector<QQuickItem *> m_handles;
    int m_orientation = Qt::Horizontal;
    int m_focused = 0;

    QString m_fontFamily = QStringLiteral("monospace");
    int m_fontSize = 15;
    QColor m_background = QColor("#1e1e1e");
    QColor m_foreground = QColor("#ffffff");
    int m_scrollbackLimit = 8000;
    QColor m_handleColor = QColor("#151515");
};
