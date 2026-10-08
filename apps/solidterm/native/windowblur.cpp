#include "windowblur.h"

#include <QGuiApplication>
#include <QQuickWindow>
#include <QWindow>
#include <qguiapplication_platform.h>
#include <qpa/qplatformwindow_p.h>

#include <climits>
#include <cstring>

#include <wayland-client.h>

#include "background-effect-strength-unstable-v1-client-protocol.h"
#include "ext-background-effect-v1-client-protocol.h"

namespace {

struct Bound {
    ext_background_effect_manager_v1 *manager = nullptr;
    zbackground_effect_strength_manager_v1 *strength = nullptr;
    wl_compositor *compositor = nullptr;
};

void registryGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t)
{
    auto *b = static_cast<Bound *>(data);
    if (!std::strcmp(interface, ext_background_effect_manager_v1_interface.name))
        b->manager = static_cast<ext_background_effect_manager_v1 *>(
            wl_registry_bind(registry, name, &ext_background_effect_manager_v1_interface, 1));
    else if (!std::strcmp(interface, zbackground_effect_strength_manager_v1_interface.name))
        b->strength = static_cast<zbackground_effect_strength_manager_v1 *>(
            wl_registry_bind(registry, name, &zbackground_effect_strength_manager_v1_interface, 1));
    else if (!std::strcmp(interface, wl_compositor_interface.name))
        b->compositor = static_cast<wl_compositor *>(wl_registry_bind(registry, name, &wl_compositor_interface, 1));
}

void registryGlobalRemove(void *, wl_registry *, uint32_t) {}

const wl_registry_listener kRegistryListener = { registryGlobal, registryGlobalRemove };

void managerCapabilities(void *data, ext_background_effect_manager_v1 *, uint32_t flags)
{
    static_cast<WindowBlur *>(data)->onCapabilities(flags);
}

const ext_background_effect_manager_v1_listener kManagerListener = { managerCapabilities };

wl_display *qtDisplay()
{
    auto *wl = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    return wl ? wl->display() : nullptr;
}

QNativeInterface::Private::QWaylandWindow *waylandWindow(QWindow *w)
{
    return w ? w->nativeInterface<QNativeInterface::Private::QWaylandWindow>() : nullptr;
}

} // namespace

WindowBlur::WindowBlur(QWindow *window)
    : QObject(window)
    , m_window(window)
{
    wl_display *display = qtDisplay();
    if (!display)
        return; // not Wayland

    // Bind on a private queue (the one-off round-trip must not dispatch Qt's events), and read the
    // initial capabilities there too; then move the manager to Qt's default queue so later
    // capability changes arrive through Qt's normal dispatch on the GUI thread.
    wl_event_queue *queue = wl_display_create_queue(display);
    auto *wrapped = static_cast<wl_display *>(wl_proxy_create_wrapper(display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapped), queue);
    wl_registry *registry = wl_display_get_registry(wrapped);
    wl_proxy_wrapper_destroy(wrapped);
    Bound bound;
    wl_registry_add_listener(registry, &kRegistryListener, &bound);
    wl_display_roundtrip_queue(display, queue); // globals
    m_manager = bound.manager;
    m_compositor = bound.compositor;
    if (m_manager) {
        ext_background_effect_manager_v1_add_listener(m_manager, &kManagerListener, this);
        wl_display_roundtrip_queue(display, queue); // capabilities (sent on bind)
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(m_manager), nullptr);
    }
    if (m_compositor)
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(m_compositor), nullptr);
    m_strengthManager = bound.strength;
    if (m_strengthManager)
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(m_strengthManager), nullptr);
    wl_registry_destroy(registry);
    wl_event_queue_destroy(queue);

    // Qt recreates the wl_surface when the window is hidden and shown again; the effect object is
    // per-surface, so follow it.
    if (auto *ww = waylandWindow(window)) {
        connect(ww, &QNativeInterface::Private::QWaylandWindow::surfaceCreated, this, &WindowBlur::attach);
        connect(ww, &QNativeInterface::Private::QWaylandWindow::surfaceDestroyed, this, &WindowBlur::detach);
    }
}

WindowBlur::~WindowBlur()
{
    detach();
    if (m_manager)
        ext_background_effect_manager_v1_destroy(m_manager);
    if (m_strengthManager)
        zbackground_effect_strength_manager_v1_destroy(m_strengthManager);
    if (m_compositor)
        wl_compositor_destroy(m_compositor);
}

void WindowBlur::onCapabilities(uint32_t flags)
{
    const bool was = isSupported();
    m_caps = flags;
    if (was != isSupported())
        emit supportedChanged();
}

void WindowBlur::setEnabled(bool on)
{
    if (m_enabled == on)
        return;
    m_enabled = on;
    if (on && !m_effect)
        attach();
    else
        apply();
}

void WindowBlur::setStrength(int strength)
{
    strength = qBound(0, strength, 100);
    if (m_strength == strength)
        return;
    m_strength = strength;
    apply();
}

void WindowBlur::attach()
{
    if (!m_manager || m_effect || !m_enabled)
        return;
    auto *ww = waylandWindow(m_window);
    wl_surface *surface = ww ? ww->surface() : nullptr;
    if (!surface)
        return; // not created yet; surfaceCreated brings us back
    m_effect = ext_background_effect_manager_v1_get_background_effect(m_manager, surface);
    if (m_strengthManager)
        m_strengthObj = zbackground_effect_strength_manager_v1_get_blur_strength(m_strengthManager, surface);
    apply();
}

void WindowBlur::detach()
{
    if (m_strengthObj) {
        zbackground_effect_strength_v1_destroy(m_strengthObj);
        m_strengthObj = nullptr;
    }
    if (m_effect) {
        ext_background_effect_surface_v1_destroy(m_effect);
        m_effect = nullptr;
    }
}

void WindowBlur::apply()
{
    if (!m_effect || !m_compositor)
        return;
    if (m_enabled) {
        // The compositor clips the region to the surface, so one maximal rect covers the whole
        // window at any size — no need to follow resizes.
        wl_region *region = wl_compositor_create_region(m_compositor);
        wl_region_add(region, 0, 0, INT_MAX, INT_MAX);
        ext_background_effect_surface_v1_set_blur_region(m_effect, region);
        wl_region_destroy(region);
    } else {
        ext_background_effect_surface_v1_set_blur_region(m_effect, nullptr);
    }
    if (m_strengthObj)
        zbackground_effect_strength_v1_set_strength(m_strengthObj, uint32_t(m_strength));
    // The region (and strength) are double-buffered: it takes effect on the surface's next commit, i.e. our next
    // frame.
    if (auto *qw = qobject_cast<QQuickWindow *>(m_window.data()))
        qw->update();
    else if (m_window)
        m_window->requestUpdate();
}
