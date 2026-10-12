#include "richtext_qt_p.h"

#include <QAbstractTextDocumentLayout>
#include <QDebug>
#include <QMetaMethod>
#include <QTextDocument>
#include <array>

namespace rt {
namespace {
// Layout ownership also bounds the document links when switching engines repeatedly.
struct Attachment final : QObject {
    std::array<QMetaObject::Connection, 6> links;
    explicit Attachment(QObject *parent) : QObject(parent) {}
    ~Attachment() override { for (const auto &link : links) QObject::disconnect(link); }
};
}

bool attachCustomLayoutToTextEdit(QObject *editor, QTextDocument *document, bool documentLinks)
{
    struct Link { QObject *sender; const char *signal; const char *receiver; bool notify; };
    if (!editor || !document) {
        qWarning("RichText: custom layout attachment needs a TextEdit and document; retaining Qt");
        return false;
    }
    auto *layout = document->documentLayout();
    const std::array<Link, 6> specs = {{
        {layout, "updateBlock(QTextBlock)", "invalidateBlock(QTextBlock)", false},
        {layout, "update(QRectF)", "moveCursorDelegate()", false},
        {layout, "update(QRectF)", "updateSize()", false},
        {document, "contentsChange(int,int,int)", "q_contentsChange(int,int,int)", false},
        {document, "undoAvailable(bool)", "canUndoChanged()", true},
        {document, "redoAvailable(bool)", "canRedoChanged()", true}
    }};
    std::array<QMetaMethod, 6> sources, receivers;
    // Validate the entire interface even on the in-place route, where document links survive.
    for (size_t i = 0; i < specs.size(); ++i) {
        const auto &spec = specs[i];
        const auto *meta = editor->metaObject();
        const int source = spec.sender->metaObject()->indexOfSignal(spec.signal);
        const int target = spec.notify ? meta->indexOfSignal(spec.receiver) : meta->indexOfSlot(spec.receiver);
        if (source < 0 || target < 0) {
            qWarning("RichText: custom layout attachment missing %s -> %s; retaining Qt", spec.signal, spec.receiver);
            return false;
        }
        sources[i] = spec.sender->metaObject()->method(source);
        receivers[i] = meta->method(target);
    }
    auto *attachment = new Attachment(layout);
    for (size_t i = 0; i < specs.size(); ++i) {
        if (i >= 3 && !documentLinks) continue;
        attachment->links[i] = QObject::connect(specs[i].sender, sources[i], editor, receivers[i]);
        if (!attachment->links[i]) {
            qWarning("RichText: custom layout connection failed for %s; retaining Qt", specs[i].receiver);
            delete attachment; // All or none, including document-owned senders.
            return false;
        }
    }
    return true;
}
} // namespace rt
