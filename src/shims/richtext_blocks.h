#pragma once

// Block identity for the RichText engine (office item 3): every paragraph/block of the live
// QTextDocument carries one of these as its QTextBlockUserData. Qt keeps user data with the
// block node itself (QTextBlockData), so it survives typing inside the block and edits elsewhere;
// it is NOT copied when a block is split (the part after the caret is a NEW node — no data), and
// it is deleted together with the node when a block is removed (a merge, a deleted range, undo).
// That deletion is how the handler learns, exactly and without position bookkeeping, which ids
// left the document: ~BlockInfo() tells its owner.
//
// The ODF reader creates them (owner == nullptr) from text:p/text:h xml:id; the handler adopts
// every block's info after a load (RichTextHandler::rebuildBlockIds).

#include <QString>
#include <QTextBlock>
#include <QTextObject>

class RichTextHandler;

namespace rt {

class BlockInfo final : public QTextBlockUserData {
public:
    explicit BlockInfo(const QString &blockId) : id(blockId) {}
    ~BlockInfo() override; // richtext.cpp: reports the removal to `owner`

    QString id;
    RichTextHandler *owner = nullptr;
    size_t textHash = 0;        // qHash of the block's text at the last scan — tells a text edit from a format-only one
    bool changePending = false; // queued for blockChanged (coalesced per block)
};

// The engine is the only writer of block user data on its documents.
inline BlockInfo *blockInfo(const QTextBlock &b) { return static_cast<BlockInfo *>(b.userData()); }

} // namespace rt
