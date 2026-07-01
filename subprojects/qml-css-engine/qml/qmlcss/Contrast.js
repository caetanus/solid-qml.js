.pragma library

function luminance(c) {
    return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b
}

function linearChannel(v) {
    return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4)
}

function relativeLuminance(c) {
    return 0.2126 * linearChannel(c.r) + 0.7152 * linearChannel(c.g) + 0.0722 * linearChannel(c.b)
}

function contrastRatio(a, b) {
    var la = relativeLuminance(a)
    var lb = relativeLuminance(b)
    var lighter = Math.max(la, lb)
    var darker = Math.min(la, lb)
    return (lighter + 0.05) / (darker + 0.05)
}

function blendOver(fg, bg) {
    var a = fg.a
    return Qt.rgba(fg.r * a + bg.r * (1 - a), fg.g * a + bg.g * (1 - a), fg.b * a + bg.b * (1 - a), 1)
}

function toHex2(v) {
    var n = Math.max(0, Math.min(255, Math.round(v * 255)))
    var s = n.toString(16)
    return s.length === 1 ? "0" + s : s
}

function toHex(c) {
    return "#" + toHex2(c.r) + toHex2(c.g) + toHex2(c.b)
}

function rgbToHsl(c) {
    var max = Math.max(c.r, c.g, c.b)
    var min = Math.min(c.r, c.g, c.b)
    var h = 0
    var s = 0
    var l = (max + min) / 2
    var d = max - min

    if (d !== 0) {
        s = l > 0.5 ? d / (2 - max - min) : d / (max + min)
        if (max === c.r) {
            h = ((c.g - c.b) / d + (c.g < c.b ? 6 : 0)) / 6
        } else if (max === c.g) {
            h = ((c.b - c.r) / d + 2) / 6
        } else {
            h = ((c.r - c.g) / d + 4) / 6
        }
    }

    return { h: h, s: s, l: l, a: c.a }
}

function hueToRgb(p, q, t) {
    if (t < 0) t += 1
    if (t > 1) t -= 1
    if (t < 1 / 6) return p + (q - p) * 6 * t
    if (t < 1 / 2) return q
    if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6
    return p
}

function hslToColor(hsl) {
    var r
    var g
    var b
    if (hsl.s === 0) {
        r = hsl.l
        g = hsl.l
        b = hsl.l
    } else {
        var q = hsl.l < 0.5 ? hsl.l * (1 + hsl.s) : hsl.l + hsl.s - hsl.l * hsl.s
        var p = 2 * hsl.l - q
        r = hueToRgb(p, q, hsl.h + 1 / 3)
        g = hueToRgb(p, q, hsl.h)
        b = hueToRgb(p, q, hsl.h - 1 / 3)
    }
    return Qt.rgba(r, g, b, hsl.a)
}

function bestBlackOrWhite(bg) {
    var black = Qt.rgba(0.10, 0.10, 0.12, 1)
    var white = Qt.rgba(0.96, 0.97, 1.00, 1)
    return contrastRatio(white, bg) >= contrastRatio(black, bg) ? white : black
}

// Preserve the foreground's hue/saturation when possible, but move its HSL
// lightness away from the background. If that still cannot clear the target
// contrast, fall back to softened black or white.
function naturalContrastColor(fg, bg, minimumRatio) {
    var target = minimumRatio !== undefined ? minimumRatio : 4.5
    if (contrastRatio(fg, bg) >= target) {
        return fg
    }

    var f = rgbToHsl(fg)
    var b = rgbToHsl(bg)
    var candidates = []
    if (b.l < 0.5) {
        candidates = [
            Math.max(f.l, Math.min(0.96, b.l + 0.45)),
            0.72,
            0.82,
            0.92,
            0.97
        ]
    } else {
        candidates = [
            Math.min(f.l, Math.max(0.08, b.l - 0.45)),
            0.34,
            0.24,
            0.14,
            0.08
        ]
    }

    var best = fg
    var bestRatio = contrastRatio(fg, bg)
    for (var i = 0; i < candidates.length; ++i) {
        var candidateHsl = { h: f.h, s: f.s, l: Math.max(0, Math.min(1, candidates[i])), a: f.a }
        var adjusted = hslToColor(candidateHsl)
        var ratio = contrastRatio(adjusted, bg)
        if (ratio > bestRatio) {
            best = adjusted
            bestRatio = ratio
        }
        if (ratio >= target) {
            return adjusted
        }
    }

    var fallback = bestBlackOrWhite(bg)
    return contrastRatio(fallback, bg) > bestRatio ? fallback : best
}

function averageGradientColor(gradient, fallback) {
    if (!gradient || gradient.stops === undefined || gradient.stops.length === 0) {
        return fallback
    }

    var r = 0
    var g = 0
    var b = 0
    var a = 0
    for (var i = 0; i < gradient.stops.length; ++i) {
        var c = gradient.stops[i].color
        r += c.r
        g += c.g
        b += c.b
        a += c.a
    }
    var count = gradient.stops.length
    return Qt.rgba(r / count, g / count, b / count, a / count)
}

function styleBackgroundColor(style, cssTheme, fallback) {
    if (!style || !cssTheme || !cssTheme.loaded) {
        return fallback
    }
    if (style["background-color"]) {
        return cssTheme.parseColor(style["background-color"])
    }

    var bgValue = style["background"] ? style["background"] : (style["background-image"] ? style["background-image"] : "")
    if (!bgValue) {
        return fallback
    }
    if (bgValue.indexOf("gradient") < 0) {
        return cssTheme.parseColor(bgValue)
    }

    return averageGradientColor(cssTheme.parseGradient(bgValue), fallback)
}

// window#waybar background-color, falling back when the CSS theme has none loaded
function barBackground(cssTheme, fallback) {
    // `fallback` is usually a "#AARRGGBB" string from the host app's theme map.
    // context property — parse it so .r/.g/.b/.a are usable by blendOver below.
    if (!cssTheme || !cssTheme.loaded) {
        return cssTheme ? cssTheme.parseColor(fallback) : fallback
    }
    var style = cssTheme.resolve("waybar")
    return style["background-color"] ? cssTheme.parseColor(style["background-color"]) : cssTheme.parseColor(fallback)
}

// What an element actually sits on: its own background if opaque/translucent,
// otherwise the bar's background (blended through if translucent).
function effectiveBackground(ownBg, cssTheme, fallback) {
    var bar = barBackground(cssTheme, fallback)
    return ownBg.a > 0 ? blendOver(ownBg, bar) : bar
}

// Whether icons/text on this background should use the dark contrast color
// (as opposed to white).
function needsDarkIcon(bg) {
    return luminance(bg) >= 0.5
}

// High-contrast foreground (icon/text) color for the given background. Pure
// black reads as too harsh on light themes, so it's softened to a charcoal
// gray by blending at reduced opacity over the background. Themes that want
// full #1a1a1a/#ffffff can set "color" explicitly in their CSS, which takes
// precedence over this fallback at every call site.
function contrastColor(bg) {
    if (!needsDarkIcon(bg)) {
        return "#ffffff"
    }
    return toHex(blendOver(Qt.rgba(26 / 255, 26 / 255, 26 / 255, 0.75), bg))
}

// Contrast color as an rgba() string at the given alpha, for canvas fills
function contrastFill(bg, alpha) {
    return luminance(bg) < 0.5
        ? "rgba(255, 255, 255, " + alpha + ")"
        : "rgba(26, 26, 26, " + alpha + ")"
}
