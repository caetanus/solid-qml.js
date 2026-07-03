#pragma once

#include <QObject>
#include <QVariantMap>

// Stand-in carrying the CssQmlItem signature (identity props + a writable `style` sink),
// so loadCss can introspect it and push resolved rules — the reverse-slot apply target.
class CssTargetStub final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString cssId MEMBER m_cssId)
    Q_PROPERTY(QVariant cssAlternateId MEMBER m_cssAlternateId)
    Q_PROPERTY(QVariant cssClass MEMBER m_cssClass NOTIFY cssClassChanged)
    Q_PROPERTY(QString cssPart MEMBER m_cssPart)
    Q_PROPERTY(QVariantMap style MEMBER m_style)

public:
    QString m_cssId;
    QVariant m_cssAlternateId;
    QVariant m_cssClass;
    QString m_cssPart;
    QVariantMap m_style;

signals:
    void cssClassChanged();
};

// A target that does NOT conform to the CssQmlItem signature (no cssId/style) — loadCss
// must refuse it loudly rather than silently doing nothing.
class NonCssStub final : public QObject {
    Q_OBJECT
};

class CssThemeTests final : public QObject {
    Q_OBJECT

private slots:
    void parsesIdClassAndSourceOrder();
    void resolvesDescendantSelectorsWithContext();
    void classAncestorSelectorsScope();
    void exactResolveIgnoresUniversalRules();
    void stripsCommentsAndAtRules();
    void parsesColors();
    void parsesCssDurationsAndEasings();
    void loadCssAppliesReappliesAndPrunes();
    void loadCssAppliesClassOnlyTargets();
    void parsesKeyframesAndAnimation();
    void appliesArrayCssClassFromQJSValue();
};
