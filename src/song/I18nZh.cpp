// Chinese translations of the song module's user-facing messages.
#include "common/I18n.h"
#include "song/Song.h"

namespace gs {

namespace {
const TrPair kSongZh[] = {
    // files / archives
    {"Cannot open file: ", "无法打开文件: "},
    {"Not a regular file: ", "不是普通文件: "},
    {"File too large (limit 64 MB): ", "文件太大 (上限 64MB): "},
    {"Failed to read file: ", "读取文件失败: "},
    {"ZIP file too small", "ZIP 文件太小"},
    {"Not a valid ZIP (central directory not found)", "不是有效的 ZIP (找不到目录)"},
    {"ZIP directory corrupt or ZIP64 (unsupported)", "ZIP 目录损坏或不支持 ZIP64"},
    {"Corrupt ZIP directory entry", "ZIP 目录项损坏"},
    {"ZIP directory read out of bounds", "ZIP 目录读取越界"},
    {"Corrupt ZIP local header", "ZIP 本地头损坏"},
    {"ZIP data out of bounds", "ZIP 数据越界"},
    {"ZIP entry too large", "ZIP 条目太大"},
    {"Unsupported ZIP compression method", "不支持的 ZIP 压缩方式"},
    {"zlib initialization failed", "zlib 初始化失败"},
    {"Decompressed data too large", "解压后太大"},
    {"ZIP decompression failed", "ZIP 解压失败"},
    {"Not found in ZIP: ", "ZIP 中找不到 "},
    {"Too many XML nodes", "XML 节点过多"},
    {"XML nested too deeply", "XML 嵌套过深"},
    {"Not valid XML", "不是有效的 XML"},
    // .gtab
    {"Line %d: invalid bpm", "第%d行: bpm 无效"},
    {"Line %d: invalid time signature (e.g. 4/4)", "第%d行: 拍号无效 (例: 4/4)"},
    {"Line %d: tuning must be 6 note names, e.g. E2 A2 D3 G3 B3 E4", "第%d行: 调弦格式应为 6 个音名, 如 E2 A2 D3 G3 B3 E4"},
    {"Line %d: invalid capo", "第%d行: capo 无效"},
    {"Line %d \"%s\": %s", "第%d行 \"%s\": %s"},
    {"Line %d: bar %d has %.3g beats, expected %.3g\n", "第%d行: 第%d小节有 %.3g 拍, 应为 %.3g 拍\n"},
    {"expected {bpm=100} or {time=3/4}", "应为 {bpm=100} 或 {time=3/4}"},
    {"invalid bpm", "bpm 无效"},
    {"invalid time signature", "拍号无效"},
    {"unknown directive", "未知指令"},
    {"missing ]", "缺少 ]"},
    {"invalid chord shape (e.g. [C:x32010])", "和弦把位格式错误 (例: [C:x32010])"},
    {"chord not in the chord library: ", "和弦库里没有这个和弦: "},
    {"chord has no notes", "和弦没有任何音"},
    {"unrecognized token", "无法识别"},
    {"expected string.fret with string 1..6 and fret 0..24", "应为 弦.品, 弦 1..6, 品 0..24"},
    {"unrecognized note", "无法识别的音符"},
    {"invalid duration (use /1 /2 /4 /8 /16 /32, optionally with . or t)", "时值错误 (应为 /1 /2 /4 /8 /16 /32, 可加 . 或 t)"},
    {"unknown modifier '", "未知修饰符 '"},
    {"Too many notes", "音符太多"},
    {"The score contains no notes", "谱子里没有任何音符"},
    // ASCII tab
    {"No ASCII tab found (expected lines like e|---0---|)", "没有找到 ASCII 六线谱 (形如 e|---0---|)"},
    {"No notes recognized in the tab", "六线谱中没有识别到音符"},
    // MIDI
    {"Not a MIDI file", "不是 MIDI 文件"},
    {"Corrupt MIDI header", "MIDI 头损坏"},
    {"Invalid MIDI time division", "MIDI 时间格式无效"},
    {"The MIDI file has no usable melodic track (drum tracks excluded)", "MIDI 文件中没有可用的旋律音轨 (鼓轨除外)"},
    {"Track ", "音轨 "},
    {"No usable track", "没有可用的音轨"},
    {"The selected track has no notes", "所选音轨没有音符"},
    // General MIDI instrument families
    {"Piano", "钢琴"},
    {"Chromatic Percussion", "色彩打击乐"},
    {"Organ", "风琴"},
    {"Guitar", "吉他"},
    {"Bass Guitar", "贝斯"},
    {"Strings", "弦乐"},
    {"Ensemble", "合奏"},
    {"Brass", "铜管"},
    {"Reed", "簧管"},
    {"Pipe", "笛"},
    {"Synth Lead", "合成主音"},
    {"Synth Pad", "合成柔音"},
    {"Synth Effects", "合成效果"},
    {"Ethnic", "民族乐器"},
    {"Percussive", "打击乐"},
    {"Sound Effects", "音效"},
    // MusicXML
    {"Not a MusicXML score (root element: ", "不是 MusicXML 乐谱 (根元素: "},
    {"The MusicXML file has no parts", "MusicXML 中没有声部"},
    {"No usable part", "没有可用的声部"},
    {"The selected part has no notes", "所选声部没有音符"},
    {"No score found inside the MXL archive", "MXL 压缩包中找不到乐谱"},
    // Guitar Pro 6/7/8 (GPIF)
    {"Corrupt GPX data", "GPX 数据损坏"},
    {"Corrupt GPX compressed data", "GPX 压缩数据损坏"},
    {"GPX file too small", "GPX 文件太小"},
    {"Invalid GPX format after decompression", "GPX 解压后格式错误"},
    {"Not a Guitar Pro 6 (.gpx) file", "不是 Guitar Pro 6 (.gpx) 文件"},
    {"score.gpif not found in the GPX file", "GPX 中找不到 score.gpif"},
    {"Not a GPIF score", "不是 GPIF 乐谱"},
    {"The GPIF score has no tracks", "GPIF 中没有音轨"},
    {"The GPIF score has no bars", "GPIF 中没有小节"},
    {"Too many bars", "小节太多"},
    {"No usable guitar track", "没有可用的吉他音轨"},
    // Guitar Pro 3/4/5
    {"Unrecognized Guitar Pro file", "无法识别的 Guitar Pro 文件"},
    {"Not a Guitar Pro 3/4/5 file", "不是 Guitar Pro 3/4/5 文件"},
    {"Unrecognized Guitar Pro version: ", "无法识别的 Guitar Pro 版本: "},
    {"Unsupported Guitar Pro version: ", "不支持的 Guitar Pro 版本: "},
    {"Corrupt Guitar Pro header", "Guitar Pro 文件头损坏"},
    {"Corrupt Guitar Pro file (bad bar/track count)", "Guitar Pro 文件损坏 (小节/音轨数异常)"},
    {"Corrupt Guitar Pro file (track info)", "Guitar Pro 文件损坏 (音轨信息)"},
    {"Corrupt Guitar Pro file (bar contents)", "Guitar Pro 文件损坏 (小节内容)"},
};
}  // namespace

void registerSongTranslations() { addTranslations(kSongZh); }

}  // namespace gs
