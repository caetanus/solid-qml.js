#pragma once

#include <QObject>
#include <QPointer>

struct ext_background_effect_manager_v1;
struct ext_background_effect_surface_v1;
struct zbackground_effect_strength_manager_v1;
struct zbackground_effect_strength_v1;
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
// Strength: the standard protocol is on/off. When the compositor also offers our own
// `background-effect-strength-unstable-v1` (the owner's sway does), the strength (0-100) is sent
// too, so the blur scales with it; elsewhere it only switches the blur on above 0.
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

    // 0-100; only reaches a compositor offering background-effect-strength-unstable-v1.
    void setStrength(int strength);

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
    zbackground_effect_strength_manager_v1 *m_strengthManager = nullptr;
    zbackground_effect_strength_v1 *m_strengthObj = nullptr;
    uint32_t m_caps = 0;
    int m_strength = 50;
    bool m_enabled = false;
};
