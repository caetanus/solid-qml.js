// A stand-in for PRE-EXISTING C++ consumed by a solid-qml project (Direção B of the embed
// plan). The legacy code ships the canonical Qt way — a QML extension plugin (qmldir +
// libacmeplugin.so) — untouched by solid-qml; TSX imports it via
// `import { AcmeGauge } from "qml:Acme"` and the loader finds it through --import-path.
// This file doubles as the template for real legacy modules.
#include <QPainter>
#include <QQmlExtensionPlugin>
#include <QQuickPaintedItem>
#include <qmath.h>

class AcmeGauge : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(qreal value READ value WRITE setValue NOTIFY valueChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)

public:
    explicit AcmeGauge(QQuickItem *parent = nullptr)
        : QQuickPaintedItem(parent)
    {
        setImplicitWidth(140);
        setImplicitHeight(140);
    }

    qreal value() const { return m_value; }
    void setValue(qreal v)
    {
        v = qBound<qreal>(0, v, 100);
        if (qFuzzyCompare(m_value, v))
            return;
        m_value = v;
        emit valueChanged();
        update();
    }

    QColor color() const { return m_color; }
    void setColor(const QColor &c)
    {
        if (m_color == c)
            return;
        m_color = c;
        emit colorChanged();
        update();
    }

    void paint(QPainter *p) override
    {
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF r = boundingRect().adjusted(8, 8, -8, -8);
        p->setPen(QPen(QColor("#d7dde5"), 10, Qt::SolidLine, Qt::RoundCap));
        p->drawArc(r, 225 * 16, -270 * 16);
        p->setPen(QPen(m_color, 10, Qt::SolidLine, Qt::RoundCap));
        p->drawArc(r, 225 * 16, int(-270 * 16 * m_value / 100.0));
        p->setPen(QColor("#2b2b2b"));
        QFont f = p->font();
        f.setPixelSize(int(r.height() / 4));
        f.setBold(true);
        p->setFont(f);
        p->drawText(r, Qt::AlignCenter, QString::number(qRound(m_value)));
    }

signals:
    void valueChanged();
    void colorChanged();

private:
    qreal m_value = 0;
    QColor m_color = QColor("#1c7690");
};

class AcmePlugin : public QQmlExtensionPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)

public:
    void registerTypes(const char *uri) override
    {
        qmlRegisterType<AcmeGauge>(uri, 1, 0, "AcmeGauge");
    }
};

#include "acmeplugin.moc"
