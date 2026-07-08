#pragma once

#include "qmlcss/cssfill.h"

#include <QPointer>

// ToolBar + ToolSeparator (ports of ToolBar.qml / ToolSeparator.qml). Semantic native chrome:
// the CSS engine owns paint AND layout; the T.ToolBar / T.ToolSeparator supply the semantic
// (accessibility) role only and ride as `root`-bound snippets. The author's children land in the
// WRAPPER's default content slot — the anchored control is a plain (non-Css) sibling in the same
// holder, skipped by the flex pass.
namespace SolidWidgets {

class ToolBar : public QmlCss::CssFill {
    Q_OBJECT

public:
    explicit ToolBar(QQuickItem *parent = nullptr);

protected:
    void componentComplete() override;
};

// ToolSeparator — the visible rule is a CssRect ["sep"] centred in the control's Item content
// host (1px wide, 60% of the available height); implicit metrics mirror onto the wrapper.
class ToolSeparator : public QmlCss::CssFill {
    Q_OBJECT

public:
    explicit ToolSeparator(QQuickItem *parent = nullptr);

protected:
    void componentComplete() override;

private:
    QPointer<QQuickItem> m_control;
};

} // namespace SolidWidgets
