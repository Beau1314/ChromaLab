# 合成 Capture One 目录（阶段 5 的样本与验证）

公开渠道没有可下载的 `.cocatalog` 样本（Capture One 试用版自带样本库，但许可限制
其再分发）。这两个脚本按公开逆向情报（Core Data 风格 SQLite：ZIMAGE/ZVARIANT/
ZVARIANTLAYER/ZVARIANTMETADATA/ZCOLLECTION/ZIMAGEINCOLLECTION/ZENTITIES/
ZDOCUMENTCONTENT/ZVERSIONINFO/ZPATHLOCATION）合成一个目录，并用 spklib-host 的
线协议验证它——`capture_one.probe` 的 unsupported 报错会点名缺失的表.列，构成
迭代回路。

## 用法

```
python -I tools/c1-sample/make_synthetic_catalog.py   # 建 D:/spk-fixtures/c1-sample/
python -I tools/c1-sample/probe_helper.py <spklib-host.exe> \
    capture_one.probe '{"path":"D:/spk-fixtures/c1-sample/MyCatalog.cocatalog"}'
```

生成器把 `D:/spk-fixtures/raw` 下的 CR2 作为被引用照片（referenced，非托管）。

## 已验证的链路（2026-11-20，spklib 0.4.0）

- `capture_one.probe` → supported: true（format 16.3.9.9 Pro Windows，schema 1621）
- `capture_one.link` → 镜像库；集合树（project/group/album 实体名正确解析）
- 相册成员、评分、色彩标签、变体、UUID 全部正确同步
- 路径解析 + `check_files` → exists: true

## 边界与坑（真实目录会踩到的）

- **绝对路径只认 `ZPATHLOCATION.ZMACROOT`**：spklib 是 macOS 移植，`ZWINROOT`
  被读但取值为空——真实 **Windows** C1 目录的绝对路径写在 ZWINROOT、ZMACROOT 为
  NULL，经此 host 链接会全部离线（path 退化为 `/文件名`，exists: false）。
  真样本到手后第一件事：确认此边界，必要时在 spklib 补 ZWINROOT 回退。
- `captured_local` 恒 null：host 二进制里没有拍摄时间列名，疑似 0.4.0 未读取。
- ZENTITIES 的键是 `Z_ENT` 列（非 Z_PK）：合成时两列同值最稳。
