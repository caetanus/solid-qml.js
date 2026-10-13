#pragma once

// tmux layout strings (docs/solidterm-tmux-control-mode.md, 2.2) — the split tree of one window,
// as %layout-change and #{window_layout} carry it. Qt-free.
//
//   layout := csum "," cell [ "<" cell { "," cell } ">" ]     ; "<…>" = floating panes (tmux 3.7)
//   cell   := W "x" H "," X "," Y ( "," paneid                 ; leaf: pane id WITHOUT the '%'
//                               | "{" cell { "," cell } "}"     ; left-right (side by side)
//                               | "[" cell { "," cell } "]" )   ; top-bottom (stacked)
//
// The checksum is tmux's layout_checksum over everything after the first comma (layout-custom.c).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tmux {

struct LayoutCell {
    enum Type : uint8_t { Pane, LeftRight, TopBottom };
    Type type = Pane;
    unsigned sx = 0, sy = 0;      // size in cells
    unsigned xoff = 0, yoff = 0;  // offset in cells from the window's top-left
    int pane = -1;                // Pane: the pane id (-1 = none given)
    std::vector<LayoutCell> children;
};

struct Layout {
    LayoutCell root;
    std::vector<LayoutCell> floating; // tmux 3.7 floating panes; SolidTerm ignores them (with a notice)
};

enum class LayoutError : uint8_t { None, Syntax, Checksum, Geometry };
const char *layoutErrorName(LayoutError e);

uint16_t layoutChecksum(std::string_view body);
// Parse and verify (checksum + tmux's own geometry check, layout_check). On error `out` is unspecified.
LayoutError parseLayout(std::string_view text, Layout &out);
// The body (no checksum) and the full string, exactly as tmux's layout_dump writes them.
std::string layoutBody(const Layout &layout);
std::string printLayout(const Layout &layout);
// tmux's layout_check: children of {} share the parent's height and their widths plus one border
// column each add up to the parent's width; [] likewise with rows.
bool layoutCheck(const LayoutCell &cell);
// The leaves, depth first (the pane order tmux shows).
void layoutLeaves(const LayoutCell &cell, std::vector<const LayoutCell *> &out);

} // namespace tmux
