#pragma once

#include "qmlcss/cssfill.h"

#include <QPointer>

// <icon name="edit-find-symbolic"> — a themed, re-colourable SYMBOLIC icon from the system icon
// theme (ports nothing: composes vendor/qml-css-engine's ALREADY-BUILT Css.CssIcon, which was
// registered but never actually wired to the icon theme — see themeiconprovider.h/.cpp for the
// missing half). A CssFill wrapper carries the CSS identity (cssPrimitive + author cssClass) and
// feeds the cascade's inherited `color` into the composed CssIcon's tint, the same composition
// TextField/TextArea already use for their own inner control (textinputs.h/.cpp).
namespace SolidWidgets {

class Icon : public QmlCss::CssFill {
    Q_OBJECT
    // A symbolic icon name resolved through the system icon theme (e.g. "edit-find-symbolic") —
    // the same freedesktop icon-naming convention GTK/Adwaita apps use.
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)

public:
    explicit Icon(QQuickItem *parent = nullptr);

    QString name() const { return m_name; }
    void setName(const QString &v);

signals:
    void nameChanged();

protected:
    void componentComplete() override;

private:
    QString m_name;
    QPointer<QQuickItem> m_icon;
};

} // namespace SolidWidgets
