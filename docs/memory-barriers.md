# 記憶體屏障（sync / lwsync / eieio）

## 問題

上游 XenonRecomp 把 PowerPC 的三個屏障指令都翻成 no-op：

| 指令 | 出現次數（本遊戲） | 上游翻譯 |
| --- | --- | --- |
| `sync` | 63 | 無 |
| `lwsync` | 49 | 無 |
| `eieio` | 14 | 無 |

在 x86-64 上這大致無害：主機本身就有 TSO（store 之間、load 之間不會重排），
而且產生的客體存取都是 volatile，編譯器也不會跨著重排。

在 **ARM64（Android）上不是**。遊戲的無鎖佇列（`'LfQu'`，`82750B30` / `82750C40`）
會先寫好節點內容再把節點掛進佇列；少了 `lwsync`，另一顆核心可以先看到節點、後看到
內容，拿到半成品的工作項目。同樣的模式也用在載入執行緒與繪圖資源上，症狀會是
「偶發崩潰」或「畫面資料錯亂」。

## 做法

`scripts/generate_diagnostic.py` 的 `rewrite_barriers()` 只對**上游完全沒有輸出**的
屏障指令補上圍欄，保留原本的指令註解：

```
	// lwsync 
	std::atomic_thread_fence(std::memory_order_acq_rel);
	// sync 
	std::atomic_thread_fence(std::memory_order_seq_cst);
```

`sync` 是全屏障（`seq_cst`），`lwsync` 與 `eieio` 只排序記憶體存取（`acq_rel`）。
若某個屏障指令後面接了其他輸出（代表上游的翻譯與這裡的假設不同），該函式會被標成
`unsupported_barrier` 而不是被默默改寫。

產生報告裡多了 `counts.barriers`（本遊戲為 126 = 63 + 49 + 14）。

## 驗證

- `tests/test_diagnostic_generation.py`：`test_barriers_become_fences_and_keep_their_comments`
  與 `test_barrier_with_an_emission_of_its_own_is_rejected`。
- 重新產生的 `out/recomp-v4/diagnostic-barriers` 與原本的輸出逐行比對，只多出 126 行
  圍欄，函式保留數不變（30703 保留 / 23 拒絕）。
- 同一段比賽畫面（present 10400 與 10800）與改動前的截圖相同。

`sync` 在 x86 上會編成 `mfence`，但只出現在鎖與佇列路徑，量測不到差異；ARM 上會編成
`dmb ish`。
