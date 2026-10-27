---
name: mozc-fork
description: "Mozcフォークのビルド・CI・Windows差し替え手順。"
---

> **DSH向け注記（2026-10-22 移行）**
> 本文は Hermes 時代の記録です。ツール名は読み替えてください: `terminal` → `pwsh`、
> `skill_view` → `skill`、`hermes kanban` → `todo`。
> このリポジトリは DSH のワークスペースとして開きます（Windows は `C:\...`、WSL は `\\wsl.localhost\Ubuntu\...`）。


# Mozc fork 改造の計装・差し替え・デバッグ（実測 2026-09-24〜25）

前提: 移行全体は既存の `mozc-windows-migration` スキル、進行中の引き継ぎは wiki `plans/ime-migration-handoff.md`。本スキルは fork 改造を回すときに必ず踏む罠だけをまとめる。

## 設計意図（方向を間違えないため）
- 最終形は「**入力は半角英数のまま、日本語と判定されたら自動でかなに変換するハイブリッドモード**」。
- 素通し（en 方向）: かなモード＋英字（判定 `en`・確信≥0.85・英字3文字以上）で半角英数へ恒久切替（`src/composer/jev_judge.{h,cc}` + `composer.cc` フック、判定はローカル Python サーバー `\\.\pipe\jevime_judge`）。ログで `switch to half-ascii` の発火までは実測済み（**画面表示への反映は組成の組み直しが前提** — 「フック位置」参照）。
- ハイブリッド（ja 方向）: `mode==HALF_ASCII` かつ判定 `ja` → ひらがなへ戻す。⚠ **モード切替だけでは画面は変わらない**（実測: `SetInputMode` 後も組成は組み直されず、直後の打鍵で `raw=""`＝組成消失。ユーザーからは「何も起きない」に見える）。切替の実装には**組成の組み直し（削除→新しいモードで生ローマ字を1文字ずつ再投入）まで含める**こと — en 方向が能動的に組み直しているのと対にする。**en/ja の両方向を必ず対で維持**（片方だけだと日本語か英語のどちらかが打てなくなる）。
- **正解の経路は session.cc の `SwitchInputMode`＝`SetInputMode(mode)`＋`SetNewInput()`**（手動切替 `CompositionModeHiragana` 等もこれを呼び `OutputFromState` で TIP へ状態を返す）。`SetNewInput()` を省くと切替が組成に反映されない。
- **入力モードの所有は TIP（クライアント）側**: サーバー側でモードを変えても、次の打鍵で TIP が自モードを送り直して戻ることがある。症状＝**ログに同一の `switch to ...` 行が毎打鍵で再発火**（raw が伸びない）。この場合はサーバー単独の切替では不足。
- **`InsertCharacterPreedit(text)` はテーブル変換をバイパスする生書き込み API**（渡した文字列をそのまま preedit に書く）→ en 方向の「成功」はモード切替の証明にならない（偽陽性）。
- **挿入 API は「テーブル（ローマ字→かな変換）を通るか」で選ぶ**（`composer.cc` の実装を直接確認済み。早見表: `references/composer-mode-apis.md`）:
  - `InsertCharacter(key)` = `input.InitFromRaw(...)` → **変換なしの生挿入**。組み直しに使うと raw が raw のまま戻るだけで無効果（`rebuilt==raw` になる正体）。
  - `InsertCharacterKeyEvent(commands::KeyEvent)` = `input.Init(*table_, key, ...)` → **テーブル変換あり＝実打鍵の本線**（かな化したいときの正解経路）。
  - `InsertCharacterKeyAndPreedit(key, preedit)` = `InitFromRawAndConv` → **表示文字列を明示指定**して挿入（変換済みかなをそのまま見せられる。実機検証は未了）。

- **現行方式の未解決副作用（2026-09-28 ユーザー実測）**: ①半角英数で打っていても ja 判定の瞬間に実モードが全角かなへ切り替わり「全角日本語入力になった」と見える。②en 判定が語尾に届くまで誤ったかな表示が一時見える（the→てぇ・hello→へっぉ クラス）。③「半角のまま preedit だけかな表示」案は内部状態が壊れるため不可（Claude.ai 見解）。④ ja 判定でかなへ戻された後、英語を打っている間も組成（下線・変換候補）が見え続けて目障り（ユーザー指摘）— 緩和は早期 en 化（①ゲート）。改善候補: 判定を「語の区切り/入力停止」まで遅らせる — 停止タイマーは **TIP 側**に持ち、Composer/Session は単一スレッド・イベント駆動のまま変えない（Claude.ai 見解）。なお「区切り＝確定」時点では既に ASCII 確定済みで判定が間に合わない点に注意。
- **実機フィードバック4件（切り分け完了 → 設計確定・次回ビルドに同梱）**: 4件とも真因まで特定済み。設計は Claude.ai レビュー込みで確定。
  - (a) **Mozc 切替直後は「直接入力」で Ctrl+Space が無反応**: 切り分け実測の結果 **TSF レベルでは閉じていない** — 直接入力状態でもキーは TIP に届いており（TIP 側ログ `TipKeyeventHandler::OnKey` が記録される）、サーバー側ログに何も届かないだけ＝キーマップに該当行が無い状態。→ 対策は **`ms-ime.tsv` の DirectInput 節に `Ctrl Space → ToggleAlphanumericMode` を追加**（DirectInput 節に Ctrl+Space 行が無いことは確認済み。⚠ **キーマップは TIP DLL に内蔵されるため、設置済み TIP DLL が旧世代だとサーバー exe だけの差し替えでは直らない** — 実測: 旧 tip64 の Ctrl Space 行=0 / 新ビルド=1。サーバー exe と TIP DLL は同一 CI ビルドの成果物でセット差し替えする（「TIP DLL の差し替え」節）。効かなければ TIP 初期モード〔compartment〕の調査へ）。Mozc config に初期入力モードの設定項目は無い。
  - (b) **ja 判定後、確定してもひらがなのまま（直感に反する）**: 設計確定 — モード遷移に出自フラグ `ModeOrigin`（kDefault / kAutoSwitchedByJudge / kManualByUser）を持たせる。**自動復帰するのは kAutoSwitchedByJudge のときだけ**（Ctrl+Space 等のユーザー操作は kManualByUser を立て自動復帰を無効化＝ユーザーの意図を巻き込まない）。Commit 時に判定器由来のときだけ HALF_ASCII へ戻す。⚠ 部分確定（文節確定）は「全体が確定しきったか」を見る（要実装確認）。Undo は優先度低。
  - (c) **文頭の英単語の後、日本語が変換されない／助詞「に」等が変換できない**: 真因は「文全体判定」ではなく **C++ ja 判定ゲートの `IsAsciiLetters` が空白入り raw を弾く**こと（半角英数で「web 」と打った後は 「web wo」 が判定サーバーへ一度も送られない — リクエストログで spaces 入り 0 件を実測）。設計: **フック側セグメント管理** — フックが「最後に判定した境界」を持ち、末尾セグメント（最後の空白以降）だけを判定・組直しし、Commit でリセット（Composer 本体は触らない）。2打鍵助詞は別途、完全一致の助詞リスト（を/は/に/が 等 → 無条件 ja、「to」は英語と衝突するため除外）を判定器へ追加。※セグメント化しても kMinLength=3 未満の「ni」単独問題は残る（defer-short 仕様が救済・C++ 反映待ち）。
  - (d) **打ち間違い→英語判定で変換不能**: 設計確定 — 判定器に**あいまい一致**を追加（打鍵ミスを許容して ja に寄せ、確定後に Mozc の変換で救う）。⚠ **比率ベース**必須（固定文字数は短語で崩壊）: 3〜4字=揺れ0／5〜8字=揺れ1／9字以上=揺れ2（上限2）。**死判定ゲート発火後のフォールバックとしてのみ**使用（常時使用は英語誤爆を増やす）。トライ上で上限付きレーベンシュタイン探索、拾ったら確信度を下げる。実装はまず `local_engine.py`（ビルド不要）。
  - 構造改善（Claude 推奨・採用）: **モード遷移を単一関数 `TransitionMode(ModeOrigin, mode)` に集約** — Ctrl+Space／フック自動切替／Commit 復帰の全てをここ経由に。状態遷移の穴が今後見つかっても修正点が1箇所になる。

## サーバーの実行コンテキスト（最重要）
- `mozc_server.exe` は**制限トークン＋低整合性レベルで起動される**ため、**通常の `%TEMP%` にログを書けない**。デバッグ出力は `%USERPROFILE%\AppData\LocalLow\Mozc\`（Mozc 自身が history.db 等を書いている実績のある場所）へ。`OutputDebugString` 併用可。TIP はアプリ内（中整合性）なので `%TEMP%` に書ける →「同じコードなのにサーバー側だけログが出ない」の正体はこれ。
- **サーバーが繋ぐ名前付きパイプには低整合性対応の SDDL が必須**: `D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;WD)(A;;GA;;;RC)S:(ML;;NW;;;LW)`。pywin32 `win32pipe.CreateNamedPipe` の第8引数（セキュリティ属性）が既定 `None` のままだと**接続が静かに失敗し「判定なし」へフォールバック**＝素通しが無言で効かない。`win32security.ConvertStringSecurityDescriptorToSecurityDescriptor` → `SECURITY_ATTRIBUTES` に詰めて渡す。
- TIP は **Composer/Session をリンクしていない** thin client。かな組成の実体はサーバー側 Composer で、TIP はキーを IPC 送信し返ってきた Output を TSF に反映するだけ。
- 判定サーバー（Python）は**再起動で落ちる**（自動起動しない）。「切替が一度も実行されない（ログに `switch to ...` が0件）」の第一容疑者 → まずパイプ生存確認（ping）。Startup フォルダの .bat（`kondate-server-start.bat` と同方式）で自動起動を登録しておく。⚠ **二重起動すると同一パイプに2プロセスが同居して不安定化する**（実測: venv 版と Python310 版の `-m jevime.judge_server` が2本同時稼働）。`judge_server_run.ps1 -Stop` は Path に `mozc-migration` を含む個体しか止めない＝取り逃す罠 → 全列挙は `Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*judge_server*' } | Select-Object ProcessId,Name,CreationDate | Format-Table`（⚠ `Name -eq 'python.exe'` を併用しないと**自分自身の powershell プロセスが混ざる** — 実行中のコマンド文字列に `judge_server` を含むため自己マッチする（2026-10-06 実測）。WSL からは `powershell.exe -NoProfile -Command "…"` へ `$_` → `\$_` のエスケープで渡す。⚠ `-Filter "Name like …"` は WQL として不正で 0x80041017 — 絞り込みは必ずパイプ先の `Where-Object` で）。**⚠ 識別の訂正（2026-10-04 実測）: venv 版（親）＋ Python310 版（子）の2プロセスは正常な1本の構造**（venv の `python.exe` はネイティブランチャーで、実サーバーは子プロセス。実測: 親は `python310.dll` をロードせず WS 0.05MB で待つのみ・子が `win32file.pyd` 等をロードして受付。irodori TTS サーバーも同一構造で正常稼働）。**真の二重起動は「`listening` 行が2本以上」か「子（実サーバー）が2本以上」で判定**（1回の正常起動は `listening` を1行だけ書く。2026-09-29 15:36 の listening 2行＝過去の真の二重の痕跡）。⚠ `-Stop` は親しか止められず**子が孤児化して生き残る** → 停止は必ず CommandLine 一致で: `Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'python.exe' -and $_.CommandLine -like '*judge_server*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }` → 0本を確認。定石（2026-10-04 11:42 実測成功）: 全停止 → 0本確認 → `judge_server_run.ps1` で1本起動 → `-Status` の ping＋プロセス2本（親＋子）＋ `listening` の増分がちょうど1本であることを確認。この一連は同梱 `scripts/restart_judge_server.ps1` を C:\ 側へコピーして実行すれば1本のログに残る（2026-10-05 実測）。
- ログの `mode=` の読み方: **HIRAGANA=0 / HALF_ASCII=2**。`after switch: mode=2` が素通し成功の証拠。
- **ログ3系統の役割（打鍵内容が残るのは1つだけ）**: 判定ログ `C:\Users\yuhi1\Tools\mozc-migration\judge_server.log` = 判定に回った組成の**生ローマ字＋判定**（精度向上の素材としてユーザー決定で**維持** — 抑制・削除を提案しない）。サーバーフック `AppData\LocalLow\Mozc\jev_judge_hook.log` と TIP 側 `%TEMP%\jev_judge_hook.log` は**動作ラベルのみ**で打鍵内容は残らない。

## 計装の入れ方
- ロガーは**ファイルごとに自己完結**（`GetTempPathW`+`CreateFileW`、追加 include 不要）で書く。TIP や session_server に `composer/jev_judge.h` を include すると **Bazel 依存が無くてビルドが落ちる**。
- 有効な計装点: `SessionServer::Process`（IPC 入口）/ `Session::SendKey`・`InsertCharacter` / `Composer::InsertCharacterKeyEvent`・`ProcessCompositionInput` / `TipKeyeventHandler::OnKey`・`TipEditSessionImpl::UpdateContext` / `client::Client`。
- ⚠ **計装ログ自体がフリーズ・クラッシュの主因になる（実測・対策済み）**: 打鍵ごとの同期ファイルI/Oが最大の負荷源だった — `ProcessCompositionInput` の毎打鍵ログがログの約7割（2,000行中1,434行）でファイルは22MBに肥大、`WriteDebugLog` は5箇所（TEMP / USERPROFILE / PUBLIC / ProgramData / LocalLow）へ毎回書き込み。症状: Vivaldi ごとフリーズ＋`mozc_server.exe` APPCRASH（イベントログ Application Error・KERNELBASE.dll / ntdll.dll）。対策: 打鍵ごとの全組成ログを削除し、出力先も LocalLow の1箇所に絞る（`switch to ...` 切替ログは維持）。実測: 出力先削減だけでは稀にフリーズが残った → **毎打鍵ログの削除でフリーズ消滅（実機確認）**。計装を足す前に「それは毎打鍵か」を必ず確認する。
- **TIP DLL はプロセスごとにロード**される。MSI を差し替えても起動済みアプリ（Vivaldi 等）は古い DLL を掴んだまま → **TIP 側を変更したときはテスト前にアプリを完全再起動**（タスクマネージャーで全プロセス終了）。しないと「新しい計装が動かない」と誤診する。**サーバー側だけの変更ならアプリ再起動は不要**（ログに `switch` が出ていれば経路は生きている＝アプリの TIP は新サーバーと通信できている）。判断に迷ったら再起動して変数を1つ減らす。
- ⚠ **TIP とサーバーは別バイナリ**: `replace_server*.ps1` が差し替えるのは `mozc_server.exe` のみで、TIP 側の計装とその出力先は TIP DLL（MSI 側）の世代のまま。「TEMP のログだけ新しく LocalLow が止まっている」なら書き手は TIP（旧 DLL）側 — サーバー側変更後の判定に使うのは LocalLow。逆に TIP 側ログは**キーが TIP に届いているかの切り分け**（TSF が閉じていないか）にも使える（DirectInput の切り分けで実使用）。
- 検証は「設置バイナリの sha256 を MSI 内ファイルと突合」＋「計装マーカー文字列のバイト探索」（`strings` は UTF-16 を取り逃すので Python で検索）。

## フック位置
- `ProcessCompositionInput` の**入口**で組成を書き換えても、以降の処理で巻き戻る（かなに戻る）→ **入力適用の直後（関数末尾）に適用**する。
- `SetTemporaryInputMode` は**その入力限りで失効**する → モードを保持するには `SetInputMode`（恒久）を使う。
- ⚠ **早期リターン（ガード）の後ろに新コードを置かない**: `if (...) return ...;` の後ろへ分岐を足すと**到達不能＝最適化で丸ごと消える**。コンパイルは通り `#error` プローブも発火するのに、成果物バイナリには新コードのマーカー文字列が1つも入らない（「CI成功・ビルド成功なのに反映されない」の正体）。新規分岐は**ガードの前に置き、ブロック内で自己完結**させる（外の変数に依存させたまま前に移すと未宣言エラーでビルドが落ちる）。
- **モード切替だけでは組成は再変換されない**: `SetInputMode` はモードを変えるが、表示中の組成はそのまま／直後の打鍵で消える。切替の実装には必ず組成の組み直しを含める。切替は `SetNewInput()` まで含めて手動経路（`SwitchInputMode`）と同一に揃える。⚠ **処理順序も揃える: `SetInputMode` → `SetNewInput` → `DeleteRange` → 再投入**。逆順（消す→再投入→モード変更）だと切替前に入力済みだった文字が全角のまま残る（実機: `ｖｓｃode` 症状 → 順序修正で `vscode` に解消・確認済み）。
- **`rebuilt=` ログは成功指標にならない**: 生ローマ字（raw）はかなモードでもローマ字のまま残る仕様で、組み直し後も `rebuilt==raw` になり得る。**内部状態の唯一確実な判定は `GetStringForPreedit()`（実際に表示される文字列）を毎打鍵ログに出すこと** — preedit が「かな」か「ASCII」かが直接見える。画面だけの確認は**「切替後に Space で漢字変換が始まるか」**を併用する（Space 無反応＝内部も英数のまま＝切替が実際には効いていない）。

## ビルド不要のシミュレータ（判定ルール反復の主戦場・2026-09-27 追加、09-28 拡張）
- 判定ルール/ポリシー/タイミングの反復は **`C:\Users\yuhi1\Tools\jev-ime-win\tools\sim\ime_sim.py`**（同 dir に README・cases_hybrid.txt）で回す。ビルド不要・実測 0.15 秒/実行:
  - `py -3 tools\sim\ime_sim.py --file tools\sim\cases_hybrid.txt`（WSL からは `python3`）
  - `--keys hello --start half --expect en` / `--bench tools\bench\jev_bench.jsonl` / `--replay %USERPROFILE%\AppData\LocalLow\Mozc\jev_judge_hook.log`
  - 英語交じりの文: `--keys "web wo hiraku" --expect en/ja/ja`（`<space>` で語を連結、期待は語ごとに `/` 区切り、結果は「段別=」で表示。`<space>` を含む列は Windows cmd ではクォート必須 — `<` `>` がリダイレクト扱いのため）
- 構造: 判定器（`jevime/local_engine.py`）と romaji 変換は**実物を直接 import**（← この層はファイル編集＋judge server 再起動だけで本番反映、ビルド不要）。C++ フックの条件・適用（`MaybeSwitchToEnglish`）は Python の `HookPolicy` に写し。CLI 一時上書き: `--min-conf-ja/-en` `--min-length` `--defer-short` `--jp-min-count` `--coverage` `--en-common-rank` `--en-strong-rank` `--policy perkey|quiet` `--quiet-sec`。
- 実機 hook ログとの再現一致は**旧ルールで 98.0%**（199/203 組成）。残差は 末尾1文字の帰属ズレ／連続試行の融合／Space=確定の単純化／TIP のモード送り戻し。**TSF/TIP 層（1テンポ遅れ・旧DLL・AHK競合）は対象外＝実機でしか確認できない**。
- ⚠ **replay は「現行実機との一致率」＝忠実度メトリクス**: ルールを意図的に変えると下がるのが正常。見るべきは「**シミュだけ切替**」の数（新規誤爆）と差分の中身。
- 2026-09-28 の実測要点（ケース集44件・bench74件・replay203組成）:
  - **判定器 `jevime/local_engine.py` に5修正を適用**（バックアップ `tools/sim/backups/local_engine.py.20260928-0124.bak`）: ①語末 n の「ん」解消（せn→せん x493 で ja 0.92 → `sen` 切替可・`han`/`nan`/`ton`/`nank` も1字早く） ②英語綴りのかな昇格ブロック（`app`→「あっｐ」・`hell`→へっ 誤爆/フリップの芯を解消） ③`EN_STRONG_RANK` 500→1000（`test` #502 救済） ④n-gram ja 信頼度 0.88→0.90（C++ 側 0.90 門に届かず語彙に無いかな語の長打鍵が半角のまま残る芯を解消。回帰なし） ⑤全大文字略語（+数字混在）→ en（ひらがなモード中の AI→あい・DB→ｄｂ・PS4 化けを解消。`[A-Z0-9]{2,}` かつ英字含む場合のみ・純数字は対象外）。**①〜⑤と語彙修復はすべて 2026-09-29 14:35 の判定サーバー再起動（pid 4468）で本番反映済み**。同日、別セッションの ①dead-column gate（カード t_f351db61・かな解析不能な死列を決定論的に即 en conf 0.92。rootfs/mymf/vscode/chrome を早期 en 化。ファズ検証 `tools/sim/ime_dead_gate_fuzz.py`・プローブ `ime_gate_probe.py`）も同じ再起動に同梱され、実機パイプ E2E で確認済み（rootfs/mymf/vscode/chrome= en 0.92、konnichiwa= ja 0.90 非発火、でーた= ja 0.92 x1254）。
  - 実測変化: cases 36→40/44（`--defer-short` 併用で 42/44）・bench 69/74 不変・**フリップ集合も旧と同一=新規誤爆ゼロ**・fullday 69→70/87・replay 190/203（下がるのは意図分のみ: 1字早い ja 切替×4 等）。
  - ⚠ **n-gram の en 判定から「未確定尾」ガードを外してはいけない**（実測で replay 12件退行・bench フリップ1→4件: する/残る/した/となりの 打鍵途中に英語フラッシュ）。`test` は EN_STRONG_RANK 側で救う。
  - **EN_STRONG_RANK の有効帯は [502, 1111)**: `test`(en #502) 救済と `son`(#1417)/`mon`(#1111) 保護の両立は 1000（ファイル既定）。4000 まで上げると日本語入力中の `son`/`mon`/`jason` が英語化する誤爆。
  - `--defer-short` = 2打鍵語は確定/<pause>の瞬間だけ判定。C++ `kMinLength=3` で死ぬ `wo`→を(34256回)/`ni`→に(26211回) の安全な救済（`--min-length 2` は2打鍵目で早すぎて誤爆量産: フリップ6件・simだけ切替13件）。**C++ は v13 以降『セグメント判定（2文字以上・即時）』として搭載済み** — sim の defer（確定/<pause> 時の判定）とはタイミングが異なる（即時救済の誤爆側は下の 2打鍵語ガードで抑える）。仕様メモ: `tools/sim/patches/defer-short-cxx.md`。
- **2打鍵語ガード（判定器側・実装済み）**: 2文字ちょうどの英字列の即時救済は**かな1文字になる語**（を/に/は/よ/ん 等）だけに許可し、`ai→あい`・`ui→うい` の2文字かな語と変則綴りの `ci→し` は保留（keep）— sim 側の計測基準と同一。導入根拠: **実機フックログを「2文字の切替 raw」で検索して `ai` の即時切替を実測**（sim 仕様にガードがあっても実装に無いクラスは、実機ログで突き合わせてから閉じる）。⚠ かな1文字判定にはローマ字→かな変換が必要なため **C++ には置かず判定器（`local_engine.py`）側で実装**（反映＝judge server 再起動のみ・ビルド不要）。検証は feature script の C 節（ai/ui/ci→keep・de-ta/me-ru/ko-hi-→ja）。
  - 残件: `push`(en #3587) と ひらがな中の `github`（かな→英が n-gram keep 止まり。ランク緩和は son/mon 誤爆と両立不可）・2打鍵語の C++ 反映。
  - **Qiita 実コーパス**（`tools/sim/corpus/`・2026-09-28 追加）: API 収集の技術記事 100 文 → 96 ケース（476 ラン ja292/en184）を自動生成。`fetch_qiita.py`（収集）→ `build_cases.py`（文→打鍵列。**SplitMode.A 必須** — C だと「生成AI」等が融合し生の漢字が打鍵列に混入）→ `eval_corpus.py`（5構成評価、結果 `RESULTS.txt`）。実測: 半角×perkey **51/96**・半角×defer **63/96**・ひら×perkey 46/96・ひら×defer 60/96・半角×defer×ゲート緩和 **72/96**。「en 誤爆」はひら開始で特に多い（英語語はひらモードから en 判定で戻す必要があり 2 文字語は不利）。
  - ⚠ **語彙ファイルの文字化け（発見・修復済み）**: `data/jp_kana_freq.tsv` の 652 語が長音 'ー' の代わりに '゜' で格納されていた（で゜た=データ x1254・さ゜ば゜=サーバー x944 等・計59,099出現）。`tools/sim/corpus/repair_vocab.py` で修復（退避 `tools/sim/backups/jp_kana_freq.tsv.20260928.bak`）。修復後は でーた/さーばー/つーる/ろーかる 等が語彙ヒット。**修復だけでは実機の切替は変わらない**（'-' が下のゲートで判定に届かないため）— ゲート緩和とセットで効く。
  - ✅ **C++ ゲート緩和は実装済み（v14 ビルド）**: 出発点は実機ログ実測「切替成立した raw 88 件は**すべて英字のみ**（'-'・数字・スペースは 0 件）」。`IsAsciiLetters` を **`IsJudgeableKeys`（英字・数字・'-'）**へ置換し、**半角モードのセグメント判定とかなモードの en 判定の両方**に適用（`src/composer/jev_judge.cc`）。長音（'-'）を2文字目に含む日本語（de-ta=でーた 等）が判定・変換されない不具合の修正がこれ。安全性実測: 数字列（2026/123/3）は keep で切替を誘発しない・'-' は死判定ゲートで**生存**（長音として解析可）。sim 回帰は **`--allow-keys-punct`** で検証（コーパス 63→72/96・cases 40/44（defer 42/44）・bench 69/74 同値・flips 不増）— ⚠ 回帰実行時に付け忘れると修正後も「未転換」に見える。**反映はビルド＋exe 差し替え**（判定器側と違い judge server 再起動だけでは入らない）。**2026-10-05 実機差し替え完了** — 設置 exe の読み戻し sha256 `aaca414a…` が CI run 37262831922 の成果物と一致（zip ダイジェスト鎖＋PE タイムスタンプ 2026-10-05 で締め、旧版は `Downloads\mozc\backup_20261005\` に退避済み）。**実機打鍵テスト（データ・コーヒー・アール→各かな）もユーザー確認で OK（2026-10-05）— v14（伸ばし棒修正）はこれで実機までクローズ。** 仕様メモ: `tools/sim/patches/defer-short-cxx.md`。
- **ゲート緩和の副作用ガードは判定器側に置く（R1/R2）**: 緩和で '-'-入り打鍵が判定に届くと `e-mai`（e-mail の打鍵途中）が ja 判定される等の新リスクが出る — 検出は sim の英語 '-' 語（e-mail/u-turn/t-shirt/x-ray）。対策は `local_engine.py` のガード2件: **R1** = 2打鍵の母音＋'-'（`i-`/`a-`）は保留（`2-key vowel-dash hold`）、**R2** = `e-` 接頭辞（e-mail/e-book/e-sports）は ja 判定より優先して保留（`e- prefix hold`）。**再ビルド不要・judge server 再起動のみで反映**（**2026-10-05 再起動で本番反映済み** — e-mai/e-book = `e- prefix hold`・i-/a-/u- = `2-key vowel-dash hold`・de-ta 等の長音語は ja 維持をパイプ実測）。伸ばし棒の回帰は新設 `tools/sim/cases_hyphen.txt`（18ケース・`--allow-keys-punct` 必須）で回す。残許容: `o-ring` 型（`o-` は オール/オーラ 等の実在語と衝突するため頻度差で許容・要観察）と 2打鍵「a-」「i-」単独（誤爆防止優先で保留のまま）。
- **「判定されない」報告の切り分け順**: ① 判定器へ直接問い合わせ（`scripts/query_judge_once.py`）で正解が出るか ② 実機フックログにその打鍵列（例: '-' 入り）が届いているか（全期間バイト検索）— ①○②✗なら **C++ ゲート/経路**、①✗なら**エンジン**。伸ばし棒クラスは ①○（de-ta→ja 0.92）②✗（'-' 入り切替 0 件）が典型。
  - ⚠ **'-' 切替の実機ログ計数法（2026-10-05 実測）**: `grep 'switch to' <log> | grep -E 'raw="[a-z0-9]+-'` で数える — 素朴な `grep -- "-"` は行内の矢印 `->` に誤マッチし「全行が '-' 入り」に見える罠。なお「コーヒー」クラスは `ko`→こ の2打鍵で切替が成立し '-' はかなモード側で消費され raw に現れない → 判定ログの `"keys": "ko-hi-"` を併せて確認する。
  - ⚠ **replay の母数が変わった（2026-09-29〜）**: 自動テスト主体で hook ログが 26,936 組成に増加。同日実測 24620/26936 = 91.4% 一致・「実機だけ切替」0 件。以前の 190/203（ユーザー打鍵）とは直接比較しない（「シミュだけ切替」の意味も自動テスト列の性質で変わる）。
  - **残る既知クラス**（コーパス実測）: 2打鍵助詞 no/de/to（keep 判定＝仕様）／日本語語彙と衝突する英字（Tika→ちか・Mini→みに）／keep＋未確定尾で en に戻れない英語（git/diff/push 等。dead gate は表で判別できる列のみ救済＝git/diff は非発火）。
  - ⚠ WSL から cmd.exe 経由でスペース入り引数（`--keys "web wo hiraku"`）を渡すと引用符が崩れて unrecognized arguments になる。ケースファイルに入れて `--file` で回すのが確実。
- quiet（無入力タイマー）の replay 検証は hook ログに時刻が無いため不可（`<pause:0.4>` を手で挿入して測る）。詳細な設定比較表は `tools/sim/README.md`。
- **旧エンジンとの A/B（旧=バックアップとの差分評価）は importlib で共存ロードできる**が2つの罠: ① `spec_from_file_location` は**拡張子が .py でないと loader=None** で失敗 → 先に `.py` へコピーする ② `LocalDecisionEngine(data_dir=...)` の data_dir を**明示**する（既定は `__file__` 相対のため、別の場所からロードすると FileNotFoundError）。
- フォークの全差分は `gh api repos/yuhi-sonehara/mozc/compare/master...ci-verify-20260923` で一括取得（jev_judge.{h,cc}・composer.cc フック・keymap・TIP/session 計装）。C++ のしきい値（kMinConfidence=0.85 / kMinConfidenceJa=0.90 / kMinLength=3）と写しを必ず突き合わせる。
- **ソース裏取りは clone 不要**（コマンド形と落とし穴は「参考実装との差分測定」節に集約）: TIP/session/keymap の挙動を疑ったら推測せずこの経路で実装を確認する（EnsureIMEIsOn 差・Preserved Key 一覧・ToCompositionMode はこの方法で特定した）。
- Windows コンソール対応: 非 cp932 文字を出力に使わない＋`sys.stdout.reconfigure(errors="replace")`（実装済み。文字化け回避に PYTHONIOENCODING=utf-8）。

## ローマ字「死判定」ゲート（①）の設計事実（実測）
設計案で「有効ローマ字接頭辞のトライに一致しなければ即 en」という話が出たら、まず flagship 例（hello 等）を実測で裏取りする — ローマ字表の `l`/`ll` を踏み抜く誤りが定番。
- **`l` は小書き仮名（la→ぁ…）と促音（ll→っ）の正当な綴り**。`hel`/`hell` は「続けられる接頭辞」に留まり、`hello` は**完全にかなへ解析できる（へっぉ・complete=True）** → 接頭辞トライ型ゲートは hello クラスを一度も捕まえられない（history.db に残る「へっぉ」誤変換の正体がこの解析そのもの）。確認: `python3 tools/bench/romaji2kana.py hello`（WSL から）。
- **converter の `complete=False` は「未完成」と「死」の区別にならない**。ゲートは「生字を挟まず全消費できる解析があるか（末尾は未完成接頭辞まで許す）」を **DP/オートマトン**で判定する（n の揺れ・促音・l/x 綴りのため単純トライ不可）。プローブ: `scripts/romaji_gate_check.py`（期待値スナップショット31件つき・不一致で exit 1）。
- 実測の発火位置（この打鍵で死＝en 確定）: `vscode`→2打鍵目、`chrome`→3、`mymf`→3、`help`→4、`test`→4、`github`→5、`rootfs`→5。非発火（生存）: `hel`/`hell`/`hello`/`diff`/`push`/`son`/`mon`/`kan`/`sushi`/`tokyo`/`konnichiwa` ほか。
- ゲートの実益は**既知残件の救済**（ひらがな中の `github` 等が決定論的に en へ。`son`/`mon`/`push` は非発火なので EN ランク保護と衝突しない）。**hello は既に n-gram 経路で en（実機 hello→en conf 0.88）** — ゲートの効果測定で hello を成果に数えない。
- 表は **Mozc 実物（またはミラー）から生成し、コンポーザ本体とランダム文字列でファズ等価テスト**する（表の取りこぼし＝偽発火が最大リスク。日本語打鍵で死んではいけない）。
- **設計案の採否（評価済み）: ①ゲートのみ採用推奨 — ②文字trigram Naive Bayes は既存 n-gram と機能重複のため不要、③文脈事前確率は保留**（既存資産優先）。
- 実装はまず `local_engine`（判定サーバー再起動のみで反映・ビルド不要）。
- **history.db は暗号化**（magic bytes 非 SQLite・`encrypt_key.db` 併設）→ SQL での誤変換クリーンアップは不可。学習の掃除は候補削除（ms-ime keymap 実測: `Prediction`＋`Ctrl Delete` = `DeleteSelectedCandidate`）か全消去（退避の上）。

## 参考実装との差分測定（パリティ測定）

「似た実装（OSS）が世に出た／これと似ているよね」と言われたら、感想ではなく**相手の公開テスト・修正バグリストを評価集合にして、我々の判定器の差分を実測で出す**。

### 手順
1. **構成はソースで確定する（推定で語らない）**。単一ファイルは `gh api repos/<o>/<r>/contents/<path>?ref=<branch> --jq .content | base64 -d` か `curl https://raw.githubusercontent.com/<o>/<r>/main/<path>`。⚠ パスが違うと API は**静かに空を返す**ので、先に `gh api repos/<o>/<r>/git/trees/<branch>?recursive=1` でファイル一覧を取る。読めば確定できることを「推定」と書くと事実確認重視のユーザーに即座に指摘される。
2. **評価集合は相手のリポジトリから取る**: テストの期待値宣言（`ExpectJapanese(...)`/`ExpectNotJapanese(...)` のように並んでいる）、テスト名とコメント（テスター報告の元ネタ）、リリースノートの修正バグリスト、相手の辞書（`dictionaries/*.txt`）、README の代表例。
3. **写像を固定してから測る**: 期待=日本語 → 我々 `ja` のみ正解（`en`/`keep` は切替しないので日本語にならず外れ）。期待=英語/未確定 → `ja` 以外は正解（相手の「未確定＝切替しない」は `keep` に対応）。指標を揃えないと差の大きさを誤報告する。
4. **2段で測る**: 語彙レベル（数千語・判定器を直接呼ぶ）と打鍵例レベル（数十件・`ime_sim.py` 経由で最終状態を見る）。打鍵例の外れは**原因クラスに分類**する（混在トークン／2打鍵語／w連続／小書き／`@`・`_`／句読点）。
5. 締めは `hermes-verify-*` の独立再計算チェック（報告値を生データから再計算して突合、FAIL 0 で exit 0）。

ハーネスの実体は `jev-ime-win/tools/bench/meltype_parity/`（`compare.py`=語彙＋打鍵例、`breakdown.py`=外れの原因分類、`hermes-verify-meltype-parity.py`=独立再計算）。測定結果は `run1.out`・`selftest.out` に残置。実測ベースラインと参考実装の構成は `references/parity-measurement.md`。

### 落とし穴
- **辞書・データはスクリプト相対で開く**（`os.path.dirname(os.path.abspath(__file__))`）。相対パスだと実行 cwd 依存になり `FileNotFoundError` で落ちる（実測: 初回実行が exit 1、修正後に再実行して合格）。
- **グループ別カウンタは必ず同じ合計バケツへ足す**。取り違えると「ラベルと母数がズレる」形で表面化する（実測: 英文グループの正解を和文バケツに足して ja 1050 / en 2812 と誤表示 → 正は 977 / 2885）。報告前に「各グループの母数の和 == 総合」を assert する。
- **sim は `--defer-short` 単独では2打鍵語が発火しない** — 確定/停止を模す `<pause:0.4>` を打鍵列末尾に付ける（`--keys 'no<pause:0.4>'`）。付けずに測ると2打鍵語が全滅に見える。
- **sim と C++ は空白でしか切らない**（`SplitTrailingSegment`＝最後の空白以降、`kMinSegmentLength=2`）→ 日本語と英単語が空白なしで連続する打鍵（`atarashiiPCwokatta` 等）は1セグメントとして丸ごと判定され構造的に外れる（実測: 外れの約6割がこのクラス）。**外れは「我々のバグ」と「仕様差（分割単位の違い）」に必ず分けて報告する**。
- **相手のコード・辞書データは移植しない**（GPL-3.0 等のライセンス。参照はアイデアとテストケースの水準まで）。
- 参考実装が「修正済み」としてテストに書いている項目は、**我々が同じ穴を独立に踏んでいる証拠**（例: 打鍵途中の部分英単語への引きずり `hima`→him・`gamen`→game）。修正の優先順位づけに使う。

## 判定器（Python）の本番反映（デプロイ）と E2E 検証
- 反映 = judge server の再起動のみ（数秒・ビルド不要）。**稼働中サーバーの再起動は勝手にやらない** — 別セッションが実機テスト中の可能性があるため、Kanban カードでタイミングの判断を仰ぎ、承認後に実行→検証→completed まで一連で片付ける。
- 再起動（WSL から）: `powershell.exe -NoProfile -ExecutionPolicy Bypass -Command 'cd C:\Users\yuhi1\Tools\mozc-migration; .\judge_server_run.ps1 -Stop; Start-Sleep -Seconds 1; .\judge_server_run.ps1'` — 起動スクリプトが内蔵の Test-Pipe で ping まで確認する（`-Status` は生存確認だけも可）。
- **反映の証明はパイプ経由のビフォーアフター問い合わせ**: 再起動の前に旧応答を記録 → 再起動後に同じ問い合わせを実行し、decision / confidence / reason の変化を比較する。同梱 `scripts/query_judge_once.py` を venv python で実行（無ければ `Tools\mozc-migration\` へコピー）:
  `cd /mnt/c/Users/yuhi1/tools/mozc-migration && ./.venv/Scripts/python.exe query_judge_once.py sen app test`
  現在の期待値（2026-10-04 再起動後の実測）: `sen` → ja 0.92『せん』x493 / `test` → en 0.92 dead column / `app` → keep 0.55 / 助詞 `wo` `ha` `ni` `ga` `mo` `wa` → ja 0.92『particle』/ `kocnnpakuto`『こんぱくと』・`bsagi`『あさぎ』→ ja 0.75『fuzzy』（タイポ救済）/ `state` `vscode` → en 0.92（英語ガード維持）。※ `query_judge_once.py` の和文 reason はコンソールで文字化けするが decision/conf は読める（正しい和文は judge_server.log 側）。2026-10-04 の再起動（カード t_8ebdf460 の承認フロー）では機能チェック 19/19・sim は従来同値（cases 40/44・defer 42/44・bench 69/74・3文字以上 60/60）を確認。
- 補助証拠: 新プロセスの StartTime が `local_engine.py` の mtime より後なら新コードをロード済み（`Get-Process python | Where-Object { $_.Path -like "*mozc-migration*" }`）。

### 変更後の検証パッケージ（引き継ぎ・差し戻し対応でこの順に）

1. **対象の同一性を先に固定**: `sha256sum jevime/local_engine.py`＋mtime を記録（検証中の書き換え混入を排除）。fork 側は `git status --porcelain` が空＋`git rev-parse HEAD` が push 済み SHA と一致を確認。
2. **構文**: `python3 -m py_compile jevime/local_engine.py`（WSL から直接で可）。
3. **機能単位**: 同梱 `scripts/verify_local_engine_features.py`（25チェック: 助詞→ja 0.92、除外語 not ja、fuzzy 救済 ja 0.75、英語ガード、回帰 sen/test/app/konnichiwa、2打鍵語ガード ai/ui/ci→keep、長音語 de-ta/me-ru/ko-hi-→ja。jev-ime-win のパスは引数で上書き可。FAIL で exit 1）。
4. **回帰スコア**: `python3 tools/sim/ime_sim.py --file tools/sim/cases_hybrid.txt` → 40/44、同 `--defer-short` → 42/44、`--bench tools/bench/jev_bench.jsonl` → [全件] 69/74・[3文字以上] 60/60。記録値と食い違ったら変更混入を疑い、差分を説明できるようにする。
5. **前セッションの検証スクリプトがあれば再実行**: scratch の `hermes-verify-*.py`（fork 側の構造＋挙動チェック・HEAD 比のコンパイラ診断差分つき）を自分の実行で通す。⚠ scratch は放置で prune される — 消えていたら fork の差分から同等チェッカーを作り直す。
6. サーバー反映の証明は前節のビフォーアフター問い合わせ（再起動前の応答記録 → 再起動後に同一問い合わせ）。

- ⚠ WSL ではプロジェクトの pytest 群は収集エラーになる（Windows 専用 import）— 代替は 2〜4。「pytest が通らない」を未検証の理由にしない。
- 証拠は常に「自分の実行ログと exit code」— サブエージェントのレポート文言や前回実行の引用は根拠にならない。

## 差し替え運用（MSI）
- 同版 MSI は自動アップグレードしない（ProductCode がビルド毎に変化）→ 先にアンインストール。**実測 2026-09-27/28: 同版 MSI を普通にインストールしてもサーバー exe は置き換わらない**（設置 exe の mtime が据え置き）→ 設置後は必ずバイト数＋mtime を実測し、変わっていなければ 7z 抽出→直接差し替えへ切替。
- **稼働中プロセスがあると MSI はファイルを置換できない**: MSI は 3010（成功＋再起動要）を返すのに実ファイルは旧版のまま（MSI 自身の保留操作でロックが残り、再起動でも解消しない）。MSI ログ末尾の `MainEngineThread is returning 3010` = この状態。
- **確実な差し替え（サーバー側 1 ファイルだけの変更ならこれで十分）**: ①`Get-Process -Name mozc* | Stop-Process -Force` で**全 mozc プロセス**を停止 ②MSI（または成果物）から `7z x` で取り出す ③管理者 PowerShell で `C:\Program Files (x86)\Mozc\` へ上書きコピー（.ps1 を書いて `Start-Process powershell -Verb RunAs -File ...`、**`Copy-Item -ErrorAction Stop` を try/catch** して結果を自前ログへ — 既定だとロック時に**無言で失敗**して成功に見える。WSL 側は結果ログを `result=` で**5秒間隔ポーリング**して完了を検知する〔UAC クリック待ちがあるため固定 sleep では不足〕。ログ行に bytes＋マーカー文字列カウントを書けば差し替え検証も1行で済む） ④**コピー先を読み戻してバイト数＋マーカー文字列を検証**（旧版のままならロックが残っている＝再起動して再試行。3010 を返した MSI は保留操作が残るため再起動しないと解消しない）。
- ⚠ **差し替えの前に現行 exe を読み出しで退避する**（例: `cp "C:\Program Files (x86)\Mozc\mozc_server.exe" /mnt/c/Users/yuhi1/Downloads/mozc/backup_<date>/mozc_server.exe.bak` — WSL から Program Files は読み出し可）。巻き戻し先を先に用意してから差し替える。
- ⚠ **退避だけでなく「巻き戻しスクリプト」も差し替え前に用意し、dry-run を合格させる**（雛形: `templates/rollback_server.ps1`）: 退避 exe を `src`・**旧版 sha（大文字）を `$expected`** に埋めた版を作り、`scripts/make_swap_dryrun.py` で実走して `result=COPY_OK | expected_match=True` を確認してから本番を差し替える。不具合が出たらこれを UAC 1回で撃つだけで数秒で旧版へ戻せる（実測: 巻き戻しは30秒・`cmp` rc=0）。巻き戻し後は①`cmp -s <退避> <設置物>` rc=0 ②新機能固有のバイトマーカーが 0 件 ③直前版は `.bak-<newver>-<date>` として実在、の3点を読み戻しで確認する。
- **差し替えスクリプトは `templates/replace_server.ps1` を雛形にする**: ログ1行に `bytes / hiragana / en / directinput / sha_match / sha16` を出し、**設置先の読み戻し＋SHA 突合まで自動化**する（目視検証を挟まない）。
- **本番実行の前にサンドボックス dry-run を実走する（同梱 `scripts/make_swap_dryrun.py`）**: 本番 .ps1 のパス文字列を sandbox へ置換し、破壊的行（`--comment-line 'Stop-Process'` 等）だけ無効化した版を**機械生成** → `powershell.exe -NoProfile -ExecutionPolicy Bypass -File '<sandbox.ps1 の Windows パス>'` で実行 → ①ログ行（`result=COPY_OK ... sha_match=True`） ②sandbox の差し替え先 sha = 新 exe ③sandbox バックアップ sha = 旧 exe ④**実体（Program Files）が未変更** の4点で締める。置換は assert 付き（パターン不在で即エラー）＝置換漏れが沈黙しない。UAC を消費する前にスクリプト本体のバグを潰せる。
- ⚠ **WSL 側で作る .ps1 は ASCII のみにする（日本語コメント禁止）**: PS 5.1 は BOM なし UTF-8 を CP932 として読むため、**日本語コメント行の末尾バイトが改行を吞み、次行（`$p = ...` 等の代入行）をコメントへ吸収**させる。症状は「パス名を空にすることはできません」等の無関係に見えるエラー（実測 2026-10-04: 同型エラーで連続失敗）。対策: コメントも英語（ASCII）で書く＋配置直前に非 ASCII バイト走査（`python3 -c` で `sum(b>127)` が 0 を確認）。併せて: WSL コピー由来のファイルは**読取専用属性**が付くことがあるので、差し替え前に `$it.IsReadOnly = $false` で解除してから上書きする。
- ⚠ **UAC プロンプトは「在席確認 → 一声 → 表示」の順でしか出さない**: 不在・確認なしで `Start-Process -Verb RunAs` を撃つと**無承認のまま消える**（実測）か、操作が重なって即キャンセルされる（「この操作はユーザーによってキャンセルされました」）。キャンセル検知後に**盲目的な再表示をしない** — ユーザーの返事を待ってから撃ち直す。検知（consent.exe）・監視（`scripts/uac_swap_watch.sh`）・在席確認の具体手順は `wsl-windows-tooling` の「長時間処理と UAC」節に集約。
- `install_mozc_fork.ps1` は**登録情報が無い状態だと「現状確認」で停止**する。その場合は `msiexec /i <msi> /qb /norestart /l*v log` を直接使う。
- `Start-Process -Verb RunAs -PassThru` は **$null を返すことがあり**終了コードが取れない → 自動化を重ねても不安定。UAC の確実性が要る場面は**ユーザーに MSI をダブルクリックしてもらう**のが最短。

### TIP DLL の差し替え（mozc_tip64.dll / mozc_tip32.dll）
- **いつ必要か**: キーマップ（ms-ime.tsv）由来の挙動を変えたとき。キーマップは **TIP DLL に内蔵**されるため、`mozc_server.exe` だけ差し替えても TIP 側は旧表のまま（直接入力 Ctrl+Space の無反応はこれが真因だった — 実測: 設置済み旧 tip64 の Ctrl Space 行=0 / 新ビルド=1。direct_mode_keys のゲートは TIP 内蔵キーマップを参照）。
- **入手**: CI 成果物 `Mozc64_x64.msi` を 7z で展開（`7z l` の一覧が初回空なら詳細モードで再実行）。中に tip64 / tip32 / server exe が居る。**同じ run の MSI から取る**（MSI 内 exe の sha256 が設置済みと一致することを確認＝世代混在なし）。
- **設置先**: `C:\Program Files (x86)\Mozc\`。差し替えは管理者権限（UAC 1回）。
- **流れの型**（雛形: `templates/replace_server.ps1`）: 読取専用属性クリア → バックアップ（`Downloads\mozc\backup_<date>\` へ .bak・元 mtime を保持）→ コピー（`Copy-Item -Force -ErrorAction Stop`）→ 失敗時は**旧ファイルを .old-<date> へリネームしてから再コピー**（実機の使用中 DLL は「コピー不可・リネーム可」— tip64 はこの経路で成功した）→ 失敗時ロールバック → **読み戻し検証**（bytes・sha256 一致・キーマップ行数の旧→新変化）を自前ログへ1行で。
- **差し替え後はアプリ完全再起動が必須**（TIP DLL はプロセスごとにロード — 「TIP とサーバーは別バイナリ」節）。それまでの間、起動中アプリは旧実体のまま動く。
- **実機適用の前にサンドボックス3フェーズ検証**（通常／使用中=リネーム／強ロック=失敗時無破損）を必ず回す — 手順とロック保持スクリプトは `references/swap-sandbox-verification.md`。
- 実施フローは Kanban 承認カードで（回答後に処理セッションが UAC 込みで代行し得る — 実施結果は必ず自分の目で実体検証してから報告する）。
- ⚠ 差し替え用 .ps1 は **ASCII のみ**で書く（理由と検出法は `wsl-windows-tooling` の PowerShell 節）。

## CI の罠（起動と成果物検証）
- public fork はランナー無料（**push で自動実行**・1回 50〜75分）。成果物は `gh run download <run_id> -n Mozc64_x64.msi`。
- **exe 単体の検証用回収**: 実体名は `mozc_server_win.exe`（ターゲット `//server:mozc_server_win`）。実パスは `bazel cquery --output=files` で取得（`bazel-out/` は junction のため glob や `dir /s` では辿れない）。CI 側でアップロードする場合は取得した実パスを直接指定する。
- **Windows ワークフローに `workflow_dispatch` が無い** → `gh workflow run windows.yaml` は HTTP 422。**ビルド起動は push のみ**（コメント1行足す trivial commit でよい）。
- `gh run view <id> --json artifacts` はこの環境の gh では未対応（Unknown JSON field）→ 成果物一覧は `gh api repos/<owner>/<repo>/actions/runs/<run_id>/artifacts --jq '.artifacts[] | "\(.name)\t\(.expired)"'` で確認してから `gh run download`。
- `gh run download` は**期待 headSha の run を特定してから**実行（過去実行の成果物を掴むと「ビルド成功なのに反映されない」と誤診する）。`gh run list --workflow windows.yaml --json headSha,databaseId` で照合。
- **CI ログに `Compiling ...` と出てビルドが成功しても成果物に新コードが入らない事例あり（原因はデッドコード — 上記「フック位置」参照）**。切り分け順: ①`#error` プローブ（意図的コンパイルエラーを push → 落ちればファイルはコンパイル対象。ただし「コードが生きている」証明にはならない） ②成果物バイナリでマーカー文字列（`WriteDebugLog` の固有文言）を Python のバイト検索 ③**PE タイムスタンプ（リンク時刻）**で成果物が今回ビルドかを判定（サイズだけでは新旧を判別できない）。差し替え前に③まで実施する。
- コールドビルド強制は `gh cache delete`（`update_deps-*` を削除）。冷えると 1h20 程度。
- **成果物の解凍先と検証**: `gh run download <id> -n mozc_server_exe -D <dir>` → `<dir>/x64_windows-opt-*/bin/server/mozc_server_win.exe`（約22MB・文字列はASCII格納）。ハイブリッド判定は Python で `b'switch to hiragana'`（ja方向）と `b'switch to half-ascii'`（en方向）のバイト数を数える（1件以上=計装入り=差し替え可）。
- **特定コミットの反映確認は文字列セット差分で**: 新旧 exe から印字可能ASCII連続（`[\x20-\x7e]{10,}`）を抽出し集合差分を取る。新exeのみに出現する文言＝そのコミットの反映証明（追記したログ文言の有無で判定でき、マーカー1本より誤診しない）。**さらに強いのは「そのコミットでしか有り得ないバイト列」のカウント**: TSV への追加行（例 `DirectInput\tCtrl Space\tToggleAlphanumericMode`）はそのまま exe に埋め込まれるので、**旧 0 → 新 1** のカウント変化が最強の証拠になる（差し替えスクリプトのログ行に入れる — `templates/replace_server.ps1` 参照）。
- **「その run の成果物である」証明は zip ダイジェストの一本鎖で**: build ジョブログに `SHA256 digest of uploaded artifact zip is <hash>` の行が出る。`gh api repos/<o>/<r>/actions/runs/<run>/artifacts --jq '.artifacts[] | select(.name=="mozc_server_exe") | .id'` で artifact_id を取得し、`gh api repos/<o>/<r>/actions/artifacts/<id>/zip > art.zip && sha256sum art.zip` がその hash と一致することを確認（zip 内 exe の sha も Python の zipfile で計算して控える）。⚠ **文字列が増えない純ロジック変更（ゲート条件の緩和等）はマーカー文字列・バイト列カウントでは証明できない** — この場合は ①zip ダイジェストチェーン（commit→run→artifact→手元ファイルの一本鎖） ②PE タイムスタンプ（リンク時刻＝当該ビルドの日時） ③実機テスト＋実機ログ（新たに通るようになった raw がログに現れる）で締める。マーカー（hiragana/en/directinput）は同世代の累積機能の確認に留まり、新規性の証明にはならない。
- 監視・差し替え判断は windows.yaml（『CI for Windows』）のみで足りる。Linux/macOS/Android の CI は失敗が続く期間があり、Windows の判断には影響しない。
- **ビルド完了待ち**: `gh run watch <run_id> -R yuhi-sonehara/mozc --exit-status --interval 60` を大きめ timeout で foreground 実行すると、600秒超は自動で tracked background＋完了通知に昇格する（ポーリング自作は不要）。ジョブ内訳は `gh api repos/<owner>/<repo>/actions/runs/<run_id>/jobs --jq '.jobs[] | "\(.name)\t\(.status)\t\(.conclusion // "-")"'`。
- 監視 cron（ci_build_watch・10分毎）は「最新の完了ビルド」要約のみ監視（in-progress は無視、**cancelled も拾う**）。cancelled は後続 push による concurrency の自動中断（cancel-in-progress: true・windows.yaml 冒頭）で**失敗ではない** — 成果物は無く、検証は次の成功ビルド待ちになる（慌てず正常動作として報告する）。
- **keymap で `Ctrl Space` を ToggleAlphanumericMode に変えると 2 テストが失敗する**（`Composition`/`Conversion` だけの変更 → 1 テスト。`Precomposition` にも足すと 2 テスト）:
  - `//session/session_handler_scenario_test / insert_characters_txt` — シナリオ（insert_characters.txt の MSIME 節）は Ctrl+Space=InsertHalfSpace（半角スペース挿入）前提の期待値のため、変更後は「あ あ」→「あa」等となり連鎖失敗（`Actual preedit: "あa"`）。x64/arm64 共通。
  - `//session/session_test / SessionTest.Issue2297060` — 「Ctrl-Space is not working」の回帰テストで `InitSessionToPrecomposition` 後に `SendKey("Ctrl Space")` し `EXPECT_FALSE(command.output().consumed())`（Precomposition で Ctrl+Space は消費されないのが正しい）。`Precomposition\tCtrl Space\tToggleAlphanumericMode` を足すと**消費される**ため `Actual: true / Expected: false` で落ちる。**この 1 本は Precomposition への追加が直接の原因**。
  - 判別法: `gh run download <run_id> -R <owner>/<repo> -n test-logs-Windows-X64 -D <dir>`（zip は自動展開）。**`test.xml` の `<failure>` が正確なアサーション**（`Value of: ... Actual / Expected`）、シナリオ系は `test.log` の `Scenario: EXPECT_... <シナリオファイル>:<行番号>` が**失敗行番号**を指す。`session_test/shard_1_of_8/`・`session_handler_scenario_test/shard_7_of_8/` を見る。
- **build ジョブと test ジョブは独立**。`gh api .../runs/<id>/jobs` で `build_on_x64` が `success` なら成果物は有効。**テスト失敗でも build ジョブは完走して成果物（mozc_server_exe 等）は生成される**ので、差し替え可否の判定は run の成否ではなく「build ジョブの conclusion ＋ 成果物の実在 ＋ マーカー確認」で行う。恒久対応は 2 テストの期待値更新（2026-09-28 実測・全緑）: ① `insert_characters.txt` の `SET_CONFIG session_keymap` は**ファイル内の複数箇所（13/94/145/184/207 行）**で再設定され、Ctrl+Space の検証点が複数節に分散している → **1 箇所だけ変えると失敗が 18 行目から 197 行目へ移るだけ**。Ctrl+Space=InsertHalfSpace が残る **CHROMEOS キーマップへ MSIME 指定の全箇所を置換**（テスト意図を保ったまま通る。KOTOERI/ATOK 節は無変更で可）。② `session_test.cc` の `Issue2297060` は `EXPECT_FALSE(command.output().consumed())` → `EXPECT_TRUE` へ変更＋理由コメント（Precomposition への割当追加で「Ctrl-Space を消費しない」旧仕様が変わるため、意図した仕様変更として正当）。

## DirectInput 節にコマンドを書く際の必須手順（2026-09-30 実測）

ms-ime.tsv の DirectInput 節にコマンドを足すだけでは**キーマップが読み込まれない**。`DirectInputState`（`src/session/keymap.h`）の enum に該当コマンドが無いと `KeyMapManager::LoadStreamWithErrors` が "Unknown command" として弾き、`keymap_test.LoadStreamWithErrors`（ms-ime.tsv を読む）が失敗する。3 点セットが揃って初めて効く:
1. `DirectInputState::Commands` に enum 値を足す（`RECONVERT` の後ろに追加。`NONE=0` の値を壊さないこと）
2. `KeyMapManager::InitCommandData()` で `RegisterDirectCommand("CmdName", DirectInputState::CMD)`（keymap.cc）
3. `Session::SendKeyDirectInputState` の switch に case を足す（session.cc）。無 case だと `-Wswitch` で落到 `return false`

判定チェック: `ParseCommandDirect` の登録テーブルに文字列があるかを `grep RegisterDirectCommand` で確認。`ToggleAlphanumericMode` は Precomposition/Composition/Conversion には既にあるが **DirectInput には無い**（2026-09-30 に追加した）。

⚠ **だが最終的に①を塞いでいたのは TIP DLL 側だった（2026-10-04 決着）**: ms-ime.tsv を直して exe を差し替えても、**TIP DLL（`mozc_tip64.dll`）内蔵のキーマップが古い世代のまま**では DirectInput の Ctrl+Space は無反応のまま（`direct_mode_keys` は TIP 側 `key_info_util.cc` から生成され、`keyevent_handler.cc` 678行〜のゲートが参照する — exe 側だけでは直らない）。判定法: 両バイナリに `DirectInput\tCtrl Space\tToggleAlphanumericMode` 行が含まれるかをバイト探索でカウント（**旧 TIP=0 / 新 TIP=1**）。**修復は再ビルド不要 — 同一 CI ビルドの MSI（`Mozc64_x64.msi`）から 7z で `mozc_tip64.dll`・`mozc_tip32.dll` を抽出して差し替える**（型: 全 mozc プロセス停止 → 使用中ロック時は旧 DLL を `.old` へリネーム退避 → 新規コピー → sha256 突合。UAC 1回）。差し替え後は**アプリ（Vivaldi/Chrome 等）を完全再起動**してから再テストする。実機: 差し替え＋再起動で「できた」＝①解消（2026-10-04）。

## 半角英数→ひらがな切替でプレフィックスを残す実装（再投入方式の正しい落とし穴）

**親指指引の「InsertCharacterPreedit(prefix + " ") で組み直す」実装は誤り**。`CharChunk` は**生成時の transliterator を保持**し、preedit は `GetString()` が `AppendResult(LOCAL)` で描くため、モード切替の**あと**から書き直した prefix は LOCAL→HIRAGANA の経路を通って全角化される（＝修正②が防ごとしている「先頭だけ全角」）が再現する。正解は **prefix を組み直さず、組んだまま触らない**:
- `DeleteRange(prefix.size(), total - prefix.size())` で**末尾セグメントだけ**消す → `SetInputMode(HIRAGANA)` → `SetNewInput()` → 末尾 1 文字ずつ再投入
- 半角英数モードでは生ローマ字 1 文字 = preedit 1 文字なので `prefix.size()` をそのまま DeleteRange の開始位置にできる
- prefix が空（空白なし入力）のときだけ従来どおり全消し経路（モード変更 → SetNewInput → DeleteRange → 再投入）を使う

確証ログは `GetStringForPreedit()`（`rebuilt=`/`GetRawString()` は生ローマ字が roman のまま残り得るため判定指標にならない）。

## スペースなしの英日連続打鍵が直らないとき（判定器ではなく適用単位を疑う）

症状クラス: `NASAni`（NASA に）→「なさに」／`APIwo`→半角のまま `APIwo` で停止／`korehaAPIdesu`→「これはあぴです」／`sorehaPythonde`→かな部分が消えて半角に戻る。**英トークンとかなの間に空白を入れない打鍵**で起きる。

**まず適用単位（セグメント分割）を疑う — 判定器は既にトークン単位で正解を返している**（実測: `ni`/`wo`→ja 0.92、`API`/`NASA`→en 0.90）。切り分けは2層で:
1. エンジン単体に**トークンだけ**を問い合わせる（`LocalDecisionEngine().decide()`）→ 正解か
2. 正解なら、その判定が**どの単位に適用されるか**を C++ 側で確認する

現行の適用単位は非対称:
- ja 方向 = `SplitTrailingSegment`（**最後の空白以降**）だけを組直し、前半は半角チャンクとして残す → 空白が無いと run 全体が1セグメントになり、略語部分までかな化する。
- en 方向 = run **全体**を半角へ平坦化し直す（部分適用が無い）→ かなプレフィックスが消える。
- よって「英トークンだけ素通しし、前後のかな変換を維持する」にはセグメント分割規則の拡張（空白基準 → 英トークン境界）＋末尾トークンの生挿入分岐が要る。**判定器側だけの変更では原理的に直らない**（sim で判定だけ変えても症状は残る）。適用単位を変える案件は sim → 判定器（再起動のみで反映）→ C++（ビルド＋差し替え）まで含めて1案件として計画する。

影響範囲の実測（報告の根拠にする）:
- 判定ログの keys から型を数える: `grep -oE '"keys":"[A-Za-z0-9-]*"' judge_server.log | sed 's/"keys":"//;s/"$//' | grep -E '[A-Z]{2}[a-z]' | sort | uniq -c | sort -rn | head`（略語→かなの連続。実測では上位1語に集中し、他は打鍵途中の断片）
- 実機フックログの raw: `grep -o 'raw="[^"]*"' <hooklog> | grep -E '[A-Z]' | sort | uniq -c | sort -rn | head`
- ⚠ **sim はこのクラスを再現しきれない**: `cases_hybrid` の既存 FAIL には空白入りの複合ケースが含まれる（分割単位の差）。新規則の評価では「対象ケース」と「既存 FAIL（仕様差）」を分けて報告し、FAIL を本体のバグと混同しない。replay の一致率は現行実機との忠実度なので、規則を足すと下がるのが正常（差分の中身で判断）。

### 実装の現状（判定器＝稼働中／C++ の略語分割＝巻き戻し済み・再挑戦待ち）
- **判定器**（ビルド不要・再起動のみ）: `local_engine.py` の全大文字略語ルールの直後に「先頭大文字＋英単語辞書（`data/en_10k.txt`）収載語＋3文字以上 → en 0.9」。実測: `Python`/`Chrome`/`Sound`/`Gem` → en 0.9（`capitalized word 'python' (en_10k #4048)`）、`So`/`Sh`/`Je` は 2 文字なので保留（打鍵途中の誤爆防止）、`Shokuho` は辞書非収載で従来どおり ja。実ログ 317,298 打鍵での発火は 2 件のみ（`Sound`・`Gem` の打鍵途中）。反映の証明は再起動前後の同問い合わせ（`Python` が keep 0.55 → en 0.9）。
- **C++**（2026-10-06 再実装・CI ビルド中。v16 の実装は実機で IME をハングさせて巻き戻し済み）: ① `SplitTrailingSegment` に略語境界（直前までが `[A-Z0-9]` 2 文字以上 → その後 `[a-z]`）を追加＝半角→かな方向で `NASAni` → NASA + に ② `FindAcronymSplit` を新設し、ひらがなモードでは「かな＋略語（＋かな）」を分割してトークンだけ半角で残す＝`korehaAPIdesu` → これはAPIです。**再実装は共通の「全組み直し」ラムダ（`(半角のまま生挿入するか, 文字列)` の並びをモード別に挿入し、最後に HIRAGANA へ戻す）に統一**し、`in_hook` を組み直し全体で保持する。
- 検証の型: 変更ファイルから**純粋関数を抽出**して harness を作り実行（実測 23 ケース `RESULT: ALL_OK`）、`g++ -fsyntax-only` はスタブ込みで base と診断同一、`cases_hyphen` 18/18。仕様メモは `tools/sim/patches/segment-english-cxx.md`。
- 残さ: 全大文字でない先頭大文字語が**途中**に来る形（`sorehaPythonde`）は分割規則が未対応（`Pythonde` は分割しない＝従来挙動のまま）。

### ⚠ 実装時の必須事項（2026-10-06 実測・v16 で実際に IME を落とした）

- **再入ガード（`in_hook`）は「組み直しが終わるまで」保持する**。入口に `if (in_hook || composer == nullptr) return false;` があるので、`in_hook` を `QueryDecision` の周りだけで true/false して組み直し（`SetInputMode`/`SetNewInput`/`DeleteRange`/`InsertCharacter`/`InsertCharacterPreedit`）を素で呼ぶと、`InsertCharacter` が `ProcessCompositionInput` 経由でフックを再入し、同じ分岐が再発火して**暴走ループ（実測 141,965 件）→ サーバー CPU 124 秒・IME 応答停止**になる（ユーザーからは「Mozc が落ちた」に見える）。en 方向の既存実装は `in_hook = true` を組み直し全体に掛けている — 新分岐も必ず同じ形にする。
- **半角英数（HALF_ASCII）モードでは「末尾セグメントだけの部分組み直し」が画面に出ない**（実測: `APIwo` で `switch to hiragana: segment="wo" conf=0.92` が毎打鍵 4 回再発火し、preedit は `APIwo` のまま＝raw が伸びない典型症状）。モードの所有はアプリ（TIP）側にあり、部分的な変更では状態が返らない。**部分組み直しをやめ、全組み直し（en 方向と同一の形）で実装する**。
- **かなモードで略語トークンを `InsertCharacterPreedit` で入れると全角表示になる**（実測: preedit が `ＡＰ`・`これはＡぴで`）。ASCII のまま見せたいなら、そのチャンクを HALF_ASCII モードで作る（en 方向の実績ある順序を流用）。
- **sim はこのクラスを再現しない**（C++ の分割は HookPolicy の写しに無い）→ 可否は「抽出ハーネス（純粋関数）＋実機ログ」で判定し、**再入・モード所有・表示の3点は実機でしか確認できない**（v16 は CI 全緑・ハーネス緑でも実機で落ちた）。
- **差し替え直後の退避は巻き戻しの生命線**: 退避を戻すだけで30秒で復旧できた。
- **「IME が落ちた」報告はまず暴走ループを疑う（診断手順）**: フックログの行数を10秒あけて2回数える（`A=$(wc -l < log); sleep 10; B=$(wc -l < log)`）— 増分が毎秒数十行なら暴走が生きている。停止済みなら `Get-Process mozc_server` の **Accumulated CPU**（`.CPU`＝秒）が分単位に膨らんでいるのが痕跡。Application イベントログに `mozc_server` のクラッシュが無いなら「死んではおらずハングした」＝再入ループの典型で、TIP/アプリの再起動まで打鍵は戻らない（実測: ログ 141,965 行・CPU 124 秒・クラッシュ記録なし）。

## 実装の土台は「設置済み exe の機能マーカー」で特定する（2026-10-06 実測）

再実装・再ビルドの前に、**どのコミットが今 実機に入っているか**を実体で確定してから差分を書く。ローカルの HEAD やリモートの tip を土台にすると、設置版に載っている機能が次のビルドから消える。

- 実測例: ローカル `ci-verify-20260923` の HEAD は `ba5b8c5`（v14）だったが、**設置済み exe（sha `b7f5918a`）はロガー改修 `fdfbda9`（v15）を含んでいた**。`ba5b8c5` を土台にビルドすると確定テキスト記録が丸ごと落ちる。
- 特定法: 設置 exe をバイト探索して機能マーカーを数える（ロガーなら `b'{"cmd":"commit","keys":"'` が 1 件）。`ba5b8c5` のビルドでは 0 件、`fdfbda9` 以降は 1 件。
- ⚠ **リモートの CI ブランチは失敗版で先行していることがある**（実測: `origin/ci-verify-20260923` の tip = v16 `e36815e`）。`git fetch` → `git log --oneline origin/<branch>` → `git rev-list --left-right --count origin/<branch>...HEAD` で先に確認する。

### 失敗版を自分の版で差し替える手順（3-way マージ）

```bash
# base=旧土台 / ours=リモート側(設置版の機能入り) / theirs=自分の実装
cp src/composer/jev_judge.cc $S/mine.cc
git show <旧土台>:src/composer/jev_judge.cc > $S/base.cc
git show <設置版のコミット>:src/composer/jev_judge.cc > $S/ours.cc
git merge-file -p $S/ours.cc $S/base.cc $S/mine.cc > $S/merged.cc
```

- 競合は「両者が同じ 1 箇所（`#ifdef _WIN32` の直前）へ挿入した」1 件だけになるのが典型。解決は**両側を残す**（自分の関数＋ロガー関数）。
- ⚠ **機械的な競合解決で「片側の閉じ括弧が競合境界の外へ出て落ちる」ことがある**（実測: `BuildCommitRequest` の `}` が欠落し、g++ が「a function-definition is not allowed here before '{' token」を出す）。**解決後は必ず `g++ -fsyntax-only` に掛ける** — 目視では気づけない。
- 検証は2方向の diff で締める: `diff 自分の版 解決後` が**追加のみ**（`^<` が 0 行＝設置版の機能が消えていない）、`diff 設置版 解決後` の削除行が**自分の書き換え対象だけ**。
- ハーネスの関数抽出は **行範囲ではなく関数名で**行う（`bool SplitTrailingSegment(` の位置から `\n}\n` まで）。行範囲方式は、無関係な関数（ロガー等）が間に挟まると壊れる（実測: 抽出範囲に `BuildCommitRequest` が混入してハーネスがコンパイル不能になった）。

- **（解決・2026-10-06 適用済み）**: 上記3点を反映した版（commit `f3376c0`・CI run `37428657425` 全7ジョブ成功・設置 sha `fb960d7f…` は成果物と `cmp` rc=0）を実機へ適用し、`NASAni`→「NASAに」・`APIwo`→「APIを」・`korehaAPIdesu`→「これはAPIです」を実機ログで確認。暴走の再発なし（acronym 行は事故時の 141,965 から +4 のみ）。**受け入れ判定はフックログの `switch …` 行の `mode=` と `preedit=` で行う**（`mode=2` のまま＝切替不成立、`preedit` が全角なら挿入方式の誤り。修正前後は `mode=2 len=5 preedit="APIwo"` → `mode=0 len=5 preedit="APIを"` と対比できる）。

## ローカルでの変更前検証（bazel が無くてもできる）

WSL に bazel が無くても**差分の構文とロジックは検証できる**。スタブは同梱 `templates/cxx-stubs/`（`composer/composer.h` = `jev_judge.cc` が使うメソッドだけ宣言／`transliteration/transliteration.h` = 使用する enum 値 `HIRAGANA`/`FULL_ASCII`/`HALF_ASCII`/`HALF_ASCII_UPPER`/`FULL_ASCII_UPPER` だけ定義 — ⚠ **空にしない**。`transliteration::HALF_ASCII` 参照で落ちる）を scratch の `jevh/` へコピーし、以下を回す:

```bash
S=~/.hermes/cache/scratch/jevh; mkdir -p "$S/out"
cp -r /home/sloth/.hermes/skills/software-development/mozc-fork-operations/templates/cxx-stubs/* "$S/"
cd /home/sloth/mozc-fork
g++ -std=c++20 -fsyntax-only -Wall -Wextra -I"$S" -I src src/composer/jev_judge.cc 2> "$S/out/new.txt"
git show HEAD:src/composer/jev_judge.cc > "$S/out/old.cc"   # ⚠ コミット前に回す（HEAD=旧コードが基準。コミット後なら HEAD~1 を基準に）
g++ -std=c++20 -fsyntax-only -Wall -Wextra -I"$S" -I src "$S/out/old.cc" 2> "$S/out/old.txt"
norm() { sed -E "s|:[0-9]+:[0-9]+:|:L:C:|g; s|$S/out/old\.cc|SRC|g; s|src/composer/jev_judge\.cc|SRC|g" "$1" | sort; }
diff <(norm "$S/out/new.txt") <(norm "$S/out/old.txt") >/dev/null && echo NEW_ERRORS_NONE || echo NEW_ERRORS_DIFF
```

- 正規化（`:L:C:`＋ファイルパスを SRC へ）しないと**パス差だけで差分に見える**。両者に残るのは `_WIN32` ガード外 `AppendLogLine` の Windows API エラー（`HANDLE`/`DWORD` 不在・約31行）=**既知の共通分**で正常。
- 判定は「**新規エラーなし＝診断集合が HEAD と同一**」。差分が出たら自分の変更由来だけを直して再実行する。
- 純粋関数（セグメント分割など）は harness に写して実行.assert する。

## どちらが変換エンジンかの判定法
- `mozc_server` を 30ms 間隔で殺し続けると「変換エンジンのプログラムが起動できませんでした」ダイアログ＋打鍵が英字のまま＝**サーバーが変換エンジン**と確定。殺しループは**単一 PowerShell プロセス内**で回す（毎回 powershell を起動すると実効約1秒になりテストが無効化される）。

## 実機テストの前提（最初に「半角英数」かを確認する）
- ハイブリッドの ja 方向が発火するのは**半角英数（HALF_ASCII）モード**のときだけ。**「全角（ひらがな）」「直接入力」では発火しない**（直接入力はキーがそもそもサーバーへ届かない）。
- **IME ON/OFF 系トグル（AHK の Ctrl+Space 含む）が行き来するのは「全角 ⇔ 直接入力」**で、「半角英数」は選べない → これを起点にテストすると必ず「全然動かない」に見える。実測: **アイコン右クリック→半角英数 にしたら初回から動作した**（半角で `aiu` → あいう）。
- テストを依頼したら「**どのモードにしてから打ったか**」を必ず確認する。同じ打鍵列でもモードで結果が変わり、ログの `mode=` だけでは本人が意図したモードは分からない。
- **半角英数と直接入力は別モード**（言語バー表示: 直接入力 = 「A」／半角英数 = 「_A」）。Mozc のモードは6種（ひらがな/全角カタカナ/半角カタカナ/全角英数/半角英数/直接入力）で、直接入力には `TransliterationType` の型が無い（`ImeContext::DIRECT` の状態側＝キーがサーバーに届かず何も効かない）。
- **どちらに居るかの実測プローブ: その状態で `aiu` を打つ** — かな化（あいう）＝機能的な半角英数 ✓／素通し（raw のまま）＝真の直接入力 ✗。ユーザーへの確認は「言語バーが A か _A か」＋このプローブで行う。
- **実機テスト依頼の前の自己チェック（崩れていると結果が解釈不能になる）**: ① `tasklist` に AHK 不在 ② Mozc 設定のキー設定が MS-IME（カスタムでない） ③ 設置 exe のマーカー文字列が意図した世代。
- **正常時の基準動作（回帰の物差し・実機確認済み）**: ひらがなで Ctrl+Space → 半角英数（_A）へ入り、半角英数で英語列（`the`・`i am a pen`・`this is an apple`）は素通し、`aiu` は あいう にかな化。残存課題: 「へっぉ」クラスの誤変換（学習履歴＋語尾タイミングの複合）。

## Claude.ai 相談（行き詰まり時のユーザー側ワークフロー）
- 深いデッドロック（TIP/TSF 層など）ではユーザーが Claude.ai に相談する。依頼されたら**コピペ用サマリを日本語コードブロックで用意**する。構成: 環境・構成／やったこと／症状（**「無反応」ではなく「起きている別動作」を正確に書く** — 診断方向が変わる）／推測していること／確認したいこと／補足。
- ⚠ Claude の回答は**ファイル名レベルで誤り得る**（実在しない `tip_preserved_key.cc` を指示された例あり。正は `tip_text_service.cc`）。鵜呑みにせず必ず fork のソースで裏取りしてから実装し、こちらの結論と食い違う点は差分として明示する。
- 質問は**番号付き（Q1..Qn）で列挙**し、返答も番号ごとに設計へ反映する（対応関係が崩れない）。設計相談では Claude 自身が「実装前に切り分けテストを先に実施せよ」と返すことが多い（例: キーが TIP に届くかの実測、バッファが未確定のまま連結されているかの確認）— 先に切り分けてから実装すると手戻りが減る。

## モード切替は IME（fork のキーマップ）側で実装する — AHK は応急処置
- ユーザー方針: **IME 自体を直すのが本筋。外部ホットキー（AHK）は最終形に残さない**（AHK は IME の ON/OFF しか操作できず「半角英数」を選べない＝ハイブリッドの起点モードに入れない。常駐依存も増える）。
- fork 側の変更先は **`src/data/keymap/ms-ime.tsv`**（タブ区切り3列・1行差し替え）。既定は `Composition`/`Conversion` の `Ctrl Space` = `InsertHalfSpace` → モード切替コマンドへ差し替える。
- 使うコマンド: `ToggleAlphanumericMode`（英数⇔かな）/ `CompositionModeHiragana` / `CompositionModeHalfAlphanumeric`（半角英数を直接選択＝メニュー「半角英数」と同一の `EnsureIMEIsOn` 入り経路。トグルでなく片方向）など（Eisu・Hiragana・F6 に既存割当あり）。**Ctrl+Space は JIS/US 両キーボードに存在**するので割当の集約先に最適（「英数」「半角/全角」キーは JIS 専用）。`Precomposition`（未入力）には Ctrl+Space 行が無い点に注意。
- ⚠ 切替先が「半角英数」になるか（全角側に倒れないか）は実機で確認する。
- **Ctrl+Space 一件の最終決着（実機確認済み）**: 「Ctrl+Space で半角英数に入らず直接入力（A）に落ちる（その状態で `aiu` が素通し）」は単一原因ではなく、**外側から順に2層のゲートが重なっていた**。① 外部ホットキー: **AHK 稼働中はキーがアプリに届かず、Mozc 側を何も変えても症状は変わらない**（→「横取り犯の切り分け」⓪）。② **Mozc のキー設定が「カスタム」だとビルドした ms-ime.tsv が丸ごと無視**され、旧 custom 表（Ctrl+Space→IMEOff/IMEOn＝「直接入力⇔かな」）が効き続ける → **MS-IME へ戻す**。両ゲートを満たした後、Ctrl+Space→半角英数（_A）へ入り、半角英数で `the`・`i am a pen`・`this is an apple` が素通し、ユーザー実機で解決を確認。**Mozc 側の変更をテストする前に「外層（AHK）→ 設定（キー設定）→ バイナリ世代（マーカー文字列）」の順で前提を潰す。**
- **`EnsureIMEIsOn()` は万能ではない**: 実装は `state == DIRECT のときだけ PRECOMPOSITION へ戻す`（session.cc）→ **PRECOMPOSITION 起点では完全な no-op**。`ToggleAlphanumericMode` への1行追加版（`af5840938ba5`）を設置しても「A」症状は残った（ただし AHK 稼働中のマスク状態で、単独効果は未切り分け）。**委譲版は実測で不採用（revert 済み）**: `ToggleAlphanumericMode` を `CompositionModeHalfASCII` / `CompositionModeHiragana` へ委譲すると、その中の `SetNewInput()` が組成を消すため `input_mode.txt` シナリオ（EXPECT_PREEDIT）が落ちる＝切替のたびに編集中の preedit が消える。**「動いている経路の再利用」でも preedit 保持テストを壊すなら不可**（テストは仕様の記述）。「A」問題の真因は AHK＋キー設定カスタムの2層で、fork 側の書き換えは不要だった。到達確認ログ（`composer::jev::DebugLog(...)`）の追加自体は有効（切り分けに実際に効く）。
- ⚠ **「フックのログに出ない＝サーバーに届いていない」は誤推論**: `ProcessCompositionInput` を通るのは**文字を挿入する入力だけ**で、`ToggleAlphanumericMode` 等の**コマンドキーは正常に処理されていてもこのログに出ない**（全角スペース挿入などは「文字」なので組成に現れる）。キー到達の確認は `Session::SendKey`（コマンド実行層）で行う — 同関数には既に `Session::SendKey` の DebugLog が入っており打鍵ごとに記録される。対象コマンド固有の到達確認は、その関数（例: `Session::ToggleAlphanumericMode`）の先頭に同ログを1行足して行う。
- **横取り犯の切り分け（この順で）**: ⓪ **外部ホットキー（AHK 等）を最初に排除** — `tasklist.exe` で `autohotkey` の有無＋スタートアップ（`%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup`）の .lnk を確認。**プロセスが稼働していても即 kill しない — まず CommandLine で「どのスクリプトか」を特定する**（`powershell.exe -NoProfile -Command 'Get-CimInstance Win32_Process -Filter "ProcessId=<pid>" | Select-Object -ExpandProperty CommandLine'`）。`#HotIf WinActive(...)` で特定アプリに限定したスクリプト（ゲーム用マウスサイドボタンの補助等）は他アプリのキーに干渉しないので**そのままでよい**（実測: 稼働中でも Ctrl+Space 経路に影響なし。ファイルが残っていてもプロセス無し＋startup .lnk 無しなら無害）。kill（`taskkill /IM AutoHotkey64.exe /F`）＋**.lnk を退避**（Mozc キーマップへ完全移行したら外すまでがセット）が必要なのは **IME を横取りするスクリプト（`ctrl_space_ime_toggle.ahk` 等）だけ**。`ctrl_space_ime_toggle.ahk` は IMM32 の `IMC_SETOPENSTATUS` で IME 開閉（＝ひらがな⇔直接入力）を実行するため、**稼働中は Mozc 側を何も変えても症状が変わらない（テスト全体が無効化される）**。kill だけでは不十分 — .lnk が残ると再起動で復活し「再起動後だけ A に戻る」症状を再発させる。**「症状が Mozc のソースから導けない動作なら外層を疑う」**（例: `ToggleAlphanumericMode` は ひらがな⇔半角英数 しか作れない＝「ひらがな⇔直接入力」のトグルは Mozc 由来ではあり得ない）。① **TIP は横取りしていない** — TIP が OS から先取りするキーの一覧は `src/win32/tip/tip_text_service.cc` の `kPreservedKeyItems` が唯一（Alt+`／VK_KANJI／VK_DBE_ROMAN／VK_DBE_NOROMAN／VK_F10）で、Ctrl+Space は含まれない。② **`config1.db` の CUSTOM(0) 残骸** — CUSTOM だと `custom_keymap_table` が優先され**ビルドした ms-ime.tsv は丸ごと無視される**。CUSTOM かどうかは **protobuf フィールド 41（`session_keymap`）の値の直読み**が確実（2=MSIME / 0=CUSTOM。プローブ: `scripts/parse_config1_db.py`）。⚠ フィールド 42（`custom_keymap_table`）は **MSIME 設定中も残骸として残り・無視される** — ファイル内の文字列検索（`IMEOff` 等）がヒットしても CUSTOM 有効の証拠にはならない（誤判定のもと）。なお config1.db 自体は **protobuf 直列化**（sqlite3 で開くと `file is not a database`）— 構造を直接編集せず、設定は Mozc 設定ダイアログ経由で変える）。旧 custom 表は Ctrl+Space→IMEOff/IMEOn＝「直接入力⇔かな」で**観測症状と一致**。CUSTOM なら MSIME に戻すか custom 表を新仕様へ作り直す（詳細: `mozc-windows-migration` の custom keymap 節 — 同スキルは user-owned のため変更する場合はユーザー承認が必要）。③ ここまで潰して初めて OS/TSF 上位層（Windows の入力言語ホットキー等）を疑う。**⓪①②（外層→設定）を確認する前に fork 側の再ビルドで直そうとしない。**

## 差し替え（成果物→実機）の定型手順とスモークの罠
- 定型: ⓪**着手前に「残工程はどれか」を実体で確定する**（カード履歴・設置ファイルの mtime・`.bak-*` の有無・稼働プロセスの起動時刻・`gh run list`）— 別セッションが前段（判定サーバー差し替え等）まで実施済みのことがあり、確認せず流すと二重適用になる。⚠ **「進行中」と報告する前に実体を実測する**（`git branch`/`git log`/`gh run list`/対象ファイルの mtime）— 未着手の作業を進行中として報告すると「ビルドはどうなっている？」「進捗は？」と即座に問い直される（2026-10-06 実測: カード完了後は測定だけが行われ実装は未着手だった）→ ①CI 成果物を取得（`gh run download <run> -n mozc_server_exe -D <dir>`）→ ②sha256・PE時刻・**機能マーカー**（例: ロガー改修なら `{"cmd":"commit","keys":"` の出現数が 0→1）を検証 → ③差し替えスクリプトの `$expected` を**実sha（大文字）で埋める**（`PENDING` のまま実行しない）→ ④UAC 起動（事前に consent ダイアログ・既存ログの有無をガード）→ ⑤**読み戻し**は `cmp -s <成果物> <設置物>`（バイト同一・rc=0）で締め、バックアップ実在＋旧版 sha も確認 → ⑥実機スモーク → ⑦カード完了。
- ⚠ **差し替え直後に mozc* プロセスが0本でも異常ではない**: 差し替えスクリプトが全停止し、Mozc は使用時に自動起動する。プロセス不在を失敗と判定せず、実打鍵かスモークで起動とマーカー動作を確認する（稼働プロセスの起動時刻は「差し替えが入った」ことの証拠にもなる）。
- ⚠ **差し替えスクリプトの src はディレクトリ構造ごと配置する**: `replace_server*.ps1` は `$srcDir\x64_windows-opt-*\bin\server\mozc_server_win.exe` を glob して `$src` を作るため、成果物をフラットに（`$srcDir\mozc_server.exe` として）置くと `$src` が null になり `引数が null であるため、パラメーター 'Path' にバインドできません` で COPY が失敗する（`cp -r <artifact>/x64_windows-opt-* $srcDir/` で構造ごと置く。dry-run で先に踏む）。
- 実機スモーク（SendKeys）の罠: Win11 の Notepad は `Start-Process notepad -PassThru` の PID では `AppActivate` が False になり得る（ウィンドウは別プロセス所有）。対策 = `MainWindowHandle -ne 0` かつ `ProcessName -like 'notepad*'` のプロセスをリトライ付きで探して `AppActivate` する（成功時のみキーを送る安全側）。実装例: `tools\mozc-migration\logger_commit_smoke2.ps1`。どうしても活性化できないときは推測で完了にせず、**ユーザーに実打鍵を1回依頼**する（本人の打鍵が最強の受け入れテストで、記録側の増分で判定できる）。
- 確定テキスト記録の実機検証は `judge_commits.jsonl` の1件で行う（`{"ts","v","keys","text","mode","source"}`）。スモーク由来の記録はラベル生成時に除外できる。
- PS スクリプトは ASCII のみで書く（CP932 誤読対策・恒久）。


## 現行アーキテクチャ（2026-10-27 時点・この節が最新）

上の Hermes 時代の記述のうち、**en 方向（かな→英字）・Space=変換・略語分割**は廃止済み。
現行は次の構成。

### 判定と組成
- ハイブリッドは**半角英数モード限定**。ひらがなモードは素の Mozc。
- ja 判定（conf >= 0.90）でひらがなへ移り、判定サーバーが返す segs（例 en:web|ja:wohiraku）
  で英単語を半角のまま残した混在組成に組み直す（MaybeConvertToKana）。
- 入力途中の**下書き（ライブ変換）**: Session::Output が preedit だけ変換結果に差し替える。
  値は converter->Convert() の結果。**英単語を含む組成では変換器を使わず**候補
  （ll_candidate_words）から拾う（web が ウェb に壊れるため）。
  末尾セグメントが3文字未満ならかなのまま保持（kDraftTailHoldChars）。
- **Enter は ConvertOrCommit**（判定由来の組成だけ文全体を変換、それ以外は確定）。
- **Space は SpaceOrConvert**（判定由来は空白、それ以外は従来どおり変換）。
- 組成が終わる全経路（確定・取消・空削除）で MaybeReturnToHalfAscii が半角英数へ戻す。
  戻した直後は server_mode_locked_ でクライアントの古いモードを無視する（追いつくまで）。
- モード表示は自動切替では出さない: Output.suppress_mode_indicator（フィールド **27**）→
  TIP の TipInputModeManager::SetSuppressNextIndicator が消費。手動 Ctrl+Space は表示する。
- kill switch: 環境変数 MOZC_JEV_DRAFT=0 で下書きを無効化。

### 触るときの罠（実測）
- Preedit.Segment は **value_length が required**。手組みで落とすとシリアライズが失敗し、
  クライアントが「互換性のない変換エンジン」を出す（クラッシュはしない）。
- proto のフィールド番号は既存と衝突しないこと（26 は server_version）。衝突は protoc が
  「already been used」で止める。
- Session::Output の中で converter->Convert() を呼ぶと状態検査で 0xc0000409 になる。
  下書きは「キー処理の側で計算し、Output では読むだけ」にする。
- サーバー exe だけ差し替えても、TIP はプロセスごとに古い DLL を掴む。**TIP を変えたときは
  MSI から tip64/tip32 も差し替え、アプリを完全再起動**する。
- mozc_server を手動起動して生死を判定しない（ランチャー経由でないと 0xc0000409 で落ちる）。

### 評価基盤（C:\Users\yuhi1\tools\jev-ime-win\tools\eval）
- uild_eval_set.py → eval_set.jsonl（確定記録＋ケース集＋英語サンプル、約1,677件）
- eval_engine.py: product view（日本語が ASCII のまま / 英語がかな化）を出力
- eatures.py + usion_experiment.py: 特徴融合（LR）の実験。**既存特徴の再重み付けでは
  ルール連鎖に勝てない**（適合率で劣る）
- ugment_vocab.py: かな語彙の拡張（活用形・助詞。実入力からも抽出）
- 2026-10-27 時点: 日本語の取りこぼし 2.4% / 英語のかな化 2.2%（ja 再現 0.976・適合 0.987）

### 判定サーバーの起動は「見張りタスク」経由にする（2026-10-27 実測）
DSH のコマンドから Start-Process で判定サーバーを起動すると、そのコマンドのジョブを
終了・キャンセルしたときに**子プロセスごと巻き添えで落ちる**（実際に起きた）。
起動は schtasks /run /tn "jev-judge-server-watchdog" を使う。タスクから起動された
プロセスは DSH のジョブツリーに属さないため残る。5分ごとの自動復旧も同じ経路。
