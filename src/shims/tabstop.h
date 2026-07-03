#pragma once

#include <QObject>

// App-wide tab-navigation switch, exposed to generated QML as the `solidTabstop` context
// property. Every interactive widget the transpiler emits binds
// `activeFocusOnTab: solidTabstop.enabled`, so flipping the property re-evaluates the tab
// chain live. Authors reach it as `import { tabstop } from "qml-solid"` — the transpiler
// rewrites `tabstop.enabled` to this object.
class SolidTabstop : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)

public:
    using QObject::QObject;

    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled)
    {
        if (m_enabled == enabled)
            return;
        m_enabled = enabled;
        emit enabledChanged();
    }

signals:
    void enabledChanged();

private:
    bool m_enabled = true;
};
