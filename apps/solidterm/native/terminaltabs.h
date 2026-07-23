#pragma once

#include <QColor>
#include <QQuickItem>
#include <QStringList>
#include <QVector>

class TerminalPanes;

// Tabs (owner's queue: "TABS … manter panes vivos por tab"). Each tab is a full split tree
// (TerminalPanes). Switching tabs must NOT destroy the others' ptys, so tabs are kept alive
// natively here (a reactive <For> in the TSX would recreate — and kill — them). This item owns a
// native tab strip on top and a stack of TerminalPanes below; only the active tab is visible.
//
// It presents the SAME surface as TerminalPanes (style props, reservedSequences, split/close/focus,
// accelerator) proxied to the active tab, so the TSX swaps <TerminalPanes> for <TerminalTabs> with
// no other change; plus newTab/closeTab and a Ctrl+Shift+T accelerator.
class TerminalTabs : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY styleChanged)
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY styleChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY styleChanged)
    Q_PROPERTY(QColor foreground READ foreground WRITE setForeground NOTIFY styleChanged)
    Q_PROPERTY(int scrollbackLimit READ scrollbackLimit WRITE setScrollbackLimit NOTIFY styleChanged)
    Q_PROPERTY(QColor handleColor READ handleColor WRITE setHandleColor NOTIFY styleChanged)
    Q_PROPERTY(QString backgroundImage READ backgroundImage WRITE setBackgroundImage NOTIFY styleChanged)
    Q_PROPERTY(qreal backgroundOpacity READ backgroundOpacity WRITE setBackgroundOpacity NOTIFY styleChanged)
    Q_PROPERTY(bool emboss READ emboss WRITE setEmboss NOTIFY styleChanged)
    Q_PROPERTY(QStringList reservedSequences READ reservedSequences WRITE setReservedSequences NOTIFY reservedChanged)
    Q_PROPERTY(int count READ count NOTIFY tabsChanged)

public:
    explicit TerminalTabs(QQuickItem *parent = nullptr);
    ~TerminalTabs() override;

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
    QString backgroundImage() const { return m_bgImage; }
    void setBackgroundImage(const QString &v);
    qreal backgroundOpacity() const { return m_bgOpacity; }
    void setBackgroundOpacity(qreal v);
    bool emboss() const { return m_emboss; }
    void setEmboss(bool v);
    QStringList reservedSequences() const { return m_reserved; }
    void setReservedSequences(const QStringList &v);
    int count() const { return m_tabs.size(); }

    Q_INVOKABLE void newTab();
    Q_INVOKABLE void closeTab(int index);
    Q_INVOKABLE void selectTab(int index);
    // Proxied to the ACTIVE tab's panes (same names as TerminalPanes, so the TSX is unchanged).
    Q_INVOKABLE void split(int orient);
    Q_INVOKABLE void closeFocused();
    Q_INVOKABLE void focusNext();
    Q_INVOKABLE void focusPrev();
    Q_INVOKABLE void copyFocused();
    Q_INVOKABLE void pasteFocused();
    Q_INVOKABLE void clearFocused();

signals:
    void styleChanged();
    void reservedChanged();
    void tabsChanged();
    void allClosed();                          // last tab closed → the app can quit
    void titleChanged(const QString &title);   // active tab's focused-pane title → window
    void accelerator(const QString &sequence); // a reserved chord from the active tab

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void focusInEvent(QFocusEvent *event) override;

private:
    struct Tab {
        TerminalPanes *panes = nullptr;
        QString title;
    };

    TerminalPanes *makePanes();
    void applyStyle(TerminalPanes *p);
    void relayout();
    void syncBar();
    TerminalPanes *active() const;

    QVector<Tab> m_tabs;
    int m_active = 0;
    bool m_didInitialFocus = false;
    class QQmlComponent *m_panesComponent = nullptr;
    class TabBar *m_bar = nullptr;

    QString m_fontFamily = QStringLiteral("monospace");
    int m_fontSize = 15;
    QColor m_background = QColor("#1e1e1e");
    QColor m_foreground = QColor("#ffffff");
    int m_scrollbackLimit = 8000;
    QColor m_handleColor = QColor("#151515");
    QString m_bgImage;
    qreal m_bgOpacity = 1.0;
    bool m_emboss = false;
    QStringList m_reserved;
};
