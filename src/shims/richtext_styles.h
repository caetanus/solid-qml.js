#pragma once

// Named paragraph / character styles for RichTextHandler (office item 2) — the model shared by the
// handler, the ODF reader and our ODF writer. Internal to the RichText engine (no Q_OBJECT).
//
// The model:
//   - A StyleSheet holds named styles (paragraph or character family), each with its OWN
//     properties (canonical values, see the Prop list) and a parent; a paragraph style with no
//     parent hangs off "Default Paragraph Style". Names are LibreOffice's, so files interoperate.
//   - A block carries its paragraph style NAME in kParagraphStyleProperty, a character format its
//     character style name in kCharStyleProperty. The formats Qt renders are the EFFECTIVE ones:
//     the style chain materialized (blockFormat / charFormat below) plus direct overrides.
//   - "Direct formatting" = a property whose value differs from what the style gives (compared on
//     normalized values: unset bold == normal weight, …). Re-styling rebases a format: properties
//     that still equal the old style's value take the new style's; the others are direct and stay.

#include <QColor>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QTextBlock>
#include <QTextFormat>
#include <QTextTableFormat>
#include <QVariantMap>

namespace rt {

// Custom QTextFormat properties (Qt keeps unknown user properties on formats, through edits).
constexpr int kParagraphStyleProperty = QTextFormat::UserProperty + 0x5301; // QString, on block formats
constexpr int kCharStyleProperty = QTextFormat::UserProperty + 0x5302;      // QString, on char formats
constexpr int kKeepWithNextProperty = QTextFormat::UserProperty + 0x5303;   // bool, on block formats
constexpr int kLanguageProperty = QTextFormat::UserProperty + 0x5304;      // language tag, on char formats

// Vertical space (px) under a paragraph whose style chain has none: LibreOffice draws such
// paragraphs with no gap; a quarter of a CSS pixel is invisible, and any positive value keeps
// QQuickTextEdit's viewport culling from over-drawing (measured: 0.25 px == 1 px, 0 px ~3x).
// Normalized comparisons treat it as 0.
constexpr qreal kMinParagraphSpacing = 0.25;

enum class Family { Paragraph, Character };

QString defaultParagraphStyle(); // "Default Paragraph Style" (ODF "Standard")
QString textBodyStyle();         // "Text Body" (ODF "Text_20_body")

// Style properties, by their API name. Character ones apply to text runs (a paragraph style may
// set them too: they style the whole paragraph's text); paragraph ones to blocks.
enum class Prop {
    FontFamily, FontSize, FontWeight, Italic, Underline, Strike, Color, LetterSpacing, Language, // character
    MarginTop, MarginBottom, MarginLeft, MarginRight, TextIndent, LineHeight, Alignment,
    OutlineLevel, KeepWithNext,                                                       // paragraph
};
constexpr int kPropCount = int(Prop::KeepWithNext) + 1;
constexpr Prop kFirstBlockProp = Prop::MarginTop;
inline bool isCharProp(Prop p) { return int(p) < int(kFirstBlockProp); }
const char *propName(Prop p);
bool propFromName(QStringView name, Prop *out);

// Canonical value types (what styleProperties returns, what defineStyle accepts after coercion):
//   fontFamily string · fontSize pt (number) · fontWeight 1..1000 (number; "bold"/"normal" ok)
//   italic/underline/strike/keepWithNext bool · color "#rrggbb" · letterSpacing px
//   margin*/textIndent px (number, or a string with a unit: "1cm", "12pt", "0.5in")
//   lineHeight: number = multiple of the line ("150%" ok), or "<n>px" = exact line height
//   alignment "left"|"center"|"right"|"justify" · outlineLevel 0..10 (0 = not a heading)
// Coerces `value` for `p`; false (and `error`) when it can't.
bool canonicalValue(Prop p, const QVariant &value, QVariant *out, QString *error);

// ODF length ("3.5in", "2.4cm", "12pt", "8px", bare number = px) → CSS px (96 dpi). NaN otherwise.
qreal lengthPx(QStringView v);

struct NamedStyle {
    QString name;               // API / UI name ("Heading 1")
    Family family = Family::Paragraph;
    QString parent;             // "" = none (a paragraph style then inherits Default Paragraph Style)
    QString next;               // paragraph styles: style of the paragraph Enter creates ("" = same)
    QVariantMap props;          // OWN properties, canonical, keyed by propName
    QString odfName;            // style:name in files ("Heading_20_1")
    QString odfDisplayName;     // style:display-name written ("" = none)
};

class StyleSheet {
public:
    // LibreOffice's defaults (names, sizes, spacing): Default Paragraph Style, Text Body, Heading,
    // Heading 1..6, Title, Quotations; character styles Emphasis, Strong Emphasis.
    static StyleSheet builtIns();

    const NamedStyle *find(const QString &name) const;
    const NamedStyle *findByOdfName(const QString &odfName) const;
    QStringList names(Family family) const; // definition order
    const QStringList &order() const { return m_order; }
    bool isEmpty() const { return m_order.isEmpty(); }

    // Adds or replaces `s` as is (the caller validated it). Fills odfName when empty.
    void put(const NamedStyle &s);
    void remove(const QString &name);
    // "" when `s` may be put: family/parent/next consistent, no inheritance cycle.
    QString validate(const NamedStyle &s) const;

    // `name`'s properties along its chain (own values win). `skipDocumentFont`: leave out the
    // font family/size Default Paragraph Style gives for app-created documents.
    QVariantMap resolved(const QString &name, bool skipDocumentFont = false) const;
    // `name` is `ancestor` or inherits from it (paragraph styles all inherit the default one).
    bool inherits(const QString &name, const QString &ancestor) const;

    // The materialized formats a style gives (memoized; an unknown name falls back to Default
    // Paragraph Style / no character style). blockFormat carries kParagraphStyleProperty;
    // charFormat the paragraph style's character properties under the character style's, plus
    // kCharStyleProperty when charStyle is not empty.
    // By value: a reference into the cache would dangle when a later lookup grows it.
    QTextBlockFormat blockFormat(const QString &paraStyle) const;
    QTextCharFormat charFormat(const QString &paraStyle, const QString &charStyle) const;
    void useDocumentFont() { m_useDocumentFont = true; m_charCache.clear(); }

private:
    bool m_useDocumentFont = false;
    QHash<QString, NamedStyle> m_styles;
    QStringList m_order;
    mutable QHash<QString, QTextBlockFormat> m_blockCache;
    mutable QHash<QString, QTextCharFormat> m_charCache;
};

// The paragraph style a block is in (a block with none — HTML content, an old document — reads as
// its heading level's style or Default Paragraph Style) / a character format's character style.
QString paragraphStyleOf(const QTextBlockFormat &f);
inline QString paragraphStyleOf(const QTextBlock &b) { return paragraphStyleOf(b.blockFormat()); }
QString charStyleOf(const QTextCharFormat &f);

// Normalized property value of a format (unset == the default look) and the overrides on top of a
// base, keyed by propName.
QVariant readProp(const QTextFormat &f, Prop p);
QVariantMap directProps(const QTextFormat &actual, const QTextFormat &base, bool blockProps);
// Writes canonical `props` (of the format's kind) onto `f` — direct formatting / materialization.
void applyProps(QTextFormat &f, const QVariantMap &props, bool blockProps);
// Rebase `f` from oldBase to newBase: each property still equal to oldBase's (or every one when
// `force`) takes newBase's raw value. Block formats get the minimum paragraph spacing back when
// they end up with none.
void rebase(QTextFormat &f, const QTextFormat &oldBase, const QTextFormat &newBase, bool blockProps, bool force);
void ensureMinSpacing(QTextBlockFormat &f);

// Office item 2b (★2, zoom): a COPY of `f` with every absolute length already baked into it —
// font point size, letter spacing, margins/indent, an exact (fixed) line height, inline image
// width/height — multiplied by `ratio`. Everything else (alignment, bold, the style-name
// properties, …) is untouched, so the result still carries kParagraphStyleProperty/
// kCharStyleProperty. Used both to rescale the LIVE document (ratio = the zoom factor) and, by
// the ODF writer, to read it back in CANONICAL units (ratio = 1 / zoom) — never touching the
// StyleSheet itself, so saveOdf's output does not depend on the zoom in effect.
QTextBlockFormat scaledLengths(const QTextBlockFormat &f, qreal ratio);
QTextCharFormat scaledLengths(const QTextCharFormat &f, qreal ratio);

// ODF style:name ↔ display name: characters outside an NCName are written _XX_ (hex), as
// LibreOffice does ("Heading 1" ↔ "Heading_20_1").
QString odfEncodeName(const QString &name);
QString odfDecodeName(const QString &odfName);

// The table look shared by insertTable and the ODF reader.
QTextTableFormat wordLikeTableFormat();

} // namespace rt
