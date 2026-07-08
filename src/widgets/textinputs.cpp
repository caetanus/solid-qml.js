#include "textinputs.h"

#include "snippetwidget.h"

namespace {

// Original TextField.qml internals — `root` = the C++ wrapper.
const char *kTextFieldBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.TextField {
    id: field
    anchors.fill: parent
    background: null
    color: cssTheme.parseColor(root.inheritedColor || "#2b2b2b")
    font.family: cssTheme.resolveFontFamily(root.inheritedFontFamily || "Sans Serif")
    font.pixelSize: cssTheme.parseFontSize(root.inheritedFontSize || "13px", 13)
    leftPadding: 12
    rightPadding: 12
    verticalAlignment: TextInput.AlignVCenter
    selectByMouse: true
    activeFocusOnTab: solidTabstop.enabled
    echoMode: root.echoMode
    maximumLength: root.maximumLength
    readOnly: root.readOnly
    onTextChanged: root.text = text
    onTextEdited: root.textEdited()
    onEditingFinished: root.editingFinished()
    Keys.onPressed: (event) => root.keyPressed(event)

    Binding on text {
        value: root.text
        restoreMode: Binding.RestoreNone
    }

    Text {
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.leftMargin: parent.leftPadding
        visible: parent.text.length === 0 && !parent.activeFocus && root.placeholder.length > 0
        text: root.placeholder
        color: "#9aa0a6"
        font: parent.font
    }
}
)";

// Original TextArea.qml internals.
const char *kTextAreaBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.TextArea {
    id: field
    anchors.fill: parent
    background: null
    wrapMode: TextEdit.Wrap
    color: cssTheme.parseColor(root.inheritedColor || "#2b2b2b")
    font.family: cssTheme.resolveFontFamily(root.inheritedFontFamily || "Sans Serif")
    font.pixelSize: cssTheme.parseFontSize(root.inheritedFontSize || "13px", 13)
    padding: 12
    selectByMouse: true
    activeFocusOnTab: solidTabstop.enabled
    readOnly: root.readOnly
    onTextChanged: root.text = text

    Binding on text {
        value: root.text
        restoreMode: Binding.RestoreNone
    }

    Text {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: parent.padding
        anchors.leftMargin: parent.padding
        visible: parent.text.length === 0 && !parent.activeFocus && root.placeholder.length > 0
        text: root.placeholder
        color: "#9aa0a6"
        font: parent.font
    }
}
)";

} // namespace

namespace SolidWidgets {

TextInputBase::TextInputBase(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    connect(this, &QQuickItem::enabledChanged, this, &TextInputBase::syncState);
}

void TextInputBase::setText(const QString &v)
{
    if (m_text == v)
        return;
    m_text = v;
    emit textChanged();
}

void TextInputBase::setPlaceholder(const QString &v)
{
    if (m_placeholder == v)
        return;
    m_placeholder = v;
    emit placeholderChanged();
}

void TextInputBase::setReadOnly(bool v)
{
    if (m_readOnly == v)
        return;
    m_readOnly = v;
    emit readOnlyChanged();
}

void TextInputBase::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    m_control = composeInternal(this, content(), snippetKey(), snippet());
    if (m_control) {
        connect(m_control, SIGNAL(activeFocusChanged(bool)), this, SLOT(syncState()));
        const auto mirror = [this] {
            setImplicitWidth(m_control->implicitWidth());
            setImplicitHeight(m_control->implicitHeight());
        };
        connect(m_control, &QQuickItem::implicitWidthChanged, this, mirror);
        connect(m_control, &QQuickItem::implicitHeightChanged, this, mirror);
        mirror();
    }
    syncState();
}

void TextInputBase::syncState()
{
    QVariantList state;
    if (m_control && m_control->hasActiveFocus())
        state << QStringLiteral("focus");
    if (!isEnabled())
        state << QStringLiteral("disabled");
    setCssState(state);
}

TextField::TextField(QQuickItem *parent)
    : TextInputBase(parent)
{
    setCssPrimitive(QStringLiteral("input"));
}

void TextField::setEchoMode(int v)
{
    if (m_echoMode == v)
        return;
    m_echoMode = v;
    emit echoModeChanged();
}

void TextField::setMaximumLength(int v)
{
    if (m_maximumLength == v)
        return;
    m_maximumLength = v;
    emit maximumLengthChanged();
}

const char *TextField::snippet() const { return kTextFieldBody; }

TextArea::TextArea(QQuickItem *parent)
    : TextInputBase(parent)
{
    setCssPrimitive(QStringLiteral("textarea"));
}

const char *TextArea::snippet() const { return kTextAreaBody; }

} // namespace SolidWidgets
