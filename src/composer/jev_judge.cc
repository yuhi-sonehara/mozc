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

#include "composer/jev_judge.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "composer/composer.h"
#include "transliteration/transliteration.h"

#ifdef _WIN32
#include <windows.h>
#endif  // _WIN32

namespace {
// 診断専用: 指定パスへ追記（失敗は無視）
void AppendLogLine(const std::wstring &path, const std::string &line) {
  HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  ::WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
  ::CloseHandle(h);
}
}  // namespace

namespace {
std::wstring JoinPath(const wchar_t *dir, const wchar_t *name) {
  std::wstring p(dir);
  if (!p.empty() && p[p.size() - 1] != L'\\') p += L'\\';
  p += name;
  return p;
}
}  // namespace

namespace mozc {
namespace composer {
namespace jev {
namespace {

// これ未満の確信度では切り替えない（判定器の「英語」の最低ライン）。
constexpr double kMinConfidence = 0.85;      // en 判定用（Judge::IsEnglish）
constexpr double kMinConfidenceJa = 0.90;    // ja 判定用（0.90 未満では切り替えない: hel=0.85 の誤変換防止）
// 末尾セグメント（最後の空白以降）に許可する最小長。
// 2 打鍵語（助詞「に」など）の救済のため 2 文字を下限とする（1 文字は誤爆する）。
constexpr size_t kMinSegmentLength = 2;
// 打鍵ごとの即時判定に許可する最小長。2 打鍵で判定すると英単語の2文字目
// （ha / ta / se 等）で誤ってかなへ切り替わるため、即時は 3 文字から。
// 例外は略語境界の直後（NASAni の "ni" / APIwo の "wo"）だけ。語の切れ目が
// 明示されているので従来どおり即時に判定する。単独の2打鍵語は Session 側が
// 区切り（Space / Enter）で判定する（defer-short）。
constexpr size_t kMinImmediateLength = 3;
// 1 回の呼び出しで許す待ち時間の上限（ミリ秒）。IME を止めないための上限。
#ifdef _WIN32
constexpr DWORD kIpcTimeoutMsec = 5;
constexpr DWORD kConnectRetryWaitMsec = 50;
const wchar_t kPipeName[] = L"\\\\.\\pipe\\jevime_judge";
#endif  // _WIN32

#ifdef _WIN32

std::string JsonEscape(const std::string &src) {
  std::string out;
  out.reserve(src.size() + 8);
  for (const char c : src) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

// "key" に対応する文字列値を取り出す（最小限のパーサ）。
bool FindStringValue(const std::string &json, const std::string &key,
                     std::string *value) {
  const std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) {
    return false;
  }
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) {
    return false;
  }
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
    ++pos;
  }
  if (pos >= json.size() || json[pos] != '"') {
    return false;
  }
  ++pos;
  const size_t end = json.find('"', pos);
  if (end == std::string::npos) {
    return false;
  }
  value->assign(json, pos, end - pos);
  return true;
}

// "key" に対応する数値を取り出す（最小限のパーサ）。
bool FindNumberValue(const std::string &json, const std::string &key,
                     double *value) {
  const std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) {
    return false;
  }
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) {
    return false;
  }
  const char *begin = json.c_str() + pos + 1;
  char *end = nullptr;
  const double parsed = std::strtod(begin, &end);
  if (end == begin) {
    return false;
  }
  *value = parsed;
  return true;
}

// 名前付きパイプへ 1 往復する。往復できなければ空文字を返す。
std::string Transact(const std::string &request) {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 2 && pipe == INVALID_HANDLE_VALUE;
       ++attempt) {
    pipe = ::CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                         OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE && ::GetLastError() == ERROR_PIPE_BUSY) {
      ::WaitNamedPipeW(kPipeName, kConnectRetryWaitMsec);
    }
  }
  if (pipe == INVALID_HANDLE_VALUE) {
    return "";  // サーバー不在。判定なし。
  }

  std::string response;
  HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event != nullptr) {
    const std::string line = request + "\n";
    OVERLAPPED write_ov = {};
    write_ov.hEvent = event;
    DWORD written = 0;
    BOOL ok = ::WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()),
                          &written, &write_ov);
    if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
      ok = (::WaitForSingleObject(event, kIpcTimeoutMsec) == WAIT_OBJECT_0);
      if (ok) {
        ok = (::GetOverlappedResult(pipe, &write_ov, &written, FALSE) != FALSE);
      } else {
        ::CancelIoEx(pipe, &write_ov);
        ::GetOverlappedResult(pipe, &write_ov, &written, TRUE);
      }
    }

    if (ok) {
      // 応答は 1 行。改行が来るまで読む（上限は kIpcTimeoutMsec）。
      for (int loop = 0; loop < 8; ++loop) {
        char buf[1024];
        ::ResetEvent(event);
        OVERLAPPED read_ov = {};
        read_ov.hEvent = event;
        DWORD read = 0;
        BOOL rok = ::ReadFile(pipe, buf, sizeof(buf), &read, &read_ov);
        if (!rok && ::GetLastError() == ERROR_IO_PENDING) {
          if (::WaitForSingleObject(event, kIpcTimeoutMsec) == WAIT_OBJECT_0) {
            rok = (::GetOverlappedResult(pipe, &read_ov, &read, FALSE) != FALSE);
          } else {
            ::CancelIoEx(pipe, &read_ov);
            ::GetOverlappedResult(pipe, &read_ov, &read, TRUE);
            break;  // 応答が来ない。判定なし。
          }
        }
        if (!rok || read == 0) {
          break;
        }
        response.append(buf, read);
        if (response.find('\n') != std::string::npos) {
          break;
        }
      }
    }
    ::CloseHandle(event);
  }
  ::CloseHandle(pipe);
  return response;
}

#endif  // _WIN32

// 生ローマ字列が判定に回してよいキー列（英字・数字・'-'）かどうか。
// 長音（'-'）を含む日本語ローマ字（de-ta = でーた、sa-ba- = さーばー 等）と
// 数字入りの打鍵を判定に回すため、英字のみ（旧 IsAsciiLetters）から緩和した
// （2026-10-05）。安全側の根拠（実測 2026-09-28〜10-05）: 死判定ゲートは
// [a-z0-9-] のみを対象とし、'-' は生存（長音）・数字列は keep（2026/123/3 で実測）
// のため切替を誘発しない。sim --allow-keys-punct で回帰なし
// （cases 42/44・bench 69/74・新規誤爆ゼロ）。
bool IsJudgeableKeys(const std::string &text) {
  if (text.empty()) {
    return false;
  }
  for (const char c : text) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

// 生ローマ字全体を判定に回してよいか（空白を含む）。判定由来の組成を
// まとめて組み直すときに使う。空白はサーバー側で区切りとして扱われる。
bool IsJudgeableRun(const std::string &text) {
  if (text.empty()) {
    return false;
  }
  for (const char c : text) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == ' ';
    if (!ok) {
      return false;
    }
  }
  return true;
}

// 末尾セグメント = 最後の空白以降の部分。空白が無ければ入力全体。
// 「web wo」のように空白入りの入力を判定に回すために使う。
//   prefix   : "web "（末尾セグメントより前の部分。判定はしない）
//   segment  : "wo"    （判定対象）
// 末尾の空白のみが入力されているときは segment が空になるので false。
//
// 空白が無い打鍵（NASAni / APIwo / GFL2wo など）は、略語境界でも分割する
// （2026-10-06 追加）。実測 16,283 件（NASAni 8,530 / NASAn 7,111 /
// NASAnii 604 / AHKko / UImojissou / GFL2wo ほか）がこの形で、従来は打鍵列
// まるごとが判定に回っていたため日本語部分が変換されなかった。
// 境界は「直前までが全て [A-Z0-9] かつ 2 文字以上」の位置に限定し、
// 1 文字プレフィックス（"S|oundcore"）は分割しない（誤爆防止）。
bool SplitTrailingSegment(const std::string &romaji, std::string *prefix,
                          std::string *segment) {
  const size_t space = romaji.find_last_of(' ');
  if (space == std::string::npos) {
    // Fork: 半角の句読点が組成に残っていても判定を止めない（2026-10-29）。
    // 確定後の自動復帰で半角英数に戻った直後に打った「,」「.」は、そのまま
    // 組成に残る。判定ゲートは [a-z0-9-] 以外を弾くため、句読点より後ろを
    // 判定対象にすれば日本語の変換が再開する。
    const size_t punct = romaji.find_last_of(",.");
    if (punct != std::string::npos && punct + 1 < romaji.size()) {
      *prefix = romaji.substr(0, punct + 1);
      *segment = romaji.substr(punct + 1);
      if (segment->size() >= kMinSegmentLength) {
        return true;
      }
    }
    if (romaji.size() >= 4) {
      for (size_t i = romaji.size() - 1; i >= 2; --i) {
        const char prev = romaji[i - 1];
        const char cur = romaji[i];
        const bool prev_upper_or_digit =
            (prev >= 'A' && prev <= 'Z') || (prev >= '0' && prev <= '9');
        const bool cur_lower = cur >= 'a' && cur <= 'z';
        if (!prev_upper_or_digit || !cur_lower) {
          continue;
        }
        const std::string head = romaji.substr(0, i);
        if (head.size() < 2) {
          continue;
        }
        bool head_is_acronym = true;
        for (std::string::size_type k = 0; k < head.size(); ++k) {
          const char c = head[k];
          if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            head_is_acronym = false;
            break;
          }
        }
        if (!head_is_acronym) {
          continue;
        }
        *prefix = head;
        *segment = romaji.substr(i);
        return !segment->empty();
      }
    }
    prefix->clear();
    *segment = romaji;
  } else {
    *prefix = romaji.substr(0, space + 1);
    *segment = romaji.substr(space + 1);
  }
  return !segment->empty();
}

// 確定テキスト記録（ロガー改修）: JSON 文字列のエスケープ。
// 確定テキストは改行・タブ等の制御文字を含み得るため、keystroke 用の
// JsonEscape より厳密に、JSON として無効な文字（0x00〜0x1F）をすべて
// エスケープする（JSONL を 1 行 1 レコードに保つ）。
std::string EscapeJsonString(const std::string &src) {
  std::string out;
  out.reserve(src.size() + 8);
  for (const unsigned char c : src) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
        break;
    }
  }
  return out;
}

// 確定テキスト記録（ロガー改修）: commit イベントの JSON リクエストを
// 組み立てる。プラットフォーム非依存の純粋関数（テスト harness から
// そのまま呼べる形にしてある）。
std::string BuildCommitRequest(const std::string &raw, int mode,
                               const std::string &text) {
  std::string out = "{\"cmd\":\"commit\",\"keys\":\"";
  out += EscapeJsonString(raw);
  out += "\",\"text\":\"";
  out += EscapeJsonString(text);
  out += "\",\"mode\":";
  out += std::to_string(mode);
  out += ",\"source\":\"mozc\",\"version\":2}";
  return out;
}

#ifdef _WIN32
// 診断用: フックが呼ばれた事実をファイルにも残す。
// パイプ不通（サーバーに届かない）とフック未発火を区別するために使う。
void WriteDebugLog(const std::string &line) {
  // クラッシュ対策: 以前は 5 箇所（TEMP / USERPROFILE / PUBLIC / ProgramData /
  // LocalLow\\Mozc）へ毎回書き込んでいた。1 打鍵ごとの同期 I/O が 5 倍になり、
  // ディスク I/O の負荷でアプリが固まる一因になっていたため、記録は実際に参照
  // している LocalLow の 1 箇所だけに絞る（内容は従来と同一）。
  static bool banner_done = false;
  const std::string banner = "=== jev_judge debug log (instrumented build) ===\n";
  const std::string body = banner_done ? line : (banner + line);
  {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) {
      std::wstring dir(buf);
      dir += L"\\AppData\\LocalLow\\Mozc";
      ::CreateDirectoryW(dir.c_str(), nullptr);
      AppendLogLine(JoinPath(dir.c_str(), L"jev_judge_hook.log"), body);
    }
  }
  ::OutputDebugStringA(body.c_str());
  banner_done = true;
}

struct Verdict {
  std::string decision;
  // 判定理由。死列（かなとして成立しない）の判定だけ半角へ戻すために使う。
  std::string reason;
  double confidence = 0.0;
  // 判定サーバーが返す区切り（"en:web|ja:wohiraku"）。空なら区切りなし。
  // first = true は「半角英数のまま残す英語トークン」を表す。
  std::vector<std::pair<bool, std::string>> segments;
};

// "en:web|ja:wohiraku" 形式の区切りを解釈する。壊れていれば何も足さない。
void ParseSegmentList(const std::string &spec,
                      std::vector<std::pair<bool, std::string>> *out) {
  std::string::size_type start = 0;
  while (start <= spec.size()) {
    std::string::size_type bar = spec.find('|', start);
    const std::string piece =
        spec.substr(start, bar == std::string::npos ? std::string::npos
                                                    : bar - start);
    const std::string::size_type colon = piece.find(':');
    if (colon != std::string::npos && colon > 0) {
      const std::string label = piece.substr(0, colon);
      const std::string text = piece.substr(colon + 1);
      if (!text.empty()) {
        out->emplace_back(label == "en", text);
      }
    }
    if (bar == std::string::npos) {
      break;
    }
    start = bar + 1;
  }
}

// 判定サーバーへ問い合わせる（生ローマ字と入力モードを送る）。
Verdict QueryDecision(const std::string &romaji, int mode) {
  Verdict verdict;
  const std::string request =
      std::string("{\"keys\":\"") + JsonEscape(romaji) + "\",\"mode\":" +
      std::to_string(mode) + ",\"source\":\"mozc\",\"version\":2}";
  const std::string response = Transact(request);
  if (response.empty()) {
    return verdict;
  }
  FindStringValue(response, "decision", &verdict.decision);
  FindStringValue(response, "reason", &verdict.reason);
  FindNumberValue(response, "confidence", &verdict.confidence);
  std::string segs;
  if (FindStringValue(response, "segs", &segs)) {
    ParseSegmentList(segs, &verdict.segments);
  }
  return verdict;
}

// 診断用: 最初のフック呼び出しでサーバーへ ping を送り、ファイルにも記録する。
void ReportFirstCall(const std::string &romaji, int mode) {
  static bool reported = false;
  if (reported) {
    return;
  }
  reported = true;
  const std::string note = "first hook call: mode=" + std::to_string(mode) +
                           " raw=\"" + romaji + "\"\n";
  WriteDebugLog(note);
  Transact(std::string("{\"cmd\":\"ping\",\"source\":\"mozc\",\"note\":") +
           "\"first-hook-call\"}");
}

// 確定テキスト記録（ロガー改修）: 確定テキストをサーバーへ送る。
// 応答は使わない。失敗（サーバー不在・タイムアウト等）は無視して
// IME を止めない。ログの取りこぼしを減らすため数回だけ再試行する
// （サーバー側パイプのインスタンス切り替えの隙間に当たった場合の救済。
// コミットは人間の操作間隔でしか発生しないため、この再試行は軽い）。
void SendCommitRequest(const std::string &raw, int mode,
                       const std::string &text) {
  const std::string request = BuildCommitRequest(raw, mode, text);
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (attempt > 0) {
      ::Sleep(1);
    }
    if (!Transact(request).empty()) {
      return;  // サーバーが受け取った（応答の中身は使わない）
    }
  }
}
#else  // !_WIN32
struct Verdict {
  std::string decision;
  std::string reason;
  double confidence = 0.0;
  std::vector<std::pair<bool, std::string>> segments;
};

Verdict QueryDecision(const std::string &romaji, int mode) {
  (void)romaji;
  (void)mode;
  return Verdict();
}

void WriteDebugLog(const std::string &line) { (void)line; }

void ReportFirstCall(const std::string &romaji, int mode) {
  (void)romaji;
  (void)mode;
}

void SendCommitRequest(const std::string &raw, int mode,
                       const std::string &text) {
  // 非 Windows ではパイプが無いため送らない。共通部（リクエスト組み立て）
  // をコンパイル対象に含め、未使用関数警告も避ける。
  (void)BuildCommitRequest(raw, mode, text);
}
#endif  // _WIN32

}  // namespace

// 現在のモード遷移の由来。IME サーバーは単一スレッドなので 1 つで足りる。
ModeOrigin g_mode_origin = ModeOrigin::kDefault;

int Judge::GetTimeoutMsec() {
#ifdef _WIN32
  return static_cast<int>(kIpcTimeoutMsec);
#else
  return 0;
#endif  // _WIN32
}

bool Judge::IsEnglish(const std::string &romaji) {
  const Verdict verdict = QueryDecision(romaji, -1);
  return verdict.decision == "en" && verdict.confidence >= kMinConfidence;
}

// 2 打鍵の半角組成を区切り（Space / Enter）で判定する。打鍵途中の即時判定は
// kMinImmediateLength で止めてあるため、2 打鍵語はここだけでかな化する。
// 判定サーバー不在・応答なしのときは false（従来どおり ASCII のまま）。
bool JudgeShortRunIsKana(const std::string &romaji) {
  const Verdict verdict = QueryDecision(romaji, -1);
  return verdict.decision == "ja" && verdict.confidence >= kMinConfidenceJa;
}

void DebugLog(const std::string &line) { WriteDebugLog(line); }

void NotifyCommit(const std::string &raw, int mode, const std::string &text) {
  SendCommitRequest(raw, mode, text);
}

namespace {
// 自動切替の直後 1 回だけ表示更新を抑制するための予約。
bool g_suppress_mode_indicator = false;
}  // namespace

void RequestModeIndicatorSuppression() { g_suppress_mode_indicator = true; }

bool ConsumeModeIndicatorSuppression() {
  const bool requested = g_suppress_mode_indicator;
  g_suppress_mode_indicator = false;
  return requested;
}

void SetModeOrigin(ModeOrigin origin) {
  // IME サーバーは単一スレッドなので状態は 1 つで足りる（thread_local 不要）。
  g_mode_origin = origin;
}

ModeOrigin GetModeOrigin() { return g_mode_origin; }

void ClearModeOrigin() { g_mode_origin = ModeOrigin::kDefault; }

bool MaybeConvertToKana(Composer *composer) {
  // 自分の書き換え（InsertCharacter / InsertCharacterPreedit）で再入しないための番人。
  // ⚠ 組み直しの間は必ず true を保持する。前回 v16 は判定問い合わせの周りだけを
  //   true/false し、組み直し（InsertCharacter 等）を素で呼んだため、
  //   InsertCharacter → ProcessCompositionInput → 本フックの再入で同じ分岐が
  //   再発火して暴走ループ（実測 141,965 件）→ IME 応答停止になった。
  static thread_local bool in_hook = false;
  if (in_hook || composer == nullptr) {
    return false;
  }
  if (composer->Empty()) {
    return false;
  }

  const transliteration::TransliterationType mode = composer->GetInputMode();
  // 2026-10-22: ハイブリッドの起点は半角英数モード。
  // 2026-10-31: 判定由来の組成（kAutoSwitchedByJudge）に限り、ひらがなモードでも
  // 判定を続ける。日本語の途中に打った英単語を、確定せずに ASCII のまま残すため。
  const bool judge_origin = GetModeOrigin() == ModeOrigin::kAutoSwitchedByJudge;
  if (mode != transliteration::HALF_ASCII &&
      !(mode == transliteration::HIRAGANA && judge_origin)) {
    return false;
  }
  const int mode_value = static_cast<int>(mode);
  const std::string romaji = composer->GetRawString();

  // 診断: フックが呼ばれた事実を1回だけサーバーとファイルの両方に残す。
  ReportFirstCall(romaji, mode_value);

  // 組成の全組み直し。segments は (半角英数のまま生挿入するか, 文字列) の並びで
  // 順に挿入する。
  //   first = true  : HALF_ASCII モードで生挿入（略語トークンを半角のまま見せる）
  //   first = false : HIRAGANA モードで挿入（ローマ字をかなへ変換）
  // ⚠ 半角英数モードでは「末尾セグメントだけの部分組み直し」が画面に反映されない
  //   （2026-10-06 実測: APIwo で segment="wo" が毎打鍵再発火し preedit は APIwo の
  //   まま = raw が伸びない典型症状）。実績のある en 方向と同じ「全消し → 再投入」
  //   に統一する（部分組み直しは行わない）。
  // ⚠ 呼び出し側は in_hook = true のまま本ラムダを呼ぶこと（再入防止）。
  auto rebuild = [&](const std::vector<std::pair<bool, std::string>> &segments) {
    bool has_english_segment = false;
    composer->SetInputMode(transliteration::HIRAGANA);
    composer->SetNewInput();
    composer->DeleteRange(0, composer->GetLength());
    for (const std::pair<bool, std::string> &seg : segments) {
      if (seg.second.empty()) {
        continue;
      }
      has_english_segment = has_english_segment || seg.first;
      composer->SetInputMode(seg.first ? transliteration::HALF_ASCII
                                       : transliteration::HIRAGANA);
      composer->SetNewInput();
      if (seg.first) {
        // 半角のまま見せたい部分は、実績のある en 方向と同じ生挿入を使う
        // （かなモードで InsertCharacterPreedit すると全角化されるため）。
        composer->InsertCharacterPreedit(seg.second);
      } else {
        for (std::string::size_type i = 0; i < seg.second.size(); ++i) {
          composer->InsertCharacter(seg.second.substr(i, 1));
        }
      }
    }
    // モードは「ひらがな」で終える（英日混在のあとも続けて日本語を打てるように）。
    // SetInputMode は以後に作るチャンクの字種を決めるだけで、組んだチャンクは
    // そのまま残る（Composition::SetInputMode は input_t12r_ のみ更新）。
    // 2026-10-27: ただし英語セグメントを含む混在組成では半角英数のまま終える。
    // ひらがなで終えると「goal が」のあとに blocked と打ったとき、b 以降が
    // ローマ字としてかな化してしまう（実測: ｂぉｃけｄ）。
    composer->SetInputMode(has_english_segment ? transliteration::HALF_ASCII
                                              : transliteration::HIRAGANA);
  };

  // 現在の表示。ASCII / かなの混在具合で「全体を判定するか、末尾だけで
  // 判定するか」を決める。
  const std::string preedit_now = composer->GetStringForPreedit();
  bool preedit_has_ascii = false;
  bool preedit_has_kana = false;
  for (const char c : preedit_now) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u >= 0x80) {
      preedit_has_kana = true;
    } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      preedit_has_ascii = true;
    }
  }
  // 判定由来の組成がすでにかなを含むなら、末尾ではなく生ローマ字全体（空白を
  // 含む）を判定して組み直す。末尾だけを組み直すと、手前のかなを ASCII で
  // 入れ直して壊す（実測: は な → ha na）。
  const bool whole_run =
      judge_origin && (mode == transliteration::HIRAGANA || preedit_has_kana);

  std::string prefix;
  std::string segment;
  if (whole_run) {
    segment = romaji;
    if (segment.size() < kMinSegmentLength || !IsJudgeableRun(segment)) {
      return false;
    }
  } else {
    if (!SplitTrailingSegment(romaji, &prefix, &segment) ||
        segment.size() < kMinSegmentLength || !IsJudgeableKeys(segment)) {
      return false;
    }
    if (segment.size() < kMinImmediateLength) {
      // 2 打鍵の即時判定は、直前が略語（大文字・数字）で終わるときだけ許可する。
      const char last = prefix.empty() ? '\0' : prefix[prefix.size() - 1];
      const bool after_acronym = (last >= 'A' && last <= 'Z') ||
                                 (last >= '0' && last <= '9');
      if (!after_acronym) {
        return false;
      }
    }
  }

  in_hook = true;  // 判定と組み直しの全体をガードする（組み直し中の再入防止）
  const Verdict verdict = QueryDecision(segment, mode_value);

  if (whole_run) {
    // サーバーが返すスパン（ja:ha|en: |ja:na|...）で全体を組み直す。
    std::string joined;
    bool has_ja_span = false;
    bool has_en_word = false;
    for (const std::pair<bool, std::string> &seg : verdict.segments) {
      joined += seg.second;
      if (!seg.first) {
        has_ja_span = true;
        continue;
      }
      for (const char c : seg.second) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
          has_en_word = true;
          break;
        }
      }
    }
    if (!verdict.segments.empty() && joined == romaji && has_ja_span) {
      if (!has_en_word && !preedit_has_ascii) {
        in_hook = false;
        return false;  // すでにかなだけ。組み直さない。
      }
      rebuild(verdict.segments);
      SetModeOrigin(ModeOrigin::kAutoSwitchedByJudge);
      if (composer->GetInputMode() != mode) {
        RequestModeIndicatorSuppression();
      }
      std::string seg_log;
      for (const std::pair<bool, std::string> &seg : verdict.segments) {
        if (!seg_log.empty()) {
          seg_log += "|";
        }
        seg_log += seg.first ? "en:" : "ja:";
        seg_log += seg.second;
      }
      WriteDebugLog("switch to hiragana (whole): raw=\"" + romaji +
                    "\" segs=\"" + seg_log + "\" preedit=\"" +
                    composer->GetStringForPreedit() + "\"\n");
      in_hook = false;
      return true;
    }
    // スパンが無い・一致しない場合は従来の単一判定にフォールバックする。
    if (verdict.decision == "en" && verdict.confidence >= kMinConfidence &&
        verdict.reason.rfind("dead column", 0) == 0 && !preedit_has_kana) {
      rebuild({std::make_pair(true, romaji)});
      SetModeOrigin(ModeOrigin::kAutoSwitchedByJudge);
      if (composer->GetInputMode() != mode) {
        RequestModeIndicatorSuppression();
      }
      WriteDebugLog("switch to half-ascii: raw=\"" + romaji +
                    "\" reason=dead column preedit=\"" +
                    composer->GetStringForPreedit() + "\"\n");
      in_hook = false;
      return true;
    }
  }

  if (verdict.decision != "ja" || verdict.confidence < kMinConfidenceJa) {
    in_hook = false;
    return false;
  }
  // 英語スパンが無く、組成にも ASCII が残っていないなら組み直さない
  // （純粋なかな入力のたびに全体を再構築しない）。
  bool has_en_span = false;
  for (const std::pair<bool, std::string> &seg : verdict.segments) {
    has_en_span = has_en_span || seg.first;
  }
  if (!has_en_span && !preedit_has_ascii) {
    in_hook = false;
    return false;
  }

  // サーバーが区切り（"en:web|ja:wohiraku"）を返したら、英単語を半角のまま残した
  // 混在組成に組み直す（2026-10-22: 空白なしの英日混在への対応）。区切りが無い、
  // または末尾セグメントと一致しない（壊れている）場合は全体をかなへ組み直す。
  std::vector<std::pair<bool, std::string>> segments;
  std::string joined;
  for (const std::pair<bool, std::string> &seg : verdict.segments) {
    joined += seg.second;
  }
  if (!prefix.empty()) {
    segments.emplace_back(true, prefix);
  }
  if (!verdict.segments.empty() && joined == segment) {
    for (const std::pair<bool, std::string> &seg : verdict.segments) {
      segments.push_back(seg);
    }
  } else {
    segments.emplace_back(false, segment);
  }

  rebuild(segments);
  SetModeOrigin(ModeOrigin::kAutoSwitchedByJudge);
  if (composer->GetInputMode() != mode) {
    RequestModeIndicatorSuppression();
  }

  std::string seg_log;
  for (const std::pair<bool, std::string> &seg : segments) {
    if (!seg_log.empty()) {
      seg_log += "|";
    }
    seg_log += seg.first ? "en:" : "ja:";
    seg_log += seg.second;
  }
  WriteDebugLog("switch to hiragana: raw=\"" + romaji + "\" segment=\"" +
                segment + "\" conf=" + std::to_string(verdict.confidence) +
                " segs=\"" + seg_log + "\" preedit=\"" +
                composer->GetStringForPreedit() + "\"\n");
  in_hook = false;
  return true;
}

}  // namespace jev
}  // namespace composer
}  // namespace mozc
