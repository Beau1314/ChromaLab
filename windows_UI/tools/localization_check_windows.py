#!/usr/bin/env python3
"""localization_check_windows.py -- the Windows port's four languages, held to their rules.

    windows_UI/tools/localization_check_windows.py --root windows_UI [--list-baseline] [--self-test]

The catalog is `windows_UI/assets/localization.json`, converted from the Mac's
`Localizable.xcstrings` by `xcstrings_to_json.py` (the English literal is the
key; `zh-Hans`, `zh-Hant` and `ja` are its values; `en` is never stored). The
pages and the models look their words up through `Localization` (`L()` in QML
and C++). This fails, naming each line, when:

  1. **shape**        the catalog is unreadable, its version is not 1, its
                      languages are not the four, or a key lacks a non-blank
                      `zh-Hans`, `zh-Hant` or `ja`;
  2. **placeholders** a translation does not use the arguments its key has
                      (`%@` in the key is one argument; a translation names
                      them `%@` in order or `%1$@`, `%2$@` by position, never
                      both in one sentence); number formats (`%.1f`) must be
                      the key's, in the key's order;
  3. **script**       a `zh-Hant` value holds a character only Simplified
                      writes, or a `zh-Hans` value one only Traditional writes
                      (`PAIRS` below: a table of telltale characters, not a
                      dictionary); a `ja` value holds a Simplified character
                      Japanese does not write (设 for 設, 图 for 図, 胶, 冲) or
                      an old form it no longer writes (圖, 黑, 會);
  4. **unwrapped**    a string literal holding Chinese or Japanese is handed
                      to the UI without `L(` -- over `windows_UI/qml` and
                      `windows_UI/src` (the `tests/` fixtures are a check's
                      own words, as on the Mac). What is grandfathered until
                      task 6.1 migrates it stands in
                      `tools/localization_exceptions.json`, each entry with a
                      reason; an entry that excepts nothing any more is a
                      failure, so the file only ever shrinks;
  5. **uncatalogued** a key the sources look up through `L()` is not in the
                      catalog.

Unlike the Mac's check there is no stale rule: the Windows catalog keeps the
Mac's whole 1203-key catalog, and only part of it is a Windows literal yet.
Also unlike the Mac's, the scan looks for CJK-holding literals (English keys
are coined at the migration, not before), so an English literal shown without
L() is not this gate's word -- task 6.1 wraps the sentences, and any *new*
English sentence should be coined as its key at the same time.

`--write-baseline` (task 6.0 only) fills the exceptions file with today's
unwrapped literals; `--self-test` breaks each rule once, on a catalog and a
source of its own, and fails unless each break is reported.
"""
import argparse
import json
import re
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
LANGUAGES = ("zh-Hans", "zh-Hant", "ja")
SOURCES = ("qml", "src")
CJK = re.compile(r"[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff]")

# ---- the telltale tables (ported verbatim from app/SpektraLab/tools/localization_check.py) ----

PAIRS = """
机機 这這 们們 说說 时時 为為 会會 对對 开開 关關 设設 计計 认認 证證 选選 择擇 显顯 图圖 览覽 过過 还還 进進 远遠 连連 运運
边邊 适適 导導 张張 强強 归歸 录錄 径徑 总總 户戶 护護 报報 拟擬 拥擁 据據 换換 损損 摄攝 数數 断斷 无無 旧舊 暂暫
杂雜 权權 条條 来來 构構 标標 样樣 栏欄 检檢 毕畢 气氣 没沒 浅淺 测測 浏瀏 渐漸 温溫 湿濕 滤濾 灯燈 灵靈 点點 烦煩 热熱
爱愛 状狀 独獨 现現 环環 电電 画畫 监監 盘盤 码碼 确確 离離 积積 称稱 稳穩 竞競 笔筆 简簡 类類 紧緊 红紅 级級 纯純 线線
组組 细細 终終 经經 结結 给給 绝絕 统統 继繼 续續 维維 网網 罗羅 罚罰 联聯 肤膚 脑腦 脱脫 节節 药藥 虑慮 补補 装裝 观觀
视視 觉覺 订訂 让讓 许許 论論 访訪 评評 识識 词詞 译譯 试試 话話 该該 详詳 语語 误誤 请請 读讀 调調 谁誰 谈談 负負 贝貝
财財 责責 贵貴 费費 资資 赖賴 赛賽 赞贊 赶趕 趋趨 跃躍 踪蹤 车車 转轉 轮輪 软軟 轻輕 载載 较較 辑輯 输輸 达達 迁遷 违違
迟遲 递遞 逻邏 遗遺 释釋 钥鑰 错錯 键鍵 长長 门門 闭閉 问問 间間 闲閒 队隊 阳陽 阴陰 阶階 际際 陆陸 陈陳 随隨 隐隱 难難
顶頂 项項 顺順 预預 领領 频頻 题題 颜顏 额額 风風 飞飛 饰飾 马馬 驱驅 验驗 鱼魚 鸟鳥 麦麥 黄黃 齐齊 龄齡 胶膠 纸紙 负負
冲衝 晕暈 颗顆 宽寬 齿齒 边邊 张張 传傳 储儲 库庫 导導 档檔 设設 备備 质質 击擊 触觸 镜鏡 头頭 闪閃 灯燈 帧幀
补補 偿償 档檔 圆圓 层層 属屬 岁歲 带帶 帮幫 广廣 应應 库庫 废廢 异異 弹彈 归歸 径徑 彻徹 忆憶 态態 惯慣 战戰 扩擴 扫掃
抢搶 挡擋 挂掛 捞撈 掷擲 搁擱 摆擺 撑撐 敌敵 斩斬 时時 昼晝 晒曬 术術 杀殺 极極 柜櫃 栋棟 梦夢 樱櫻 欢歡 残殘 毁毀
汉漢 沟溝 泪淚 济濟 涨漲 润潤 涩澀 渊淵 滚滾 满滿 灭滅 炉爐 烂爛 烧燒 爷爺 牵牽 犹猶 狱獄 猎獵 玛瑪 琐瑣 畅暢 疗療
痒癢 盖蓋 矿礦 砖磚 础礎 祸禍 禅禪 窍竅 窝窩 笼籠 筛篩 篮籃 粮糧 纠糾 纪紀 纲綱 纳納 纵縱 纷紛 纹紋 纺紡 绍紹 绑綁 绒絨
绕繞 绘繪 绩績 绪緒 绿綠 缓緩 编編 缘緣 缩縮 缝縫 缠纏 罢罷 翘翹 耸聳 聪聰 肃肅 肠腸 肿腫 胁脅 胜勝 腾騰 舰艦 艺藝 苏蘇
荐薦 荣榮 莲蓮 获獲 营營 萧蕭 萨薩 蓝藍 虏虜 虫蟲 蚀蝕 蛮蠻 衬襯 袭襲 规規 览覽 誉譽 讨討 训訓 记記 讲講 讽諷 诀訣 诉訴
诊診 诗詩 诚誠 诞誕 询詢 诱誘 课課 谅諒 谊誼 谋謀 谎謊 谓謂 谢謝 谣謠 谦謙 谨謹 谱譜 贡貢 败敗 账賬 货貨 贩販 贪貪 贫貧
购購 贯貫 贱賤 贴貼 贷貸 贸貿 贺賀 贼賊 赁賃 赋賦 赌賭 赏賞 赐賜 赔賠 赚賺 赠贈 跷蹺 践踐 踊踴 轨軌 轩軒 轰轟 轴軸 辅輔
辆輛 辈輩 辉輝 辞辭 辩辯 迈邁 逊遜 遥遙 邓鄧 邮郵 邻鄰 郑鄭 酱醬 酿釀 针針 钉釘 钓釣 钙鈣 钝鈍 钞鈔 钢鋼 钦欽 钩鉤 钱錢
钻鑽 铁鐵 铃鈴 铅鉛 铜銅 铝鋁 银銀 铸鑄 铺鋪 链鏈 销銷 锁鎖 锅鍋 锐銳 锡錫 锤錘 锦錦 镀鍍 镇鎮 闯闖 闷悶 闸閘 闹鬧 阁閣
阅閱 阔闊 阵陣 险險 陕陝 隶隸 雾霧 韩韓 页頁 顷頃 顽頑 顾顧 顿頓 颁頒 颂頌 颇頗 颈頸 颖穎 颠顛 飘飄 饥飢 饭飯 饮飲 饱飽
饼餅 饿餓 馆館 驰馳 驳駁 驶駛 驻駐 驾駕 骂罵 骄驕 骑騎 骗騙 骤驟 鸡雞 鸣鳴 鸭鴨 鸿鴻 鹅鵝 鹰鷹 龙龍 龟龜 么麼 个個 仅僅
从從 众眾 优優 伤傷 体體 侧側 俩倆 债債 倾傾 偿償 儿兒 兰蘭 关關 兴興 养養 写寫 军軍 农農 冯馮 决決 况況 冻凍 净淨 凉涼
减減 凤鳳 击擊 凿鑿 刘劉 则則 刚剛 创創 删刪 别別 剧劇 剂劑 剑劍 劝勸 办辦 务務 动動 励勵 劲勁 劳勞 势勢 匀勻 区區 医醫
华華 协協 单單 卖賣 卢盧 卫衛 却卻 厂廠 厅廳 厉厲 压壓 厌厭 厕廁 县縣 参參 双雙 变變 叙敘 号號 叹嘆 吓嚇 吕呂 吗嗎
员員 呜嗚 咏詠 响響 哑啞 唤喚 啸嘯 喷噴 嘱囑 团團 园園 围圍 国國 圣聖 场場 坏壞 块塊 坚堅 坛壇 坝壩 坟墳 垒壘 垦墾 垫墊
堕墮 墙牆 壮壯 声聲 壳殼 处處 备備 头頭 夹夾 夺奪 奋奮 奖獎 妆妝 妇婦 妈媽 娱娛 婴嬰 孙孫 学學 宁寧 宝寶 实實 宠寵 审審
宪憲 宫宮 宾賓 寝寢 对對 寻尋 导導 寿壽 将將 尔爾 尘塵 尝嘗 层層 屉屜 届屆 属屬 岁歲 岛島 岭嶺 峡峽 币幣 师師 帅帥 帐帳
帜幟 带帶 帮幫 干幹 庄莊 庆慶 庐廬 库庫 应應 庙廟 废廢 开開 异異 弃棄 张張 弥彌 弯彎 弹彈 强強 归歸 彦彥 彻徹 忆憶 忧憂
怀懷 态態 总總 恋戀 恳懇 恶惡 悦悅 悬懸 惊驚 惧懼 惨慘 惩懲 惫憊 愤憤 愿願 慑懾 懒懶 戏戲 战戰 户戶 扑撲 执執 扩擴 扫掃
扬揚 扰擾 抚撫 抛拋 抢搶 护護 报報 拟擬 拢攏 拣揀 拥擁 拦攔 拧擰 拨撥 择擇 挚摯 挛攣 挠撓 挡擋 挣掙 挤擠 挥揮 捞撈 损損
捡撿 换換 捣搗 据據 掳擄 掷擲 掸撣 掺摻 揽攬 搀攙 摄攝 摆擺 摇搖 摊攤 撑撐 撵攆 撷擷 敌敵 敛斂 数數 斋齋 斗鬥 斩斬 断斷
""".split()
NOT_BOTH = set("干斗冲获愿么弥")  # ambiguous after all: both scripts write them
SIMPLIFIED_ONLY = {p[0] for p in PAIRS if len(p) == 2 and p[0] != p[1]} - NOT_BOTH
TRADITIONAL_ONLY = {p[1] for p in PAIRS if len(p) == 2 and p[0] != p[1]}
SIMPLIFIED_ONLY -= TRADITIONAL_ONLY

JA_WRITES = set("会体写区医励却参双叙号嘱国堕壮声学宝寝寿将届属峡彦径恋悦惨据数断旧昼机条来残没浅温湿灯炉点状独画"
                "禅称脱虫蛮装触誉遥麦黄辞踊")
JA_ALSO_NOT = set("冲获愿么后种专")
KYUJITAI = """
圖図 黑黒 會会 體体 學学 國国 來来 數数 點点 畫画 圓円 邊辺 轉転 濕湿 溫温 燈灯 錄録 號号 對対 應応 經経 檢検 驗験
縣県 區区 醫医 單単 雙双 聲声 寶宝 將将 屬属 舊旧 晝昼 條条 殘残 淺浅 爐炉 狀状 獨独 稱称 蟲虫 裝装 觸触 譽誉 麥麦
黃黄 擇択 戰戦 樂楽 實実 發発 變変 廣広 當当 綠緑 鐵鉄 價価 鹽塩 擴拡 歲歳 處処 譯訳 讀読 賣売 續続 輕軽 關関 險険
靜静 顯顕 齒歯 龍竜 龜亀 壓圧 嚴厳 劑剤 勞労 參参 圍囲 壞壊 壽寿 惡悪 圍囲 澤沢 擔担 據拠 曉暁 櫻桜 氣気 滿満 燒焼
爭争 獸獣 疊畳 盡尽 稅税 穩穏 竊窃 絲糸 繪絵 纖繊 缺欠 聽聴 肅粛 膽胆 藝芸 藏蔵 蘇蘇 衞衛 觀観 覺覚 讓譲 豐豊 賴頼
轉転 辭辞 遲遅 邊辺 醉酔 釋釈 鑄鋳 鑛鉱 錢銭 鐘鐘 隨随 隱隠 雜雑 難難 靈霊 顏顔 驛駅 髮髪 鬪闘 黨党 齊斉 廳庁 傳伝
兩両 兒児 剩剰 勵励 卽即 卷巻 嘆嘆 團団 圖図 壯壮 奧奥 孃嬢 寢寝 屆届 峽峡 帶帯 廢廃 彈弾 從従 徑径 戀恋 拜拝 搖揺
攝摂 敍叙 晉晋 暑暑 曆暦 歐欧 歸帰 殼殻 淨浄 淚涙 濟済 濱浜 瀨瀬 爲為 獻献 瓣弁 癡痴 發発 盜盗 碎砕 祕秘 稻稲 穀穀
粹粋 經経 縱縦 總総 繩縄 繼継 脫脱 臺台 與与 舊旧 莊荘 螢蛍 蠶蚕 裏裏 覽覧 謠謡 證証 譯訳 豫予 貳弐 賣売 踐践 輕軽
辨弁 遞逓 鄕郷 醫医 錄録 鎭鎮 關関 陷陥 隸隷 霸覇 頰頬 顯顕 驅駆 驗験 髓髄 鷄鶏 默黙 齡齢 歷歴 檔档
""".split()
KYUJITAI_ONLY = {p[0] for p in KYUJITAI if len(p) == 2 and p[0] != p[1]}
NOT_JAPANESE_SIMPLIFIED = (SIMPLIFIED_ONLY - JA_WRITES) | JA_ALSO_NOT

EXCEPTIONS_FILE = HERE / "localization_exceptions.json"


def exceptions() -> dict:
    if not EXCEPTIONS_FILE.exists():
        return {}
    return json.loads(EXCEPTIONS_FILE.read_text(encoding="utf-8")).get("unwrapped", {})


# ---- reading QML and C++ -------------------------------------------------------------------------

def strip_comments(source: str) -> str:
    """The source with // and /* */ comments blanked (positions preserved)."""
    out, i, n = list(source), 0, len(source)
    while i < n:
        if source.startswith("//", i):
            j = source.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif source.startswith("/*", i):
            j = source.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def scan(source: str):
    """Every `"`-literal of a QML/C++ source, comments blanked: (start, end, text)."""
    source = strip_comments(source)
    out, i, n = [], 0, len(source)
    while i < n:
        if source[i] == '"':
            start = i
            i += 1
            text = []
            while i < n and source[i] != '"':
                if source[i] == "\\" and i + 1 < n:
                    text.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\"}.get(source[i + 1], source[i + 1]))
                    i += 2
                    continue
                text.append(source[i])
                i += 1
            out.append((start, i + 1, "".join(text)))
            i += 1
        else:
            i += 1
    return out


def chains(literals, source: str, cpp: bool):
    """The literal chains a lookup holds: a literal directly preceded by
    `L(`, continued while a `+` (QML and C++'s QString concatenation)
    separates it from the next, or — C++ only — nothing but whitespace does
    (adjacent literals are one literal to the compiler). Yields
    (offset, key, [spans])."""
    out, i = [], 0
    while i < len(literals):
        a, b, _ = literals[i]
        if not re.search(r"L\s*\(\s*$", source[max(0, a - 8):a]):
            i += 1
            continue
        key, spans, k = "", [], i
        while True:
            key += literals[k][2]
            spans.append((literals[k][0], literals[k][1]))
            if k + 1 >= len(literals):
                break
            between = source[literals[k][1]:literals[k + 1][0]]
            joins = bool(re.fullmatch(r"\s*\+\s*", between)) or (cpp and between.strip() == "")
            if not joins:
                break
            k += 1
        out.append((a, key, spans))
        i = k + 1
    return out


def wrapped_spans(literals, source: str, cpp: bool):
    spans = set()
    for _, _, chain in chains(literals, source, cpp):
        spans.update(chain)
    return spans


def lookups(literals, source: str, cpp: bool):
    """Every L() key: (offset, key)."""
    return [(a, key) for a, key, _ in chains(literals, source, cpp)]


def line_of(source: str, offset: int) -> int:
    return source.count("\n", 0, offset) + 1


# ---- the rules -----------------------------------------------------------------------------------

def arguments_of(text: str):
    """(indices a format uses, whether it mixes `%@` and `%N$@`). `%%` is a per-cent sign."""
    used, positional, sequential, nxt = set(), False, False, 1
    for m in re.finditer(r"%(%|@|(\d+)\$@)", text):
        if m.group(1) == "%":
            continue
        if m.group(2):
            positional = True
            used.add(int(m.group(2)))
        else:
            sequential = True
            used.add(nxt)
            nxt += 1
    return used, positional and sequential


def check_catalog(path: Path, problems: list) -> dict:
    try:
        catalog = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        problems.append(f"shape: {path.name} cannot be read as JSON: {error}")
        return {}
    if catalog.get("version") != 1:
        problems.append(f"shape: {path.name}: its version is {catalog.get('version')!r}, not 1")
    if catalog.get("languages") != ["en", *LANGUAGES]:
        problems.append(f"shape: {path.name}: its languages are {catalog.get('languages')!r}, not the four")
    strings = catalog.get("strings", {})
    for key, entry in sorted(strings.items()):
        values = {}
        for language in LANGUAGES:
            value = entry.get(language, "")
            if not isinstance(value, str) or not value.strip():
                problems.append(f"shape: {path.name}: {language} of {key!r} is missing or blank")
                continue
            values[language] = value
        for language, value in values.items():
            if "%" in key:
                want, _ = arguments_of(key)
                got, mixed = arguments_of(value)
                numbers = r"%[-+ 0#]*\d*(?:\.\d+)?(?:ll|l|h)?[dfgsuxX]"
                if re.findall(numbers, key) != re.findall(numbers, value):
                    problems.append(f"placeholders: {path.name}: {language} of {key!r} is {value!r}: its numbers "
                                    f"({', '.join(re.findall(numbers, value)) or 'none'}) are not the key's, in the key's order")
                if want and (got != want or mixed):
                    problems.append(f"placeholders: {path.name}: {language} of {key!r} is {value!r}: it uses the arguments "
                                    f"{sorted(got)}{' both by order and by position' if mixed else ''}, the key has {sorted(want)}")
            if language == "ja":
                for table, what in ((NOT_JAPANESE_SIMPLIFIED, "Simplified Chinese, which Japanese does not write"),
                                    (KYUJITAI_ONLY, "an old form (旧字体) modern Japanese does not write")):
                    wrong = sorted(set(value) & table)
                    if wrong:
                        problems.append(f"script: {path.name}: ja of {key!r} has {''.join(wrong)}: {what}: {value!r}")
                continue
            wrong = sorted(set(value) & (SIMPLIFIED_ONLY if language == "zh-Hant" else TRADITIONAL_ONLY))
            if wrong:
                other = "Simplified" if language == "zh-Hant" else "Traditional"
                problems.append(f"script: {path.name}: {language} of {key!r} has {''.join(wrong)}, which only {other} writes: {value!r}")
    return strings


def check_sources(root: Path, problems: list):
    """Rule 4 over qml/ and src/ (the tests' fixtures are a check's own words);
    returns the L() keys the sources look up."""
    excepted = exceptions()
    used_exceptions = set()
    looked: dict = {}
    for directory in SOURCES:
        for path in sorted((root / directory).rglob("*")):
            if path.suffix not in (".qml", ".cpp", ".hpp", ".h"):
                continue
            relative = str(path.relative_to(root)).replace("\\", "/")
            source = path.read_text(encoding="utf-8")
            literals = scan(source)
            cpp = path.suffix != ".qml"
            for offset, key in lookups(literals, source, cpp):
                looked.setdefault(key, f"{relative}:{line_of(source, offset)}")
            spans = wrapped_spans(literals, source, cpp)
            for a, b, text in literals:
                if not CJK.search(text) or (a, b) in spans:
                    continue
                name = f"{relative}:{text}"
                if name in excepted:
                    used_exceptions.add(name)
                    continue
                problems.append(f"unwrapped: {relative}:{line_of(source, a)}: a CJK literal {text!r} is shown without L(")
    for name in sorted(set(excepted) - used_exceptions):
        problems.append(f"unwrapped: the exception {name!r} excepts nothing any more: take it out of {EXCEPTIONS_FILE.name}")
    return looked


def run(root: Path) -> list:
    problems: list = []
    strings = check_catalog(root / "assets" / "localization.json", problems)
    looked = check_sources(root, problems)
    for key, where in sorted(looked.items()):
        if key not in strings:
            problems.append(f"uncatalogued: {where}: L({key!r}) is not in localization.json")
    return problems


def write_baseline(root: Path) -> int:
    """Every unwrapped CJK literal of today becomes an exception, each with the
    reason it still stands (task 6.1 migrates them; the file only shrinks)."""
    existing = exceptions()
    unwrapped = {}
    for directory in SOURCES:
        for path in sorted((root / directory).rglob("*")):
            if path.suffix not in (".qml", ".cpp", ".hpp", ".h"):
                continue
            relative = str(path.relative_to(root)).replace("\\", "/")
            source = path.read_text(encoding="utf-8")
            literals = scan(source)
            cpp = path.suffix != ".qml"
            spans = wrapped_spans(literals, source, cpp)
            for a, b, text in literals:
                if CJK.search(text) and (a, b) not in spans:
                    unwrapped[f"{relative}:{text}"] = (
                        existing.get(f"{relative}:{text}", "pre-6.1 Windows string; wrap with L() and coin its English key in task 6.1"))
    EXCEPTIONS_FILE.write_text(json.dumps({"unwrapped": unwrapped}, ensure_ascii=False, indent=1) + "\n",
                               encoding="utf-8")
    print(f"localization_check_windows: {len(unwrapped)} baseline exceptions -> {EXCEPTIONS_FILE.name}")
    return 0


# ---- the check, checked --------------------------------------------------------------------------

def self_test() -> int:
    def catalog_of(changes):
        strings = {
            "Settings": {"zh-Hans": "设置", "zh-Hant": "設定", "ja": "設定"},
            "Done": {"zh-Hans": "完成", "zh-Hant": "完成", "ja": "完了"},
            "%@ of %@ frames": {"zh-Hans": "第 %1$@ 张，共 %2$@ 张",
                                "zh-Hant": "第 %1$@ 張，共 %2$@ 張",
                                "ja": "%2$@枚中%1$@枚"},
        }
        for change in changes:
            change(strings)
        return {"version": 1, "languages": ["en", "zh-Hans", "zh-Hant", "ja"], "strings": strings}

    good_source = ('// a comment with 中文 inside stays quiet\n'
                   'import QtQuick\n'
                   'Text { text: Localization.L("Settings") }\n'
                   'Text { text: Localization.L("Done") }\n'
                   'Text { text: Localization.L("%@ of %@ frames") }\n')

    def trial(name, want, catalog=None, source=None, baseline=None):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "assets").mkdir()
            (root / "assets" / "localization.json").write_text(
                json.dumps(catalog or catalog_of([]), ensure_ascii=False), encoding="utf-8")
            (root / "qml").mkdir()
            (root / "qml" / "Page.qml").write_text(source or good_source, encoding="utf-8")
            (root / "src").mkdir()
            if baseline is not None:
                (root / "tools").mkdir()
                (root / "tools" / "localization_exceptions.json").write_text(
                    json.dumps({"unwrapped": baseline}, ensure_ascii=False), encoding="utf-8")
            global EXCEPTIONS_FILE
            saved, EXCEPTIONS_FILE = EXCEPTIONS_FILE, root / "tools" / "localization_exceptions.json"
            try:
                problems = run(root)
            finally:
                EXCEPTIONS_FILE = saved
        kinds = sorted({p.split(":", 1)[0] for p in problems})
        ok = kinds == ([want] if want else [])
        print(f"{'ok  ' if ok else 'FAIL'} {name}: " + ("; ".join(problems) if problems else "no problem reported"))
        return ok

    results = [
        trial("the good catalog and source pass", None),
        trial("1 a key with no zh-Hant fails", "shape",
              catalog=catalog_of([lambda s: s["Done"].pop("zh-Hant")])),
        trial("1 a blank ja fails", "shape",
              catalog=catalog_of([lambda s: s["Done"].update(ja=" ")])),
        trial("2 a translation that drops an argument fails", "placeholders",
              catalog=catalog_of([lambda s: s["%@ of %@ frames"].update(**{"zh-Hans": "第 %1$@ 张"})])),
        trial("2 a translation that names an argument the key lacks fails", "placeholders",
              catalog=catalog_of([lambda s: s["%@ of %@ frames"].update(**{"zh-Hant": "第 %1$@ 張，共 %3$@ 張"})])),
        trial("2 a translation that mixes order and position fails", "placeholders",
              catalog=catalog_of([lambda s: s["%@ of %@ frames"].update(**{"zh-Hant": "第 %@ 張，共 %2$@ 張"})])),
        trial("3 Simplified in a zh-Hant value fails", "script",
              catalog=catalog_of([lambda s: s["Settings"].update(**{"zh-Hant": "设置"})])),
        trial("3 Traditional in a zh-Hans value fails", "script",
              catalog=catalog_of([lambda s: s["Settings"].update(**{"zh-Hans": "設定"})])),
        trial("3 Simplified in a ja value fails (设定 for 設定)", "script",
              catalog=catalog_of([lambda s: s["Settings"].update(**{"ja": "设定"})])),
        trial("3 Simplified that Traditional shares, in a ja value, fails (冲)", "script",
              catalog=catalog_of([lambda s: s["Done"].update(**{"ja": "冲洗完了"})])),
        trial("3 a correct Japanese value with 選択・拡張・導入・頁 passes", None,
              catalog=catalog_of([lambda s: s["Done"].update(**{"ja": "選択・拡張・導入・頁・写真・図・設定"})])),
        trial("3 an old form in a ja value fails (圖 for 図)", "script",
              catalog=catalog_of([lambda s: s["Done"].update(**{"ja": "圖を完了"})])),
        trial("4 a CJK literal shown without the lookup fails", "unwrapped",
              source=good_source.replace('Text { text: Localization.L("Done") }\n',
                                         'Text { text: "完成" }\n')),
        trial("4 a CJK literal an exception holds passes", None,
              source=good_source.replace('Text { text: Localization.L("Done") }\n',
                                         'Text { text: "完成" }\n'),
              baseline={"qml/Page.qml:完成": "grandfathered"}),
        trial("4 an exception that excepts nothing any more fails", "unwrapped",
              baseline={"qml/Absent.qml:完成": "stale"}),
        trial("4 a CJK literal inside a comment passes", None,
              source=good_source.replace('Text { text: Localization.L("Done") }\n',
                                         '// Text { text: "完成" }\n')),
        trial("4 a chain of two literals is one wrapped lookup", None,
              source=good_source.replace('Localization.L("Settings")',
                                         'Localization.L("Set" + "tings")')),
        trial("5 a lookup the catalog lacks fails", "uncatalogued",
              source=good_source.replace('Localization.L("Done")', 'Localization.L("Not in it")')),
    ]
    failed = results.count(False)
    print(f"localization_check_windows --self-test: {len(results)} trials, {failed} failed")
    return 1 if failed else 0


def main() -> int:
    # This machine's console codepage (GBK) cannot print Japanese; the gate's
    # words are names and values from the catalog, so speak UTF-8 throughout.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    root_default = HERE.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(root_default), help="the windows_UI directory")
    ap.add_argument("--write-baseline", action="store_true",
                    help="fill the exceptions file with today's unwrapped literals (task 6.0 only)")
    ap.add_argument("--self-test", action="store_true", help="break each rule once and see it reported")
    args = ap.parse_args()
    root = Path(args.root).resolve()
    if not (root / "assets" / "localization.json").exists():
        print(f"localization_check_windows: no catalog under {root}", file=sys.stderr)
        return 2
    if args.self_test:
        return self_test()
    if args.write_baseline:
        return write_baseline(root)
    problems = run(root)
    for problem in problems:
        print(problem)
    strings = json.loads((root / "assets" / "localization.json").read_text(encoding="utf-8")).get("strings", {})
    print(f"localization_check_windows: {len(strings)} keys in four languages, {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
