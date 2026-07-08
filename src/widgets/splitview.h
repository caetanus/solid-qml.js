#pragma once

#include "containerbase.h"

// SplitView (port of SplitView.qml + SplitHandle.qml). The C++ wrapper forwards its DEFAULT
// property (the panes) into the T.SplitView's contentData (SlotContainer) — the Container manages
// their geometry (each pane's implicit size comes from its own Css layout). The handle delegate
// is inlined in the snippet: a plain-Rectangle root keeps a real implicit thickness (a bare
// Css.CssRect measures its empty content as 0 → SplitView reserves no space → invisible handle);
// `cssAncestor: root` re-anchors the CSS walk to this wrapper so `.my-split .handle` rules match.
// SplitHandle.qml is gone — it was module-internal, never emitted by the transpiler.
namespace SolidWidgets {

class SplitView : public SlotContainer {
    Q_OBJECT
    Q_PROPERTY(int orientation READ orientation WRITE setOrientation NOTIFY orientationChanged)

public:
    explicit SplitView(QQuickItem *parent = nullptr);

    int orientation() const { return m_orientation; }
    void setOrientation(int v);

signals:
    void orientationChanged();

protected:
    void componentComplete() override;

private:
    int m_orientation = Qt::Horizontal;
};

} // namespace SolidWidgets
