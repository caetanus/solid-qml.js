#include "keyrecorder.h"

#include <QKeyEvent>
#include <QKeySequence>
#include <QPainter>

KeyRecorder::KeyRecorder(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setActiveFocusOnTab(true);
    setImplicitWidth(150);
    setImplicitHeight(30);
}

void KeyRecorder::setSequence(const QString &v)
{
    if (m_sequence == v)
        return;
    m_sequence = v;
    // No sequenceChanged here: WRITE is the two-way binding restoring the persisted value; only a
    // user record/clear (below) counts as an edit so the config isn't written back in a loop.
    update();
}

void KeyRecorder::setRecording(bool v)
{
    if (m_recording == v)
        return;
    m_recording = v;
    emit recordingChanged();
    update();
}

void KeyRecorder::mousePressEvent(QMouseEvent *event)
{
    forceActiveFocus(Qt::MouseFocusReason);
    setRecording(true);
    event->accept();
}

void KeyRecorder::focusOutEvent(QFocusEvent *)
{
    setRecording(false);
}

void KeyRecorder::keyPressEvent(QKeyEvent *event)
{
    if (!m_recording) {
        QQuickPaintedItem::keyPressEvent(event);
        return;
    }
    const int key = event->key();
    if (key == Qt::Key_Escape) { setRecording(false); event->accept(); return; }
    if (key == Qt::Key_Backspace || key == Qt::Key_Delete) {
        m_sequence.clear();
        emit sequenceChanged(m_sequence);
        setRecording(false);
        event->accept();
        return;
    }
    // Ignore a lone modifier press — wait for the actual key.
    if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta) {
        event->accept();
        return;
    }
    const QKeySequence seq(event->keyCombination());
    const QString text = seq.toString(QKeySequence::PortableText);
    if (!text.isEmpty()) {
        m_sequence = text;
        emit sequenceChanged(m_sequence);
        setRecording(false);
    }
    event->accept();
}

void KeyRecorder::paint(QPainter *p)
{
    p->setRenderHint(QPainter::Antialiasing);
    const QRectF r = boundingRect().adjusted(0.5, 0.5, -0.5, -0.5);
    p->setPen(QPen(m_recording ? m_accent : m_border, 1));
    p->setBrush(m_background);
    p->drawRoundedRect(r, 7, 7);

    p->setPen(m_recording ? m_accent : m_foreground);
    QFont f = p->font();
    f.setPixelSize(13);
    p->setFont(f);
    const QString label = m_recording ? QStringLiteral("Press keys…  (Esc cancel)")
                        : m_sequence.isEmpty() ? QStringLiteral("Unbound") : m_sequence;
    p->drawText(r.adjusted(10, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
}

void KeyRecorder::setBackground(const QColor &c) { if (m_background == c) return; m_background = c; emit styleChanged(); update(); }
void KeyRecorder::setForeground(const QColor &c) { if (m_foreground == c) return; m_foreground = c; emit styleChanged(); update(); }
void KeyRecorder::setAccent(const QColor &c) { if (m_accent == c) return; m_accent = c; emit styleChanged(); update(); }
void KeyRecorder::setBorder(const QColor &c) { if (m_border == c) return; m_border = c; emit styleChanged(); update(); }
