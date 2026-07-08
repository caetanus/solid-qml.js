#pragma once

#include "qmlcss/cssfill.h"

#include <QVariant>

// ListView (port of ListView.qml). A CssFill "div" wrapper hosting a REAL QtQuick ListView
// (delegate recycling / virtualization) in a snippet. Data is a plain array of strings or
// { label, … }; each row is a `.list-item` (hover + selected state), a click or keyboard commit
// fires selected(item, index). The mount-squelch (QtQuick forces currentIndex=0 on model load)
// and the selection-follows-focus keyboard model ride in the snippet verbatim.
namespace SolidWidgets {

class ListView : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(QVariant __listData READ listData WRITE setListData NOTIFY listDataChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)

public:
    explicit ListView(QQuickItem *parent = nullptr);

    QVariant listData() const { return m_listData; }
    void setListData(const QVariant &v);
    int currentIndex() const { return m_currentIndex; }
    void setCurrentIndex(int v);

signals:
    void listDataChanged();
    void currentIndexChanged();
    void selected(const QVariant &item, int index);

protected:
    void componentComplete() override;

private:
    QVariant m_listData = QVariantList();
    int m_currentIndex = -1;
};

} // namespace SolidWidgets
