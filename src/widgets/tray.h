#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QUrl>

// Tray (port of Tray.qml). Qt.labs.platform SystemTrayIcon is a QObject, NOT an Item — the C++
// root is the zero-size host that lets the tag sit anywhere in the tree; the icon rides as a
// snippet with `shown` folded into its `visible` (Item visibility does not cascade to non-Item
// resources). The `menu` is set by the transpiler ONLY when the author gave <MenuItem> children
// (a Platform.Menu built in the emit) — it forwards through a QObject* property so registration
// semantics stay identical; `available` mirrors the platform guard back for author fallbacks.
namespace SolidWidgets {

class Tray : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString tooltip READ tooltip WRITE setTooltip NOTIFY tooltipChanged)
    Q_PROPERTY(QUrl iconSource READ iconSource WRITE setIconSource NOTIFY iconSourceChanged)
    Q_PROPERTY(bool shown READ shown WRITE setShown NOTIFY shownChanged)
    Q_PROPERTY(QObject *menu READ menu WRITE setMenu NOTIFY menuChanged)
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)

public:
    explicit Tray(QQuickItem *parent = nullptr);

    QString tooltip() const { return m_tooltip; }
    void setTooltip(const QString &v);
    QUrl iconSource() const { return m_iconSource; }
    void setIconSource(const QUrl &v);
    bool shown() const { return m_shown; }
    void setShown(bool v);
    QObject *menu() const { return m_menu; }
    void setMenu(QObject *v);
    bool available() const;

signals:
    void tooltipChanged();
    void iconSourceChanged();
    void shownChanged();
    void menuChanged();
    void availableChanged();
    void activated(const QVariant &reason);

protected:
    void componentComplete() override;

private:
    QString m_tooltip;
    QUrl m_iconSource;
    bool m_shown = true;
    QObject *m_menu = nullptr;
    QPointer<QObject> m_tray;
};

} // namespace SolidWidgets
