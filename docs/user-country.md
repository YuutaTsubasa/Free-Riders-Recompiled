# Windows 使用者國家與原始國家轉換

本工作直接接續 `ExGetXConfigSetting` category 3 / setting 14 的真實啟動需求。
Windows 的使用者地理設定、介面語言、遊戲執行區域是三個獨立來源。

`query_native_country()` 每次快照呼叫一次 `GetUserDefaultGeoName`；實際啟動已記錄
`iso=TW xbox_country=101`。未使用介面語言推測國家，也沒有採用模擬器的美國預設值。
API 失敗、numeric M49、自訂或無映射值明確停止。支援資料集為固定來源中的107個
ISO國家代碼，17及94保留，不接受。Windows API需求為Windows10 1709或更新。

原始查詢 `0x82ACB5FC` / LR `0x824D1BD4` 的 buffer=`0x701311D0`，capacity=1，
required=`0x701311D2`。相容層精確寫入一個國家BYTE與兩個BE required-size bytes。
所有寫入範圍先驗證；維持 output-before-required 重疊次序。語言仍是4-byte輸出。
不支援、未設定、過短或null buffer沿用已核對的錯誤／長度語意。

原始國家轉換函式 `0x824D1BA8..0x824D1DA8` 有128條指令。首次成功查詢後，
執行到 `bctr`，因原重編譯器將函式內跳轉誤當作函式呼叫而停止：
`indirect-call @0x824D1D2C`。真實查詢證據在 `out/user-country-boot-retry.log`。
表格在 `0x82000968`，105 bytes，38個不同函式內目的地；原始frame、CTR計算及
國家轉換保留，不能加入假回傳值或把函式中間地址當成獨立函式。

診斷產生器的實作以完整原始函式LF正規化SHA-256限制適用範圍：
`323a22fa66dd76eda2f701f414cf8c6fdf23a2d71377567178cbc9ee587b94de`。
僅把該 `bctr` 的呼叫／return改為函式內switch/goto；未知CTR停止。
原始table load、128條指令註解、所有分支及region fallback必須保留。
在下一個原始語言函式入口 `0x824D0B58` / LR `0x82439678` 唯讀記錄 incoming r3，
用以驗證國家轉換真正返回後的結果。

首次實跑另觀察到既有臨界區等待未實作造成的偶發停止：
`critical-section-contention @0xFFCFFFD4`，
日誌 `out/user-country-boot.log`。後續已實作真正的臨界區等待並完成針對性驗證；
原始國家轉換也在整合實跑返回34，詳見 [最新啟動證據](startup-continuation.md)。

原始資料來源：

- [Xenia固定版本國家映射及ExGetXConfigSetting ABI](https://raw.githubusercontent.com/xenia-project/xenia/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_xconfig.cc)
- [Microsoft GetUserDefaultGeoName](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getuserdefaultgeoname)

這些初始化進展尚不等於原始標題畫面、繪圖提交、確認輸入或主選單。
