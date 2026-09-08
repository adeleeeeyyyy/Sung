#include "romanizer.h"
#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>

namespace {

static QHash<QString, QString> s_romanizeCache;
static QMutex s_cacheMutex;

struct KoreanData {
  static const QStringList &choseong() {
    static const QStringList list = {
        "g", "kk", "n", "d", "tt", "r", "m", "b", "pp", "s", "ss", "", "j", "jj", "ch", "k", "t", "p", "h"
    };
    return list;
  }
  static const QStringList &jungseong() {
    static const QStringList list = {
        "a", "ae", "ya", "yae", "eo", "e", "yeo", "ye", "o", "wa", "wae", "oe", "yo", "u", "wo", "we", "wi", "yu", "eu", "ui", "i"
    };
    return list;
  }
  // Standard 28 Hangul Jongseong (final consonants)
  static const QStringList &jongseong() {
    static const QStringList list = {
        "",   "k",  "kk", "ks", "n",  "nj", "nh", "t",  "l",  "lg",
        "lm", "lb", "ls", "lt", "lp", "lh", "m",  "p",  "ps", "t",
        "ss", "ng", "t",  "t",  "k",  "t",  "p",  "h"
    };
    return list;
  }
};

QString romanizeKoreanWord(const QString &word) {
  const auto &choList = KoreanData::choseong();
  const auto &jungList = KoreanData::jungseong();
  const auto &jongList = KoreanData::jongseong();

  QString res;
  res.reserve(word.size() * 3);

  for (int i = 0; i < word.size(); ++i) {
    const char32_t code = word[i].unicode();
    if (code >= 0xAC00 && code <= 0xD7A3) {
      const int sIdx = static_cast<int>(code - 0xAC00);
      const int cho = sIdx / 588;
      const int jung = (sIdx % 588) / 28;
      const int jong = sIdx % 28;

      QString cStr = (cho >= 0 && cho < choList.size()) ? choList[cho] : QString();
      const QString vStr = (jung >= 0 && jung < jungList.size()) ? jungList[jung] : QString();
      QString fStr = (jong >= 0 && jong < jongList.size()) ? jongList[jong] : QString();

      // Liaison rule: if next syllable starts with 'ㅇ' (cho == 11)
      if (jong > 0 && i + 1 < word.size()) {
        const char32_t nextCode = word[i + 1].unicode();
        if (nextCode >= 0xAC00 && nextCode <= 0xD7A3) {
          const int nextCho = static_cast<int>((nextCode - 0xAC00) / 588);
          if (nextCho == 11) {
            fStr = QString();
          }
        }
      }

      // Initial 'ㄹ' rule
      if (cho == 5) {
        if (i > 0 && word[i - 1].unicode() >= 0xAC00 && word[i - 1].unicode() <= 0xD7A3) {
          const int prevJong = static_cast<int>((word[i - 1].unicode() - 0xAC00) % 28);
          if (prevJong == 8) {
            cStr = "l";
          } else {
            cStr = "r";
          }
        } else {
          cStr = "r";
        }
      }

      res += cStr + vStr + fStr;
    } else {
      res += word[i];
    }
  }
  return res;
}

const QHash<QString, QString> &japaneseWordDict() {
  static const QHash<QString, QString> dict = {
      {"祭り", "matsuri"}, {"祭りに", "matsuri ni"}, {"祭りに行く", "matsuri ni iku"}, {"祭りが", "matsuri ga"},
      {"祭りの", "matsuri no"}, {"祭", "matsuri"}, {"ありがとう", "arigatou"}, {"夜に駆ける", "yoru ni kakeru"},
      {"君のことが好き", "kimi no koto ga suki"}, {"君が好き", "kimi ga suki"}, {"愛してる", "aishiteru"},
      {"寝溜めした", "nedame shita"}, {"寝溜め", "nedame"}, {"意味無いの", "imi naino"}, {"意味無い", "imi nai"},
      {"意味", "imi"}, {"知ってる", "shitteru"}, {"焦りが", "aseri ga"}, {"焦り", "aseri"},
      {"はみ出した", "hamidashita"}, {"日差し", "hizashi"}, {"眩しい", "mabushii"}, {"体", "karada"},
      {"だる重", "daru omo"}, {"感情", "kanjou"}, {"モドキ", "modoki"}, {"踊ったとて", "odotta tote"},
      {"何者", "nanimono"}, {"今更", "imasara"}, {"引き下がれない", "hikisagarenai"},
      {"皆が寝静まれば", "mina ga neshizumareba"}, {"寝静まれば", "neshizumareba"}, {"出番来る", "deban kuru"},
      {"出番", "deban"}, {"来る", "kuru"}, {"冴えない", "saenai"}, {"踊り明かすからね", "odoriakasu kara ne"},
      {"踊り明かす", "odoriakasu"}, {"海馬まで", "kaiba made"}, {"海馬", "kaiba"}, {"灰だらけ", "haidarake"},
      {"わかった気になれんのかね", "wakatta ki ni naren no ka ne"}, {"夜は情け", "yoru wa nasake"},
      {"情け", "nasake"}, {"肺が鳴け", "hai ga nake"}, {"肺", "hai"}, {"鳴け", "nake"}, {"ネット上", "netto jou"},
      {"息してる", "iki shiteru"}, {"結んで開いて", "musunde hiraite"}, {"顔も見えない", "kao mo mienai"},
      {"助言", "jogen"}, {"一過性", "ikkasei"}, {"エンカウント", "enkaunto"}, {"通じ合えない", "tsuujiaenai"},
      {"礼儀", "reigi"}, {"命令通り", "meireidoori"}, {"傷んでく", "itandeku"}, {"腐ってく", "kusatteku"},
      {"綺羅キラ星", "kirakira hoshi"}, {"綺羅", "kira"}, {"キラ星", "kirahoshi"}, {"吸って吐いて", "sutte haite"},
      {"貸し借り", "kashikari"}, {"段々", "dandan"}, {"ステップ複雑", "suteppu fukuzatsu"}, {"複雑", "fukuzatsu"},
      {"刻み込まれてしまった", "kizamikomarete shimatta"}, {"惨め", "mijime"}, {"庇った", "kabatta"},
      {"葬", "hou"}, {"過去問", "kakomon"}, {"解いて", "toite"}, {"夜明け", "yoake"},
      {"残酷な天使のテーゼ", "zankoku na tenshi no teeze"}, {"残酷な", "zankoku na"}, {"天使の", "tenshi no"},
      {"テーゼ", "teeze"}, {"少年よ", "shounen yo"}, {"神話になれ", "shinwa ni nare"}, {"神話", "shinwa"},
      {"少年", "shounen"}, {"蒼い風", "aoi kaze"}, {"胸のドア", "mune no doa"}, {"叩いても", "tadaitemo"},
      {"微笑んでる", "hohoenderu"}, {"運命さえ", "unmei sae"}, {"瞳", "hitomi"}, {"羽があること", "hane ga aru koto"},
      {"窓辺から", "madobe kara"}, {"飛び立つ", "tobitatsu"}, {"ほとばしる", "hotobashiru"}, {"熱いパトス", "atsui patosu"},
      {"思い出を", "omoide o"}, {"裏切るなら", "uragiru nara"}, {"宇宙を抱いて", "uchuu o daite"}, {"輝く", "kagayaku"},
      {"揺りかご", "yurikago"}, {"使者", "shisha"}, {"月あかり", "tsukiakari"}, {"映してる", "utsushiteru"},
      {"バイブル", "baiburu"}, {"悲しみ", "kanashimi"}, {"抱きしめた", "dakishimeta"}, {"命のかたち", "inochi no katachi"},
      {"光を放つ", "hikari o hanatsu"}, {"歴史をつくる", "rekishi o tsukuru"}, {"女神", "megami"}, {"生きる", "ikiru"},
      {"愛", "ai"}, {"君", "kimi"}, {"夜", "yoru"}, {"駆ける", "kakeru"}, {"好き", "suki"}, {"こと", "koto"},
      {"私", "watashi"}, {"僕", "boku"}, {"俺", "ore"}, {"あなた", "anata"}, {"心", "kokoro"}, {"胸", "mune"},
      {"目", "me"}, {"手", "te"}, {"声", "koe"}, {"夢", "yume"}, {"空", "sora"}, {"海", "umi"}, {"風", "kaze"},
      {"雨", "ame"}, {"雪", "yuki"}, {"花", "hana"}, {"星", "hoshi"}, {"月", "tsuki"}, {"太陽", "taiyou"},
      {"光", "hikari"}, {"影", "kage"}, {"世界", "sekai"}, {"未来", "mirai"}, {"過去", "kako"}, {"今", "ima"},
      {"明日", "ashita"}, {"昨日", "kinou"}, {"今日", "kyou"}, {"時間", "jikan"}, {"永遠", "eien"},
      {"幸せ", "shiawase"}, {"涙", "namida"}, {"笑う", "warau"}, {"泣く", "naku"}, {"歌", "uta"}, {"曲", "kyoku"},
      {"音", "oto"}, {"言葉", "kotoba"}, {"想い", "omoi"}, {"一人", "hitori"}, {"二人", "futari"},
      {"一緒", "issho"}, {"友達", "tomodachi"}, {"恋", "koi"}, {"人", "hito"}, {"道", "michi"}, {"街", "machi"},
      {"家", "ie"}, {"場所", "basho"}, {"扉", "tobira"}
  };
  return dict;
}

const QHash<QString, QString> &kanjiSingleDict() {
  static const QHash<QString, QString> dict = {
      {"祭", "matsuri"}, {"愛", "ai"}, {"気", "ki"}, {"心", "kokoro"}, {"人", "hito"}, {"日", "hi"}, {"月", "tsuki"},
      {"火", "hi"}, {"水", "mizu"}, {"木", "ki"}, {"金", "kin"}, {"土", "tsuchi"}, {"天", "ten"},
      {"地", "chi"}, {"男", "otoko"}, {"女", "onna"}, {"子", "ko"}, {"目", "me"}, {"手", "te"},
      {"足", "ashi"}, {"耳", "mimi"}, {"口", "kuchi"}, {"顔", "kao"}, {"頭", "atama"}, {"声", "koe"},
      {"言", "i"}, {"話", "hana"}, {"思", "omo"}, {"見", "mi"}, {"知", "shi"}, {"聞", "ki"},
      {"行", "i"}, {"来", "ki"}, {"出", "de"}, {"入", "hai"}, {"立", "tatsu"}, {"座", "suwa"},
      {"走", "hashi"}, {"飛", "tobi"}, {"泳", "oyo"}, {"買", "ka"}, {"売", "uri"}, {"書", "ka"},
      {"読", "yo"}, {"食", "tabe"}, {"飲", "nomi"}, {"作", "tsuku"}, {"会", "a"}, {"合", "a"},
      {"生", "iki"}, {"死", "shi"}, {"笑", "wara"}, {"泣", "na"}, {"歌", "uta"}, {"音", "oto"},
      {"光", "hika"}, {"影", "kage"}, {"風", "kaze"}, {"雨", "ame"}, {"雪", "yuki"}, {"空", "sora"},
      {"海", "umi"}, {"山", "yama"}, {"川", "kawa"}, {"花", "hana"}, {"星", "hoshi"}, {"夜", "yoru"},
      {"朝", "asa"}, {"昼", "hiru"}, {"夕", "yuu"}, {"春", "haru"}, {"夏", "natsu"}, {"秋", "aki"},
      {"冬", "fuyu"}, {"今", "ima"}, {"昔", "mukashi"}, {"前", "mae"}, {"後", "ato"}, {"上", "ue"},
      {"下", "shita"}, {"中", "naka"}, {"外", "soto"}, {"左", "hidari"}, {"右", "migi"}, {"東", "higashi"},
      {"西", "nishi"}, {"南", "minami"}, {"北", "kita"}, {"大", "oo"}, {"小", "chii"}, {"高", "taka"},
      {"安", "yasu"}, {"新", "atara"}, {"古", "furu"}, {"長", "naga"}, {"短", "mijika"}, {"重", "omo"},
      {"軽", "karu"}, {"強", "tsuyo"}, {"弱", "yowa"}, {"白", "shiro"}, {"黒", "kuro"}, {"赤", "aka"},
      {"青", "ao"}, {"黄", "kii"}, {"緑", "midori"}, {"君", "kimi"}, {"僕", "boku"}, {"俺", "ore"},
      {"私", "watashi"}, {"彼", "kare"}, {"神", "kami"}, {"鬼", "oni"}, {"竜", "ryuu"}, {"王", "ou"},
      {"夢", "yume"}, {"命", "inochi"}, {"世", "se"}, {"界", "kai"}, {"時", "toki"}, {"間", "aida"},
      {"道", "michi"}, {"街", "machi"}, {"家", "ie"}, {"國", "kuni"}, {"国", "kuni"}, {"城", "shiro"},
      {"無", "nai"}, {"有", "ari"}, {"非", "hi"}, {"不", "fu"}, {"未", "mi"}, {"切", "setsu"},
      {"絶", "zetsu"}, {"対", "tai"}, {"同", "ona"}, {"異", "koto"}, {"親", "oya"}, {"友", "tomo"},
      {"敵", "teki"}, {"勝", "katsu"}, {"負", "make"}, {"戦", "tata"}, {"争", "araso"}, {"平", "hei"},
      {"和", "wa"}, {"希", "ki"}, {"望", "bou"}, {"勇", "yuu"}, {"情", "jou"}
  };
  return dict;
}

const QHash<QString, QString> &yoonKanaMap() {
  static const QHash<QString, QString> map = {
      {"きゃ", "kya"}, {"きゅ", "kyu"}, {"きょ", "kyo"},
      {"しゃ", "sha"}, {"しゅ", "shu"}, {"しょ", "sho"},
      {"ちゃ", "cha"}, {"ちゅ", "chu"}, {"ちょ", "cho"},
      {"にゃ", "nya"}, {"にゅ", "nyu"}, {"にょ", "nyo"},
      {"ひゃ", "hya"}, {"ひゅ", "hyu"}, {"ひょ", "hyo"},
      {"みゃ", "mya"}, {"みゅ", "myu"}, {"みょ", "myo"},
      {"りゃ", "rya"}, {"りゅ", "ryu"}, {"りょ", "ryo"},
      {"ぎゃ", "gya"}, {"ぎゅ", "gyu"}, {"ぎょ", "gyo"},
      {"じゃ", "ja"}, {"じゅ", "ju"}, {"じょ", "jo"},
      {"ぢゃ", "ja"}, {"ぢゅ", "ju"}, {"ぢょ", "jo"},
      {"びゃ", "bya"}, {"びゅ", "byu"}, {"びょ", "byo"},
      {"ぴゃ", "pya"}, {"ぴゅ", "pyu"}, {"ぴょ", "pyo"},
      {"キャ", "kya"}, {"キュ", "kyu"}, {"キョ", "kyo"},
      {"シャー", "shaa"}, {"シャ", "sha"}, {"シュ", "shu"}, {"ショ", "sho"},
      {"チャ", "cha"}, {"チュ", "chu"}, {"チョ", "cho"},
      {"ニャ", "nya"}, {"ニュ", "nyu"}, {"ニョ", "nyo"},
      {"ヒャ", "hya"}, {"ヒュ", "hyu"}, {"ヒョ", "hyo"},
      {"ミャ", "mya"}, {"ミュ", "myu"}, {"ミョ", "myo"},
      {"リャ", "rya"}, {"リュ", "ryu"}, {"リョ", "ryo"},
      {"ギャ", "gya"}, {"ギュ", "gyu"}, {"ギョ", "gyo"},
      {"ジャ", "ja"}, {"ジュ", "ju"}, {"ジョ", "jo"},
      {"ビャ", "bya"}, {"ビュ", "byu"}, {"ビョ", "byo"},
      {"ピャ", "pya"}, {"ピュ", "pyu"}, {"ピョ", "pyo"},
      {"ファ", "fa"}, {"フィ", "fi"}, {"フェ", "fe"}, {"フォ", "fo"},
      {"ティ", "ti"}, {"ディ", "di"}, {"デュ", "dyu"},
      {"ウィ", "wi"}, {"ウェ", "we"}, {"ウォ", "wo"},
      {"ツィ", "tsi"}, {"ヴァ", "va"}, {"ヴィ", "vi"}, {"ヴ", "vu"}, {"ヴェ", "ve"}, {"ヴォ", "vo"}
  };
  return map;
}

const QHash<QString, QString> &singleKanaMap() {
  static const QHash<QString, QString> map = {
      {"あ", "a"}, {"い", "i"}, {"う", "u"}, {"え", "e"}, {"お", "o"},
      {"か", "ka"}, {"き", "ki"}, {"く", "ku"}, {"け", "ke"}, {"こ", "ko"},
      {"さ", "sa"}, {"し", "shi"}, {"す", "su"}, {"せ", "se"}, {"そ", "so"},
      {"た", "ta"}, {"ち", "chi"}, {"つ", "tsu"}, {"て", "te"}, {"と", "to"},
      {"な", "na"}, {"に", "ni"}, {"ぬ", "nu"}, {"ね", "ne"}, {"の", "no"},
      {"は", "ha"}, {"ひ", "hi"}, {"ふ", "fu"}, {"へ", "he"}, {"ほ", "ho"},
      {"ま", "ma"}, {"み", "mi"}, {"む", "mu"}, {"め", "me"}, {"も", "mo"},
      {"や", "ya"}, {"ゆ", "yu"}, {"よ", "yo"},
      {"ら", "ra"}, {"り", "ri"}, {"る", "ru"}, {"れ", "re"}, {"ろ", "ro"},
      {"わ", "wa"}, {"を", "wo"}, {"ん", "n"},
      {"が", "ga"}, {"ぎ", "gi"}, {"ぐ", "gu"}, {"げ", "ge"}, {"ご", "go"},
      {"ざ", "za"}, {"じ", "ji"}, {"ず", "zu"}, {"ぜ", "ze"}, {"ぞ", "zo"},
      {"だ", "da"}, {"ぢ", "ji"}, {"づ", "zu"}, {"で", "de"}, {"ど", "do"},
      {"ば", "ba"}, {"び", "bi"}, {"ぶ", "bu"}, {"べ", "be"}, {"ぼ", "bo"},
      {"ぱ", "pa"}, {"ぴ", "pi"}, {"ぷ", "pu"}, {"ぺ", "pe"}, {"ぽ", "po"},
      {"ぁ", "a"}, {"ぃ", "i"}, {"ぅ", "u"}, {"ぇ", "e"}, {"ぉ", "o"},
      {"ア", "a"}, {"イ", "i"}, {"ウ", "u"}, {"エ", "e"}, {"オ", "o"},
      {"カ", "ka"}, {"キ", "ki"}, {"ク", "ku"}, {"ケ", "ke"}, {"コ", "ko"},
      {"サ", "sa"}, {"シ", "shi"}, {"ス", "su"}, {"セ", "se"}, {"ソ", "so"},
      {"タ", "ta"}, {"チ", "chi"}, {"ツ", "tsu"}, {"テ", "te"}, {"ト", "to"},
      {"ナ", "na"}, {"ニ", "ni"}, {"ヌ", "nu"}, {"ネ", "ne"}, {"ノ", "no"},
      {"ハ", "ha"}, {"ヒ", "hi"}, {"フ", "fu"}, {"ヘ", "he"}, {"ホ", "ho"},
      {"マ", "ma"}, {"ミ", "mi"}, {"ム", "mu"}, {"メ", "me"}, {"モ", "mo"},
      {"ヤ", "ya"}, {"ユ", "yu"}, {"ヨ", "yo"},
      {"ラ", "ra"}, {"リ", "ri"}, {"ル", "ru"}, {"レ", "re"}, {"ロ", "ro"},
      {"ワ", "wa"}, {"ヲ", "wo"}, {"ン", "n"},
      {"ガ", "ga"}, {"ギ", "gi"}, {"グ", "gu"}, {"ゲ", "ge"}, {"ゴ", "go"},
      {"ザ", "za"}, {"ジ", "ji"}, {"ズ", "zu"}, {"ゼ", "ze"}, {"ぞ", "zo"},
      {"ダ", "da"}, {"ヂ", "ji"}, {"ヅ", "zu"}, {"デ", "de"}, {"ド", "do"},
      {"バ", "ba"}, {"ビ", "bi"}, {"ブ", "bu"}, {"ベ", "be"}, {"ボ", "bo"},
      {"パ", "pa"}, {"ピ", "pi"}, {"プ", "pu"}, {"ペ", "pe"}, {"ポ", "po"},
      {"ァ", "a"}, {"ィ", "i"}, {"ゥ", "u"}, {"ェ", "e"}, {"ォ", "o"}
  };
  return map;
}

QString callPythonRomanizer(const QString &text) {
  if (text.isEmpty()) {
    return text;
  }

  {
    QMutexLocker locker(&s_cacheMutex);
    if (s_romanizeCache.contains(text)) {
      return s_romanizeCache.value(text);
    }
  }

  QString python = qEnvironmentVariable("SUNG_PYTHON");
  if (python.isEmpty()) {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString bundled = appDir + "/../runtime/bin/python";
    python = QFile::exists(bundled) ? bundled : QStringLiteral("python3");
  }

  const QString helper = QCoreApplication::applicationDirPath() + "/../helper/romanize.py";
  if (!QFile::exists(helper)) {
    return QString();
  }

  QProcess proc;
  proc.start(python, {helper});
  if (!proc.waitForStarted(1000)) {
    return QString();
  }

  QJsonObject req;
  req["action"] = "romanize";
  req["text"] = text;

  proc.write(QJsonDocument(req).toJson(QJsonDocument::Compact));
  proc.closeWriteChannel();

  if (!proc.waitForFinished(3000)) {
    proc.kill();
    return QString();
  }

  if (proc.exitCode() != 0) {
    return QString();
  }

  const QByteArray output = proc.readAllStandardOutput();
  const QJsonDocument doc = QJsonDocument::fromJson(output);
  if (!doc.isObject()) {
    return QString();
  }

  const QJsonObject resp = doc.object();
  if (!resp.value("ok").toBool()) {
    return QString();
  }

  const QString res = resp.value("result").toString();
  if (!res.isEmpty()) {
    QMutexLocker locker(&s_cacheMutex);
    s_romanizeCache.insert(text, res);
  }
  return res;
}

QString romanizeJapaneseSegment(const QString &segment) {
  const auto &wDict = japaneseWordDict();
  const auto &kDict = kanjiSingleDict();
  const auto &yMap = yoonKanaMap();
  const auto &sMap = singleKanaMap();

  QString text = segment;

  // Step 1: Replace multi-character Kanji compounds / phrases (longest first)
  QList<QString> keys = wDict.keys();
  std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
  for (const auto &key : keys) {
    if (text.contains(key)) {
      text.replace(key, " " + wDict.value(key) + " ");
    }
  }

  // Step 2: Replace individual Kanji using kanjiSingleDict
  QString step2;
  step2.reserve(text.size() * 2);
  for (int i = 0; i < text.size(); ++i) {
    const QChar ch = text[i];
    const char32_t u = ch.unicode();
    if ((u >= 0x4E00 && u <= 0x9FAF) || (u >= 0x3400 && u <= 0x4DBF)) {
      const QString s(ch);
      if (kDict.contains(s)) {
        step2 += " " + kDict.value(s) + " ";
      } else {
        step2 += " ";
      }
    } else {
      step2 += ch;
    }
  }

  // Step 3: Convert Yōon pairs, Sokuon, and single Kana
  QString out;
  out.reserve(step2.size() * 2);

  for (int i = 0; i < step2.size(); ++i) {
    if (i + 1 < step2.size()) {
      const QString pair = step2.mid(i, 2);
      if (yMap.contains(pair)) {
        out += yMap.value(pair);
        i++;
        continue;
      }
    }

    const QChar ch = step2[i];
    const QString chStr(ch);

    if (ch == QChar(0x3063) || ch == QChar(0x30C3)) {
      if (i + 1 < step2.size()) {
        const QString nextPair = (i + 2 < step2.size()) ? step2.mid(i + 1, 2) : QString();
        const QString nextSingle = step2.mid(i + 1, 1);
        QString nextRomaji;
        if (!nextPair.isEmpty() && yMap.contains(nextPair)) {
          nextRomaji = yMap.value(nextPair);
        } else if (sMap.contains(nextSingle)) {
          nextRomaji = sMap.value(nextSingle);
        }

        if (!nextRomaji.isEmpty() && nextRomaji[0].isLetter()) {
          const QChar c = nextRomaji[0].toLower();
          if (c == 'c' || c == 's' || c == 't' || c == 'k' || c == 'p' || c == 'g' || c == 'z' || c == 'd' || c == 'b') {
            out += c;
            continue;
          }
        }
      }
      out += "'";
      continue;
    }

    if (ch == QChar(0x30FC)) {
      if (!out.isEmpty()) {
        const QChar last = out[out.size() - 1];
        if (last == 'a' || last == 'i' || last == 'u' || last == 'e' || last == 'o') {
          out += last;
          continue;
        }
      }
      continue;
    }

    if (sMap.contains(chStr)) {
      out += sMap.value(chStr);
    } else {
      out += ch;
    }
  }

  static const QRegularExpression multiSpace(R"(\s+)");
  return out.replace(multiSpace, " ").trimmed();
}

bool containsNonLatin(const QString &str) {
  for (const QChar &ch : str) {
    const char32_t u = ch.unicode();
    if ((u >= 0xAC00 && u <= 0xD7A3) || (u >= 0x3040 && u <= 0x30FF) || (u >= 0x4E00 && u <= 0x9FAF) || (u >= 0x3400 && u <= 0x4DBF)) {
      return true;
    }
  }
  return false;
}


QString romanizeSingleLine(const QString &line) {
  if (line.isEmpty() || !containsNonLatin(line)) {
    return line;
  }

  enum Script { Latin, Korean, Japanese };

  struct Segment {
    Script script;
    QString text;
  };

  QList<Segment> segments;

  Script currentScript = Script::Latin;
  QString currentText;

  for (int i = 0; i < line.size(); ++i) {
    const QChar ch = line[i];
    const char32_t u = ch.unicode();

    Script s = Script::Latin;
    if (u >= 0xAC00 && u <= 0xD7A3) {
      s = Script::Korean;
    } else if ((u >= 0x3040 && u <= 0x30FF) || (u >= 0x4E00 && u <= 0x9FAF) || (u >= 0x3400 && u <= 0x4DBF)) {
      s = Script::Japanese;
    } else {
      s = Script::Latin;
    }

    if (segments.isEmpty()) {
      currentScript = s;
      currentText.append(ch);
      segments.append({currentScript, currentText});
    } else {
      if (s == currentScript || (s == Script::Latin && (ch.isSpace() || ch.isPunct()))) {
        segments.last().text.append(ch);
      } else {
        currentScript = s;
        segments.append({currentScript, QString(ch)});
      }
    }
  }

  QString out;
  for (const auto &seg : segments) {
    QString converted;
    if (seg.script == Script::Korean) {
      converted = romanizeKoreanWord(seg.text);
    } else if (seg.script == Script::Japanese) {
      const QString pyRes = callPythonRomanizer(seg.text);
      if (!pyRes.isEmpty()) {
        converted = pyRes;
      } else {
        converted = romanizeJapaneseSegment(seg.text);
      }
    } else {
      converted = seg.text;
    }

    if (!out.isEmpty() && !out.endsWith(' ') && !converted.isEmpty() && !converted.startsWith(' ')) {
      out += " ";
    }
    out += converted;
  }

  static const QRegularExpression multiSpace(R"(\s+)");
  out = out.replace(multiSpace, " ").trimmed();

  if (!out.isEmpty() && out[0].isLower()) {
    out[0] = out[0].toUpper();
  }

  return out;
}

} // namespace

bool Romanizer::containsNonLatin(const QString &text) {
  return ::containsNonLatin(text);
}

QString Romanizer::romanizeText(const QString &text) {
  if (text.isEmpty() || !containsNonLatin(text)) {
    return text;
  }

  const QStringList lines = text.split('\n');
  QStringList romanizedLines;
  romanizedLines.reserve(lines.size());

  for (const auto &l : lines) {
    romanizedLines.append(romanizeSingleLine(l));
  }

  return romanizedLines.join('\n');
}

QVariantList Romanizer::romanizeLines(const QVariantList &lines) {
  if (lines.isEmpty()) {
    return {};
  }

  QVariantList result;
  result.reserve(lines.size());

  for (const auto &v : lines) {
    auto line = v.toMap();
    const QString origText = line.value("text").toString();
    line["text"] = romanizeSingleLine(origText);
    result.append(line);
  }

  return result;
}
