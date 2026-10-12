#pragma once

class QObject;
class QTextDocument;
class QTextFrameFormat;

// Internal compatibility boundary; not part of the QML or application API.
namespace rt {
void setRootMarginsNoUndo(QTextDocument *document, const QTextFrameFormat &format);
bool refreshTextEditSize(QObject *editor);
// Call before editing; false requires retaining/restoring the Qt layout.
bool attachCustomLayoutToTextEdit(QObject *editor, QTextDocument *document, bool documentLinks = true);
}
