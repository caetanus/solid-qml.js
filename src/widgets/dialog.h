#pragma once

#include <QPointer>
#include <QQmlListProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QVariant>

// Dialog (port of Dialog.qml) — a REAL modal Window (flags Qt.Dialog, modality WindowModal), NOT
// a Templates dialog or overlay popup (owner directive: dialog is a window). The C++ root is the
// zero-size page anchor; it exposes the page window (tracked via ItemSceneChange) so the snippet
// can bind the dialog's transientParent — the QML original attached `wrap.Window.window`, which a
// context-property `root` cannot express. The Window snippet keeps the focus-on-open (prefer the
// default button), Enter-fires-default bubbling, Esc→dialogClosed shortcut, per-window Tabstop
// and the RestoreNone visible Binding (survives the chrome's self-close) verbatim. The author's
// class forwards to the Css root via `cssClass`; children route into its CONTENT slot, buffered
// until the snippet composes.
namespace SolidWidgets {

class Dialog : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(bool open READ open WRITE setOpen NOTIFY openChanged)
    Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY titleChanged)
    Q_PROPERTY(QVariant cssClass READ cssClass WRITE setCssClass NOTIFY cssClassChanged)
    Q_PROPERTY(QQuickWindow *pageWindow READ pageWindow NOTIFY pageWindowChanged)
    Q_PROPERTY(QQmlListProperty<QObject> content READ contentSlot CONSTANT)
    Q_CLASSINFO("DefaultProperty", "content")

public:
    explicit Dialog(QQuickItem *parent = nullptr);

    bool open() const { return m_open; }
    void setOpen(bool v);
    QString title() const { return m_title; }
    void setTitle(const QString &v);
    QVariant cssClass() const { return m_cssClass; }
    void setCssClass(const QVariant &v);
    QQuickWindow *pageWindow() const { return window(); }
    QQmlListProperty<QObject> contentSlot();

signals:
    void openChanged();
    void titleChanged();
    void cssClassChanged();
    void pageWindowChanged();
    void dialogClosed();

protected:
    void componentComplete() override;
    void itemChange(QQuickItem::ItemChange change, const QQuickItem::ItemChangeData &data) override;

private:
    static void content_append(QQmlListProperty<QObject> *prop, QObject *obj);

    void appendContent(QObject *obj);

    bool m_open = false;
    QString m_title;
    QVariant m_cssClass = QVariantList();
    QList<QObject *> m_pending;
    QPointer<QQuickItem> m_cssRoot; // the dialog window's Css root ("content" receiver)
};

} // namespace SolidWidgets
