#pragma once

#include <QtQuickTemplates2/private/qquickmenu_p.h>
#include <QtQuickTemplates2/private/qquickmenuitem_p.h>
#include <QtQuickTemplates2/private/qquickmenuseparator_p.h>

#include <QVariant>

// Menu + MenuItem + MenuSeparator (ports of the .qml trio) — subclass the PRIVATE Templates
// classes because QQuickMenu manages its rows (highlight, arrow navigation, trigger-closes)
// through qobject_cast to those exact types (owner-approved private dependency, same precedent
// as QZipReader). The transpiler drives the ORCHESTRATION (controlled `open` Binding or the
// self-managed trigger toggle) from the host Item; these classes hold only the popup SHELL and
// row chrome, with the Css slot items riding as `root`-bound snippets.
namespace SolidWidgets {

class Menu : public QQuickMenu {
    Q_OBJECT
    // The CSS anchor the transpiler passes in: popup contents reparent to the window Overlay,
    // severing the visual chain CSS scoping walks — both sibling slots re-anchor the walk here.
    Q_PROPERTY(QQuickItem *cssAncestor READ cssAncestor WRITE setCssAncestor NOTIFY cssAncestorChanged)
    // Author `class` on <Menu> — merged with "popup" for the background scope.
    Q_PROPERTY(QVariant authorClass READ authorClass WRITE setAuthorClass NOTIFY authorClassChanged)
    // Recorded on every close so the trigger form can swallow the dismissing click
    // (QToolButton+QMenu idiom — the emit reads Date.now() - __menuN.__closedAt).
    Q_PROPERTY(double __closedAt READ closedAt NOTIFY closedAtChanged)

public:
    explicit Menu(QObject *parent = nullptr);

    QQuickItem *cssAncestor() const { return m_cssAncestor; }
    void setCssAncestor(QQuickItem *v);
    QVariant authorClass() const { return m_authorClass; }
    void setAuthorClass(const QVariant &v);
    double closedAt() const { return m_closedAt; }

signals:
    void cssAncestorChanged();
    void authorClassChanged();
    void closedAtChanged();
    // Re-exposes the close so the author's onClose runs alongside the __closedAt record.
    void menuClosed();

protected:
    // QQuickMenu::contentData() (the default-property getter the QML engine calls to append EVERY
    // declared child — a static MenuItem, a <For>'s Repeater, …) forces its own default content
    // item into existence via `executeContentItem()` the FIRST time it's called, if `d->contentItem`
    // is still null (see qquickmenu.cpp — this is how a vanilla style's Menu "just works" with a
    // Repeater child with no C++ involved at all). That first call happens while the QML engine is
    // still building this object's children — i.e. BEFORE componentComplete(). Setting OUR content
    // item there (as this class used to) is too late: the engine's own default already won the
    // getter's `if (!d->contentItem)` race. A STATIC MenuItem is unaffected (contentData_append adds
    // it straight to contentModel, independent of contentItem) — but a Repeater/Instantiator child
    // (a <For>'s dynamic rows) IS: contentData_append reparents it under `p->contentItem` — the
    // ENGINE'S discarded default, not ours — so it (and every item it creates) is silently lost the
    // moment componentComplete() swaps in our real content item. classBegin() runs right after this
    // object's own QML context
    // is attached (the engine's QQmlParserStatus protocol: setContextForObject, THEN classBegin,
    // before any child's property gets appended) — early enough to win that race.
    void classBegin() override;
    void componentComplete() override;

private:
    Q_SLOT void syncImplicit();

    QQuickItem *m_cssAncestor = nullptr;
    QVariant m_authorClass = QVariantList();
    double m_closedAt = 0;
};

class MenuItem : public QQuickMenuItem {
    Q_OBJECT

public:
    explicit MenuItem(QQuickItem *parent = nullptr);

    // Mnemonic marker: `&N` underlines N like every desktop toolkit (`&&` is a literal
    // ampersand). The label opts into styledText so the <u> renders; author text is
    // entity-escaped. Functional Alt+letter activation is the keyboard-model pass.
    Q_INVOKABLE QString __mnemonicMarkup(const QString &s) const;

protected:
    void componentComplete() override;

private:
    Q_SLOT void syncImplicit();
};

class MenuSeparator : public QQuickMenuSeparator {
    Q_OBJECT

public:
    explicit MenuSeparator(QQuickItem *parent = nullptr);

protected:
    void componentComplete() override;
};

} // namespace SolidWidgets
