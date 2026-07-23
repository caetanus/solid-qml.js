#pragma once

#include <QColor>
#include <QQuickItem>
#include <QStringList>
#include <QVector>

class TerminalPanes;

// The session/tab stack (owner's queue: "TABS … manter panes vivos por tab"; + directive: chrome
// in Solid, native only for the terminal). Each tab is a full split tree (TerminalPanes). Switching
// tabs must NOT destroy the others' ptys, so tabs are kept alive HERE — a reactive <For> in the TSX
// would recreate and kill them. This item is HEADLESS: it owns the live panes and shows one by
// index, but paints nothing; the tab BAR is rendered in the TSX from the model this exposes.
//
// Bridge: the TSX drives newTab/closeTab/selectTab and mirrors the model from tabsChanged(titles,
// active) (emitted on any add/close/select/title change). It presents the SAME surface as
// TerminalPanes (style, reservedSequences, split/close/focus, accelerator) proxied to the active tab.
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
    Q_INVOKABLE void refocus(); // keyboard focus back to the active pane (e.g. closing the search bar)
    Q_INVOKABLE void copyFocused();
    Q_INVOKABLE void pasteFocused();
    Q_INVOKABLE void clearFocused();
    Q_INVOKABLE void searchFocused(const QString &query);
    Q_INVOKABLE void searchNext();
    Q_INVOKABLE void searchPrev();
    Q_INVOKABLE void clearSearch();

signals:
    void styleChanged();
    void reservedChanged();
    // The full tab model for the TSX to mirror (titles per tab + the active index). Emitted on any
    // add / close / select / OSC-title change.
    void tabsChanged(const QStringList &titles, int active);
    void allClosed();                          // last tab closed → the app can quit
    void titleChanged(const QString &title);   // active tab's focused-pane title → window
    void accelerator(const QString &sequence); // a reserved chord from the active tab
    void searchChanged(int index, int count);  // active tab's search state → the Solid search bar

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
    void emitModel();          // push (titles, active) to the TSX
    TerminalPanes *active() const;

    QVector<Tab> m_tabs;
    int m_active = 0;
    bool m_didInitialFocus = false;
    class QQmlComponent *m_panesComponent = nullptr;

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
