# 原始取樣器初始化與過濾設定

Windows 真實啟動已完成原始 MINFILTER、MAGFILTER 呼叫及原遊戲直接
寫入的 MIPFILTER，三項皆為 LINEAR。隨後原始流程繼續，目前停於
`824EC0A8`，LR `8249BC68`。**尚無原始 Draw／Present、標題畫面或
鍵盤／手把確認後進入主選單的證據。**

已核對原始零配置、建立前後的指標／copy 範圍、92個不同 render
default 函式及完整20筆 sampler defaults。原始 `8250B698` 的26個
texture-fetch 初始化只寫 dword0；相鄰 vertex-fetch 迴圈第一筆寫入
為 device+6F0，不覆蓋最後一個 sampler 的 W4（+6E8）。

因此，現有 fresh mode1／flags0 原生裝置契約下，所有26個 slot 的
完整 W3=`01000000`、W4=`0`，requested max-anisotropy byte為1、volume
control為0、min/max mip bytes為0／13。尚未綁定貼圖時，request13
不會直接變成 W4 的有效 max-mip13。建立前驗證全部20筆 setter/value，
再提取這些原始預設值；未知 profile 在建立 host graphics 或發布
device 前停止。外部診斷 callback 契約仍不在原生建立支援範圍內。

兩個新增完整 ABI setter 為 `824E8248`（min）及 `824E83F0`（mag）：

- device／slot／raw DWORD value 為輸入；支援原始26-slot範圍，void
  ABI 不覆寫 r3 為假成功碼。
- 保留原始各向異性 walk、共用 aniso 欄位及 volume filter 的完整
  rotate/mask／unsigned 運算；不把高位元參數正規化為 bool。
- 原始 lookup 只接受已核對的 N=0..16 範圍，讀取實際選定字組並驗證
  其值。不是把 byte index 當作任意255筆有效資料表。
- 所有實際輸出與 lookup 先檢查，再依原始順序寫 W4 intermediate、
  W3、W4 final，最後將 dirty qword+18 OR `1 << (31-slot)`。
- 返回本次計算快照供日誌使用，沒有額外讀取遊戲記憶體。

`sampler_filter_state` 每次讀取真正 W3／W4，因此保留原始 inline
MIP 寫入。原始 `822116A8` 沒有被替換；在其 mag 呼叫之後，下一個
原始入口必須為 `8222BDB0`，唯讀觀察確認原始 inline 結果與 dirty。
同次日誌為：

```text
NATIVE_SAMPLER_DEFAULTS table=0x82ad0f20 slots=26 word3=0x1000000 word4=0x0 requested_anisotropy=1 volume_control=0
NATIVE_SAMPLER_FILTER source=0x824e8248 slot=0 value=0x1 word3=0x1200000 word4=0x2 lr=0x82211750 state=retained-fetch-fields
NATIVE_SAMPLER_FILTER source=0x824e83f0 slot=0 value=0x1 word3=0x1280000 word4=0x3 lr=0x82211760 state=retained-fetch-fields
ORIGINAL_SAMPLER_INLINE_MIP_RETURN source=0x8221176c slot=0 word3=0xa80000 word4=0x3 dirty=0xffffffffffffffff next=0x8222bdb0 filters=linear/linear/linear
STOP native-graphics-function @0x824ec0a8: original function cannot consume an unimplemented native object layout
```

首次驗證已完成三項設定，但在後續原始初始化超過舊100000次 diagnostic
call limit。現為250000次，10秒 watchdog 保留；這是診斷限制調整，
不是更改遊戲分支或成功條件。最終實跑 exit3 停於實際未支援函式。

65項CTest及192項Python測試全數通過，無略過。測試核對原始序列、
完整存入字組與返回快照、raw高位元／volume／aniso運算、slot25邊界、
其他byte保留、guard／reservation／lookup錯誤不部分更新，以及
getter能讀到原始 inline 修改。獨立審查通過；審查加強了存入字組
檢查與 inline 結果的原始返回入口辨識。

這是正確保留 sampler-fetch 狀態，尚未建立／綁定可供遊戲 draw 消費
的完整 native sampler。未來 materialization 必須结合真實 texture、
shader sampler overrides 及當前 fetch state。Volume filter 另需
shader／texture路徑，不能只改 D3D12 sampler descriptor 的 min/mag。
任意 raw state 的 CPU 保留不代表全部組合都已支援原生採樣。

```powershell
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-partial-loads
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets --game-region=ntsc-us
```

證據：`out/sampler-filters-{build,ctest,python,boot}.log`、
`out/sampler-filters-checkpoint.json`、`out/graphics-824e8248-next-audit.md`
與 `out/sampler-initialization-provenance.md` 及其 JSON。原始機器字組、
資料表及來源雜湊在本機 audit 中，不提交 ROM／資產。

消費端參考：[固定 Xenia D3D12 sampler](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/d3d12/d3d12_texture_cache.cc#L952)、
[volume filter shader](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/dxbc_shader_translator_fetch.cc#L1702)。
