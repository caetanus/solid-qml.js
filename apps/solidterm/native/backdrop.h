#pragma once

#include <QImage>
#include <QQuickWindow>
#include <QRect>

// Backdrop — the overlay window that carries the processed desktop behind a translucent SolidTerm.
//
// Why a second window: the effect (emboss/blur) must act on what shows THROUGH the terminal, and a
// client cannot ask the compositor to filter its backdrop. So we own that pixel area ourselves: a
// `wlr-layer-shell` surface on the BACKGROUND layer — the layer swaybg uses — which sits above the
// wallpaper and below every window, including ours. We render the processed image there and let the
// translucent terminal reveal it.
//
// Pipeline (see docs/SOLIDTERM-QUEUE.md): capture what is visible in this window's region →
// process it (in the GL pipeline) → paint it here. Refresh is driven by this window changing, not
// by a timer.
class Backdrop : public QQuickWindow {
    Q_OBJECT

public:
    explicit Backdrop(QWindow *follow = nullptr);

    // True when the compositor accepted a layer-shell surface for us. On a compositor without the
    // protocol (or a non-Wayland session) the backdrop is simply unavailable and the caller skips it.
    bool isAvailable() const { return m_available; }

    // The processed image to show. Set by the pipeline; drawn to fill the window.
    void setImage(const QImage &img);

    // Track a window: the backdrop covers the same screen area, so the terminal's translucent
    // region always has processed content underneath.
    void follow(QWindow *w);

signals:
    // The region we cover changed — the pipeline recaptures and reprocesses off this.
    void regionChanged(const QRect &region);

private:
    void syncGeometry();

    QWindow *m_follow = nullptr;
    QImage m_image;
    bool m_available = false;
};
