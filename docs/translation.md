# 翻譯遊戲文字（字型檔）

日期：2026-10-07。分支 `claude/zh-tw`。目標是做繁體中文化 MOD（見 [Mods](mods.md)）。

## 文字放在哪裡

遊戲的文字不是字串，而是**每個檔案自帶的字型裡的字號**。每種語言一組檔案
（字尾 E／F／G／I／J／S，例如 `gxfJ`）：

| 檔案 | 內容 | 字串數 |
| --- | --- | --- |
| `gxf?` | World Grand Prix 劇情字幕 | 1585 |
| `adv?` | 選單、說明文字（version 2 的 pack，前面另有圖片與版面） | 901 |
| `gxfr?` | 劇情相關 | 58 |
| `pauseF?` | 暫停選單 | 39 |
| `resiF?` | 結果畫面 | 8 |
| `FNT_S?` | 系統訊息 | 41 |
| `s08tf?` | 教學 | 397 |

`l01?`、`FL_?` 等檔沒有字型；畫在圖片上的字見下面「圖片上的字」。

光碟上的檔案都壓縮過（開頭 `0F F5 12 ED`，遊戲用靜態連結的 LZX 解碼器 `824E2B10` 串流解開）。
**MOD 可以直接放解壓後的檔案**：遊戲也接受沒壓縮的 pack（已用日文 `gxfJ` 驗證），不需要壓縮器。

## 拿到解壓後的檔案

```bash
SFR_DUMP_DECOMPRESSED=<資料夾> ...            # 跑一次遊戲，載入想要的檔案
python scripts/unpack_assets.py <資料夾> <遊戲資料夾> <輸出資料夾> [檔名...]
```

`SFR_DUMP_DECOMPRESSED` 把解碼器每次呼叫的輸出存成 `<n>.bin`，`index.txt` 記下大小、解碼器
context 與壓縮資料開頭 64 bytes；`unpack_assets.py` 用這 64 bytes 在光碟檔裡找出是哪個檔，
同一個 context 連續的呼叫依序接起來。只會拿到該次執行有載入的檔案（日文要
`SFR_GAME_LANGUAGE=ja`）。

## 字型檔格式

一個 `pack`（`"pack" 00 01 03 00`，u16 項目數 3，+12 的 u32 照抄，+16 起是 4 個 u32 偏移），三個項目：

1. **FONTDATF**：60 bytes 標頭（+16 字數、+20 每頁格數、+24 頁數、+26 頁寬高、+30 最後一頁寬高、
   +34 字級 22、+36 每頁列數、+42 格子 23×34），接著每字 16 bytes（big-endian）：
   墨跡寬、左側距、UTF-16 字碼、字號、右側距；最後 4 bytes 照抄。字號依字碼排序，0 是空白。
2. **字頁**：u16 頁數、u16 1、u32 偏移[頁數]、u32 大小[頁數]、每頁一個 `0x11`、頁名（以 0 結尾），
   補到每頁 64 bytes 後接各頁的 DDS（A8R8G8B8、寬 512、高 512／256／128）。第 n 字在第 n 格，
   每列 22 格、格子 23×34。
3. **FONTSTLB**：`"FONTSTLB"`、8 bytes 照抄、u32 字串數、從本段開頭算的偏移，接著每個字串是
   32-bit 字號，`0x01000000` 結尾；`0x01000002` 換行，`0x01000013`／`0x01000014` 夾住注音（假名讀音）。
   檔案結尾對齊 16 bytes。

## 改字

```bash
python scripts/sfr_font_text.py export <檔案> <strings.json>     # 匯出（[漢字|讀音] 表示注音）
python scripts/sfr_font_text.py build <檔案> <strings.json> <字型> <輸出> [--size 20]
python scripts/sfr_font_text.py check <檔案>                     # 解析後重寫，應完全相同
```

`build` 依新字串重排字號、重畫字頁、重寫三段：原檔已有的非漢字（英數、假名、標點）沿用原字形，
漢字用給的字型重畫（字型沒有的字沿用原字形）。字型用 Noto Sans TC（`NotoSansTC-VF.ttf`，粗細 700，
20 px，基線 y=27）與原字形粗細相近。`check` 對 E／J 的 gxf、gxfr、pauseF、resiF、FNT_S、adv 都重寫出
完全相同的檔案。

驗證：新存檔進 World Grand Prix 的開場劇情，`gxfJ` 前 22 句換成中文，字幕正確顯示；`advJ` 換上中文
選單文字後，主選單與 Offline Mode 的說明框和圓環項目名稱顯示中文（頁面標題與 Omochao 對話框在別的檔）。

## 圖片上的字

日文版有日文字的貼圖：`advJ` 的 16 張賽道名稱條（1024×64）與標題 LOGO 下的「ソニック フリーライダーズ」
（第 17 張，含其光暈）、`s08tsuiJ` 的教學標題（第 13～16 張）。比賽 HUD（`FL_?_ST`）在日文版也是英文。
這些都是一般的 DDS（多為 DXT5、沒有 mipmap），`scripts/sfr_texture.py` 可列出、匯出 PNG，並把新圖編碼成
同格式同大小後原地寫回，檔案其他部分不動（DXT 編碼用主軸端點加最小平方修正，重新編碼原圖約 50 dB）。

## 繁體中文化 MOD

譯文與建置腳本在另一個 repo：
[Free-Riders-Recompiled-Traditional-Chinese-Mod](https://github.com/YuutaTsubasa/Free-Riders-Recompiled-Traditional-Chinese-Mod)。
那裡只放譯文與工具，原文由 `tools/prepare.py` 從玩家自己的遊戲檔產生；建置用到這裡的
`sfr_font_text.py`、`sfr_texture.py` 與 `unpack_assets.py`。

選了日文後語音也是日文；文字與語音要不要能分開選，之後再看。
