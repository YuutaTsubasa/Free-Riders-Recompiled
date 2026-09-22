# 原始渲染器內嵌 shader 與 primitive restart

此工作延續真實遊戲入口。原始827F6420建立的十二份頂點宣告、三個vertex
shader及三個pixel shader已通過；後續primitive restart設定也已執行。
尚無原遊戲draw／Present、標題畫面或確認輸入，目標仍未達成。

## 兩種原始來源與同一份快取

`prepare_shaders.py --image`在既有basic資產68筆之外，加入固定映像內
820D2600、820D2728、820D2848、820D2988、820D2A28、820D2B10六個容器。
它先核對整個映像大小與SHA-256、原始82AF6D30指標表、每筆範圍、stage及
完整容器雜湊，再檢查原始容器結構。未掃描或接受映像中的其他shader。

Schema2 manifest保留既有asset_sha256，新增完整source inventory及每筆
source_kind、來源offset與guest_address。全部74筆在新的generation產生
original.bin、HLSL及DXIL；來源命名分開以避免offset衝突。
全部通過後才發布cache與manifest，任何晚期失敗仍保留上一份可用快取。
省略--image時，原本的68筆basic準備方式仍可用。

本輪generation為`generation-3c923a0003f44c539e269f44e9c560da`，保存在
`out/shaders/basic`。每筆來源、HLSL、DXIL及不可變manifest均核對；原先68筆
source／HLSL／DXIL內容與translator／compiler provenance全部不變。
新增三個vertex的specialization mask為0，已編成vs_6_0；三個pixel mask為2，
仍保留lib_6_3，尚未替它們猜測alpha狀態或連結PSO。
完整核對在`out/embedded-shaders-cache-audit.json`及
`out/embedded-shaders-prepare.log`。

NativeShaders沿用完整原始容器與stage比對，真實Plume shader或library
由原生owner持有。沒有以另一個basic shader替代，也未暴露假的Xbox標頭。

## 原始宣告與shader保存的實跑核對

`out/embedded-shaders-boot.log`從原始入口執行，十二份declaration的
metadata、完整elements、terminator、非零與互異pointer全部吻合；
counts依序為1、2、3、1、2、3、2、3、2、3、4、4。
Observer只讀取原始constructor建立的物件，不建立或修復資料。

六個shader建立後，原始827F6420自行回傳0；下一個原始呼叫的observer
核對六個全域handle、owner、stage、完整來源bytes及唯一性：

```text
ORIGINAL_RENDERER_SHADER_PUBLICATION result=0 handles=0x71844000,0x71845000,0x71846000,0x71847000,0x71848000,0x71849000 owners_valid=1 stages_valid=1 sources_match=1 unique_handles=1
```

此執行隨後到達824E7F68/LR827F6414、r4=1。宣告的原生input layout與
shader實際消費仍需後續draw狀態，這些讀取核對不能當作遊戲畫面。

## Primitive restart的精確狀態

原始packet builder將device+2948送往register2205。
固定[Xenia register定義](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/registers.h#L426)
及[primitive處理](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/primitive_processor.cc#L589)
確認bit21為indexed primitive restart enable。Setter只取輸入bit0，保存
此位並將device+10的64位dirty word OR40；其他bits（包括cull）保持。
同值設定仍寫dirty，不將高位輸入當成原始程式不存在的驗證錯誤。

原生端先preflight兩個完整寫入範圍，再讀取／寫入，最後保存optional狀態。
Hook只允許已稽核的device／LR，保留原始void ABI。未指定的reset index、
topology、index格式及endian必須由實際draw取得，不能預設FFFF或FFFFFFFF。
原始28-byte opcode與語意稽核位於`out/state-824e7f68-next-audit.md/json`。

`out/primitive-restart-boot.log`實際記錄：

```text
NATIVE_PRIMITIVE_RESTART source=0x824e7f68 value=0x1 enabled=1 retained=1
STOP unsupported-function @0x8222b520: sub_8222B520: logged_unsupported_instruction
```

這個後續CPU缺口是8222B534的plain eqv，保留原始整個函式繼續執行仍是
下一個必要依賴。當時42項CTest與172項Python測試通過，無略過；記錄為
`out/primitive-restart-ctest.log`與`out/primitive-restart-python.log`。
Shader準備、原生資源與observer、restart setter／hook皆經獨立唯讀審查。

## 重現shader準備

```powershell
python scripts/prepare_shaders.py --image out/recomp/image-loader/image.bin
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-stfsu
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

以上紀錄是本輪內嵌shader／restart工作當時的停止點；後續CPU轉譯若已更新，
請使用最新實作紀錄所列的generated directory與同次執行證據。
