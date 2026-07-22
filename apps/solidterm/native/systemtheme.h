#pragma once

#include <QColor>
#include <QObject>

// Obeys the LOCAL desktop theme (owner 2026-07-22: "isso é uma app nativa, então obedeça o theme
// local"). Qt already derives QPalette from the platform theme (the GTK/Adwaita colours on
// Linux, the system palette on Windows/macOS); this reads those roles, exposes them to the solid
// UI (so the terminal's own bg/fg and any inline use follow the desktop), and — since the CSS
// engine has no var() — GENERATES the app's colour layer from the palette, loaded over the
// structural term.css. Re-emitted on palette change so a live theme switch reflows.
class SystemTheme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor base READ base NOTIFY changed)        // terminal background
    Q_PROPERTY(QColor text READ text NOTIFY changed)        // terminal foreground
    Q_PROPERTY(QColor window READ window NOTIFY changed)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)    // Highlight (menu hover, primary btn)
    Q_PROPERTY(bool dark READ dark NOTIFY changed)

public:
    explicit SystemTheme(QObject *parent = nullptr);

    QColor base() const;
    QColor text() const;
    QColor window() const;
    QColor accent() const;
    bool dark() const;

    // The generated colour stylesheet (semantic app classes → palette roles), loaded as a layer
    // over the structural term.css.
    Q_INVOKABLE QString styleSheet() const;

signals:
    void changed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};
