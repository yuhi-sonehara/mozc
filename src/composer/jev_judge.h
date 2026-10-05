// Copyright 2026, yuhi-sonehara
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of the copyright holder nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef MOZC_COMPOSER_JEV_JUDGE_H_
#define MOZC_COMPOSER_JEV_JUDGE_H_


#include <string>

namespace mozc {
namespace composer {

class Composer;

namespace jev {

// 外部の日英判定エンジン（ローカルで動く常駐サーバー）に問い合わせる口。
// Windows では名前付きパイプ、それ以外の環境では常に「判定なし」を返す。
// サーバーが動いていない場合は即座に判定なしを返し、IME を止めない。
class Judge {
 public:
  Judge() = delete;

  // ローマ字列が英語入力かどうかを判定する。
  // 判定できないとき (サーバー不通・応答なし) は false を返す。
  static bool IsEnglish(const std::string &romaji);

  // 応答までの待ち時間の上限（ミリ秒）。
  static int GetTimeoutMsec();
};

// 組成中の生ローマ字が英語と判定されたら、かな組成を生ローマ字に置き換えて
// 半角英数の素通しへ切り替える。切り替えた場合のみ true を返す。
// Composer::ProcessCompositionInput から呼ばれる。
bool MaybeSwitchToEnglish(Composer *composer);

// 確定テキストの記録（ロガー改修）: 判定サーバーへ commit イベントを送る。
// 確定したテキストをサーバー側の JSONL（judge_commits）へ記録し、
// 判定モデルの正解ラベルを作るための素材になる。
//   raw  : 確定時の生ローマ字（ベストエフォート。空の場合あり）
//   mode : 確定時の入力モード（transliteration::TransliterationType の値）
//   text : クライアントへ渡された確定テキストそのもの
// サーバー不在・送信失敗時は何もしない（IME の動作に影響しない）。
void NotifyCommit(const std::string &raw, int mode, const std::string &text);

// 入力モードの遷移がどこから来たかを表す。
// 自動切替（判定フック）とユーザー操作（Ctrl+Space 等のトグル）を区別し、
// 確定時の自動復帰を「判定器由来だけ」に限定するために使う。
enum class ModeOrigin {
  kDefault,             // まだ遷移していない / 由来不明
  kAutoSwitchedByJudge,  // 日英判定フックが自動的に切り替えた
  kManualByUser,         // ユーザー操作（トグル・メニュー等）で切り替えた
};

// 現在のモード遷移の由来を記録する。
void SetModeOrigin(ModeOrigin origin);

// 現在のモード遷移の由来を返す。
ModeOrigin GetModeOrigin();

// 記録した由来を kDefault に戻す。
void ClearModeOrigin();

// 診断用: %TEMP%\jev_judge_hook.log に1行追記する。
// 入力経路のどこまで到達しているかを層ごとに確認するために使う。
void DebugLog(const std::string &line);

}  // namespace jev
}  // namespace composer
}  // namespace mozc

#endif  // MOZC_COMPOSER_JEV_JUDGE_H_
