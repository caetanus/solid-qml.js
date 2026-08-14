#include "backdrop.h"

#include <QGuiApplication>
#include <QQuickItem>
#include <QScreen>

#include <LayerShellQt/Window>

Backdrop::Backdrop(QWindow *follow)
{
    // Transparent by default: until the pipeline hands us a processed image we must not paint over
    // the wallpaper (a blank backdrop would look like a bug, not an effect).
    setColor(Qt::transparent);
    setFlag(Qt::FramelessWindowHint);

    // Make this a layer-shell surface BEFORE the first show(): LayerShellQt configures the surface
    // role at map time, so the call has to happen while the window is still unmapped.
    if (auto *layer = LayerShellQt::Window::get(this)) {
        layer->setLayer(LayerShellQt::Window::LayerBackground);
        // No keyboard, no pointer: this window is scenery. Without this it would steal input from
        // the terminal sitting on top of it.
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        layer->setExclusiveZone(-1); // do not reserve space; other surfaces lay out as if we are not here
        layer->setScope(QStringLiteral("solidterm-backdrop"));
        m_available = true;
    }

    if (follow)
        this->follow(follow);
}

void Backdrop::follow(QWindow *w)
{
    if (m_follow == w)
        return;
    if (m_follow)
        disconnect(m_follow, nullptr, this, nullptr);
    m_follow = w;
    if (!m_follow)
        return;
    // Geometry, screen and visibility all move the region we must show — every one of them is a
    // reason to re-capture, which is the refresh policy: driven by this window changing.
    connect(m_follow, &QWindow::xChanged, this, &Backdrop::syncGeometry);
    connect(m_follow, &QWindow::yChanged, this, &Backdrop::syncGeometry);
    connect(m_follow, &QWindow::widthChanged, this, &Backdrop::syncGeometry);
    connect(m_follow, &QWindow::heightChanged, this, &Backdrop::syncGeometry);
    connect(m_follow, &QWindow::screenChanged, this, &Backdrop::syncGeometry);
    syncGeometry();
}

void Backdrop::syncGeometry()
{
    if (!m_follow)
        return;
    // A background-layer surface is positioned by the compositor, not by us: anchoring to all four
    // edges makes it span the output, and we simply show the part of the capture that matters.
    if (auto *layer = LayerShellQt::Window::get(this)) {
        layer->setAnchors({ LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom
                            | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight });
    }
    const QRect region = m_follow->geometry();
    emit regionChanged(region);
}

void Backdrop::setImage(const QImage &img)
{
    m_image = img;
    // The image is handed to the QML scene through a property on the root item; the effect material
    // samples it. Kept as a plain setter so the pipeline stays testable without a scene.
    if (QQuickItem *root = contentItem())
        root->setProperty("backdropImage", QVariant::fromValue(m_image));
    update();
}
