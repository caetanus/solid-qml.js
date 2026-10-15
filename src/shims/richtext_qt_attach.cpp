#include "richtext_qt_p.h"
#include "richtext_styles.h"

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

#include <QImage>
#include <QPointer>
#include <QEvent>
#include <QPainter>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTextFrame>
#include <private/qtextimagehandler_p.h>
#include <QTextBlock>
#include <QTextFragment>

namespace rt {
namespace {
// Supply the same sampled pixels to exports and Qt Quick image nodes.
class ExportImageHandler final : public QTextImageHandler {
public:
    ExportImageHandler(QTextDocument *doc, QObject *editor) : QTextImageHandler(doc), m_doc(doc), m_editor(editor) {
        if (auto *item = qobject_cast<QQuickItem *>(editor)) {
            const auto watch = [this](QQuickWindow *window) { if (window) window->installEventFilter(this); };
            watch(item->window());
            connect(item, &QQuickItem::windowChanged, this, [this, watch](QQuickWindow *window) {
                watch(window); prepareImageSampling(m_doc, m_editor);
            });
        }
    }
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::DevicePixelRatioChange) prepareImageSampling(m_doc, m_editor);
        return false;
    }
    QSizeF intrinsicSize(QTextDocument *doc, int pos, const QTextFormat &format) override {
        const auto image = format.toImageFormat();
        const qreal relative = format.doubleProperty(kRelativeImageWidthProperty);
        if (relative > 0) {
            const auto root = doc->rootFrame()->frameFormat();
            const qreal width = qMax(1., doc->pageSize().width() - root.leftMargin() - root.rightMargin()) * relative / 100;
            const auto source = originalImage(doc, QUrl(image.name())).value<QImage>();
            const qreal ratio = image.width() > 0 && image.height() > 0 ? image.height() / image.width()
                : source.width() > 0 ? qreal(source.height()) / source.width() : 1.;
            return QSizeF(width, width * ratio);
        }
        return QTextImageHandler::intrinsicSize(doc, pos, format);
    }
    void drawObject(QPainter *p, const QRectF &rect, QTextDocument *doc, int, const QTextFormat &f) override {
        p->save();
        p->setRenderHint(QPainter::SmoothPixmapTransform);
        p->drawImage(rect, doc->resource(QTextDocument::ImageResource, QUrl(f.toImageFormat().name())).value<QImage>());
        p->restore();
    }
private:
    QTextDocument *m_doc;
    QPointer<QObject> m_editor;
};
}
QVariant originalImage(const QTextDocument *doc, const QUrl &name)
{
    const auto current = doc->resource(QTextDocument::ImageResource, name);
    const auto entry = doc->property("_rtImageSampling").toMap().value(name.toString()).toMap();
    return entry.value("key").toLongLong() == current.value<QImage>().cacheKey()
        ? entry.value("source") : current;
}
void installPrintImageHandler(QTextDocument *doc)
{
    doc->documentLayout()->registerHandler(QTextFormat::ImageObject, new ExportImageHandler(doc, nullptr));
}
void prepareImageSampling(QTextDocument *doc, QObject *editor)
{
    if (!doc) return;
    const auto *item = qobject_cast<QQuickItem *>(editor);
    const qreal dpr = item && item->window() ? item->window()->effectiveDevicePixelRatio() : 1.;
    const auto root = doc->rootFrame()->frameFormat();
    QHash<QUrl, QSize> sizes;
    bool hasImages = false;
    for (const auto &f : doc->allFormats()) if (f.isImageFormat()) { hasImages = true; break; }
    if (!hasImages) return;
    auto *layout = doc->documentLayout();
    if (!dynamic_cast<ExportImageHandler *>(layout->handlerForObject(QTextFormat::ImageObject)))
        layout->registerHandler(QTextFormat::ImageObject, new ExportImageHandler(doc, editor));
    for (auto block = doc->begin(); block.isValid(); block = block.next())
    for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
        const auto f = fragment.fragment().charFormat();
        if (!f.isImageFormat()) continue;
        const auto image = f.toImageFormat();
        const QUrl name(image.name());
        QImage source = originalImage(doc, name).value<QImage>();
        if (source.isNull()) continue;
        qreal w = image.width() > 0 ? image.width()
            : image.height() > 0 ? image.height() * source.width() / source.height()
            : source.width() / source.devicePixelRatio();
        qreal h = image.height() > 0 ? image.height() : w * source.height() / source.width();
        if (const qreal relative = f.doubleProperty(kRelativeImageWidthProperty); relative > 0) {
            const qreal width = qMax(1., doc->pageSize().width() - root.leftMargin() - root.rightMargin()) * relative / 100;
            h *= width / w; w = width;
        }
        sizes[name] = QSize(qMax(1, qRound(w * dpr)), qMax(1, qRound(h * dpr)));
    }
    QSet<QUrl> changed;
    auto cache = doc->property("_rtImageSampling").toMap();
    for (auto it = sizes.cbegin(); it != sizes.cend(); ++it) {
        QImage source = originalImage(doc, it.key()).value<QImage>();
        const QImage current = doc->resource(QTextDocument::ImageResource, it.key()).value<QImage>();
        if (current.size() == it.value() && qFuzzyCompare(current.devicePixelRatio(), dpr)) continue;
        QImage sampled = source.scaled(it.value(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        sampled.setDevicePixelRatio(dpr);
        doc->addResource(QTextDocument::ImageResource, it.key(), sampled);
        cache.insert(it.key().toString(), QVariantMap{{"source", source}, {"key", sampled.cacheKey()}});
        changed.insert(it.key());
    }
    doc->setProperty("_rtImageSampling", cache);
    if (!changed.isEmpty())
        for (auto block = doc->begin(); block.isValid(); block = block.next())
            for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
                const auto f = fragment.fragment();
                if (f.charFormat().isImageFormat() && changed.contains(QUrl(f.charFormat().toImageFormat().name())))
                    doc->markContentsDirty(f.position(), f.length());
            }
}
}
