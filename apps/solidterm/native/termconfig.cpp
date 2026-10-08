#include "termconfig.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontDialog>
#include <QGuiApplication>
#include <QScreen>
#include <QJsonDocument>
#include <QStandardPaths>

TermConfig::TermConfig(QObject *parent)
    : QObject(parent)
{
    load();
}

QString TermConfig::path() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return dir + QStringLiteral("/solidterm/config.json");
}

void TermConfig::load()
{
    QFile f(path());
    if (!f.open(QIODevice::ReadOnly))
        return;
    m_data = QJsonDocument::fromJson(f.readAll()).object();
}

void TermConfig::save() const
{
    const QString p = path();
    QDir().mkpath(QFileInfo(p).absolutePath());
    QFile f(p);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(m_data).toJson(QJsonDocument::Indented));
}

QVariant TermConfig::get(const QString &key, const QVariant &fallback) const
{
    const auto it = m_data.constFind(key);
    return it == m_data.constEnd() ? fallback : it->toVariant();
}

QString TermConfig::getString(const QString &key, const QString &fallback) const
{
    return get(key, fallback).toString();
}

int TermConfig::getInt(const QString &key, int fallback) const
{
    return get(key, fallback).toInt();
}

void TermConfig::set(const QString &key, const QVariant &value)
{
    if (m_data.value(key).toVariant() == value)
        return;
    m_data.insert(key, QJsonValue::fromVariant(value));
    save();
    emit changed(key);
}

QString TermConfig::pickImage() const
{
    const QString start = getString(QStringLiteral("bgImage"), QDir::homePath());
    return QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("Choose background image"),
        start.isEmpty() ? QDir::homePath() : start,
        QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif);;All files (*)"));
}

QVariantMap TermConfig::pickFont() const
{
    // The dialog speaks points, the terminal pixels: convert through the screen's logical DPI.
    const QScreen *screen = QGuiApplication::primaryScreen();
    const qreal dpi = screen ? screen->logicalDotsPerInchY() : 96.0;
    QFont initial(getString(QStringLiteral("fontFamily"), QStringLiteral("monospace")));
    initial.setPointSizeF(getInt(QStringLiteral("fontSize"), 13) * 72.0 / dpi);

    bool ok = false;
    const QFont chosen = QFontDialog::getFont(&ok, initial, nullptr, QStringLiteral("Terminal font"),
                                              QFontDialog::MonospacedFonts);
    if (!ok)
        return {};
    const int px = chosen.pixelSize() > 0 ? chosen.pixelSize() : qRound(chosen.pointSizeF() * dpi / 72.0);
    return { { QStringLiteral("family"), chosen.family() }, { QStringLiteral("size"), px } };
}
