#include "backend.h"
#include "roundedart.h"
#include "lrc.h"
#include "romanizer.h"
#include <QThread>
#include <QBuffer>
#include <QClipboard>
#include <QImageReader>
#include <QSet>
#include <cmath>
#include <algorithm>
#include <queue>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QUuid>
#ifdef Q_OS_UNIX
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#endif

static QString dataPath() {
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}
static std::shared_ptr<QTemporaryDir> audioDirectory() {
  const auto root=QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  if(!QDir().mkpath(root))return {};
  // Use the application cache filesystem instead of /tmp, which is commonly
  // tmpfs on CachyOS. QTemporaryDir still removes each file on normal teardown.
  return std::make_shared<QTemporaryDir>(root+"/audio-XXXXXX");
}
static QString itemId(const QVariant &v) {
  return v.toMap().value("id").toString();
}
Backend::Backend(QObject *parent) : QObject(parent) {
  m_audio.setVolume(m_settings.value("volume", 0.65).toDouble());
  m_media.setAudioOutput(&m_audio);
  setPlaybackRate(m_settings.value("playbackRate",1.0).toDouble());
  setPreservePitch(m_settings.value("preservePitch",true).toBool());
  connect(&m_media,&QMediaPlayer::playbackRateChanged,this,&Backend::settingsChanged);
  m_collection.setSourceModel(&m_results);
  m_librarySortKey = m_settings.value("collectionSort", "original").toString();
  m_librarySortReverse = m_settings.value("collectionSortReverse", false).toBool();
  applyCollectionSort(m_librarySortKey, m_librarySortReverse, {});
  connect(&m_collection, &CollectionView::optionsChanged, this, [this] {
    if (!m_ignoreSortSignal && m_page != "search") {
      m_librarySortKey = m_collection.sortKey();
      m_librarySortReverse = m_collection.sortReverse();
      m_settings.setValue("collectionSort", m_librarySortKey);
      m_settings.setValue("collectionSortReverse", m_librarySortReverse);
      m_saveTimer.start();
    }
  });
  connect(&m_queue,&Entries::countChanged,this,[this]{
    m_queueSuffix.fill(0,m_queue.count()+1);
    for(int i=m_queue.count()-1;i>=0;--i){
      const auto seconds=m_queue.get(i).value("seconds").toLongLong();
      m_queueSuffix[i]=seconds>0&&seconds<=604800&&m_queueSuffix[i+1]>=0 ? seconds*1000+m_queueSuffix[i+1] : -1;
    }
    emit queueInfoChanged();
  });
  connect(this,&Backend::trackChanged,this,&Backend::queueInfoChanged);
  connect(this,&Backend::trackChanged,this,&Backend::updateAlbumColors);
  connect(this,&Backend::positionChanged,this,&Backend::queueInfoChanged);
  connect(this,&Backend::playbackChanged,this,&Backend::queueInfoChanged);
  connect(this,&Backend::settingsChanged,this,&Backend::queueInfoChanged);
  connect(&m_devices,&QMediaDevices::audioOutputsChanged,this,[this]{applyAudioDevice();emit audioDevicesChanged();});
  applyAudioDevice();
  m_saveTimer.setSingleShot(true);
  m_saveTimer.setInterval(600);
  connect(&m_saveTimer, &QTimer::timeout, this, &Backend::save);
  m_sleepTick.setInterval(60000);
  connect(&m_sleepTick, &QTimer::timeout, this, &Backend::settingsChanged);
  m_sleepTimer.setSingleShot(true);
  m_sleepTimer.setTimerType(Qt::PreciseTimer);
  connect(&m_sleepTimer, &QTimer::timeout, this, [this] {
    pause();
    emit settingsChanged();
    m_sleepTick.stop();
    emit toast("Sleep timer ended");
  });
  // Only invalidate lyric delegates at a timestamp boundary. Position still updates
  // at the original cadence, and the getter remains exact for immediate seeks.
  const auto updateLyricIndex=[this]{
    const auto index=lyricIndex();
    if(index!=m_notifiedLyricIndex){m_notifiedLyricIndex=index;emit lyricIndexChanged();}
  };
  connect(this,&Backend::positionChanged,this,updateLyricIndex);
  connect(this,&Backend::lyricsChanged,this,updateLyricIndex);
  // Throttle visible progress to four updates per second, with no idle timer.
  m_positionTick.setInterval(250);
  connect(&m_positionTick, &QTimer::timeout, this, [this] {
    emit positionChanged();
    if (m_sleepTimer.isActive())
      emit settingsChanged();
  });
  connect(&m_media, &QMediaPlayer::durationChanged, this,
          &Backend::playbackChanged);
  connect(&m_media, &QMediaPlayer::playbackStateChanged, this,
          [this] {
            if(playing()) {m_stopped=false;if(m_uiActive)m_positionTick.start();recordHistory();notifyTrack();QTimer::singleShot(0,this,&Backend::restorePlaybackPosition);} else m_positionTick.stop();
            emit positionChanged(); emit playbackChanged();
          });
  connect(&m_media,&QMediaPlayer::seekableChanged,this,[this](bool seekable){if(seekable)QTimer::singleShot(0,this,&Backend::restorePlaybackPosition);});
  connect(&m_media, &QMediaPlayer::mediaStatusChanged, this,
          [this](QMediaPlayer::MediaStatus s) {
            if (s == QMediaPlayer::LoadedMedia ||
                s == QMediaPlayer::BufferedMedia) {
              m_recovering=false;
              QTimer::singleShot(0,this,&Backend::restorePlaybackPosition);
              if (m_resolving && m_restorePosition<=0) {
                m_resolving = false;
                emit playbackChanged();
              }
            }
            if (s == QMediaPlayer::EndOfMedia) {
              if(m_sleepAtEnd){setSleep(0);pause();emit toast("Sleep timer ended");return;}
              if (repeat() == 2) {
                m_media.setPosition(0);
                m_media.play();
              } else
                next();
            }
          });
  connect(&m_media, &QMediaPlayer::errorOccurred, this,
          [this](QMediaPlayer::Error err, const QString &) {
            if (err == QMediaPlayer::NoError || m_recovering)
              return;
            if(!current().value("localPath").toString().isEmpty()){
              m_resolving=false;m_wantPlay=false;notifyError("Could not play this local file. Check that it is readable and supported.","play");emit playbackChanged();return;
            }
            if (!current().isEmpty() && m_wantPlay && m_recoveryAttempts<2) {
              recoverStream(); return;
            }
            m_resolving=false; m_wantPlay=false;
            emit playbackChanged();
            notifyError("The audio stream was interrupted. Retry to reconnect.","play");
          });
  m_prepareTimer.setSingleShot(true); m_prepareUpdate.setSingleShot(true);
  connect(&m_prepareTimer,&QTimer::timeout,this,&Backend::updatePreparation);
  connect(&m_prepareUpdate,&QTimer::timeout,this,&Backend::updatePreparation);
  const auto schedule=[this]{m_prepareUpdate.start(0);};
  connect(this,&Backend::trackChanged,this,schedule);
  connect(this,&Backend::playbackChanged,this,schedule);
  connect(this,&Backend::settingsChanged,this,schedule);
  connect(this,&Backend::seeked,this,schedule);
  connect(&m_queue,&Entries::countChanged,this,schedule);
  connect(this,&Backend::libraryChanged,this,[this]{if(m_page=="library"&&(m_libraryId.startsWith("mix-")||m_libraryId=="files")){const auto rows=libraryRows(m_libraryId);if(rows!=m_results.rows)m_results.assign(rows);}});
  load();
}
Backend::~Backend() {
  m_notifier.clear();
  save();
  for (const auto &key : m_processes.keys())
    cancel(key);
}
void Backend::cancel(const QString &channel) {
  auto p = m_processes.take(channel);
  if (!p)
    return;
  p->disconnect(this);
  for(auto timer:p->findChildren<QTimer*>()) timer->stop();
  connect(p,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),p,&QObject::deleteLater);
  if(p->processId()>0) ::kill(-p->processId(),SIGKILL);
  if(p->state()==QProcess::Starting) connect(p,&QProcess::started,p,[p]{if(p->processId()>0)::kill(-p->processId(),SIGKILL);p->kill();});
  p->kill();
  if(p->state()==QProcess::NotRunning)p->deleteLater();
}
void Backend::request(const QString &channel, QVariantMap args, Callback done, std::shared_ptr<QTemporaryDir> lifetime) {
  cancel(channel);
  auto p = new QProcess(this);
  // Keep the unique directory alive until the worker has actually exited, even
  // when cancel() disconnects the backend callback while killing its process group.
  if(lifetime) connect(p,&QObject::destroyed,[lifetime]{});
  m_processes.insert(channel, p);
  p->setChildProcessModifier([] { ::setsid(); });
  QString helper = qEnvironmentVariable("SUNG_HELPER");
  if (helper.isEmpty())
    helper = QCoreApplication::applicationDirPath() + "/../helper/catalog.py";
  if (!QFile::exists(helper))
    helper = QCoreApplication::applicationDirPath() + "/../lib/sung/catalog.py";
  QString python = qEnvironmentVariable("SUNG_PYTHON");
  if (python.isEmpty()) {
    auto bundled =
        QCoreApplication::applicationDirPath() + "/../runtime/bin/python";
    if (!QFile::exists(bundled))
      bundled = QCoreApplication::applicationDirPath() +
                "/../lib/sung/runtime/bin/python";
    python = QFile::exists(bundled) ? bundled : QStringLiteral("python3");
  }
  auto timer = new QTimer(p);
  timer->setSingleShot(true);
  timer->setInterval((channel == "play" || channel == "prepare") ? 75000 : 45000);
  connect(timer, &QTimer::timeout, this, [this, channel, done] {
    cancel(channel);
    done({{"ok", false}, {"error", "Connection timed out. Try again."}});
  });
  connect(p, &QProcess::errorOccurred, this,
          [this, p, channel, done](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart)
              return;
            m_processes.remove(channel);
            done({{"ok", false},
                  {"error",
                   "YouTube helper could not start. Run scripts/setup.sh."}});
            p->deleteLater();
          });
  connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this, p, channel, done](int, QProcess::ExitStatus) {
            if (m_processes.value(channel) != p)
              return;
            m_processes.remove(channel);
            const auto bytes = p->readAllStandardOutput();
            const auto obj = QJsonDocument::fromJson(bytes).object();
            QVariantMap result = obj.toVariantMap();
            if (result.isEmpty())
              result = {{"ok", false},
                        {"error", "YouTube helper returned no data. Check the "
                                  "installation and connection."}};
            p->deleteLater();
            done(result);
          });
  p->start(python, {helper});
  p->write(QJsonDocument::fromVariant(args).toJson(QJsonDocument::Compact));
  p->closeWriteChannel();
  timer->start();
}
void Backend::notifyError(const QString &message, const QString &retryTarget) {
  m_retryTarget = retryTarget;
  m_error = message;
  m_error.remove(QRegularExpression("\\x1b\\[[0-9;]*m"));
  if (m_error.contains("Unable to download") ||
      m_error.contains("ConnectionError") || m_error.contains("timed out"))
    m_error =
        "Couldn’t connect to YouTube. Check your connection and try again.";
  else if (m_error.contains("Sign in") || m_error.contains("sign in") ||
           m_error.contains("bot"))
    m_error =
        "YouTube requires sign-in for this track. Import cookies in Settings.";
  else if (m_error.contains("not available") ||
           m_error.contains("Video unavailable"))
    m_error = "This track isn’t available. Try another upload.";
  m_error = m_error.left(350);
  emit errorChanged();
}
QVariantMap Backend::snapshot() const {
  return {{"page", m_page},          {"title", m_title},
          {"cover", m_cover},        {"sections", m_sections},
          {"items", m_results.rows}, {"request", m_request},
          {"more", m_more},          {"library", m_libraryId},
          {"collectionQuery",m_collection.query()},{"collectionSort",m_collection.sortKey()}};
}
void Backend::applyCollectionSort(const QString &key, bool reverse, const QString &query) {
  m_ignoreSortSignal = true;
  m_collection.setQuery(query);
  m_collection.setSortKey(key);
  m_collection.setSortReverse(reverse);
  m_ignoreSortSignal = false;
}
void Backend::restore(const QVariantMap &s) {
  m_page = s.value("page").toString();
  if (m_page == "search") {
    applyCollectionSort("original", false, {});
  } else {
    applyCollectionSort(m_librarySortKey, m_librarySortReverse, s.value("collectionQuery").toString());
  }
  m_title = s.value("title").toString();
  m_cover = s.value("cover").toString();
  m_sections = s.value("sections").toList();
  m_results.assign(s.value("items").toList());
  m_request = s.value("request").toMap();
  m_more = s.value("more").toBool();
  m_libraryId = s.value("library").toString();
  m_busy = false;
  emit catalogChanged();
}
void Backend::navigate(const QString &page, const QString &title, bool push) {
  cancel("catalog");
  if (push) {
    m_back.append(snapshot());
    if (m_back.size() > 12)
      m_back.removeFirst();
  }
  m_page = page;
  if (m_page == "search") {
    applyCollectionSort("original", false, {});
  } else {
    applyCollectionSort(m_librarySortKey, m_librarySortReverse, {});
  }
  m_title = title;
  m_cover.clear();
  m_results.assign({});
  m_sections.clear();
  m_request.clear();
  m_more = false;
  m_busy = false;
  m_libraryId.clear();
  dismissError();
  emit catalogChanged();
}
void Backend::back() {
  if (m_back.isEmpty())
    return;
  cancel("catalog");
  auto last = m_back.takeLast().toMap();
  restore(last);
  dismissError();
  if(m_page=="local") {
    bool found=false;
    for(const auto &v:m_playlists)if(v.toMap().value("id")==m_libraryId){found=true;m_title=v.toMap().value("title").toString();m_results.assign(v.toMap().value("tracks").toList());}
    if(!found)library("playlists");
  } else if(m_page=="library") m_results.assign(libraryRows(m_libraryId));
  emit catalogChanged();
}
void Backend::startSearch() {
  if (m_page != "search")
    navigate("search", "Search");
}
void Backend::retry() {
  auto target = m_retryTarget;
  dismissError();
  if (target == "play") {
    m_wantPlay=true; m_stopped=false; m_recoveryAttempts=1;
    m_restorePosition=position(); m_streams.remove(current().value("videoId").toString());
    resolveCurrent(true);
  }
  else if (target == "catalog")
    refresh();
}
void Backend::home() {
  QVariantMap req{{"op", "home"}};
  const QString ck = cookies();
  if (!ck.isEmpty()) req["cookies"] = ck;
  const QString oa = m_settings.value("oauthFile").toString();
  if (!oa.isEmpty()) req["oauthFile"] = oa;
  browseRequest(req, m_page != "home");
}
void Backend::search(const QString &query, const QString &filter) {
  if (query.trimmed().isEmpty())
    return;
  if (query.trimmed().startsWith("https://") || query.trimmed().startsWith("http://")) {
    openLink(query.trimmed());
    return;
  }
  rememberSearch(query);
  browseRequest({{"op", "search"},
                 {"query", query.trimmed()},
                 {"filter", filter},
                 {"limit", 5}},
                m_page != "search");
}
void Backend::browseRequest(QVariantMap req, bool push) {
  auto op = req.value("op").toString();
  QString label =
      req.value("title",
                op == "home" ? "Listen" : req.value("query", "Loading…"))
          .toString();
  const bool extend=!push && op==m_page && req.value("limit").toInt()>m_request.value("limit").toInt();
  if(push || op!=m_page) navigate(op,label,push);
  else {cancel("catalog");dismissError();}
  m_request = req;
  m_busy = true;
  emit catalogChanged();
  request("catalog", req, [this, op, extend](const QVariantMap &data) {
    m_busy = false;
    if (!data.value("ok").toBool()) {
      notifyError(data.value("error").toString(), "catalog");
      emit catalogChanged();
      return;
    }
    const auto incoming=data.value("items").toList();
    bool prefix=extend && incoming.size()>=m_results.count();
    for(int i=0;prefix && i<m_results.count();++i) prefix=itemId(incoming[i])==itemId(m_results.rows[i]);
    if(prefix)m_results.append(incoming.mid(m_results.count())); else m_results.assign(incoming);
    m_sections = data.value("sections").toList();
    if (data.contains("title"))
      m_title = data.value("title").toString();
    m_cover = data.value("art").toString();
    const auto limit = m_request.value("limit", op == "search" ? 5 : 30).toInt();
    m_more = (op == "search" && m_results.count() >= limit && limit < 200) ||
             (op == "playlist" &&
              data.value("total").toInt() > m_results.count() && limit < 5000);
    emit catalogChanged();
  });
}
void Backend::more() {
  if (!m_more || m_busy)
    return;
  auto req = m_request;
  const int step = (m_page == "search" ? 5 : 100);
  req["limit"] =
      req.value("limit", m_page == "search" ? 5 : 30).toInt() + step;
  browseRequest(req, false);
}
void Backend::refresh() {
  if (m_page == "home") {
    home();
    return;
  }
  if (!m_request.isEmpty())
    browseRequest(m_request, false);
  else if (m_page == "library")
    library(m_libraryId);
}
void Backend::open(const QVariantMap &item) {
  auto kind = item.value("kind").toString();
  if(kind=="smart"){library(item.value("id").toString());return;}
  if(kind=="local"){openPlaylist(item.value("id").toString());return;}
  if (kind == "song" || kind == "video") {
    playItem(item);
    return;
  }
  auto op = kind == "artist"  ? "artist"
            : kind == "album" ? "album"
                              : "playlist";
  auto id = item.value("browseId", item.value("id")).toString();
  if (id.isEmpty())
    return;
  browseRequest(
      {{"op", op}, {"id", id}, {"title", item.value("title")}, {"limit", 100}});
}
void Backend::openLink(const QString &url) {
  browseRequest(
      {{"op", "link"}, {"url", url.trimmed()}, {"title", "YouTube Music"}});
}
QVariantList Backend::playable(const QVariantList &items) {
  QVariantList r;
  for (const auto &i : items) {
    const auto t = i.toMap();
    if ((!t.value("videoId").toString().isEmpty() || (t.value("id").toString().startsWith("local_") && QDir::isAbsolutePath(t.value("localPath").toString()))) &&
        t.value("available", true).toBool())
      r.append(t);
  }
  return r;
}
void Backend::playResults(int index) {
  if(index<0 || index>=m_results.count())return;
  invalidateUndo("queue");
  const auto target = m_results.get(index);
  auto items = playable(m_results.rows);
  if (items.isEmpty())
    return;
  int actual = 0;
  for (int i = 0; i < items.size(); ++i)
    if (itemId(items[i]) == target.value("id").toString()) {
      actual = i;
      break;
    }
  m_queue.assign(items);
  playAt(actual);
}
void Backend::playItem(const QVariantMap &item) {
  if (playable({item}).isEmpty())
    return;
  invalidateUndo("queue");
  m_queue.assign({item});
  playAt(0);
}
void Backend::playAt(int index) {
  if (index < 0 || index >= m_queue.count())
    return;
  invalidateUndo("queue-order");
  if(m_sleepAtEnd)setSleep(0);
  m_wantPlay = true; m_stopped=false; m_recoveryAttempts=0; m_recovering=false; ++m_trackToken; m_savedPosition=0;
  cancel("play");
  cancel("linkplay");
  cancel("lyrics");
  cancel("radio");
  m_media.stop();
  m_media.setSource(QUrl());
  m_index = index;
  dismissError();
  m_retry = false;
  m_restorePosition = 0;
  m_lyricsLoaded=false;m_lyricsSource.clear();m_lyrics.clear();m_lyricLines.clear();
  m_lyricsBusy = false;
  emit lyricsChanged();
  emit trackChanged();
  emit libraryChanged();
  resolveCurrent();
  m_saveTimer.start();
}
void Backend::restorePlaybackPosition() {
  if(m_restorePosition<=0 || !m_media.isSeekable() || m_media.playbackState()==QMediaPlayer::StoppedState)return;
  const auto target=m_restorePosition;
  m_restorePosition=0;
  m_savedPosition=target;
  m_media.setPosition(target);
  m_resolving=false;
  emit positionChanged();emit playbackChanged();
}
void Backend::recoverStream() {
  if(m_recovering)return;
  m_recovering=true;
  m_restorePosition=qMax(m_restorePosition,position());
  m_resolving=true;
  emit playbackChanged();
  ++m_recoveryAttempts;
  m_streams.remove(current().value("videoId").toString());
  // Defer resetting the media source until the decoder's error callback unwinds.
  const auto token=m_trackToken;
  QTimer::singleShot(0,this,[this,token]{if(m_wantPlay && token==m_trackToken){m_media.stop();m_media.setSource({});resolveCurrent(true);}});
}
void Backend::resolveCurrent(bool retry) {
  if(!current().value("localPath").toString().isEmpty()) {
    cancelPreparation();m_audioCache.reset();m_recovering=false;m_resolving=false;
    const QFileInfo file(current().value("localPath").toString());
    if(!file.isFile()||!file.isReadable()){m_wantPlay=false;notifyError("Local file is missing or unreadable. Locate it from the song menu.","play");emit playbackChanged();return;}
    if(!retry&&m_savedPosition>0)m_restorePosition=m_savedPosition;
    m_media.setSource(QUrl::fromLocalFile(file.absoluteFilePath()));
    if(m_wantPlay)m_media.play();
    emit playbackChanged();return;
  }
  auto id = current().value("videoId").toString();
  if (id.isEmpty())
    return;
  m_retry = retry;
  m_stopped=false;
  m_resolving = true;
  emit playbackChanged();
  auto apply = [this, id](const QVariantMap &data) {
    if (current().value("videoId").toString() != id)
      return;
    if (!data.value("ok").toBool()) {
      m_recovering=false;
      if(m_wantPlay && m_recoveryAttempts==1){recoverStream();return;}
      m_resolving = false;
      m_wantPlay = false;
      notifyError(data.value("error").toString(), "play");
      emit playbackChanged();
      return;
    }
    if(!data.contains("file"))m_streams[id] = data;
    if (m_streams.size() > 5) {
      auto keys = m_streams.keys();
      for (const auto &k : keys)
        if (k != id) {
          m_streams.remove(k);
          break;
        }
    }
    auto url = QUrl(data.value("url").toString());
    const auto file=data.value("file").toString();
    if(!file.isEmpty() && m_audioCache && QFileInfo(file).isFile() && QFileInfo(file).canonicalPath()==QFileInfo(m_audioCache->path()).canonicalFilePath())url=QUrl::fromLocalFile(file);
    if (url.scheme() != "https" && !url.isLocalFile()) {
      m_resolving = false;
      notifyError("The stream URL is invalid.");
      emit playbackChanged();
      return;
    }
    if(m_media.source()==url)m_media.setSource({});
    m_media.setSource(url);
    if (m_wantPlay)
      m_media.play();
  };
  if(!retry && m_preparedId==id && !m_preparedData.isEmpty()) {
    m_audioCache=std::move(m_preparedDirectory);const auto data=m_preparedData;
    m_preparedData.clear();m_preparedId.clear();apply(data);return;
  }
  cancelPreparation();
  auto cached = m_streams.value(id);
  if (!retry && !cached.isEmpty()) {
    const auto expire = QUrlQuery(QUrl(cached.value("url").toString()))
                            .queryItemValue("expire")
                            .toLongLong();
    if (expire > QDateTime::currentSecsSinceEpoch() + 120) {
      apply(cached);
      return;
    }
  }
  if(!retry && m_media.source().isEmpty() && m_savedPosition>0)m_restorePosition=m_savedPosition;
  m_audioCache=audioDirectory();
  if(!m_audioCache||!m_audioCache->isValid()){m_resolving=false;m_wantPlay=false;notifyError("Could not create the audio buffer.");emit playbackChanged();return;}
  request("play", {{"op", "buffer"}, {"id", id}, {"cookies", cookies()},
                    {"fallback",m_recoveryAttempts>0},{"directory",m_audioCache->path()}},
          apply,m_audioCache);
}
void Backend::enqueue(const QVariantMap &item, bool next) {
  if (playable({item}).isEmpty())
    return;
  invalidateUndo("queue");
  auto q = m_queue.rows;
  q.insert(next ? qBound(0, m_index + 1, int(q.size())) : q.size(), item);
  m_queue.assign(q);
  m_saveTimer.start();
  emit toast(next ? "Playing next" : "Added to queue");
}
void Backend::enqueueResults() {
  invalidateUndo("queue");
  auto q = m_queue.rows;
  q.append(playable(m_results.rows));
  m_queue.assign(q);
  m_saveTimer.start();
  emit toast("Added to queue");
}
void Backend::moveQueue(int from, int to) {
  if (from < 0 || to < 0 || from >= m_queue.count() || to >= m_queue.count() ||
      from == to)
    return;
  invalidateUndo("queue");
  auto q = m_queue.rows;
  q.move(from, to);
  if (m_index == from)
    m_index = to;
  else if (from < m_index && to >= m_index)
    --m_index;
  else if (from > m_index && to <= m_index)
    ++m_index;
  m_queue.assign(q);
  emit trackChanged();
  m_saveTimer.start();
}
void Backend::removeQueue(int i) {
  if (i < 0 || i >= m_queue.count())
    return;
  m_undoType="queue"; m_undoRows=m_queue.rows; m_undoIndex=m_index; m_undoMessage="Removed from queue";
  auto q = m_queue.rows;
  q.removeAt(i);
  bool currentRemoved = i == m_index;
  bool wasPlaying = playing() || m_resolving;
  if (i < m_index)
    --m_index;
  m_queue.assign(q);
  if (currentRemoved) {
    stop();
    ++m_trackToken; clearLyrics();
    m_index = q.isEmpty() ? -1 : qMin(i, int(q.size() - 1));
    if (wasPlaying && m_index >= 0)
      playAt(m_index);
  }
  emit trackChanged();
  emit libraryChanged();
  m_saveTimer.start();
  emit toast(m_undoMessage);
}
void Backend::clearQueue() {
  if(m_queue.count()==0)return;
  if (m_queue.count() > 0) {
    m_undoType = "queue";
    m_undoRows = m_queue.rows;
    m_undoIndex = m_index;
    m_undoMessage = "Queue cleared";
  }
  stop();
  m_index = -1;
  clearLyrics();
  m_queue.assign({});
  emit trackChanged();
  emit libraryChanged();
  m_saveTimer.start();
  if (!m_undoMessage.isEmpty()) emit toast(m_undoMessage);
}
void Backend::smartShuffleQueue() {
  const int first=qMax(0,m_index+1);if(m_queue.count()-first<2)return;
  auto artistKey=[](const QVariantMap &t){const auto artist=t.value("artist").toString().simplified().toCaseFolded();return artist.isEmpty()?t.value("id").toString():artist;};
  QHash<QString,QVariantList> artists;
  for(int i=first;i<m_queue.count();++i){const auto t=m_queue.get(i);artists[artistKey(t)].append(t);}
  auto keys=artists.keys();std::shuffle(keys.begin(),keys.end(),*QRandomGenerator::global());
  std::priority_queue<std::pair<int,int>> pending;
  for(int i=0;i<keys.size();++i){auto &items=artists[keys[i]];std::shuffle(items.begin(),items.end(),*QRandomGenerator::global());pending.emplace(items.size(),i);}
  auto ordered=m_queue.rows.mid(0,first);auto previous=artistKey(current());
  while(!pending.empty()){
    auto chosen=pending.top();pending.pop();
    if(keys[chosen.second]==previous&&!pending.empty()){auto other=pending.top();pending.pop();pending.push(chosen);chosen=other;}
    auto &items=artists[keys[chosen.second]];ordered.append(items.takeLast());previous=keys[chosen.second];
    if(!items.isEmpty())pending.emplace(items.size(),chosen.second);
  }
  m_undoType="queue-order";m_undoRows=m_queue.rows;m_undoIndex=m_index;m_undoShuffle=shuffle();m_undoMessage="Upcoming songs shuffled";
  setShuffle(false);m_queue.assign(ordered);m_saveTimer.start();emit libraryChanged();emit toast(m_undoMessage);
}
void Backend::next() {
  if (m_queue.count() == 0)
    return;
  if (shuffle() && m_queue.count() > 1) {
    int i = m_index;
    while (i == m_index)
      i = QRandomGenerator::global()->bounded(m_queue.count());
    playAt(i);
    return;
  }
  if (m_index + 1 < m_queue.count()) {
    playAt(m_index + 1);
    return;
  }
  if (repeat() == 1) {
    playAt(0);
    return;
  }
  if (autoplay() && !current().value("videoId").toString().isEmpty()) {
    m_resolving = true;
    emit playbackChanged();
    const QString id = current().value("id").toString();
    request("radio", {{"op", "radio"}, {"id", id}},
            [this, id](const QVariantMap &data) {
              m_resolving = false;
              emit playbackChanged();
              if (id != current().value("id").toString())
                return;
              if (!data.value("ok").toBool()) {
                notifyError(data.value("error").toString());
                return;
              }
              auto q = m_queue.rows;
              auto rows = playable(data.value("items").toList());
              for (const auto &t : rows) {
                bool exists = false;
                for (const auto &old : q)
                  if (itemId(old) == itemId(t)) {
                    exists = true;
                    break;
                  }
                if (!exists)
                  q.append(t);
              }
              m_queue.assign(q);
              if (m_index + 1 < q.size())
                playAt(m_index + 1);
              else
                pause();
            });
  } else
    pause();
}
void Backend::previous() {
  if (position() > 3000) {
    seek(0);
    return;
  }
  if (m_index > 0)
    playAt(m_index - 1);
  else
    seek(0);
}
void Backend::toggle() {
  if (playing() || m_resolving)
    pause();
  else
    play();
}
void Backend::play() {
  if (m_queue.count() == 0)
    return;
  m_wantPlay = true; m_stopped=false;
  if (m_queue.count() == 0)
    return;
  if (m_index < 0)
    playAt(0);
  else if (m_media.source().isEmpty() ||
           m_media.error() != QMediaPlayer::NoError)
    resolveCurrent();
  else
    m_media.play();
}
void Backend::pause() {
  cancel("linkplay");
  m_recovering=false;
  m_savedPosition=position();
  m_wantPlay = false;
  if (m_resolving) {
    cancel("play");
    cancel("radio");
    m_resolving = false;
  }
  m_media.pause();
  emit playbackChanged();
}
void Backend::stop() {
  if(m_sleepAtEnd)setSleep(0);
  cancel("linkplay");
  m_recovering=false;
  m_stopped=true; m_savedPosition=0; m_restorePosition=0;
  clearLyrics();
  m_wantPlay = false;
  cancel("play");
  cancel("radio");
  m_resolving = false;
  m_media.stop();
  m_media.setSource(QUrl());
  emit playbackChanged();
}
void Backend::seek(qint64 p) {
  const auto target=qBound<qint64>(0,p,duration());
  if(m_media.source().isEmpty()){m_savedPosition=target;emit positionChanged();}
  else m_media.setPosition(target);
  emit positionChanged();
  emit seeked(target);
}
void Backend::radio(const QVariantMap &item) {
  auto id = item.value("videoId").toString();
  if (id.isEmpty())
    return;
  playItem(item);
  request("radio", {{"op", "radio"}, {"id", id}},
          [this, id](const QVariantMap &data) {
            if (id != current().value("id").toString())
              return;
            if (!data.value("ok").toBool()) {
              notifyError(data.value("error").toString());
              return;
            }
            auto q = m_queue.rows;
            for (const auto &t : playable(data.value("items").toList()))
              if (itemId(t) != id)
                q.append(t);
            m_queue.assign(q);
            m_saveTimer.start();
            emit toast("Radio started");
          });
}
void Backend::applyLyrics(const QVariantMap &data) {
  m_trackToken++;
  m_lyricsBusy=false;m_lyricsLoaded=data.value("ok").toBool();
  m_lyricLines=data.contains("lrc")?Lrc::parse(data.value("lrc").toString(),duration()):data.value("lines").toList();
  m_lyrics=m_lyricLines.isEmpty()?data.value("lyrics").toString():Lrc::plain(m_lyricLines);
  m_romanizedLyricLines.clear();
  m_romanizedLyrics.clear();
  m_romanizedLyricsFinished = false;
  m_romanizedLyricsBusy = false;
  m_lyricsSource=m_lyrics.isEmpty()?QString():data.value("source","YouTube").toString();
  emit positionChanged();emit lyricsChanged();
  startAsyncRomanizationIfNeeded();
}
void Backend::startAsyncRomanizationIfNeeded() {
  if (m_lyrics.isEmpty()) {
    return;
  }
  if (!Romanizer::containsNonLatin(m_lyrics)) {
    m_romanizedLyricLines = m_lyricLines;
    m_romanizedLyrics = m_lyrics;
    m_romanizedLyricsFinished = true;
    emit lyricsChanged();
    return;
  }
  if (m_romanizedLyricsFinished || m_romanizedLyricsBusy) {
    return;
  }

  m_romanizedLyricLines = m_lyricLines;
  m_romanizedLyrics = m_lyrics;
  m_romanizedLyricsBusy = true;

  const auto token = m_trackToken;
  const auto id = current().value("id").toString();
  const auto linesToRomanize = m_lyricLines;
  const auto textToRomanize = m_lyrics;
  const int currentIdx = lyricIndex();

  QThread::create([this, token, id, linesToRomanize, textToRomanize, currentIdx]() {
    const int CHUNK_SIZE = 12;

    if (!linesToRomanize.isEmpty()) {
      const int totalLines = linesToRomanize.size();
      const int numChunks = (totalLines + CHUNK_SIZE - 1) / CHUNK_SIZE;

      int startChunk = 0;
      if (currentIdx >= 0 && currentIdx < totalLines) {
        startChunk = currentIdx / CHUNK_SIZE;
      }

      QList<int> chunkOrder;
      chunkOrder.reserve(numChunks);
      chunkOrder.append(startChunk);

      for (int step = 1; step < numChunks; ++step) {
        int right = startChunk + step;
        int left = startChunk - step;
        if (right < numChunks) chunkOrder.append(right);
        if (left >= 0) chunkOrder.append(left);
      }

      for (int cIdx : chunkOrder) {
        if (token != m_trackToken || id != current().value("id").toString()) {
          return;
        }

        const int start = cIdx * CHUNK_SIZE;
        const int end = qMin(start + CHUNK_SIZE, totalLines);

        QVariantList chunkRomanized;
        chunkRomanized.reserve(end - start);
        for (int i = start; i < end; ++i) {
          auto lineMap = linesToRomanize[i].toMap();
          const QString origText = lineMap.value("text").toString();
          lineMap["text"] = Romanizer::romanizeLine(origText);
          chunkRomanized.append(lineMap);
        }

        if (token != m_trackToken || id != current().value("id").toString()) {
          return;
        }

        QMetaObject::invokeMethod(this, [this, token, id, start, end, chunkRomanized]() {
          if (token != m_trackToken || current().value("id").toString() != id) {
            return;
          }
          for (int i = start; i < end && (i - start) < chunkRomanized.size(); ++i) {
            if (i < m_romanizedLyricLines.size()) {
              m_romanizedLyricLines[i] = chunkRomanized[i - start];
            }
          }
          QStringList parts;
          parts.reserve(m_romanizedLyricLines.size());
          for (const auto &v : m_romanizedLyricLines) {
            parts.append(v.toMap().value("text").toString());
          }
          m_romanizedLyrics = parts.join('\n');
          emit lyricsChanged();
        });
      }
    } else {
      const QStringList rawLines = textToRomanize.split('\n');
      const int totalLines = rawLines.size();
      const int numChunks = (totalLines + CHUNK_SIZE - 1) / CHUNK_SIZE;

      for (int cIdx = 0; cIdx < numChunks; ++cIdx) {
        if (token != m_trackToken || id != current().value("id").toString()) {
          return;
        }

        const int start = cIdx * CHUNK_SIZE;
        const int end = qMin(start + CHUNK_SIZE, totalLines);

        QStringList chunkRomanized;
        chunkRomanized.reserve(end - start);
        for (int i = start; i < end; ++i) {
          chunkRomanized.append(Romanizer::romanizeLine(rawLines[i]));
        }

        if (token != m_trackToken || id != current().value("id").toString()) {
          return;
        }

        QMetaObject::invokeMethod(this, [this, token, id, start, end, totalLines, chunkRomanized]() {
          if (token != m_trackToken || current().value("id").toString() != id) {
            return;
          }
          QStringList currentLines = m_romanizedLyrics.split('\n');
          while (currentLines.size() < totalLines) {
            currentLines.append(QString());
          }
          for (int i = start; i < end && (i - start) < chunkRomanized.size(); ++i) {
            if (i < currentLines.size()) {
              currentLines[i] = chunkRomanized[i - start];
            }
          }
          m_romanizedLyrics = currentLines.join('\n');
          emit lyricsChanged();
        });
      }
    }

    QMetaObject::invokeMethod(this, [this, token, id]() {
      if (token == m_trackToken && current().value("id").toString() == id) {
        m_romanizedLyricsFinished = true;
        m_romanizedLyricsBusy = false;
      }
    });
  })->start();
}
void Backend::setRomanizedLyrics(bool enabled) {
  m_settings.setValue("romanizedLyrics", enabled);
  emit settingsChanged();
  if (enabled && !m_romanizedLyricsFinished) {
    startAsyncRomanizationIfNeeded();
  }
  emit lyricsChanged();
}
void Backend::fetchLyrics() {
  if(current().isEmpty()||m_lyricsBusy||m_lyricsLoaded)return;
  const auto id=current().value("id").toString();
  static const QRegularExpression valid("^(?:[A-Za-z0-9_-]{11}|local_[a-f0-9]{64})$");
  if(valid.match(id).hasMatch()) {
    QFile file(dataPath()+"/lyrics/"+id+".lrc");
    if(file.size()<=262144&&file.open(QIODevice::ReadOnly)) {
      const auto lrc=QString::fromUtf8(file.readAll());
      if(!Lrc::parse(lrc).isEmpty()){applyLyrics({{"ok",true},{"lrc",lrc},{"source","Imported LRC"}});return;}
    }
  }
  const auto local=current().value("localPath").toString();
  if(!local.isEmpty()) {
    const QFileInfo music(local);QFile sidecar(music.path()+"/"+music.completeBaseName()+".lrc");
    if(sidecar.size()<=262144&&sidecar.open(QIODevice::ReadOnly)){const auto text=QString::fromUtf8(sidecar.read(262145));if(!Lrc::parse(text).isEmpty()){applyLyrics({{"ok",true},{"lrc",text},{"source","Local LRC"}});return;}}
  }
  m_lyricsBusy=true;emit lyricsChanged();const auto token=m_trackToken;
  request("lyrics",{{"op",local.isEmpty()?"lyrics":"local-lyrics"},{"id",id},{"title",current().value("title")},{"artist",current().value("artist")},{"album",current().value("album")},{"seconds",duration()/1000},{"fallback",lyricsFallback()},{"lyricCache",QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/lyrics"}},[this,id,token](const QVariantMap &data){
    if(id!=current().value("id").toString()||token!=m_trackToken)return;
    applyLyrics(data);if(!data.value("ok").toBool())notifyError(data.value("error").toString());
  });
}
void Backend::reloadLyrics(){clearLyrics();fetchLyrics();}
void Backend::setLyricsFallback(bool enabled){m_settings.setValue("lyricsFallback",enabled);emit settingsChanged();reloadLyrics();}
void Backend::importLyrics(const QUrl &url,const QString &songId){
  static const QRegularExpression valid("^(?:[A-Za-z0-9_-]{11}|local_[a-f0-9]{64})$");
  if(!url.isLocalFile()||!valid.match(songId).hasMatch())return;
  QFile input(url.toLocalFile());
  if(input.size()>262144||!input.open(QIODevice::ReadOnly)){notifyError("Choose an LRC file smaller than 256 KiB.");return;}
  const auto bytes=input.read(262145);
  if(bytes.size()>262144||Lrc::parse(QString::fromUtf8(bytes)).isEmpty()){notifyError("This file has no valid timed lyrics.");return;}
  const QString path=dataPath()+"/lyrics";QDir().mkpath(path);QDir directory(path);
  qint64 size=0;for(const auto &file:directory.entryInfoList({"*.lrc"},QDir::Files))if(file.fileName()!=songId+".lrc")size+=file.size();
  if(size+bytes.size()>8*1024*1024){notifyError("Imported lyrics have reached the 8 MiB limit. Remove an imported lyric first.");return;}
  QSaveFile output(path+"/"+songId+".lrc");
  if(!output.open(QIODevice::WriteOnly)){notifyError("Could not save lyrics.");return;}
  output.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
  if(output.write(bytes)!=bytes.size()||!output.commit()){notifyError("Could not save lyrics.");return;}
  if(current().value("id").toString()==songId)reloadLyrics();
  emit toast("Lyrics imported");
}
void Backend::resetLyrics(){
  const auto id=current().value("id").toString();
  static const QRegularExpression valid("^(?:[A-Za-z0-9_-]{11}|local_[a-f0-9]{64})$");
  if(valid.match(id).hasMatch()) {QFile f(dataPath()+"/lyrics/"+id+".lrc");if(f.exists()&&!f.remove()){notifyError("Could not remove imported lyrics.");return;}}
  reloadLyrics();
}
void Backend::setVolume(double v) {
  v = qBound(0.0, v, 1.0);
  m_audio.setVolume(v);
  m_settings.setValue("volume", v);
  emit settingsChanged();
}
void Backend::setShuffle(bool v) {
  m_settings.setValue("shuffle", v);
  emit settingsChanged();
}
void Backend::setRepeat(int v) {
  m_settings.setValue("repeat", qBound(0, v, 2));
  emit settingsChanged();
}
void Backend::setAutoplay(bool v) {
  m_settings.setValue("autoplay", v);
  emit settingsChanged();
}
void Backend::setMotion(bool v) {
  m_settings.setValue("motion", v);
  emit settingsChanged();
}
void Backend::setTheme(const QString &v) {
  if (v != "dark" && v != "light" && v != "system")
    return;
  m_settings.setValue("theme", v);
  emit settingsChanged();
}
QString Backend::formatTime(qint64 ms) const {
  auto s = qMax<qint64>(0, ms / 1000);
  return s >= 3600
             ? QString("%1:%2:%3")
                   .arg(s / 3600)
                   .arg(s / 60 % 60, 2, 10, QChar('0'))
                   .arg(s % 60, 2, 10, QChar('0'))
             : QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}
void Backend::setSleep(int minutes) {
  m_sleepAtEnd=minutes==-1 && m_index>=0;
  m_sleepTimer.stop();
  m_sleepTick.stop();
  if (minutes > 0) {
    m_sleepTimer.start(qMin(minutes,1440) * 60000);
    m_sleepTick.start();
  }
  emit settingsChanged();
}
QString Backend::sleepLabel() const {
  if(m_sleepAtEnd)return "End of track";
  return m_sleepTimer.isActive()
             ? QString::number((m_sleepTimer.remainingTime() + 59999) / 60000) +
                   " min"
             : "Off";
}
void Backend::copyLink(const QVariantMap &t) {
  if(!t.value("localPath").toString().isEmpty()){QGuiApplication::clipboard()->setText(t.value("localPath").toString());emit toast("Path copied");return;}
  QString url = "https://music.youtube.com/";
  if (!t.value("videoId").toString().isEmpty())
    url += "watch?v=" + t.value("videoId").toString();
  else if (t.value("kind") == "playlist")
    url += "playlist?list=" + t.value("id").toString();
  else
    url += "browse/" + t.value("id").toString();
  QGuiApplication::clipboard()->setText(url);
  emit toast("Link copied");
}
void Backend::setCookieFile(const QUrl &url) {
  QFile file(url.toLocalFile());
  if (!file.open(QIODevice::ReadOnly)) {
    notifyError("Cannot read the cookie file.");
    return;
  }
  const auto bytes = file.read(2 * 1024 * 1024);
  if (!bytes.startsWith("# Netscape HTTP Cookie File") &&
      !bytes.startsWith("# HTTP Cookie File")) {
    notifyError("Choose a Netscape format cookies.txt file.");
    return;
  }
  QDir().mkpath(dataPath());
  auto path = dataPath() + "/cookies.txt";
  QSaveFile out(path);
  if (!out.open(QIODevice::WriteOnly)) {
    notifyError("Cannot save the cookie file.");
    return;
  }
  out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  out.write(bytes);
  if (!out.commit()) {
    notifyError("Could not save cookies.");
    return;
  }
  cancelPreparation();m_streams.clear();
  m_settings.setValue("cookies", path);
  m_youtubeConnected = true;
  m_youtubeAccountName = "Cookies session";
  m_youtubeChannelHandle.clear();
  m_settings.setValue("youtubeConnected", true);
  m_settings.setValue("youtubeAccountName", m_youtubeAccountName);
  m_settings.remove("youtubeChannelHandle");
  emit settingsChanged();
  emit youtubeAccountChanged();
  emit toast("Cookies imported");
  syncYouTubeData();
}
void Backend::clearCookies() {
  const auto p = cookies();
  if (p == dataPath() + "/cookies.txt")
    QFile::remove(p);
  cancelPreparation();m_streams.clear();
  m_settings.remove("cookies");
  m_youtubeConnected = false;
  m_youtubeAccountName.clear();
  m_youtubeChannelHandle.clear();
  m_youtubePlaylists.clear();
  m_youtubeSubscriptions.clear();
  m_settings.remove("youtubeConnected");
  m_settings.remove("youtubeAccountName");
  m_settings.remove("youtubeChannelHandle");
  emit settingsChanged();
  emit youtubeAccountChanged();
  emit youtubeDataChanged();
}
void Backend::clearCache() {
  const auto p =
      QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/art";
  QDir(p).removeRecursively();
  m_streams.clear();
  m_paletteCache.clear();
  emit artworkCacheCleared();
  emit toast("Cache cleared");
}
void Backend::setUiActive(bool active) {
  if(m_uiActive==active)return;
  m_uiActive=active;
  if(active){
    emit positionChanged();
    if(playing())m_positionTick.start();
  }else m_positionTick.stop();
}
void Backend::localTestSource(const QUrl &url) {
  m_stopped=false;
  m_media.setSource(url);
  m_media.play();
}

void Backend::load() {
  QFile f(dataPath() + "/library.json");
  if (!f.open(QIODevice::ReadOnly))
    return;
  QJsonParseError parse;
  const auto document=QJsonDocument::fromJson(f.readAll(),&parse);
  if(parse.error!=QJsonParseError::NoError || !document.isObject()) {m_storageHealthy=false;notifyError("Your saved library could not be read. The original file has been preserved.");return;}
  auto d = document.object().toVariantMap();
  m_localTracks=playable(d.value("localTracks").toList());
  m_musicFolders=d.value("musicFolders").toStringList().mid(0,64);
  m_favorites = d.value("favorites").toList();
  m_history = d.value("history").toList();
  m_lastPlayed=d.value("lastPlayed").toMap();
  // Legacy history proves a play, but provides no trustworthy date.
  for(const auto &v:m_history)if(!m_lastPlayed.contains(itemId(v)))m_lastPlayed[itemId(v)]=0;
  m_playlists = d.value("playlists").toList();
  m_pins = d.value("pins").toList().mid(0,24);
  m_lyricOffsets=d.value("lyricOffsets").toMap();
  m_queue.assign(playable(d.value("queue").toList()));
  m_index = qBound(-1, d.value("index", -1).toInt(), m_queue.count() - 1);
  m_savedPosition=qMax<qint64>(0,d.value("position").toLongLong());
  m_youtubeConnected = m_settings.value("youtubeConnected", false).toBool();
  m_youtubeAccountName = m_settings.value("youtubeAccountName").toString();
  m_youtubeChannelHandle = m_settings.value("youtubeChannelHandle").toString();
  if (m_youtubeConnected) {
    syncYouTubeData();
  }
}
void Backend::save() {
  if(!m_storageHealthy)return;
  QDir().mkpath(dataPath());
  QSaveFile f(dataPath() + "/library.json");
  if (!f.open(QIODevice::WriteOnly)) {
    notifyError("Could not save your library.");
    return;
  }
  f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  f.write(QJsonDocument::fromVariant(QVariantMap{{"musicFolders",m_musicFolders},{"localTracks",m_localTracks},{"favorites", m_favorites},
                                                 {"history", m_history},{"lastPlayed",m_lastPlayed},
                                                 {"playlists", m_playlists},{"pins",m_pins},{"lyricOffsets",m_lyricOffsets},
                                                 {"queue", m_queue.rows},
                                                 {"index", m_index},{"position",position()}})
              .toJson(QJsonDocument::Compact));
  if (!f.commit())
    notifyError("Could not save your library.");
}
void Backend::recordHistory() {
  if(m_historyPaused){m_skipHistoryToken=m_trackToken;return;}
  if(m_skipHistoryToken&&m_skipHistoryToken==m_trackToken)return;
  auto t = current();
  if(!t.isEmpty()&&m_recordedToken!=m_trackToken){
    m_recordedToken=m_trackToken;m_lastPlayed[t.value("id").toString()]=QDateTime::currentSecsSinceEpoch();
    // Retain dates only for saved songs and the bounded recent history.
    QSet<QString> keep;for(const auto &v:m_localTracks)keep.insert(itemId(v));for(const auto &v:m_favorites)keep.insert(itemId(v));for(const auto &v:m_history)keep.insert(itemId(v));
    for(const auto &v:m_playlists)for(const auto &row:v.toMap().value("tracks").toList())keep.insert(itemId(row));
    keep.insert(t.value("id").toString());
    for(auto i=m_lastPlayed.begin();i!=m_lastPlayed.end();)if(!keep.contains(i.key()))i=m_lastPlayed.erase(i);else ++i;
    emit libraryChanged();m_saveTimer.start();
  }
  if (t.isEmpty() || (!m_history.isEmpty() && itemId(m_history.first())==t.value("id").toString()))
    return;
  for (int i = m_history.size() - 1; i >= 0; --i)
    if (itemId(m_history[i]) == t.value("id").toString())
      m_history.removeAt(i);
  invalidateUndo("history");
  m_history.prepend(t);
  while (m_history.size() > 200)
    m_history.removeLast();
  emit libraryChanged();
  m_saveTimer.start();
}
void Backend::notifyTrack() {
  if(m_announcedToken==m_trackToken)return;
  m_announcedToken=m_trackToken;
  if(!trackNotifications()||m_historyPaused)return;
  const auto t=current();m_notifier.show(t.value("title").toString(),t.value("artist").toString());
}
void Backend::setHistoryPaused(bool paused) {
  if(m_historyPaused==paused)return;
  m_historyPaused=paused;
  if(paused){m_skipHistoryToken=m_trackToken;m_notifier.clear();}
  emit settingsChanged();
}
void Backend::setTrackNotifications(bool enabled) {
  m_settings.setValue("trackNotifications",enabled);
  if(!enabled)m_notifier.clear();
  emit settingsChanged();
}
void Backend::setKeepCompletedLyrics(bool enabled) {m_settings.setValue("keepCompletedLyrics",enabled);emit settingsChanged();}
void Backend::setVolumeStep(int percent) {
  if(percent!=1&&percent!=2&&percent!=5&&percent!=10)return;
  m_settings.setValue("volumeStep",percent);emit settingsChanged();
}
bool Backend::isLiked(const QString &id) const {
  for (const auto &t : m_favorites)
    if (itemId(t) == id)
      return true;
  return false;
}
bool Backend::liked() const {
  return isLiked(current().value("id").toString());
}
void Backend::toggleLike(const QVariantMap &item) {
  if (playable({item}).isEmpty())
    return;
  bool removed = false;
  for (int i = 0; i < m_favorites.size(); ++i)
    if (itemId(m_favorites[i]) == item.value("id").toString()) {
      m_favorites.removeAt(i);
      removed = true;
      break;
    }
  if (!removed)
    m_favorites.prepend(item);
  emit libraryChanged();
  if (m_page == "library" && m_libraryId == "favorites")
    m_results.assign(m_favorites);
  m_saveTimer.start();
  emit toast(removed ? "Removed from liked songs" : "Added to liked songs");
}
QVariantList Backend::playlists() const {
  QVariantList result;
  for (const auto &v : m_playlists) {
    auto p = v.toMap();
    p["count"] = p.value("tracks").toList().size();
    p.remove("tracks");
    result.append(p);
  }
  return result;
}
void Backend::library(const QString &kind) {
  navigate("library",
           kind == "files" ? "Local files" : kind == "mixes" ? "Mixes" : kind=="mix-recent" ? "Recently liked" : kind=="mix-rediscover" ? "Rediscover" : kind=="mix-unplayed" ? "Unplayed" :
           kind == "history"     ? "Recently played"
           : kind == "playlists" ? "Playlists"
                                 : "Liked songs",
           m_page != "library");
  m_libraryId = kind;
  m_results.assign(libraryRows(kind));
  emit catalogChanged();
}
void Backend::clearHistory() {
  m_undoType = "history";
  m_undoRows = m_history;m_undoLastPlayed=m_lastPlayed;m_lastPlayed.clear();
  m_undoMessage = "History cleared";
  m_history.clear();
  if (m_page == "library" && m_libraryId == "history")
    m_results.assign({});
  emit libraryChanged();
  m_saveTimer.start();
  if (!m_undoMessage.isEmpty()) emit toast(m_undoMessage);
}
QString Backend::createPlaylist(const QString &name) {
  invalidateUndo("playlists");
  if (name.trimmed().isEmpty())
    return {};
  auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  m_playlists.append(QVariantMap{{"id", id},
                                 {"title", name.trimmed().left(120)},
                                 {"tracks", QVariantList{}}});
  emit libraryChanged();
  m_saveTimer.start();
  return id;
}
void Backend::openPlaylist(const QString &id) {
  for (const auto &v : m_playlists) {
    auto p = v.toMap();
    if (p.value("id") == id) {
      navigate("local", p.value("title").toString());
      m_libraryId = id;
      m_results.assign(p.value("tracks").toList());
      emit catalogChanged();
      return;
    }
  }
}
void Backend::renamePlaylist(const QString &id, const QString &name) {
  invalidateUndo("playlists");
  if (name.trimmed().isEmpty())
    return;
  for (int i = 0; i < m_playlists.size(); ++i) {
    auto p = m_playlists[i].toMap();
    if (p.value("id") == id) {
      p["title"] = name.trimmed().left(120);
      m_playlists[i] = p;
      if (m_libraryId == id) {
        m_title = p.value("title").toString();
        emit catalogChanged();
      }
      emit libraryChanged();
      m_saveTimer.start();
      return;
    }
  }
}
void Backend::deletePlaylist(const QString &id) {
  for (int i = 0; i < m_playlists.size(); ++i)
    if (m_playlists[i].toMap().value("id") == id) {
      m_undoType = "playlists";
      m_undoRows = m_playlists;
      m_undoMessage = "Playlist deleted";
      m_playlists.removeAt(i);
      if (m_libraryId == id)
        library();
      emit libraryChanged();
      m_saveTimer.start();
      if (!m_undoMessage.isEmpty()) emit toast(m_undoMessage);
      return;
    }
}
void Backend::addToPlaylist(const QString &id, const QVariantMap &item) {
  invalidateUndo("playlists");
  if (playable({item}).isEmpty())
    return;
  for (int i = 0; i < m_playlists.size(); ++i) {
    auto p = m_playlists[i].toMap();
    if (p.value("id") == id) {
      auto tracks = p.value("tracks").toList();
      for (const auto &t : tracks)
        if (itemId(t) == item.value("id").toString()) {
          emit toast("Already in playlist");
          return;
        }
      tracks.append(item);
      p["tracks"] = tracks;
      m_playlists[i] = p;
      if (m_libraryId == id)
        m_results.assign(tracks);
      emit libraryChanged();
      m_saveTimer.start();
      emit toast("Added to playlist");
      return;
    }
  }
}
void Backend::removeFromPlaylist(const QString &id, int index) {
  for (int i = 0; i < m_playlists.size(); ++i) {
    auto p = m_playlists[i].toMap();
    if (p.value("id") == id) {
      auto tracks = p.value("tracks").toList();
      if (index < 0 || index >= tracks.size())
        return;
      m_undoType="playlists"; m_undoRows=m_playlists; m_undoMessage="Removed from playlist";
      tracks.removeAt(index);
      p["tracks"] = tracks;
      m_playlists[i] = p;
      if (m_libraryId == id)
        m_results.assign(tracks);
      emit libraryChanged();
      m_saveTimer.start();
      emit toast(m_undoMessage);
      return;
    }
  }
}

void Backend::undo() {
  if(m_undoType=="queue-order"){
    m_index=m_undoIndex;m_queue.assign(m_undoRows);setShuffle(m_undoShuffle);emit trackChanged();
  } else if (m_undoType == "queue") {
    stop();
    m_queue.assign(m_undoRows);
    m_index = qBound(-1, m_undoIndex, m_queue.count() - 1);
    emit trackChanged();
  } else if (m_undoType == "history") {
    m_history = m_undoRows;m_lastPlayed=m_undoLastPlayed;m_undoLastPlayed.clear();
    if (m_page == "library" && m_libraryId == "history")
      m_results.assign(m_history);
  } else if (m_undoType == "playlists") {
    m_playlists = m_undoRows;
    if (m_page=="local") { for(const auto &v:m_playlists) if(v.toMap().value("id")==m_libraryId) m_results.assign(v.toMap().value("tracks").toList()); }
  } else
    return;
  m_undoRows.clear();
  m_undoType.clear();
  m_undoMessage.clear();
  emit libraryChanged();
  m_saveTimer.start();
  emit toast("Restored");
}

void Backend::invalidateUndo(const QString &type) {
  if(m_undoType!=type && !(type=="queue"&&m_undoType=="queue-order"))return;
  m_undoType.clear();m_undoRows.clear();m_undoLastPlayed.clear();m_undoMessage.clear();emit libraryChanged();
}
void Backend::clearLyrics() {
  m_trackToken++;cancel("lyrics");m_lyricsLoaded=false;m_lyricsSource.clear();m_lyrics.clear();m_lyricLines.clear();m_romanizedLyrics.clear();m_romanizedLyricLines.clear();m_romanizedLyricsFinished=false;m_romanizedLyricsBusy=false;m_lyricsBusy=false;emit lyricsChanged();
}
void Backend::saveQueue(const QString &name) {
  if(m_queue.count()==0)return;
  auto id=createPlaylist(name);
  if(id.isEmpty())return;
  for(const auto &v:m_queue.rows)addToPlaylist(id,v.toMap());
  emit toast("Queue saved as playlist");
}
void Backend::movePlaylistTrack(const QString &id,int from,int to) {
  for(int i=0;i<m_playlists.size();++i){auto p=m_playlists[i].toMap();if(p.value("id")!=id)continue;
    auto rows=p.value("tracks").toList();if(from<0||to<0||from>=rows.size()||to>=rows.size()||from==to)return;
    invalidateUndo("playlists");rows.move(from,to);p["tracks"]=rows;m_playlists[i]=p;
    if(m_page=="local"&&m_libraryId==id)m_results.assign(rows);
    emit libraryChanged();m_saveTimer.start();return;
  }
}
void Backend::exportLibrary(const QUrl &url) {
  if(!url.isLocalFile())return;
  QSaveFile file(url.toLocalFile());if(!file.open(QIODevice::WriteOnly)){notifyError("Couldn’t export the library.");return;}
  file.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
  file.write(QJsonDocument::fromVariant(QVariantMap{{"sung",1},{"musicFolders",m_musicFolders},{"localTracks",m_localTracks},{"favorites",m_favorites},{"playlists",m_playlists},{"pins",pins()},{"lyricOffsets",m_lyricOffsets}}).toJson());
  if(!file.commit())notifyError("Couldn’t export the library.");else emit toast("Library exported");
}
void Backend::importLibrary(const QUrl &url) {
  if(!url.isLocalFile())return;
  QFile file(url.toLocalFile());if(!file.open(QIODevice::ReadOnly)||file.size()>10*1024*1024){notifyError("Choose a Sung library JSON file smaller than 10 MB.");return;}
  auto d=QJsonDocument::fromJson(file.readAll());auto map=d.object().toVariantMap();
  if(!d.isObject()||map.value("sung").toInt()!=1){notifyError("This isn’t a Sung library export.");return;}
  for(const auto &v:playable(map.value("localTracks").toList()))if(!v.toMap().value("localPath").toString().isEmpty())mergeLocalTrack(v.toMap());
  for(const auto &path:map.value("musicFolders").toStringList())if(QDir::isAbsolutePath(path)&&!m_musicFolders.contains(path)&&m_musicFolders.size()<64)m_musicFolders.append(path);
  invalidateUndo("playlists");
  for(const auto &v:playable(map.value("favorites").toList()))if(!isLiked(itemId(v)))m_favorites.append(v);
  for(const auto &v:map.value("playlists").toList()){
    auto p=v.toMap();auto rows=playable(p.value("tracks").toList());auto name=p.value("title").toString().trimmed();if(name.isEmpty())continue;
    int found=-1;for(int i=0;i<m_playlists.size();++i)if(m_playlists[i].toMap().value("id")==p.value("id")){found=i;break;}
    if(found<0){const auto importedId=p.value("id").toString();createPlaylist(name);found=m_playlists.size()-1;p=m_playlists[found].toMap();if(!importedId.isEmpty())p["id"]=importedId;}else p=m_playlists[found].toMap();
    auto existing=p.value("tracks").toList();for(const auto &t:rows){bool seen=false;for(const auto &old:existing)if(itemId(old)==itemId(t)){seen=true;break;}if(!seen)existing.append(t);}
    p["tracks"]=existing;m_playlists[found]=p;
  }
  const auto offsets=map.value("lyricOffsets").toMap();
  for(auto it=offsets.cbegin();it!=offsets.cend();++it){bool ok=false;const auto value=it.value().toInt(&ok);if(ok&&!it.key().isEmpty()&&it.key().size()<=128&&value>=-10000&&value<=10000)m_lyricOffsets[it.key()]=value;}
  emit lyricsChanged();emit positionChanged();
  for(const auto &v:map.value("pins").toList())if(!isPinned(v.toMap()))togglePin(v.toMap());
  emit libraryChanged();if(m_page=="library")library(m_libraryId);else if(m_page=="local") {const auto id=m_libraryId;openPlaylist(id);}
  save();emit toast("Library imported");
}
void Backend::playLink(const QString &url) {
  request("linkplay",{{"op","link"},{"url",url}},[this](const QVariantMap &data){
    if(!data.value("ok").toBool()){notifyError(data.value("error").toString());return;}
    auto items=playable(data.value("items").toList());if(items.isEmpty())return;
    invalidateUndo("queue");m_queue.assign(items);playAt(0);
  });
}

int Backend::lyricIndex() const {
  const auto pos=position()+lyricOffset();int low=0,high=m_lyricLines.size();
  while(low<high){const int mid=(low+high)/2;if(m_lyricLines[mid].toMap().value("start").toLongLong()<=pos)low=mid+1;else high=mid;}
  if(low==0)return -1;
  const auto line=m_lyricLines[low-1].toMap();const auto end=line.value("end").toLongLong();
  return end>0&&pos>=end ? -1 : low-1;
}

QVariantList Backend::audioDevices() const {
  QVariantList result{{QVariantMap{{"id",""},{"name","System default"}}}};
  for(const auto &device:QMediaDevices::audioOutputs())
    result.append(QVariantMap{{"id",QString::fromLatin1(device.id().toBase64())},{"name",device.description()}});
  return result;
}
QString Backend::audioDeviceName() const {
  if(audioDeviceId().isEmpty())return "System default";
  for(const auto &device:QMediaDevices::audioOutputs())if(QString::fromLatin1(device.id().toBase64())==audioDeviceId())return device.description();
  return "System default";
}
void Backend::setAudioDeviceId(const QString &id) {
  if(!id.isEmpty()){
    bool found=false;for(const auto &device:QMediaDevices::audioOutputs())if(QString::fromLatin1(device.id().toBase64())==id){found=true;break;}
    if(!found)return;
  }
  if(audioDeviceId()==id)return;
  m_settings.setValue("audioDevice",id);applyAudioDevice();emit audioDevicesChanged();
}
void Backend::applyAudioDevice() {
  auto chosen=QMediaDevices::defaultAudioOutput();bool found=audioDeviceId().isEmpty();
  for(const auto &device:QMediaDevices::audioOutputs())if(QString::fromLatin1(device.id().toBase64())==audioDeviceId()){chosen=device;found=true;break;}
  // A disconnected device falls back immediately. It cannot steal playback if it reconnects.
  if(!found)m_settings.remove("audioDevice");
  if(m_audio.device()!=chosen)m_audio.setDevice(chosen);
}
void Backend::playCollection(int index) {
  if(index<0||index>=m_collection.count())return;
  const auto all=m_collection.items();const auto target=m_collection.get(index);
  if(playable({target}).isEmpty())return;
  int actual=0;for(int i=0;i<index;++i)if(!playable({all[i]}).isEmpty())++actual;
  invalidateUndo("queue");m_queue.assign(playable(all));playAt(actual);
}
void Backend::enqueueCollection() {
  auto items=playable(m_collection.items());if(items.isEmpty())return;
  invalidateUndo("queue");m_queue.append(items);m_saveTimer.start();emit toast("Added to queue");
}

static QString pinKey(const QVariantMap &item) {
  return item.value("kind").toString()+":"+item.value("browseId",item.value("id")).toString();
}
QVariantList Backend::pins() const {
  QVariantList visible;
  for(const auto &v:m_pins){
    auto item=v.toMap();
    if(item.value("kind")=="local"){
      bool found=false;
      for(const auto &p:m_playlists)if(itemId(p)==item.value("id").toString()){
        const auto list=p.toMap();item["title"]=list.value("title");
        const auto tracks=list.value("tracks").toList();item["art"]=tracks.isEmpty()?QString():tracks.first().toMap().value("art");
        found=true;break;
      }
      if(!found)continue;
    }
    visible.append(item);
  }
  return visible;
}
bool Backend::isPinned(const QVariantMap &item) const {
  for(const auto &p:m_pins)if(pinKey(p.toMap())==pinKey(item))return true;
  return false;
}
void Backend::togglePin(const QVariantMap &item) {
  const auto kind=item.value("kind").toString(),id=item.value("browseId",item.value("id")).toString();
  if(!QStringList{"album","playlist","artist","local"}.contains(kind)||id.isEmpty()||item.value("title").toString().trimmed().isEmpty())return;
  for(int i=0;i<m_pins.size();++i)if(pinKey(m_pins[i].toMap())==pinKey(item)){
    m_pins.removeAt(i);emit libraryChanged();m_saveTimer.start();emit toast("Unpinned from Home");return;
  }
  if(m_pins.size()>=24){emit toast("Unpin an item before adding another");return;}
  QVariantMap pin{{"kind",kind},{"id",id},{"title",item.value("title").toString().left(120)}};
  for(const auto &key:{"browseId","art","artist"})if(item.contains(key))pin[key]=item.value(key).toString();
  m_pins.prepend(pin);emit libraryChanged();m_saveTimer.start();emit toast("Pinned to Home");
}
QVariantMap Backend::collectionItem() const {
  if(m_page=="local")return {{"id",m_libraryId},{"kind","local"},{"title",m_title},{"art",m_cover}};
  if(!QStringList{"album","artist","playlist"}.contains(m_page)||m_request.value("id").toString().isEmpty())return {};
  return {{"id",m_request.value("id")},{"browseId",m_request.value("id")},{"kind",m_page},{"title",m_title},{"art",m_cover}};
}

void Backend::setPlaybackRate(double rate) {
  if(!std::isfinite(rate)||rate<0.5||rate>2.0)return;
  m_settings.setValue("playbackRate",rate);m_media.setPlaybackRate(rate);
}
void Backend::setLyricTextSize(int size) {
  size=qBound(20,size,32);if(size==lyricTextSize())return;
  m_settings.setValue("lyricTextSize",size);emit settingsChanged();
}
qint64 Backend::queueRemainingMs() const {
  if(m_queue.count()==0)return 0;
  if(shuffle())return -1;
  const int first=qMax(0,m_index);
  if(first>=m_queue.count()||m_queueSuffix.size()!=m_queue.count()+1)return -1;
  qint64 remaining=m_queueSuffix[first];
  if(m_index>=0){
    if(duration()<=0||m_queueSuffix[first+1]<0)return -1;
    remaining=qMax<qint64>(0,duration()-position())+m_queueSuffix[first+1];
  }
  return remaining<0 ? -1 : qint64(std::ceil(remaining/playbackRate()));
}
QString Backend::queueTime() const {
  const auto ms=queueRemainingMs();if(ms<=0)return {};
  const auto minutes=(ms+59999)/60000;
  const auto time=minutes>=60 ? QString("%1 h %2 min").arg(minutes/60).arg(minutes%60) : QString("%1 min").arg(minutes);
  return time+(repeat()||autoplay()?" queued":" left");
}
QString Backend::queueEnd() const {
  const auto ms=queueRemainingMs();
  if(ms<=0||!playing()||resolving()||repeat()||autoplay()||m_sleepAtEnd||m_sleepTimer.isActive())return {};
  const auto now=QDateTime::currentDateTime(),end=now.addMSecs(ms);
  return "Ends around "+end.toString(end.date()==now.date()?"h:mm AP":"ddd h:mm AP");
}
bool Backend::pitchAdjustable() const {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
  return m_media.pitchCompensationAvailability()==QMediaPlayer::PitchCompensationAvailability::Available;
#else
  return false;
#endif
}
bool Backend::preservePitch() const {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
  return m_media.pitchCompensation();
#else
  return false;
#endif
}
void Backend::setPreservePitch(bool enabled) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
  if(!pitchAdjustable())return;
  m_media.setPitchCompensation(enabled);m_settings.setValue("preservePitch",enabled);emit settingsChanged();
#else
  Q_UNUSED(enabled);
#endif
}
int Backend::lyricOffset() const {
  return qBound(-10000,m_lyricOffsets.value(current().value("id").toString(),0).toInt(),10000);
}
void Backend::setLyricOffset(int value) {
  const auto id=current().value("id").toString();if(id.isEmpty())return;
  value=qBound(-10000,value,10000);if(value==lyricOffset())return;
  if(value==0)m_lyricOffsets.remove(id);else m_lyricOffsets[id]=value;
  emit lyricsChanged();emit positionChanged();m_saveTimer.start();
}
void Backend::seekLyric(qint64 milliseconds) {seek(milliseconds-lyricOffset());}
void Backend::playKeepingQueue(const QVariantMap &item) {
  if(playable({item}).isEmpty())return;
  const auto id=item.value("id").toString();
  if(m_index>=0&&current().value("id").toString()==id){play();return;}
  auto q=m_queue.rows;const int target=qBound(0,m_index+1,int(q.size()));
  int existing=-1;for(int i=target;i<q.size();++i)if(q[i].toMap().value("id").toString()==id){existing=i;break;}
  if(existing>=0)q.insert(target,q.takeAt(existing));else q.insert(target,item);
  invalidateUndo("queue");m_queue.assign(q);playAt(target);emit toast("Playing · queue kept");
}


void Backend::rememberSearch(const QString &value) {
  const auto query=value.simplified().left(256);
  if(query.isEmpty() || query.startsWith("https://") || query.startsWith("http://") || historyPaused())return;
  auto recent=recentSearches();
  recent.removeIf([&](const QString &s){return s.compare(query,Qt::CaseInsensitive)==0;});
  recent.prepend(query);m_settings.setValue("recentSearches",recent.mid(0,12));emit recentSearchesChanged();
}
void Backend::removeRecentSearch(const QString &query) {
  auto recent=recentSearches();recent.removeAll(query);m_settings.setValue("recentSearches",recent);emit recentSearchesChanged();
}
QVariantList Backend::localMatches(const QString &query) const {
  const auto cleanQuery = query.simplified();
  if (cleanQuery.isEmpty()) return {};
  const auto terms = cleanQuery.split(' ', Qt::SkipEmptyParts);
  if (terms.isEmpty()) return {};

  struct Candidate {
    QVariantMap item;
    int score;
  };
  QList<Candidate> candidates;
  QSet<QString> seen;

  auto collect = [&](QVariantMap item, const QString &origin, int queueIndex = -1) {
    const auto key = item.value("kind").toString() + ":" + item.value("id").toString();
    if (seen.contains(key)) return;

    const QString title = item.value("title").toString();
    const QString artist = item.value("artist").toString();
    const QString album = item.value("album").toString();
    const QString text = (title + " " + artist + " " + album).simplified();

    for (const auto &term : terms) {
      if (!text.contains(term, Qt::CaseInsensitive)) return;
    }

    seen.insert(key);
    item["origin"] = origin;
    if (queueIndex >= 0) item["queueIndex"] = queueIndex;

    int score = 0;
    if (title.compare(cleanQuery, Qt::CaseInsensitive) == 0) {
      score += 10000;
    } else if (title.startsWith(cleanQuery, Qt::CaseInsensitive)) {
      score += 5000;
    } else {
      const auto titleWords = title.split(' ', Qt::SkipEmptyParts);
      for (const auto &word : titleWords) {
        if (word.startsWith(cleanQuery, Qt::CaseInsensitive)) {
          score += 3000;
          break;
        }
      }
    }
    if (score < 3000 && title.contains(cleanQuery, Qt::CaseInsensitive)) {
      score += 2000;
    }
    if (artist.startsWith(cleanQuery, Qt::CaseInsensitive)) score += 1500;
    else if (artist.contains(cleanQuery, Qt::CaseInsensitive)) score += 500;

    if (album.startsWith(cleanQuery, Qt::CaseInsensitive)) score += 1000;
    else if (album.contains(cleanQuery, Qt::CaseInsensitive)) score += 300;

    for (const auto &term : terms) {
      if (title.startsWith(term, Qt::CaseInsensitive)) score += 400;
      else if (title.contains(term, Qt::CaseInsensitive)) score += 200;
    }

    if (item.value("kind").toString() == "local") {
      score += 5;
    }

    candidates.append({item, score});
  };

  for (const auto &v : m_playlists) {
    auto p = v.toMap();
    collect({{"kind", "local"}, {"id", p.value("id")}, {"title", p.value("title")}}, "Playlist");
  }
  for (int i = 0; i < m_queue.count(); ++i) collect(m_queue.get(i), "Queue", i);
  for (const auto &v : m_favorites) collect(v.toMap(), "Liked songs");
  for (const auto &v : m_playlists) {
    for (const auto &t : v.toMap().value("tracks").toList()) collect(t.toMap(), v.toMap().value("title").toString());
  }
  for (const auto &v : m_localTracks) collect(v.toMap(), "Local files");
  for (const auto &v : m_history) collect(v.toMap(), "History");

  std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
    return a.score > b.score;
  });

  QVariantList result;
  for (int i = 0; i < qMin(8, candidates.size()); ++i) {
    result.append(candidates[i].item);
  }
  return result;
}
static QList<int> validRows(const QVariantList &indices,int count) {
  QSet<int> unique;for(const auto &v:indices){bool ok=false;int i=v.toInt(&ok);if(ok&&i>=0&&i<count)unique.insert(i);}
  auto rows=unique.values();std::sort(rows.begin(),rows.end());return rows;
}
static QList<int> movedOrder(int count,const QList<int> &selected,int before) {
  QSet<int> set(selected.begin(),selected.end());QList<int> order;int insert=0;
  for(int i=0;i<count;++i)if(!set.contains(i)){if(i<before)++insert;order.append(i);}
  for(int i=0;i<selected.size();++i)order.insert(insert+i,selected[i]);
  return order;
}
void Backend::enqueueItems(const QVariantList &items,bool next,int before) {
  const auto songs=playable(items);if(songs.isEmpty())return;
  m_undoType="queue-order";m_undoRows=m_queue.rows;m_undoIndex=m_index;m_undoShuffle=shuffle();m_undoMessage="Added to queue";
  auto rows=m_queue.rows;int at=before>=0?qBound(0,before,int(rows.size())):next?qBound(0,m_index+1,int(rows.size())):int(rows.size());
  if(m_index>=at)m_index+=songs.size();
  for(int i=0;i<songs.size();++i)rows.insert(at+i,songs[i]);
  m_queue.assign(rows);emit trackChanged();emit libraryChanged();m_saveTimer.start();emit toast(m_undoMessage);
}
void Backend::moveQueueRows(const QVariantList &indices,int before) {
  const auto selected=validRows(indices,m_queue.count());if(selected.isEmpty()||before<0||before>m_queue.count())return;
  const auto order=movedOrder(m_queue.count(),selected,before);bool changed=false;for(int i=0;i<order.size();++i)changed|=order[i]!=i;if(!changed)return;
  m_undoType="queue-order";m_undoRows=m_queue.rows;m_undoIndex=m_index;m_undoShuffle=shuffle();m_undoMessage="Queue reordered";
  QVariantList rows;for(int i:order)rows.append(m_queue.rows[i]);m_index=order.indexOf(m_index);
  m_queue.assign(rows);emit trackChanged();emit libraryChanged();m_saveTimer.start();emit toast(m_undoMessage);
}
void Backend::removeQueueRows(const QVariantList &indices) {
  const auto selected=validRows(indices,m_queue.count());if(selected.isEmpty())return;
  const bool removed=selected.contains(m_index),resume=playing()||m_resolving;
  m_undoType=removed?"queue":"queue-order";m_undoRows=m_queue.rows;m_undoIndex=m_index;m_undoShuffle=shuffle();m_undoMessage="Removed from queue";
  int before=0;for(int i:selected)if(i<m_index)++before;
  auto rows=m_queue.rows;for(auto it=selected.crbegin();it!=selected.crend();++it)rows.removeAt(*it);
  m_index-=before;m_queue.assign(rows);
  if(removed){stop();++m_trackToken;clearLyrics();m_index=rows.isEmpty()?-1:qMin(m_index,int(rows.size()-1));if(resume&&m_index>=0)playAt(m_index);}
  emit trackChanged();emit libraryChanged();m_saveTimer.start();emit toast(m_undoMessage);
}
void Backend::addItemsToPlaylist(const QString &id,const QVariantList &items) {
  const auto songs=playable(items);if(songs.isEmpty())return;
  for(int i=0;i<m_playlists.size();++i){auto p=m_playlists[i].toMap();if(p.value("id")!=id)continue;
    auto rows=p.value("tracks").toList();QSet<QString> seen;for(const auto &t:rows)seen.insert(itemId(t));const int old=rows.size();
    for(const auto &t:songs)if(!seen.contains(itemId(t))){rows.append(t);seen.insert(itemId(t));}
    if(rows.size()==old){emit toast("Already in playlist");return;}
    m_undoType="playlists";m_undoRows=m_playlists;m_undoMessage="Added to playlist";
    p["tracks"]=rows;m_playlists[i]=p;if(m_page=="local"&&m_libraryId==id)m_results.assign(rows);
    emit libraryChanged();m_saveTimer.start();emit toast(m_undoMessage);return;
  }
}
void Backend::removePlaylistRows(const QString &id,const QVariantList &indices) {
  for(int i=0;i<m_playlists.size();++i){auto p=m_playlists[i].toMap();if(p.value("id")!=id)continue;
    auto rows=p.value("tracks").toList();const auto selected=validRows(indices,rows.size());if(selected.isEmpty())return;
    m_undoType="playlists";m_undoRows=m_playlists;m_undoMessage="Removed from playlist";
    for(auto it=selected.crbegin();it!=selected.crend();++it)rows.removeAt(*it);
    p["tracks"]=rows;m_playlists[i]=p;if(m_page=="local"&&m_libraryId==id)m_results.assign(rows);
    emit libraryChanged();m_saveTimer.start();emit toast(m_undoMessage);return;
  }
}
void Backend::movePlaylistRows(const QString &id,const QVariantList &indices,int before) {
  // Display order must be unambiguous before writing a saved playlist order.
  if(m_page!="local"||m_libraryId!=id||!m_collection.query().isEmpty()||m_collection.sortKey()!="original")return;
  for(int i=0;i<m_playlists.size();++i){auto p=m_playlists[i].toMap();if(p.value("id")!=id)continue;
    auto rows=p.value("tracks").toList();const auto selected=validRows(indices,rows.size());if(selected.isEmpty()||before<0||before>rows.size())return;
    const auto order=movedOrder(rows.size(),selected,before);QVariantList moved;bool changed=false;for(int n=0;n<order.size();++n){moved.append(rows[order[n]]);changed|=order[n]!=n;}if(!changed)return;
    m_undoType="playlists";m_undoRows=m_playlists;m_undoMessage="Playlist reordered";
    p["tracks"]=moved;m_playlists[i]=p;m_results.assign(moved);emit libraryChanged();m_saveTimer.start();emit toast(m_undoMessage);return;
  }
}

QVariantList Backend::libraryRows(const QString &kind) const {
  if(kind=="files")return m_localTracks;
  if(kind=="favorites")return m_favorites;
  if(kind=="history")return m_history;
  if(kind=="mixes")return {QVariantMap{{"id","mix-recent"},{"kind","smart"},{"title","Recently liked"}},QVariantMap{{"id","mix-rediscover"},{"kind","smart"},{"title","Rediscover"}},QVariantMap{{"id","mix-unplayed"},{"kind","smart"},{"title","Unplayed"}}};
  if(kind=="mix-recent")return m_favorites.mid(0,50);
  QVariantList rows;QSet<QString> seen;
  const auto cutoff=QDateTime::currentSecsSinceEpoch()-30*86400;
  const auto append=[&](const QVariantList &items){for(const auto &v:items){const auto id=itemId(v);if(id.isEmpty()||seen.contains(id))continue;seen.insert(id);const auto date=m_lastPlayed.value(id).toLongLong();if((kind=="mix-unplayed"&&!m_lastPlayed.contains(id))||(kind=="mix-rediscover"&&date>0&&date<cutoff))rows.append(v);}};
  append(m_favorites);append(m_localTracks);for(const auto &v:m_playlists)append(v.toMap().value("tracks").toList());
  if(kind=="mix-rediscover")std::stable_sort(rows.begin(),rows.end(),[this](const QVariant&a,const QVariant&b){return m_lastPlayed.value(itemId(a)).toLongLong()<m_lastPlayed.value(itemId(b)).toLongLong();});
  return rows;
}
void Backend::setPrepareNext(bool enabled){m_settings.setValue("prepareNext",enabled);emit settingsChanged();}
void Backend::cancelPreparation(){
  ++m_preparationGeneration;cancel("prepare");m_prepareTimer.stop();m_preparedData.clear();m_preparedDirectory.reset();m_preparedId.clear();
}
void Backend::updatePreparation(){
  m_prepareTimer.stop();
  QString nextId;
  if(prepareNext()&&playing()&&m_wantPlay&&!m_resolving&&!shuffle()&&repeat()!=2&&!m_sleepAtEnd&&m_index>=0){
    const int next=m_index+1<m_queue.count()?m_index+1:repeat()==1?0:-1;
    if(next>=0&&next!=m_index)nextId=m_queue.get(next).value("videoId").toString();
    if(m_sleepTimer.isActive()&&m_sleepTimer.remainingTime()<qMax<qint64>(0,duration()-position())/playbackRate())nextId.clear();
  }
  if(nextId.isEmpty()){cancelPreparation();m_preparationAttempt.clear();return;}
  if(m_preparedId!=nextId){cancelPreparation();m_preparationAttempt.clear();m_preparedId=nextId;}
  if(!m_preparedData.isEmpty()||m_processes.contains("prepare")||m_preparationAttempt==nextId)return;
  const qint64 remaining=(duration()-position())/playbackRate();
  if(duration()<=0)return;
  if(remaining>45000){m_prepareTimer.start(int(qMin<qint64>(remaining-45000,2147483647)));return;}
  m_preparationAttempt=nextId;const auto generation=m_preparationGeneration;
  auto directory=audioDirectory();if(!directory||!directory->isValid())return;
  m_preparedDirectory=directory;
  request("prepare",{{"op","buffer"},{"id",nextId},{"directory",directory->path()},{"cookies",cookies()}},[this,generation,nextId,directory](const QVariantMap &data){
    if(generation!=m_preparationGeneration||m_preparedId!=nextId)return;
    const QFileInfo file(data.value("file").toString());
    // Preload failures and oversized/direct streams leave normal playback in charge.
    if(data.value("ok").toBool()&&file.isFile()&&file.size()<=32*1024*1024&&file.canonicalPath()==QFileInfo(directory->path()).canonicalFilePath()){
      m_preparedData=data;
      QFile buffered(file.filePath());if(buffered.open(QIODevice::ReadOnly))::posix_fadvise(buffered.handle(),0,0,POSIX_FADV_DONTNEED);
    }
    else m_preparedDirectory.reset();
  },directory);
}

QString Backend::localImportStatus() const {return m_scanningFolders ? QString("Scanning folders…") : QString("Importing %1 / %2 files…").arg(m_importDone).arg(m_importTotal);}
void Backend::importLocalFiles(const QVariantList &urls) {
  if(importingLocal()){emit toast("An import is already running");return;}
  QSet<QString> seen;
  for(const auto &v:urls){const QUrl url(v.toString());if(!url.isLocalFile())continue;const auto path=QFileInfo(url.toLocalFile()).absoluteFilePath();if(!seen.contains(path)&&m_importFiles.size()<10000){seen.insert(path);m_importFiles.append(path);}}
  if(m_importFiles.isEmpty())return;
  m_importTotal=m_importFiles.size();m_importDone=0;m_importFailed=0;emit localImportChanged();importNextLocalBatch();
}
void Backend::cancelLocalImport(){cancel("folder-scan");m_scanningFolders=false;m_scanLimited=false;m_scanFailed=0;cancel("local-import");m_importFiles.clear();m_importTotal=0;m_relocateId.clear();emit localImportChanged();}
void Backend::locateLocalFile(const QUrl &url,const QString &id){
  if(importingLocal()||!url.isLocalFile())return;
  bool found=false;for(const auto &v:m_localTracks)if(itemId(v)==id)found=true;
  if(!found)for(const auto &v:m_favorites)if(itemId(v)==id)found=true;
  if(!found)for(const auto &v:m_history)if(itemId(v)==id)found=true;
  if(!found)for(const auto &v:m_queue.rows)if(itemId(v)==id)found=true;
  if(!found)for(const auto &v:m_playlists)for(const auto &t:v.toMap().value("tracks").toList())if(itemId(t)==id)found=true;
  if(!found)return;
  m_relocateId=id;importLocalFiles({url});
}
void Backend::importNextLocalBatch(){
  if(!importingLocal()||m_processes.contains("local-import"))return;
  if(m_importFiles.isEmpty()){
    const int successCount = qMax(0, m_importDone - m_importFailed);
    const int failCount = m_importFailed + m_scanFailed;
    const auto message = m_scanLimited ? QString("Scan limit reached. Add a smaller folder to continue.")
                         : failCount > 0 ? QString("Import completed · %1 files imported, %2 failed").arg(successCount).arg(failCount)
                         : m_importTotal == 1 ? QString("Import completed · 1 file imported")
                         : QString("Import completed · %1 files imported").arg(m_importDone);
    m_scanLimited=false;m_scanFailed=0;
    m_importTotal=0;m_relocateId.clear();emit localImportChanged();emit toast(message);return;
  }
  QStringList batch;int bytes=0;
  while(!m_importFiles.isEmpty()&&batch.size()<4&&bytes<16000){batch.append(m_importFiles.takeFirst());bytes+=batch.last().toUtf8().size()*2;}
  request("local-import",{{"op","local-files"},{"files",batch},{"artDirectory",dataPath()+"/local-art"}},[this,count=batch.size()](const QVariantMap &data){
    const auto items=data.value("items").toList();m_importDone+=count;m_importFailed+=count-items.size();
    for(const auto &v:items){auto track=v.toMap();if(!m_relocateId.isEmpty())track["id"]=m_relocateId;mergeLocalTrack(track);}
    emit localImportChanged();m_saveTimer.start();QTimer::singleShot(0,this,&Backend::importNextLocalBatch);
  });
}
void Backend::mergeLocalTrack(QVariantMap track){
  if(m_relocateId.isEmpty())for(const auto &v:m_localTracks)if(v.toMap().value("localPath")==track.value("localPath")){track["id"]=v.toMap().value("id");break;}
  if(playable({track}).isEmpty()||track.value("localPath").toString().isEmpty())return;
  if(!track.contains("dateAdded")||track.value("dateAdded").toLongLong()<=0){track["dateAdded"]=QDateTime::currentMSecsSinceEpoch();}
  const auto id=track.value("id").toString();bool known=false;
  const auto replace=[&](QVariantList &rows){bool changed=false;for(auto &v:rows)if(itemId(v)==id){v=track;changed=true;}return changed;};
  known=replace(m_localTracks);if(!known)m_localTracks.append(track);
  replace(m_favorites);replace(m_history);
  if(m_undoType=="playlists"){for(auto &v:m_undoRows){auto list=v.toMap();auto rows=list.value("tracks").toList();if(replace(rows)){list["tracks"]=rows;v=list;}}}
  else replace(m_undoRows);
  for(auto &v:m_playlists){auto list=v.toMap();auto rows=list.value("tracks").toList();if(replace(rows)){list["tracks"]=rows;v=list;}}
  auto queue=m_queue.rows;if(replace(queue)&&queue!=m_queue.rows)m_queue.assign(queue);
  auto results=m_results.rows;if(replace(results)&&results!=m_results.rows)m_results.assign(results);
  if(current().value("id").toString()==id)emit trackChanged();
  emit libraryChanged();
}
void Backend::removeLocalFile(const QString &id){
  // Forget the library entry only; playlists and original audio files are untouched.
  for(int i=m_localTracks.size()-1;i>=0;--i)if(itemId(m_localTracks[i])==id)m_localTracks.removeAt(i);
  emit libraryChanged();m_saveTimer.start();emit toast("Removed from local files");
}
QVariantList Backend::searchLyrics(const QString &query) const {
  const auto text=query.simplified();if(text.isEmpty())return {};
  QVariantList rows;
  if(!m_lyricLines.isEmpty()){
    for(int i=0;i<m_lyricLines.size();++i){auto row=m_lyricLines[i].toMap();if(row.value("text").toString().contains(text,Qt::CaseInsensitive)){row["index"]=i;rows.append(row);}}
  }else{int index=0;for(const auto &line:m_lyrics.split('\n')){if(line.contains(text,Qt::CaseInsensitive))rows.append(QVariantMap{{"text",line},{"index",index},{"start",-1}});++index;}}
  return rows;
}

void Backend::importMusicFolder(const QUrl &url) {
  if(importingLocal()||!url.isLocalFile())return;
  const QFileInfo info(url.toLocalFile());const auto path=info.canonicalFilePath();
  if(!info.isDir()||!info.isReadable()||path.isEmpty()){emit toast("Choose a readable music folder");return;}
  if(!m_musicFolders.contains(path)){
    if(m_musicFolders.size()>=64){emit toast("You can save up to 64 music folders");return;}
    m_musicFolders.append(path);emit libraryChanged();m_saveTimer.start();
  }
  scanMusicFolders({path});
}
void Backend::rescanMusicFolders(){if(!importingLocal()&&!m_musicFolders.isEmpty())scanMusicFolders(m_musicFolders);}
void Backend::forgetMusicFolder(const QString &path){
  if(importingLocal())return;
  if(m_musicFolders.removeAll(path)){emit libraryChanged();m_saveTimer.start();emit toast("Folder forgotten; songs kept");}
}
void Backend::scanMusicFolders(const QStringList &folders){
  m_scanningFolders=true;m_scanLimited=false;m_scanFailed=0;emit localImportChanged();
  QVariantMap known;for(const auto &v:m_localTracks){const auto t=v.toMap();known[t.value("localPath").toString()]=t.value("localStamp");}
  request("folder-scan",{{"op","scan-folders"},{"folders",folders},{"known",known}},[this](const QVariantMap &data){
    m_scanningFolders=false;
    if(!data.value("ok").toBool()){emit localImportChanged();emit toast("Could not scan music folders. Try Rescan.");return;}
    m_scanLimited=data.value("limited").toBool();m_scanFailed=data.value("failed").toInt();
    QVariantList urls;for(const auto &v:data.value("files").toList())urls.append(QUrl::fromLocalFile(v.toString()));
    if(urls.isEmpty()){
      emit localImportChanged();emit toast(m_scanLimited?"Scan limit reached. Choose a smaller folder.":m_scanFailed?"Some folders could not be read; songs kept":"No new or changed audio files");
      m_scanLimited=false;m_scanFailed=0;return;
    }
    importLocalFiles(urls);
  });
}
void Backend::closePlaylistCleanup(){
  cancel("cleanup");m_cleanupBusy=false;m_cleanupId.clear();m_cleanupRows.clear();m_cleanupItems.clear();emit cleanupChanged();
}
void Backend::inspectPlaylist(const QString &id){
  closePlaylistCleanup();
  for(const auto &v:m_playlists){const auto p=v.toMap();if(p.value("id")!=id)continue;
    m_cleanupId=id;m_cleanupRows=p.value("tracks").toList();m_cleanupBusy=true;emit cleanupChanged();
    request("cleanup",{{"op","playlist-cleanup"},{"rows",m_cleanupRows}},[this](const QVariantMap &data){
      m_cleanupBusy=false;
      if(!data.value("ok").toBool()){emit cleanupChanged();emit toast("Could not inspect playlist. Try again.");return;}
      for(const auto &v:data.value("issues").toList()){
        const auto issue=v.toMap();const int i=issue.value("index").toInt();if(i<0||i>=m_cleanupRows.size())continue;
        auto row=m_cleanupRows[i].toMap();row["index"]=i;row["duplicate"]=issue.value("duplicate");row["missing"]=issue.value("missing");m_cleanupItems.append(row);
      }
      emit cleanupChanged();
    });return;
  }
}
void Backend::applyPlaylistCleanup(bool duplicates,bool missing){
  if(m_cleanupBusy||m_cleanupId.isEmpty())return;
  const auto id=m_cleanupId;const auto snapshot=m_cleanupRows;
  QVariantList selected;
  for(const auto &v:m_cleanupItems){const auto issue=v.toMap();if((duplicates&&issue.value("duplicate").toBool())||(missing&&issue.value("missing").toBool()))selected.append(issue.value("index"));}
  if(selected.isEmpty())return;
  m_cleanupBusy=true;emit cleanupChanged();
  // Recheck disk state off the UI thread before removing reviewed entries.
  request("cleanup",{{"op","playlist-cleanup"},{"rows",snapshot}},[this,id,snapshot,selected,duplicates,missing](const QVariantMap &data){
    m_cleanupBusy=false;
    if(!data.value("ok").toBool()){emit cleanupChanged();emit toast("Could not check playlist. Nothing removed.");return;}
    for(const auto &v:m_playlists){const auto p=v.toMap();if(p.value("id")!=id)continue;
      if(p.value("tracks").toList()!=snapshot){inspectPlaylist(id);emit toast("Playlist changed. Review the updated results.");return;}
      QVariantList indices;
      for(const auto &v:data.value("issues").toList()){const auto issue=v.toMap();
        if(selected.contains(issue.value("index"))&&((duplicates&&issue.value("duplicate").toBool())||(missing&&issue.value("missing").toBool())))indices.append(issue.value("index"));
      }
      removePlaylistRows(id,indices);inspectPlaylist(id);return;
    }
    closePlaylistCleanup();emit toast("Playlist no longer exists");
  });
}

void Backend::setDynamicAlbumColors(bool enabled) {
  if (dynamicAlbumColors() == enabled) return;
  m_settings.setValue("dynamicAlbumColors", enabled);
  emit settingsChanged();
  updateAlbumColors();
}

void Backend::updateAlbumColors() {
  if (!dynamicAlbumColors()) {
    if (m_hasAlbumColors) {
      m_hasAlbumColors = false;
      m_albumColors.clear();
      emit albumColorsChanged();
    }
    return;
  }

  const QString coverUrl = cover();
  if (coverUrl.isEmpty()) {
    if (m_hasAlbumColors) {
      m_hasAlbumColors = false;
      m_albumColors.clear();
      emit albumColorsChanged();
    }
    return;
  }

  if (m_paletteCache.contains(coverUrl)) {
    const auto palette = m_paletteCache.value(coverUrl);
    if (!palette.isEmpty()) {
      m_albumColors = palette;
      m_hasAlbumColors = true;
      emit albumColorsChanged();
      return;
    }
  }

  QImage img = RoundedArt::getCachedImage(QUrl(coverUrl));
  if (img.isNull()) {
    const QUrl url(coverUrl);
    if (url.isLocalFile()) {
      img.load(url.toLocalFile());
    } else if (QFile::exists(coverUrl)) {
      img.load(coverUrl);
    }
  }

  if (!img.isNull()) {
    QVariantMap newPalette = extractMaterialPalette(img);
    if (!newPalette.isEmpty()) {
      m_paletteCache.insert(coverUrl, newPalette);
      m_albumColors = newPalette;
      m_hasAlbumColors = true;
    } else {
      m_hasAlbumColors = false;
      m_albumColors.clear();
    }
    emit albumColorsChanged();
    return;
  }

  const QUrl url(coverUrl);
  if (url.scheme() == "http" || url.scheme() == "https") {
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    auto *net = RoundedArt::networkManager();
    if (net) {
      auto *reply = net->get(req);
      connect(reply, &QNetworkReply::finished, this, [this, reply, coverUrl] {
        reply->deleteLater();
        if (!dynamicAlbumColors()) return;
        if (cover() != coverUrl) return;
        if (reply->error() == QNetworkReply::NoError) {
          auto bytes = reply->readAll();
          QBuffer buffer(&bytes);
          buffer.open(QIODevice::ReadOnly);
          QImageReader reader(&buffer);
          reader.setAutoTransform(true);
          QImage fetched = reader.read();
          if (!fetched.isNull()) {
            QVariantMap newPalette = extractMaterialPalette(fetched);
            if (!newPalette.isEmpty()) {
              m_paletteCache.insert(coverUrl, newPalette);
              m_albumColors = newPalette;
              m_hasAlbumColors = true;
              emit albumColorsChanged();
              return;
            }
          }
        }
        
        // If it failed to load or extract from network
        if (m_hasAlbumColors) {
          m_hasAlbumColors = false;
          m_albumColors.clear();
          emit albumColorsChanged();
        }
      });
    }
  } else {
    // If it's a local file and we couldn't load it
    if (m_hasAlbumColors) {
      m_hasAlbumColors = false;
      m_albumColors.clear();
      emit albumColorsChanged();
    }
  }
}

QVariantMap Backend::extractMaterialPalette(const QImage &img) {
  if (img.isNull()) return {};

  QImage small = img.scaled(36, 36, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32);

  int hueBins[12] = {0};
  double satBins[12] = {0.0};
  double valBins[12] = {0.0};
  int totalCount = 0;

  for (int y = 0; y < small.height(); ++y) {
    const QRgb *scan = reinterpret_cast<const QRgb*>(small.constScanLine(y));
    for (int x = 0; x < small.width(); ++x) {
      QColor c(scan[x]);
      if (c.alpha() < 128) continue;
      float h = 0.0f, s = 0.0f, l = 0.0f;
      c.getHslF(&h, &s, &l);

      if (s > 0.12f && l > 0.10f && l < 0.90f) {
        int bin = qBound(0, static_cast<int>(h * 12.0f), 11);
        hueBins[bin]++;
        satBins[bin] += s;
        valBins[bin] += l;
        totalCount++;
      }
    }
  }

  float seedH = 0.60f;
  float seedS = 0.40f;
  float seedL = 0.50f;

  if (totalCount > 0) {
    int bestBin = 0;
    double maxScore = -1.0;
    for (int i = 0; i < 12; ++i) {
      double score = hueBins[i] * (satBins[i] / qMax(1, hueBins[i]));
      if (score > maxScore) {
        maxScore = score;
        bestBin = i;
      }
    }
    if (hueBins[bestBin] > 0) {
      seedH = static_cast<float>((bestBin + 0.5) / 12.0);
      seedS = static_cast<float>(qBound(0.25, satBins[bestBin] / hueBins[bestBin], 0.85));
      seedL = static_cast<float>(qBound(0.35, valBins[bestBin] / hueBins[bestBin], 0.65));
    }
  } else {
    qint64 rSum = 0, gSum = 0, bSum = 0, count = 0;
    for (int y = 0; y < small.height(); ++y) {
      const QRgb *scan = reinterpret_cast<const QRgb*>(small.constScanLine(y));
      for (int x = 0; x < small.width(); ++x) {
        QColor c(scan[x]);
        if (c.alpha() >= 128) {
          rSum += c.red();
          gSum += c.green();
          bSum += c.blue();
          count++;
        }
      }
    }
    if (count > 0) {
      QColor avgC(rSum / count, gSum / count, bSum / count);
      avgC.getHslF(&seedH, &seedS, &seedL);
    }
  }

  qreal secH = std::fmod(seedH + 0.04, 1.0);

  auto hslHex = [](qreal h, qreal s, qreal l) -> QString {
    h = std::fmod(h + 1.0, 1.0);
    s = qBound(0.0, s, 1.0);
    l = qBound(0.0, l, 1.0);
    return QColor::fromHslF(h, s, l).name();
  };

  QVariantMap darkRoleMap;
  darkRoleMap["primary"] = hslHex(seedH, qMin(1.0, seedS * 1.2 + 0.1), 0.78);
  darkRoleMap["primaryText"] = hslHex(seedH, 0.20, 0.10);
  darkRoleMap["primaryContainer"] = hslHex(seedH, seedS, 0.26);
  darkRoleMap["containerText"] = hslHex(seedH, qMin(1.0, seedS * 1.1), 0.88);
  darkRoleMap["secondary"] = hslHex(secH, seedS * 0.6, 0.72);
  darkRoleMap["background"] = hslHex(seedH, qMin(0.20, seedS * 0.25), 0.08);
  darkRoleMap["surface"] = hslHex(seedH, qMin(0.20, seedS * 0.25), 0.11);
  darkRoleMap["container"] = hslHex(seedH, qMin(0.25, seedS * 0.30), 0.15);
  darkRoleMap["high"] = hslHex(seedH, qMin(0.30, seedS * 0.35), 0.20);
  darkRoleMap["text"] = hslHex(seedH, 0.10, 0.94);
  darkRoleMap["muted"] = hslHex(seedH, qMin(0.20, seedS * 0.25), 0.68);
  darkRoleMap["outline"] = hslHex(seedH, qMin(0.20, seedS * 0.25), 0.30);

  QVariantMap lightRoleMap;
  lightRoleMap["primary"] = hslHex(seedH, qMin(1.0, seedS * 1.1 + 0.1), 0.40);
  lightRoleMap["primaryText"] = hslHex(seedH, 0.20, 0.98);
  lightRoleMap["primaryContainer"] = hslHex(seedH, qMin(0.60, seedS * 0.7), 0.88);
  lightRoleMap["containerText"] = hslHex(seedH, seedS, 0.16);
  lightRoleMap["secondary"] = hslHex(secH, seedS * 0.6, 0.45);
  lightRoleMap["background"] = hslHex(seedH, qMin(0.12, seedS * 0.15), 0.98);
  lightRoleMap["surface"] = hslHex(seedH, qMin(0.12, seedS * 0.15), 0.95);
  lightRoleMap["container"] = hslHex(seedH, qMin(0.15, seedS * 0.20), 0.90);
  lightRoleMap["high"] = hslHex(seedH, qMin(0.18, seedS * 0.25), 0.84);
  lightRoleMap["text"] = hslHex(seedH, 0.10, 0.10);
  lightRoleMap["muted"] = hslHex(seedH, qMin(0.15, seedS * 0.20), 0.40);
  lightRoleMap["outline"] = hslHex(seedH, qMin(0.15, seedS * 0.20), 0.78);

  QVariantMap res;
  res["dark"] = darkRoleMap;
  res["light"] = lightRoleMap;
  return res;
}

void Backend::setYoutubeUseForRecommendations(bool enabled) {
  if (youtubeUseForRecommendations() == enabled) return;
  m_settings.setValue("youtubeUseForRecommendations", enabled);
  m_saveTimer.start();
  emit settingsChanged();
}

void Backend::disconnectYouTube() {
  const auto p = cookies();
  if (p == dataPath() + "/cookies.txt")
    QFile::remove(p);
  m_settings.remove("cookies");
  const QString oauthFile = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/Sung/youtube_oauth.json";
  QVariantMap req{{"op", "yt-disconnect"}, {"oauthFile", oauthFile}};
  request("catalog", req, [this](const QVariantMap &) {
    m_youtubeConnected = false;
    m_youtubeAccountName.clear();
    m_youtubeChannelHandle.clear();
    m_youtubePlaylists.clear();
    m_youtubeSubscriptions.clear();
    m_settings.remove("youtubeConnected");
    m_settings.remove("youtubeAccountName");
    m_settings.remove("youtubeChannelHandle");
    m_saveTimer.start();
    emit settingsChanged();
    emit youtubeAccountChanged();
    emit youtubeDataChanged();
    emit toast("YouTube account disconnected.");
  });
}

void Backend::syncYouTubeData() {
  if (!m_youtubeConnected) return;
  const QString oauthFile = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/Sung/youtube_oauth.json";

  QVariantMap reqPlaylists{{"op", "yt-library-playlists"}, {"oauthFile", oauthFile}, {"cookies", cookies()}, {"limit", 50}};
  request("catalog", reqPlaylists, [this](const QVariantMap &data) {
    m_youtubePlaylists = data.value("items").toList();
    emit youtubeDataChanged();
  });

  QVariantMap reqSubs{{"op", "yt-library-subscriptions"}, {"oauthFile", oauthFile}, {"cookies", cookies()}, {"limit", 50}};
  request("catalog", reqSubs, [this](const QVariantMap &data) {
    m_youtubeSubscriptions = data.value("items").toList();
    emit youtubeDataChanged();
  });
}

QVariantList Backend::generateYouTubePersonalizationSignals() const {
  if (!youtubeConnected() || !youtubeUseForRecommendations()) return {};

  QVariantList ytSignals;
  QSet<QString> seenArtists;

  for (const auto &pVar : m_youtubePlaylists) {
    const QVariantMap p = pVar.toMap();
    const QString title = p.value("title").toString().trimmed();
    if (!title.isEmpty()) {
      ytSignals.append(QVariantMap{{"type", "playlist"}, {"name", title}, {"weight", 1.5}});
    }
  }

  for (const auto &sVar : m_youtubeSubscriptions) {
    const QVariantMap s = sVar.toMap();
    const QString artist = s.value("artist").toString().trimmed();
    if (!artist.isEmpty() && !seenArtists.contains(artist.toLower())) {
      seenArtists.insert(artist.toLower());
      ytSignals.append(QVariantMap{{"type", "artist"}, {"name", artist}, {"weight", 2.0}});
    }
  }

  return ytSignals;
}
