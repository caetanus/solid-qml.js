#pragma once

#include <QJsonObject>
#include <QObject>
#include <QVariant>

// solidterm's own config store, at ~/.config/solidterm/config.json (owner: shortcuts + prefs
// saved under .config/.local). XDG ConfigLocation, written on every set — so preferences AND the
// user's custom keybindings survive across runs in a real, hand-editable config file (not the
// localStorage sqlite blob). Exposed to the solid UI as `SolidTerm.TermConfig` / a context object.
class TermConfig : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString path READ path CONSTANT)

public:
    explicit TermConfig(QObject *parent = nullptr);

    QString path() const;

    Q_INVOKABLE QVariant get(const QString &key, const QVariant &fallback = QVariant()) const;
    Q_INVOKABLE QString getString(const QString &key, const QString &fallback) const;
    Q_INVOKABLE int getInt(const QString &key, int fallback) const;
    Q_INVOKABLE void set(const QString &key, const QVariant &value);

signals:
    void changed(const QString &key);

private:
    void load();
    void save() const;

    QJsonObject m_data;
};
