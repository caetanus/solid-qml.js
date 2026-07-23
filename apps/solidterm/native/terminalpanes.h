#pragma once

#include <QQuickItem>
#include <QStringList>
#include <QVector>

class TerminalView;
class PaneHeader;

// The split container (owner: "split screen … parecido com tilix"). Splitting a terminal is
// terminal-domain layout that For→SplitView can't express through the transpiler subset, so it's
// native C++ here — a BINARY SPLIT TREE (like tilix): every internal node is a horizontal or
// vertical split of its children, leaves are terminals. A leaf split H then V then H nests
// arbitrarily, so Alt+D (split right) and Alt+Shift+D (split down) always do what they say,
// regardless of what came before — unlike the old flat single-orientation list, where the first
// split locked the orientation for the whole container.
//
// Logical tree only: every leaf cell and every divider is still a direct child of this item,
// positioned by a recursive layout walk — so the scene graph stays flat and animations/focus are
// unchanged.
class TerminalPanes : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY panesChanged)
    // Forwarded to every pane so preferences apply live across splits.
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY styleChanged)
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY styleChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY styleChanged)
    Q_PROPERTY(QColor foreground READ foreground WRITE setForeground NOTIFY styleChanged)
    Q_PROPERTY(int scrollbackLimit READ scrollbackLimit WRITE setScrollbackLimit NOTIFY styleChanged)
    Q_PROPERTY(QColor handleColor READ handleColor WRITE setHandleColor NOTIFY styleChanged)
    Q_PROPERTY(QStringList reservedSequences READ reservedSequences WRITE setReservedSequences NOTIFY reservedChanged)

public:
    explicit TerminalPanes(QQuickItem *parent = nullptr);
    ~TerminalPanes() override;

    int count() const;

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
    QStringList reservedSequences() const { return m_reserved; }
    void setReservedSequences(const QStringList &v);

    // Split the FOCUSED pane along `orient` (Qt::Horizontal = side by side / "split right",
    // Qt::Vertical = stacked / "split down"). Nests independently of prior splits.
    Q_INVOKABLE void split(int orient);
    Q_INVOKABLE void closeFocused();
    Q_INVOKABLE void focusNext();
    Q_INVOKABLE void focusPrev();
    // Paste/copy proxied to the focused pane (menu actions).
    Q_INVOKABLE void copyFocused();
    Q_INVOKABLE void pasteFocused();
    Q_INVOKABLE void clearFocused();

signals:
    void panesChanged();
    void styleChanged();
    void allClosed();           // last pane's shell exited → the app can quit/close the tab
    void titleChanged(const QString &title); // focused pane's OSC title
    void reservedChanged();
    void accelerator(const QString &sequence); // a reserved chord pressed in any pane

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void focusInEvent(QFocusEvent *event) override;

private:
    // A node in the split tree. Leaf: children empty, holds cell/view/header. Split: 2+ children
    // laid out along `orientation` by `fractions`, with `dividers` between them.
    struct Node {
        Node *parent = nullptr;
        int orientation = 1;            // Qt::Horizontal(1) / Qt::Vertical(2) for splits
        QVector<Node *> children;
        QVector<qreal> fractions;       // per-child share (sums to 1)
        qreal lastAvail = 1;            // main-axis px length from last layout (for divider drag px→fraction)
        QVector<class DividerItem *> dividers; // children.size()-1
        // Leaf payload:
        QQuickItem *cell = nullptr;
        TerminalView *view = nullptr;
        PaneHeader *header = nullptr;
        bool isLeaf() const { return children.isEmpty(); }
    };

    Node *makeLeaf();                   // compose a cell + wire it
    void wirePane(TerminalView *v, PaneHeader *header);
    void applyStyle(TerminalView *v);

    void layoutNode(Node *node, qreal x, qreal y, qreal w, qreal h);
    void rebuildDividers(Node *splitNode); // (re)create the divider items for a split node
    void collectLeaves(Node *node, QVector<Node *> &out) const;
    void removeLeaf(Node *leaf);
    void deleteSubtree(Node *node);     // frees dividers + cells recursively
    void setFocused(Node *leaf);
    Node *leafOfView(TerminalView *v) const;
    void refreshHeaderFocus();
    void relayout();

    Node *m_root = nullptr;
    Node *m_focused = nullptr;          // focused LEAF
    bool m_didInitialFocus = false;
    class QQmlComponent *m_cellComponent = nullptr;

    QString m_fontFamily = QStringLiteral("monospace");
    int m_fontSize = 15;
    QColor m_background = QColor("#1e1e1e");
    QColor m_foreground = QColor("#ffffff");
    int m_scrollbackLimit = 8000;
    QColor m_handleColor = QColor("#151515");
    QStringList m_reserved;
};
