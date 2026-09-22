# 原始未選定玩家初始化與後續圖形設定

**標題至主選單仍未達成。** Windows實跑已完成四個玩家欄位的未登入
初始化、原始資料清除及空姓名轉換，接著通過原始深度與剔除設定。
目前停於新的圖形狀態函式 `824E6A40`，LR `8221171C`，value為1。
尚無原始Draw／Present或鍵盤／手把確認畫面。

目前尚未提供原生遊戲使用者的建立／選擇介面。`XamUserGetSigninState`
對一般slot0..3明確回報未選定狀態0；未知special index停止。這是當前
absence backend，未來真正的本機profile選擇服務必須取代它。Windows
登入、語言或Winsock初始化不代表遊戲登入；本查詢不建立身分、XUID、
姓名、登入通知或Live連線。

原始 `822344B0` 自行保存state0並執行 `822329E0` 清除資料。其中經
`82232598` 發出的原始virtual call仍然執行，目標 `82234EC0` 返回後
才記錄完成。唯讀觀察確認每個record的48-byte姓名區與1000-byte
profile區已清除、state為0、manager選擇仍為-1。這些資料未由觀察
程式改写。

後續原始 `824D0D98` 要求 `RtlMultiByteToUnicodeN` 將一個NUL byte轉成
UTF-16BE。已實作經核對的ASCII範圍：明確輸入長度、內嵌NUL、容量
截斷、奇數byte容量、可省略的BE32輸出byte count。它不額外加入NUL，
只讀取實際使用的前綴、只寫入實際產生的code units。所有範圍及
輸出先檢查，再保存並驗證完整輸入前綴，最後寫入；錯誤不留部分結果。
來源／目的或byte-count衝突重疊、超過1Mi code units及非ASCII的
已消費位元組明確停止。Xbox ACP仍未核對，未猜測為主機ACP、UTF-8
或Latin-1。原始wrapper對65001本來就選擇另一段UTF-8程式。

同一次 `out/user-initialization-boot.log` 對slot0..3各觀察到：

```text
NATIVE_USER_SIGNIN index=0 state=0 lr=0x822344d8 backend=unselected-game-users
ORIGINAL_UNSELECTED_USER index=0 record=0x70137d50 state=0 selected=-1 names_cleared=1 profile_cleared=1 reset_virtual=0x82234ec0
NATIVE_MULTIBYTE_UNICODE input=0x701398bc input_bytes=1 output=0x701398cc capacity=32 written_output=0x0 output_bytes=2 status=0x0 lr=0x824d0e3c encoding=ascii-subset
ORIGINAL_USER_NAME_CONVERSION_RETURN index=0 characters=1 lr=0x822345ec
```

上方摘錄slot0；完整日誌另外保存slot1、2、3的獨立record。RTL結果
為status0與兩個輸出bytes，原始外層wrapper自行返回字元數1。

原始程式隨後設定880×720 viewport，要求depth enable1、comparison6
（GREATER_EQUAL）、write1，以及cull6。先前已實作的七個完整
ABI state setters不依賴LR，因此改用共用的精確函式入口辨識，保留
装置、值域、attachment、常數、memory及dirty-cache檢查；LR留在
日誌作為證據。這不是逐指令PPC重播：ABI volatile暫存器／temporary
stack spill未重播，alpha reference仍保留原有strict FP與flush設定。
其他不完整或含callback的bridge仍使用原本限制。

原始 `82A560BC` 六個stack-only還原指令已獨立核對machine words；
它完全不讀r3，因此可在r3仍留有device值時執行原始epilogue。
最新同次停止紀錄為：

```text
NATIVE_RENDER_STATE source=0x824e7140 offset=0x2c value=0x6 retained=1 lr=0x827f6188
NATIVE_RENDER_STATE source=0x824e69a8 offset=0x38 value=0x6 retained=1 lr=0x822116d0
STOP native-graphics-function @0x824e6a40: original function cannot consume an unimplemented native object layout
```

63項CTest及192項Python測試通過，無略過。回歸核對四個slot的原始
query→reset→conversion→return順序與後續原始圖形呼叫；轉換單元測試
另涵蓋所有128個ASCII值、截斷、NUL、邊界、alias及reservation。
獨立審查未發現此次使用者、轉換或完整setter入口政策的缺陷。

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-partial-loads
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```

實跑exit3。建置、測試、實跑日誌分別為
`out/user-initialization-{build,ctest,python,boot}.log`；來源／執行檔／
證據雜湊為 `out/user-initialization-checkpoint.json`。
原始指令核對在 `out/signin-state-next-audit.md`、
`out/multibyte-unicode-next-audit.md`、`out/graphics-824e70d0-next-audit.md`。

介面參考：[固定版本XAM user程式](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xam/xam_user.cc#L60)、
[Microsoft RTL轉換契約](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-rtlmultibytetounicoden)。
未採用Xenia固定profile／XUID，也未將其標示sketchy的高位元組轉換當作完整Xbox編碼規格。
