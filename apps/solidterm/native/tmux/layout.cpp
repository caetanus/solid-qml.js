#include "layout.h"

namespace tmux {

namespace {

struct Reader {
    std::string_view s;
    size_t i = 0;
    bool eof() const { return i >= s.size(); }
    char peek() const { return i < s.size() ? s[i] : '\0'; }
    bool take(char c)
    {
        if (peek() != c)
            return false;
        ++i;
        return true;
    }
    bool number(unsigned &out)
    {
        const size_t start = i;
        unsigned long long v = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + unsigned(s[i++] - '0');
            if (v > 0xffffffu)
                return false;
        }
        out = unsigned(v);
        return i > start;
    }
    // Digits followed by 'x' at the cursor (a cell starts there), without consuming.
    bool cellAhead() const
    {
        size_t j = i;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9')
            ++j;
        return j > i && j < s.size() && s[j] == 'x';
    }
};

constexpr int kMaxDepth = 64;

bool parseCell(Reader &r, LayoutCell &cell, int depth)
{
    if (depth > kMaxDepth)
        return false;
    if (!r.number(cell.sx) || !r.take('x') || !r.number(cell.sy) || !r.take(',') || !r.number(cell.xoff)
        || !r.take(',') || !r.number(cell.yoff))
        return false;
    // layout_construct: a ',' followed by digits then 'x' starts the NEXT cell (a sibling), so
    // only digits not followed by 'x' are this leaf's pane id.
    if (r.peek() == ',' && !(r.i + 1 < r.s.size() && Reader { r.s, r.i + 1 }.cellAhead())) {
        r.take(',');
        unsigned id;
        if (!r.number(id))
            return false;
        cell.type = LayoutCell::Pane;
        cell.pane = int(id);
        return true;
    }
    const char open = r.peek();
    if (open != '{' && open != '[') {
        cell.type = LayoutCell::Pane; // a leaf without an id
        return true;
    }
    r.take(open);
    const char close = open == '{' ? '}' : ']';
    cell.type = open == '{' ? LayoutCell::LeftRight : LayoutCell::TopBottom;
    for (;;) {
        cell.children.emplace_back();
        if (!parseCell(r, cell.children.back(), depth + 1))
            return false;
        if (r.take(','))
            continue;
        if (r.take(close))
            break;
        return false;
    }
    return true;
}

void dumpCell(const LayoutCell &c, std::string &out)
{
    out += std::to_string(c.sx);
    out += 'x';
    out += std::to_string(c.sy);
    out += ',';
    out += std::to_string(c.xoff);
    out += ',';
    out += std::to_string(c.yoff);
    switch (c.type) {
    case LayoutCell::Pane:
        if (c.pane >= 0) {
            out += ',';
            out += std::to_string(c.pane);
        }
        break;
    case LayoutCell::LeftRight:
    case LayoutCell::TopBottom:
        out += c.type == LayoutCell::LeftRight ? '{' : '[';
        for (size_t k = 0; k < c.children.size(); ++k) {
            if (k)
                out += ',';
            dumpCell(c.children[k], out);
        }
        out += c.type == LayoutCell::LeftRight ? '}' : ']';
        break;
    }
}

// Remove pane `id`'s leaf from the tree; a split left with one child is replaced by that child.
void removePane(LayoutCell &cell, int id)
{
    if (cell.type == LayoutCell::Pane)
        return;
    for (size_t k = 0; k < cell.children.size();) {
        LayoutCell &c = cell.children[k];
        if (c.type == LayoutCell::Pane && c.pane == id) {
            cell.children.erase(cell.children.begin() + long(k));
            continue;
        }
        removePane(c, id);
        ++k;
    }
    if (cell.children.size() == 1) {
        LayoutCell only = std::move(cell.children.front());
        cell = std::move(only);
    }
}

} // namespace

const char *layoutErrorName(LayoutError e)
{
    switch (e) {
    case LayoutError::None: return "ok";
    case LayoutError::Syntax: return "syntax error";
    case LayoutError::Checksum: return "checksum mismatch";
    case LayoutError::Geometry: return "inconsistent geometry";
    }
    return "?";
}

uint16_t layoutChecksum(std::string_view body)
{
    uint16_t csum = 0;
    for (const char ch : body) {
        csum = uint16_t((csum >> 1) + ((csum & 1) << 15));
        csum = uint16_t(csum + uint8_t(ch));
    }
    return csum;
}

LayoutError parseLayout(std::string_view text, Layout &out)
{
    out = Layout {};
    if (text.size() < 6 || text[4] != ',')
        return LayoutError::Syntax;
    unsigned csum = 0;
    for (int k = 0; k < 4; ++k) {
        const char c = text[size_t(k)];
        unsigned d;
        if (c >= '0' && c <= '9')
            d = unsigned(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = unsigned(c - 'a' + 10);
        else
            return LayoutError::Syntax;
        csum = csum * 16 + d;
    }
    const std::string_view body = text.substr(5);
    Reader r { body, 0 };
    if (!parseCell(r, out.root, 0))
        return LayoutError::Syntax;
    if (r.take('<')) {
        for (;;) {
            out.floating.emplace_back();
            if (!parseCell(r, out.floating.back(), 0))
                return LayoutError::Syntax;
            if (r.take(','))
                continue;
            if (r.take('>'))
                break;
            return LayoutError::Syntax;
        }
    }
    if (!r.eof())
        return LayoutError::Syntax;
    if (layoutChecksum(body) != csum)
        return LayoutError::Checksum;
    // tmux 3.7c lists a floating pane twice: in "<…>" AND as an extra child of the tiled root
    // (recorded: "{40x24,0,0,0,39x24,41,0[…],40x6,4,2,7}<40x6,4,2,7>"), which breaks the tiled
    // geometry. The tiled tree is what SolidTerm lays out, so the floating ids leave it first.
    for (const LayoutCell &f : out.floating) {
        std::vector<const LayoutCell *> ids;
        layoutLeaves(f, ids);
        for (const LayoutCell *leaf : ids)
            if (leaf->pane >= 0)
                removePane(out.root, leaf->pane);
    }
    if (!layoutCheck(out.root))
        return LayoutError::Geometry;
    return LayoutError::None;
}

std::string layoutBody(const Layout &layout)
{
    std::string body;
    dumpCell(layout.root, body);
    if (!layout.floating.empty()) {
        body += '<';
        for (size_t k = 0; k < layout.floating.size(); ++k) {
            if (k)
                body += ',';
            dumpCell(layout.floating[k], body);
        }
        body += '>';
    }
    return body;
}

std::string printLayout(const Layout &layout)
{
    const std::string body = layoutBody(layout);
    static const char hex[] = "0123456789abcdef";
    const uint16_t c = layoutChecksum(body);
    std::string out;
    out.reserve(body.size() + 5);
    out += hex[(c >> 12) & 15];
    out += hex[(c >> 8) & 15];
    out += hex[(c >> 4) & 15];
    out += hex[c & 15];
    out += ',';
    out += body;
    return out;
}

bool layoutCheck(const LayoutCell &cell)
{
    if (cell.type == LayoutCell::Pane)
        return true;
    if (cell.children.empty())
        return false;
    unsigned long n = 0;
    for (const LayoutCell &c : cell.children) {
        if (cell.type == LayoutCell::LeftRight) {
            if (c.sy != cell.sy)
                return false;
            n += c.sx + 1;
        } else {
            if (c.sx != cell.sx)
                return false;
            n += c.sy + 1;
        }
        if (!layoutCheck(c))
            return false;
    }
    return n - 1 == (cell.type == LayoutCell::LeftRight ? cell.sx : cell.sy);
}

void layoutLeaves(const LayoutCell &cell, std::vector<const LayoutCell *> &out)
{
    if (cell.type == LayoutCell::Pane) {
        out.push_back(&cell);
        return;
    }
    for (const LayoutCell &c : cell.children)
        layoutLeaves(c, out);
}

} // namespace tmux
