#pragma once

#include "containerbase.h"

// SwipeView + PageIndicator (ports of SwipeView.qml / PageIndicator.qml). SwipeView forwards its
// DEFAULT property (the pages) into the T.SwipeView's contentData (SlotContainer — the Container
// adopts and resizes each page) and owns the controlled `currentIndex` (RestoreNone Binding in,
// onCurrentIndexChanged mirror out). PageIndicator forwards count/currentIndex into the
// T.PageIndicator whose Basic-style Row + Repeater of ["dot"] CssRects rides in the snippet.
namespace SolidWidgets {

class SwipeView : public SlotContainer {
    Q_OBJECT
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)

public:
    explicit SwipeView(QQuickItem *parent = nullptr);

    int currentIndex() const { return m_currentIndex; }
    void setCurrentIndex(int v);

signals:
    void currentIndexChanged();

protected:
    void componentComplete() override;

private:
    int m_currentIndex = 0;
};

class PageIndicator : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(int count READ count WRITE setCount NOTIFY countChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)

public:
    explicit PageIndicator(QQuickItem *parent = nullptr);

    int count() const { return m_count; }
    void setCount(int v);
    int currentIndex() const { return m_currentIndex; }
    void setCurrentIndex(int v);

signals:
    void countChanged();
    void currentIndexChanged();

protected:
    void componentComplete() override;

private:
    int m_count = 0;
    int m_currentIndex = 0;
    QPointer<QQuickItem> m_control;
};

} // namespace SolidWidgets
