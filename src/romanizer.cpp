#include "romanizer.h"
#include <QRegularExpression>
#include <QHash>
#include <QStringList>
#include <algorithm>

namespace {

struct KoreanData {
  static const QStringList &choseong() {
    static const QStringList list = {"g", "kk", "n", "d", "tt", "r", "m", "b", "pp", "s", "ss", "", "j", "jj", "ch", "k", "t", "p", "h"};
    return list;
  }
  static const QStringList &jungseong() {
    static const QStringList list = {"a", "ae", "ya", "yae", "eo", "e", "yeo", "ye", "o", "wa", "wae", "oe", "yo", "u", "wo", "we", "wi", "yu", "eu", "ui", "i"};
    return list;
  }
  static const QStringList &jongseong() {
    static const QStringList list = {"", "k", "k", "ks", "n", "nj", "nh", "t", "l", "lg", "lm", "lb", "ls", "lt", "lp", "lh", "m", "p", "ps", "t", "ss", "ng", "t", "t", "p", "h"};
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

      QString cStr = choList[cho];
      const QString vStr = jungList[jung];
      QString fStr = jongList[jong];

      if (cStr == "r") {
        if (i == 0 || word[i - 1].unicode() < 0xAC00 || word[i - 1].unicode() > 0xD7A3) {
          cStr = "r";
        } else {
          const int prevJong = static_cast<int>((word[i - 1].unicode() - 0xAC00) % 28);
          if (prevJong == 8) {
            cStr = "l";
          }
        }
      }

      res += cStr + vStr + fStr;
    } else {
      res += word[i];
    }
  }
  return res;
}

const QHash<QString, QString> &japaneseDict() {
  static const QHash<QString, QString> dict = {
      {"ありがとう", "arigatou"},
      {"夜に駆ける", "yoru ni kakeru"},
      {"君のことが好き", "kimi no koto ga suki"},
      {"君が好き", "kimi ga suki"},
      {"愛してる", "aishiteru"},
      {"愛", "ai"},
      {"君", "kimi"},
      {"夜", "yoru"},
      {"駆ける", "kakeru"},
      {"好き", "suki"},
      {"こと", "koto"},
      {"私", "watashi"},
      {"僕", "boku"},
      {"俺", "ore"},
      {"あなた", "anata"},
      {"心", "kokoro"},
      {"胸", "mune"},
      {"瞳", "hitomi"},
      {"目", "me"},
      {"手", "te"},
      {"声", "koe"},
      {"夢", "yume"},
      {"空", "sora"},
      {"海", "umi"},
      {"風", "kaze"},
      {"雨", "ame"},
      {"雪", "yuki"},
      {"花", "hana"},
      {"星", "hoshi"},
      {"月", "tsuki"},
      {"太陽", "taiyou"},
      {"光", "hikari"},
      {"影", "kage"},
      {"世界", "sekai"},
      {"未来", "mirai"},
      {"過去", "kako"},
      {"今", "ima"},
      {"明日", "ashita"},
      {"昨日", "kinou"},
      {"今日", "kyou"},
      {"時間", "jikan"},
      {"永遠", "eien"},
      {"幸せ", "shiawase"},
      {"涙", "namida"},
      {"笑う", "warau"},
      {"泣く", "naku"},
      {"歌", "uta"},
      {"曲", "kyoku"},
      {"音", "oto"},
      {"言葉", "kotoba"},
      {"想い", "omoi"},
      {"一人", "hitori"},
      {"二人", "futari"},
      {"一緒", "issho"},
      {"友達", "tomodachi"},
      {"恋", "koi"},
      {"人", "hito"},
      {"道", "michi"},
      {"街", "machi"},
      {"家", "ie"},
      {"場所", "basho"},
      {"扉", "tobira"},
      {"寝溜め", "nedame"},
      {"意味", "imi"},
      {"知ってる", "shitteru"},
      {"他人", "tanin"},
      {"人生", "jinsei"},
      {"機嫌", "kigen"},
      {"礼典", "reiten"},
      {"失点", "shitten"},
      {"怠惰", "taida"},
      {"論理", "ronri"},
      {"絶頂", "zetchou"},
      {"返事", "henji"},
      {"街灯", "gaitou"},
      {"明かり", "akari"},
      {"照らして", "terashite"},
      {"誰か", "dareka"},
      {"祝ってる", "iwatteru"},
      {"僻んで", "higande"},
      {"焦り", "aseri"},
      {"日差し", "hizashi"},
      {"眩しい", "mabushii"},
      {"体", "karada"},
      {"感情", "kanjou"},
      {"何者", "nanimono"},
      {"今更", "imasara"},
      {"引き下がれない", "hikisagarenai"},
      {"皆", "mina"},
      {"寝静まれば", "neshizumareba"},
      {"出番", "deban"},
      {"来る", "kuru"},
      {"冴えない", "saenai"},
      {"踊り明かす", "odoriakasu"},
      {"海馬", "kaiba"},
      {"灰だらけ", "haidarake"},
      {"誰", "dare"},
      {"情け", "nasake"},
      {"肺", "hai"},
      {"鳴け", "nake"},
      {"廃", "hai"},
      {"息", "iki"},
      {"助言", "jogen"},
      {"一過性", "ikkasei"},
      {"通じ合えない", "tsuujiaenai"},
      {"礼儀", "reigi"},
      {"命令通り", "meireidoori"},
      {"傷んでく", "itandeku"},
      {"腐ってく", "kusatteku"},
      {"貸し借り", "kashikari"},
      {"段々", "dandan"},
      {"複雑", "fukuzatsu"},
      {"刻み込まれてしまった", "kizamikomarete shimatta"},
      {"惨め", "mijime"},
      {"庇った", "kabatta"},
      {"過去問", "kakomon"},
      {"解いて", "toite"},
      {"夜明け", "yoake"}
  };
  return dict;
}

const QHash<QString, QString> &kanaMap() {
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
      {"ア", "a"}, {"イ", "i"}, {"ウ", "u"}, {"エ", "e"}, {"オ", "o"},
      {"カ", "ka"}, {"キ", "ki"}, {"ク", "ku"}, {"ケ", "ke"}, {"コ", "ko"},
      {"サ", "sa"}, {"シ", "shi"}, {"ス", "su"}, {"セ", "se"}, {"ソ", "so"},
      {"タ", "ta"}, {"チ", "chi"}, {"ツ", "tsu"}, {"テ", "te"}, {"ト", "to"},
      {"ナ", "na"}, {"ニ", "ni"}, {"ヌ", "nu"}, {"네", "ne"}, {"ノ", "no"},
      {"ハ", "ha"}, {"ヒ", "hi"}, {"フ", "fu"}, {"ヘ", "he"}, {"ホ", "ho"},
      {"マ", "ma"}, {"ミ", "mi"}, {"ム", "mu"}, {"メ", "me"}, {"モ", "mo"},
      {"ヤ", "ya"}, {"ユ", "yu"}, {"ヨ", "yo"},
      {"ラ", "ra"}, {"リ", "ri"}, {"ル", "ru"}, {"レ", "re"}, {"ロ", "ro"},
      {"ワ", "wa"}, {"ヲ", "wo"}, {"ン", "n"},
      {"ガ", "ga"}, {"ギ", "gi"}, {"グ", "gu"}, {"ゲ", "ge"}, {"ゴ", "go"},
      {"ザ", "za"}, {"ジ", "ji"}, {"ズ", "zu"}, {"ゼ", "ze"}, {"ゾ", "zo"},
      {"ダ", "da"}, {"ヂ", "ji"}, {"ヅ", "zu"}, {"デ", "de"}, {"ド", "do"},
      {"バ", "ba"}, {"ビ", "bi"}, {"ブ", "bu"}, {"ベ", "be"}, {"ボ", "bo"},
      {"パ", "pa"}, {"ピ", "pi"}, {"プ", "pu"}, {"ペ", "pe"}, {"ポ", "po"}
  };
  return map;
}

QString romanizeJapaneseLine(const QString &line) {
  const auto &dict = japaneseDict();
  const auto &kMap = kanaMap();

  QString res = line;
  QList<QString> keys = dict.keys();
  std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });

  for (const auto &key : keys) {
    if (res.contains(key)) {
      res.replace(key, " " + dict.value(key) + " ");
    }
  }

  QString out;
  out.reserve(res.size() * 2);
  for (int i = 0; i < res.size(); ++i) {
    const QString ch(res[i]);
    if (kMap.contains(ch)) {
      out += kMap.value(ch);
    } else {
      out += res[i];
    }
  }

  static const QRegularExpression multiSpace(R"(\s+)");
  return out.replace(multiSpace, " ").trimmed();
}

bool containsNonLatin(const QString &str) {
  for (const QChar &ch : str) {
    const char32_t u = ch.unicode();
    if ((u >= 0xAC00 && u <= 0xD7A3) || (u >= 0x3040 && u <= 0x30FF) || (u >= 0x4E00 && u <= 0x9FAF)) {
      return true;
    }
  }
  return false;
}

QString romanizeSingleLine(const QString &line) {
  if (line.isEmpty() || !containsNonLatin(line)) {
    return line;
  }

  bool hasKorean = false;
  for (const QChar &ch : line) {
    const char32_t u = ch.unicode();
    if (u >= 0xAC00 && u <= 0xD7A3) {
      hasKorean = true;
      break;
    }
  }

  QString romanized;
  if (hasKorean) {
    romanized = romanizeKoreanWord(line);
  } else {
    romanized = romanizeJapaneseLine(line);
  }

  if (!romanized.isEmpty() && romanized[0].isLower()) {
    romanized[0] = romanized[0].toUpper();
  }

  return romanized;
}

} // namespace

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
