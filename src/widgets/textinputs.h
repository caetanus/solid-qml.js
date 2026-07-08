#pragma once

#include "qmlcss/cssfill.h"

#include <QPointer>

// TextField + TextArea (ports of TextField.qml / TextArea.qml). The C++ wrapper carries the
// controlled `text` (kept in lock-step with the control both ways: RestoreNone Binding forwards
// root→control, onTextChanged mirrors control→root so author reads like e.target.value stay
// live) plus the placeholder/readOnly surface and the focus/disabled cssState; the T.TextField /
// T.TextArea with the CSS-bridged colour/font and the placeholder overlay ride the original QML
// bodies as `root`-bound snippets.
namespace SolidWidgets {

class TextInputBase : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(QString placeholder READ placeholder WRITE setPlaceholder NOTIFY placeholderChanged)
    Q_PROPERTY(bool readOnly READ readOnly WRITE setReadOnly NOTIFY readOnlyChanged)

public:
    explicit TextInputBase(QQuickItem *parent = nullptr);

    QString text() const { return m_text; }
    void setText(const QString &v);
    QString placeholder() const { return m_placeholder; }
    void setPlaceholder(const QString &v);
    bool readOnly() const { return m_readOnly; }
    void setReadOnly(bool v);

signals:
    void textChanged();
    void placeholderChanged();
    void readOnlyChanged();

protected:
    void componentComplete() override;

    virtual const char *snippet() const = 0;
    virtual QString snippetKey() const = 0;

    QPointer<QQuickItem> m_control;

private:
    Q_SLOT void syncState();

    QString m_text;
    QString m_placeholder;
    bool m_readOnly = false;
};

// TextField — single-line <input type="text|email|password|search">: echoMode/maximumLength and
// the user-edit relays (textEdited/editingFinished/keyPressed — no echo on Binding re-assertion).
class TextField : public TextInputBase {
    Q_OBJECT
    Q_PROPERTY(int echoMode READ echoMode WRITE setEchoMode NOTIFY echoModeChanged)
    Q_PROPERTY(int maximumLength READ maximumLength WRITE setMaximumLength NOTIFY maximumLengthChanged)

public:
    explicit TextField(QQuickItem *parent = nullptr);

    int echoMode() const { return m_echoMode; }
    void setEchoMode(int v);
    int maximumLength() const { return m_maximumLength; }
    void setMaximumLength(int v);

signals:
    void echoModeChanged();
    void maximumLengthChanged();
    void textEdited();
    void editingFinished();
    void keyPressed(const QVariant &event);

protected:
    const char *snippet() const override;
    QString snippetKey() const override { return QStringLiteral("solidwidgets-textfield"); }

private:
    int m_echoMode = 0;          // TextInput.Normal
    int m_maximumLength = 32767; // Qt's TextInput default
};

// TextArea — multi-line <textarea>: wrap on, top-aligned placeholder; authors wire onTextChanged
// (TextEdit has no textEdited signal — the Binding echo is harmless, change-gated).
class TextArea : public TextInputBase {
    Q_OBJECT

public:
    explicit TextArea(QQuickItem *parent = nullptr);

protected:
    const char *snippet() const override;
    QString snippetKey() const override { return QStringLiteral("solidwidgets-textarea"); }
};

} // namespace SolidWidgets
