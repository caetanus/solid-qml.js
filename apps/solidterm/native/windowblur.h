#pragma once

#include <QObject>
#include <QPointer>

struct ext_background_effect_manager_v1;
struct ext_background_effect_surface_v1;
struct wl_compositor;
class QWindow;

// WindowBlur — asks the COMPOSITOR to blur what shows through a translucent window, via the standard
// `ext-background-effect-v1` protocol (wayland-protocols staging; Mutter 51, KWin 6.7, niri 26.04…).
//
// A client never sees the pixels behind its own surface — translucency is blended by the compositor
// after our frame leaves the process — so a backdrop effect can only be applied there. This class
// just declares "blur behind this surface"; the blur itself (its radius, its shader) is compositor
// policy. Compositor-agnostic: on one without the protocol, `isSupported()` is false and the window
// stays plainly translucent.
//
// Runs on Qt's own wl_display. Needs the window's wl_surface, which Qt exposes only through its
// private QWaylandWindow native interface (also what tells us when the surface is recreated).
class WindowBlur : public QObject {
    Q_OBJECT

public:
    explicit WindowBlur(QWindow *window);
    ~WindowBlur() override;

    // The compositor advertises the blur capability.
    bool isSupported() const { return m_manager && (m_caps & 1u); }

    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }

    // Capability trampoline target (C callback); not part of the API.
    void onCapabilities(uint32_t flags);

signals:
    void supportedChanged();

private:
    void attach();   // (re)create the effect object for the current wl_surface
    void detach();
    void apply();    // push the current region (or none) and get it committed

    QPointer<QWindow> m_window;
    ext_background_effect_manager_v1 *m_manager = nullptr;
    wl_compositor *m_compositor = nullptr;
    ext_background_effect_surface_v1 *m_effect = nullptr;
    uint32_t m_caps = 0;
    bool m_enabled = false;
};
