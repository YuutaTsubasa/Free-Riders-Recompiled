# 原始 eqv 初始化與最新啟動證據

**標題畫面及確認後進入主選單仍未達成。** 本輪已完成
[六個內嵌shader與primitive restart](embedded-renderer-shaders.md)，接著保留
原始8222B520的plain eqv，讓真實初始化繼續至ExGetXConfigSetting查詢。
未加入替代UI、靜態遊戲畫面、模擬器或成功stub。

## Plain eqv

實際8222B534的word7C875A38為`eqv r7,r4,r11`。依
[IBM指令參考](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-eqv-equivalent-instruction)
及固定[Xenia 64位GPR實作](https://raw.githubusercontent.com/xenia-project/xenia/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/cpu/ppc/ppc_hir_builder.cc)，
plain eqv是64位元XOR的逐位反相，只更新目的暫存器。轉譯使用uint64欄位，
r0與所有source/destination aliases均按一般暫存器處理，不改CR或XER。
這點保留了原始subfc產生、後續addze使用的carry。

Strict generator只接納完整canonical格式、精確log與空白emission；
label不能藏入其他程式碼。Rc變體、格式異常、其他log事件與其他函式
拒絕原因均保持。全部原始179筆註記／日誌涵蓋174個不同PC、85個函式；
逐筆核對opcode31、XO284、Rc0及三個operands皆吻合。

新目錄`out/recomp/diagnostic-eqv`保留31,941個函式、拒絕14,562個，
新增82個完整函式、176處eqv。186個輸入檔及完整log雜湊一致，無新增
拒絕，舊保留函式內容不變。仍有三個含eqv的函式因sthux／lfsu保持拒絕；
並未因修復其中一個指令而略過另一個缺口。
原始指令與generation稽核分別在`out/eqv-next-audit.md/json`及
`out/eqv-generation-audit.md/json`。

## 同次真實入口

`out/eqv-boot.log`退出碼3，保留本輪全部實際進展：

- 37個原生vertex stage、37個保留pixel library，由完整原始容器取得。
- 十二份原始renderer宣告的metadata／elements／terminator核對通過。
- 六個新增shader的全域保存、owner／stage／來源bytes及唯一性核對通過；
  原始827F6420回傳0。
- 原始primitive restart設定為enabled，未猜reset index或draw參數。
- 原始8222B520通過，程序抵達以下新的系統查詢。

```text
IMPORT_CONTEXT name=__imp__ExGetXConfigSetting address=0x82acb5fc lr=0x824d0b84 sp=0x70131660 r3=0x3 r4=0x9 r5=0x701316b4 r6=0x4 r7=0x701316b0 r8=0x8270d980 r9=0x821a9c48 r10=0xffffffff821a9c48
STOP import-function @0x82acb5fc: __imp__ExGetXConfigSetting
LAST_FUNCTION sub_824D0B58 @0x824d0b58 LR=0x824d0b84 calls=15498
```

呼叫數受原生執行緒排程影響。下一步需稽核category3／setting9、輸出大小、
原始錯誤／fallback與原生設定來源；不因需要往前而回傳猜測的成功設定。
當前仍無原始遊戲draw／Present、標題或確認輸入證據。

## 重現與回歸

從專案根目錄執行；本機映像與資產均已完整驗證：

```powershell
python scripts/prepare_shaders.py --image out/recomp/image-loader/image.bin
python scripts/generate_diagnostic.py --input out/recomp/ppc --log out/recomp/recompile.log --output out/recomp/diagnostic-eqv
./scripts/build_tools.ps1 -Diagnostic -DiagnosticDirectory out/recomp/diagnostic-eqv
./out/build/host/sfr_cpu_diagnostic.exe out/recomp/image-loader private/assets
```

生成目錄已存在時直接使用既有來源建置，或指定新的生成目錄並同步修改
build參數；生成器不覆蓋既有目錄。建置紀錄為`out/eqv-build.log`。
42項CTest與178項Python全部通過、無略過，見`out/eqv-ctest.log`及
`out/eqv-python.log`。eqv generator先RED再GREEN（88項generator測試），
真實入口亦先以舊執行檔確認新斷言失敗。
來源commit、執行檔SHA-256與同次證據索引保存在`out/eqv-checkpoint.json`。

ROM、原始shader、生成碼及測試輸出皆保存在忽略目錄；本機git只提交
來源、測試與文件。
