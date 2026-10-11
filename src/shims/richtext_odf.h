#pragma once

// OpenDocument text IO for RichTextHandler (internal to the RichText engine).
//
// Reader: content.xml + styles.xml through the private QZipReader + QXmlStreamReader — the
// word-like subset (paragraphs, headings with their outline level, spans, lists, tables, inline
// images) and the named styles (office:styles → the StyleSheet, automatic styles → direct
// formatting on top of the named style they derive from).
//
// Writer (ours, not QTextDocumentWriter): mimetype (first, stored), META-INF/manifest.xml,
// meta.xml, styles.xml (office:styles: the named paragraph/character styles, so LibreOffice shows
// them as styles) and content.xml (text:h with text:outline-level / text:p, text:span, text:list,
// table:table, draw:frame images; automatic P/T styles carry the direct formatting with
// style:parent-style-name pointing at the named style).

#include "richtext_styles.h"

#include <QString>

class QTextDocument;

namespace rt {

// Reads `path` into `doc` (cleared, rebuilt without undo history) and `sheet` (the built-in styles
// plus the file's). A missing / non-ODF file fails before touching either. false + `error`.
bool readOdt(const QString &path, QTextDocument *doc, StyleSheet *sheet, QString *error);

// Writes `doc` with its `sheet` to `path` (atomically: a failed write leaves the old file).
// `zoom`: the live document's current zoom (office item 2b, ★2) — every length read off `doc`'s
// actual formats is divided by it before being compared/written, so the file is identical at any
// zoom (the StyleSheet itself is never scaled).
bool writeOdt(const QString &path, const QTextDocument *doc, const StyleSheet &sheet, qreal zoom, QString *error);

} // namespace rt
