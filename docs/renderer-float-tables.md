# 原始貼圖清除與浮點資料表初始化

本輪延續真實 `_xstart824D22F0`，保留原始渲染器初始化及迴圈。
標題畫面、確認輸入及主選單驗收仍未達成。

## 八個貼圖槽

原始827F5F40依序讀取82B62914起的八個全域變數，再呼叫824F4220，
LR為827F5FC8。實際執行的八個新／舊binding均為null，完整紀錄保存在
`out/texture-bind-boot.log`。原生端逐槽保存明確的unbound狀態；未設定的
槽仍保持未知。每次只清除device+480+24×slot的低兩個type bits，並將
device+31B0+4×slot寫為零；此原始null分支不讀寫texture dirty word。

Hook僅允許已稽核的device／caller、slot0..7及完整64位精確mask。
所有寫入先通過檢查，非零新資源或舊binding在副作用前停止。
這些情況仍需真正的view、sampler、upload及fence retirement；未以null
處理代替。後續繪製也仍需依shader ABI建立適當的null descriptors。
原始396-byte setter與caller稽核在`out/texture-bind-next-audit.md/json`。

## stfsu 與原始函式

82811050中兩處缺失指令82811188／82811268都是原始word D41F000C，
即f0存到r31+12後更新r31；各自前一條原始fmuls完整保留。
實作依[PowerPC Book I §4.6.3](https://powerpc.dev/general/PPC_Vers202_Book1_public.pdf#page=116)
以FPR原始64位資料做整數位元格式轉換，不使用host float cast。
非零exponent小於874屬未定義輸入，明確停止。NaN payload及截斷位元
依格式規則處理，不另外quiet或round，也不改變浮點環境。

地址沿用現有scalar profile：完整64位加上signed16 displacement，低32位
進行有保護的big-endian四byte儲存，成功後才更新完整64位RA。
沿用GuestMemory的非對齊scalar行為，未宣稱完整硬體例外模擬。
指令產生器僅接受canonical operands、完全一致的日誌及空白emission；
label不能隱藏額外程式碼，其他不支援原因仍會拒絕整個函式。

新的忽略目錄`out/recomp/diagnostic-stfsu`保留31,859個函式，拒絕14,644個，
較前版新增133個保留函式，包含234處stfsu。生成紀錄在
`out/stfsu-generation.log`。原始ROM、assets及生成碼不納入git。
獨立稽核逐檔確認186份輸入與完整log雜湊一致，無新拒絕函式，既有保留
函式內容不變。584筆註記／日誌（574個不同PC）均與映像中的opcode及
運算元吻合；234個實際helper呼叫的PC及參數也全部核對。
詳見`out/stfsu-generation-audit.md/json`。

## Alpha 比較實驗

固定translator產生的`clip(alpha-reference)`會保留相等值，不能直接
代表這次遊戲要求的GREATER。已針對一個完整原始pixel container建立
獨立編譯實驗，使用strict flags，對候選GREATER、停用及舊版負向對照
各編譯／連結DXIL。實際候選為ordered-gt條件分支，失敗路徑discard。
SSA、相等值、signed zero及NaN分類的比較驗證通過；停用變體沒有discard。

本機實驗腳本及結果位於`out/alpha-greater-prototype.py`與
`out/alpha-greater-prototype/verification.json`，設計在
`out/alpha-greater-prototype-design.md`。這是編譯結果驗證，尚未修改
production shader cache或接通GPU繪製。後續必須版本化比較契約、接入
實際alpha狀態、特殊化、constant upload及PSO，不能接受舊library冒充。

## 重現

```powershell
python scripts/generate_diagnostic.py --input out/recomp/ppc --log out/recomp/recompile.log --output out/recomp/diagnostic-stfsu
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-stfsu
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

生成器拒絕覆蓋既有輸出；本機目錄已存在時，使用新的輸出名稱並同步調整
build參數，或直接使用已生成的目錄建置。原生測試先以可編譯stub確認
失敗（`out/stfsu-runtime-red.log`），轉譯測試RED／GREEN分別在
`out/stfsu-generator-red.log`／`out/stfsu-generator-green.log`。
真實入口的新進展斷言也先在舊執行檔失敗（`out/stfsu-entry-red.log`）。

## 本次真實執行結果

`out/stfsu-boot.log`退出碼3；同次保留全部68個basic shader資源、21次原始
貼圖建立、原始矩陣複製、混色與七項render state，並完成八個null bindings。
原始82811050返回後，唯讀observer記錄其完整1440-byte浮點資料表：

```text
ORIGINAL_RENDERER_FLOAT_TABLE_RETURN source=0x82811050 destination=0x83e59580 bytes=1440 sha1=ae15c15956e5b3b6a7ee4377936adc8b9ad0ac78
ORIGINAL_SHADER_CREATE source=0x824ed770 stage=vertex container=0x820d2600 words=102a1101,d4,54 bytes=296 sha1=137f901318ccfc84bca32413841e7e86c80be385
STOP native-shader @0x820d2600: original shader is absent from the prepared native cache
LAST_FUNCTION sub_824ED770 @0x824ed770 LR=0x827f6494 calls=13708
```

Hash是本次原始計算的觀測結果，未宣稱與Xbox硬體輸出逐位核對。
函式呼叫數受原生執行緒排程影響。到達此shader呼叫也證明原始827F6420已
依序完成十二份declaration建立及非零結果檢查；尚未逐byte檢查這十二份
metadata，也尚未將它們接至原生input layout。先前四份declaration的
metadata／element核對仍有效，但不能取代這十二份的後續驗證。

建置紀錄`out/stfsu-green-build.log`；42項CTest全部通過，162項Python
測試全部通過、無略過，分別在`out/stfsu-ctest.log`與`out/stfsu-python.log`。
Null binding、指令helper與strict generator均經獨立唯讀審查。
Commit、執行檔SHA-256與同次證據索引保存於`out/stfsu-checkpoint.json`。

下一個實際缺口是映像內嵌shader快取。原始827F6420接下來依序要求三個
vertex及三個pixel containers，均與basic shader的68筆完整容器不同；
不能回傳其中另一個handle代替。位址、大小、stage及完整SHA-256已稽核於
`out/renderer-resources-next-audit.md/json`。後續需準備這六筆真正原始資源，
保持stage、cache provenance及實際內容核對，再繼續原始初始化。
