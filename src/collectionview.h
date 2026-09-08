#pragma once
#include <QSortFilterProxyModel>
#include <QCollator>
#include <QVariantMap>
#include <QRandomGenerator>
#include <QRegularExpression>

class CollectionView : public QSortFilterProxyModel {
  Q_OBJECT
  Q_PROPERTY(int count READ count NOTIFY countChanged)
  Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY optionsChanged)
  Q_PROPERTY(QString sortKey READ sortKey WRITE setSortKey NOTIFY optionsChanged)
  Q_PROPERTY(bool sortReverse READ sortReverse WRITE setSortReverse NOTIFY optionsChanged)
public:
  explicit CollectionView(QObject *parent=nullptr):QSortFilterProxyModel(parent) {
    if(m_collator.locale().language()==QLocale::C)m_collator.setLocale(QLocale(QLocale::English));
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);m_collator.setNumericMode(true);
    connect(this,&QAbstractItemModel::modelReset,this,&CollectionView::countChanged);
    connect(this,&QAbstractItemModel::rowsInserted,this,&CollectionView::countChanged);
    connect(this,&QAbstractItemModel::rowsRemoved,this,&CollectionView::countChanged);
    connect(this,&QAbstractItemModel::layoutChanged,this,&CollectionView::countChanged);
  }
  int count() const {return rowCount();}
  QString query() const {return m_query;}
  QString sortKey() const {return m_sort;}
  bool sortReverse() const {return m_reverse;}

  void setQuery(const QString &value) {
    if(m_query==value)return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif
    m_query=value;m_terms=value.simplified().split(' ',Qt::SkipEmptyParts);
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(Direction::Rows);
#else
    invalidateFilter();
#endif
    emit optionsChanged();
  }

  void setSortKey(const QString &value) {
    static const QStringList validKeys = {
        "original", "default", "none", "displayed_text", "title", "album",
        "album_track", "year_album", "year_album_track", "artist_year_album",
        "artist_year_album_track", "last_modified", "date_added", "duration", "shuffle"
    };
    if(!validKeys.contains(value)) return;
    if(value=="shuffle" && m_sort=="shuffle") {
      m_shuffleSeed = QRandomGenerator::global()->generate();
    }
    if(m_sort==value) return;
    m_sort=value;
    if(m_sort=="shuffle" && m_shuffleSeed==0) {
      m_shuffleSeed = QRandomGenerator::global()->generate();
    }
    updateSortOrder();
    emit optionsChanged();
  }

  void setSortReverse(bool reverse) {
    if(m_reverse==reverse) return;
    m_reverse=reverse;
    updateSortOrder();
    emit optionsChanged();
  }

  Q_INVOKABLE QVariantMap get(int row) const {return row>=0&&row<count()?data(index(row,0),Qt::UserRole).toMap():QVariantMap{};}
  Q_INVOKABLE int sourceIndex(int row) const {return row>=0&&row<count()?mapToSource(index(row,0)).row():-1;}
  QVariantList items() const {QVariantList result;result.reserve(count());for(int i=0;i<count();++i)result.append(get(i));return result;}

  static qint64 seconds(const QVariantMap &item) {
    if(item.value("seconds").toLongLong()>0)return item.value("seconds").toLongLong();
    qint64 result=0;const auto parts=item.value("duration").toString().split(':');
    if(parts.size()>3)return 0;
    for(const auto &p:parts){bool ok=false;const auto n=p.toInt(&ok);if(!ok||n<0)return 0;result=result*60+n;}return result;
  }

  static qint64 extractNumber(const QVariantMap &item, const QString &key) {
    const auto val = item.value(key);
    if(val.typeId() == QMetaType::Int || val.typeId() == QMetaType::LongLong) {
      return val.toLongLong();
    }
    const QString str = val.toString().trimmed();
    if(str.isEmpty()) return 0;
    static const QRegularExpression rx(R"(\d+)");
    const auto match = rx.match(str);
    if(match.hasMatch()) {
      return match.captured(0).toLongLong();
    }
    return 0;
  }

  static qint64 extractMtime(const QVariantMap &item) {
    if(item.value("mtime").toLongLong() > 0) return item.value("mtime").toLongLong();
    const QString stamp = item.value("localStamp").toString();
    if(!stamp.isEmpty()) {
      bool ok = false;
      const qint64 ns = stamp.section(':', 0, 0).toLongLong(&ok);
      if(ok && ns > 0) return ns / 1000000000LL;
    }
    return 0;
  }

  static int compareInt(qint64 a, qint64 b) {
    return (a < b) ? -1 : (a > b) ? 1 : 0;
  }

signals:
  void countChanged();
  void optionsChanged();

protected:
  void updateSortOrder() {
    invalidate();
    if(m_sort=="original" || m_sort=="default" || m_sort=="none") {
      if(m_reverse) {
        sort(0, Qt::DescendingOrder);
      } else {
        sort(-1, Qt::AscendingOrder);
      }
    } else {
      sort(0, m_reverse ? Qt::DescendingOrder : Qt::AscendingOrder);
    }
  }

  bool filterAcceptsRow(int row,const QModelIndex &parent) const override {
    if(m_terms.isEmpty())return true;
    const auto item=sourceModel()->data(sourceModel()->index(row,0,parent),Qt::UserRole).toMap();
    const auto text=item.value("title").toString()+' '+item.value("artist").toString()+' '+item.value("album").toString();
    for(const auto &term:m_terms) {if(!text.contains(term,Qt::CaseInsensitive))return false;}
    return true;
  }

  bool lessThan(const QModelIndex &a,const QModelIndex &b) const override {
    const auto x=sourceModel()->data(a,Qt::UserRole).toMap(),y=sourceModel()->data(b,Qt::UserRole).toMap();

    if(m_sort=="original" || m_sort=="default" || m_sort=="none") {
      return a.row() < b.row();
    }

    if(m_sort=="shuffle") {
      const quint32 hX = qHash(x.value("id").toString(), m_shuffleSeed);
      const quint32 hY = qHash(y.value("id").toString(), m_shuffleSeed);
      if(hX != hY) return hX < hY;
      return a.row() < b.row();
    }

    if(m_sort=="duration") {
      const auto xs=seconds(x), ys=seconds(y);
      if(xs!=ys) return xs<ys;
      return a.row()<b.row();
    }

    if(m_sort=="last_modified") {
      const qint64 xm=extractMtime(x), ym=extractMtime(y);
      if(xm!=ym) return xm<ym;
      return a.row()<b.row();
    }

    if(m_sort=="date_added") {
      const qint64 xa=x.value("dateAdded").toLongLong(), ya=y.value("dateAdded").toLongLong();
      if(xa!=ya) return xa<ya;
      return a.row()<b.row();
    }

    if(m_sort=="displayed_text") {
      const QString xt = (x.value("title").toString()+" "+x.value("artist").toString()+" "+x.value("album").toString()).trimmed();
      const QString yt = (y.value("title").toString()+" "+y.value("artist").toString()+" "+y.value("album").toString()).trimmed();
      const int cmp = m_collator.compare(xt, yt);
      if(cmp!=0) return cmp<0;
      return a.row()<b.row();
    }

    if(m_sort=="title") {
      const int cmp = m_collator.compare(x.value("title").toString(),y.value("title").toString());
      if(cmp!=0) return cmp<0;
      return a.row()<b.row();
    }

    if(m_sort=="album") {
      const int cmp = m_collator.compare(x.value("album").toString(),y.value("album").toString());
      if(cmp!=0) return cmp<0;
      return a.row()<b.row();
    }

    if(m_sort=="album_track") {
      int cmp = m_collator.compare(x.value("album").toString(),y.value("album").toString());
      if(cmp!=0) return cmp<0;
      cmp = compareInt(extractNumber(x,"disc"), extractNumber(y,"disc"));
      if(cmp!=0) return cmp<0;
      cmp = compareInt(extractNumber(x,"track"), extractNumber(y,"track"));
      if(cmp!=0) return cmp<0;
      cmp = m_collator.compare(x.value("title").toString(),y.value("title").toString());
      if(cmp!=0) return cmp<0;
      return a.row()<b.row();
    }

    if(m_sort=="year_album") {
      int cmp = compareInt(extractNumber(x,"year"), extractNumber(y,"year"));
      if(cmp!=0) return cmp<0;
      cmp = m_collator.compare(x.value("album").toString(),y.value("album").toString());
      if(cmp!=0) return cmp<0;
      cmp = m_collator.compare(x.value("title").toString(),y.value("title").toString());
      if(cmp!=0) return cmp<0;
      return a.row()<b.row();
    }

    if(m_sort=="year_album_track") {
      int cmp = compareInt(extractNumber(x,"year"), extractNumber(y,"year"));
      if(cmp!=0) return cmp<0;
      cmp = m_collator.compare(x.value("album").toString(),y.value("album").toString());
      if(cmp!=0) return cmp<0;
      cmp = compareInt(extractNumber(x,"disc"), extractNumber(y,"disc"));
      if(cmp!=0) return cmp<0;
      cmp = compareInt(extractNumber(x,"track"), extractNumber(y,"track"));
      if(cmp!=0) return cmp<0;
      cmp = m_collator.compare(x.value("title").toString(),y.value("title").toString());
      if(cmp!=0) return cmp<0;
      return a.row()<b.row();
    }

    if(m_sort=="artist_year_album") {
      int cmp = m_collator.compare(x.value("artist").toString(),y.value("artist").toString());
      if(cmp!=0) return cmp<0;
      cmp = compareInt(extractNumber(x,"year"), extractNumber(y,"year"));
      if(cmp!=0) return cmp<0;
      cmp = m_collator.compare(x.value("album").toString(),y.value("album").toString());
      if(cmp!=0) return cmp<0;
      cmp = m_collator.compare(x.value("title").toString(),y.value("title").toString());
      if(cmp!=0) return cmp<0;
      return a.row()<b.row();
    }

    if(m_sort=="artist_year_album_track") {
      int cmp = m_collator.compare(x.value("artist").toString(),y.value("artist").toString());
      if(cmp!=0) return cmp<0;
      cmp = compareInt(extractNumber(x,"year"), extractNumber(y,"year"));
      if(cmp!=0) return cmp<0;
      cmp = m_collator.compare(x.value("album").toString(),y.value("album").toString());
      if(cmp!=0) return cmp<0;
      cmp = compareInt(extractNumber(x,"disc"), extractNumber(y,"disc"));
      if(cmp!=0) return cmp<0;
      cmp = compareInt(extractNumber(x,"track"), extractNumber(y,"track"));
      if(cmp!=0) return cmp<0;
      cmp = m_collator.compare(x.value("title").toString(),y.value("title").toString());
      if(cmp!=0) return cmp<0;
      return a.row()<b.row();
    }

    const int cmp = m_collator.compare(x.value(m_sort).toString(),y.value(m_sort).toString());
    if(cmp!=0) return cmp<0;
    return a.row()<b.row();
  }

private:
  QString m_query,m_sort="original";
  bool m_reverse = false;
  quint32 m_shuffleSeed = 0;
  QStringList m_terms;
  QCollator m_collator;
};
