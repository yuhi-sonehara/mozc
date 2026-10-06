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
constexpr double kMinConfidence = 0.85;      // en 判定用（従来どおり）
constexpr double kMinConfidenceJa = 0.90;    // ja 判定用（0.90 未満では切り替えない: hel=0.85 の誤変換防止）
// これ未満の長さでは問い合わせない（誤爆を避ける）。
constexpr size_t kMinLength = 3;
// 末尾セグメント（最後の空白以降）に許可する最小長。
// 2 打鍵語（助詞「に」など）の救済のため kMinLength より緩くする。
// 1 文字は誤爆するので 2 文字を下限とする。
constexpr size_t kMinSegmentLength = 2;
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

// 末尾セグメント = 最後の空白以降の部分。空白が無ければ入力全体。
// 「web wo」のように空白入りの入力を判定に回すために使う。
//   prefix   : "web "（末尾セグメントより前の部分。判定はしない）
//   segment  : "wo"    （判定対象）
// 末尾の空白のみが入力されているときは segment が空になるので false。
bool SplitTrailingSegment(const std::string &romaji, std::string *prefix,
                          std::string *segment) {
  const size_t space = romaji.find_last_of(' ');
  if (space == std::string::npos) {
    // 空白が無いときは「略語＋ローマ字かな」の境界でも分割する（2026-10-06 追加・
    // 選択肢2）。例: "NASAni" -> prefix="NASA" / segment="ni"、
    // "GFL2wo" -> "GFL2" / "wo"。実測 16,283 件（NASAni 8,530 / NASAn 7,111 /
    // NASAnii 604 / AHKko / UImojissou / GFL2wo ほか）がこの形で、従来は打鍵列
    // まるごとが判定されていたため日本語部分が変換されなかった。
    // 誤爆防止のため、境界は「直前までが全て [A-Z0-9] かつ 2 文字以上」の位置に
    // 限定する（"Soundcore" の "S|oundcore" のような 1 文字プレフィックスは
    // 分割せず、従来どおり打鍵列まるごとを判定する）。
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

// 打鍵列中の最後の「略語（[A-Z0-9]{2,}）| ローマ字かな」境界を求める
// （2026-10-06 追加・選択肢2）。例: "korehaAPIdesu" -> prefix=6 / token=3 /
// tail="desu"、"UImojissou" -> prefix=0 / token=2 / tail="mojissou"。
// 境界が見つからないときは false を返す（呼び出し側は従来の処理に進む）。
bool FindAcronymSplit(const std::string &romaji, size_t *prefix_len,
                      size_t *token_len, std::string *tail) {
  const size_t n = romaji.size();
  if (n < 4) {
    return false;
  }
  // 末尾が略語（[A-Z0-9]{2,}）で終わっている形（"korehaAPI"、"nihonGO"）は、
  // 後続の小文字が来る前でもトークンを保持したいので先に調べる。HDD のような
  // 略語をかなモードで打ったとき、次の打鍵を待たずに半角のまま残す。
  {
    size_t start = n;
    while (start > 0) {
      const char c = romaji[start - 1];
      const bool is_upper_or_digit =
          (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
      if (!is_upper_or_digit) {
        break;
      }
      --start;
    }
    if (start < n && start > 0 && n - start >= 2) {
      *prefix_len = start;
      *token_len = n - start;
      tail->clear();
      return true;
    }
  }
  for (size_t i = n - 1; i >= 2; --i) {
    const char prev = romaji[i - 1];  // 略語トークンの最後の文字
    const char cur = romaji[i];       // かな部分の先頭
    const bool prev_upper_or_digit =
        (prev >= 'A' && prev <= 'Z') || (prev >= '0' && prev <= '9');
    const bool cur_lower = cur >= 'a' && cur <= 'z';
    if (!prev_upper_or_digit || !cur_lower) {
      continue;
    }
    // トークンの先頭まで [A-Z0-9] をさかのぼる。
    size_t start = i;
    while (start > 0) {
      const char c = romaji[start - 1];
      const bool is_upper_or_digit =
          (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
      if (!is_upper_or_digit) {
        break;
      }
      --start;
    }
    if (i - start < 2) {
      continue;  // 略語は 2 文字以上（1 文字の大文字は対象外）
    }
    *prefix_len = start;
    *token_len = i - start;
    *tail = romaji.substr(i);
    return true;
  }
  return false;
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
  double confidence = 0.0;
};

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
  FindNumberValue(response, "confidence", &verdict.confidence);
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
  double confidence = 0.0;
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

void DebugLog(const std::string &line) { WriteDebugLog(line); }

void NotifyCommit(const std::string &raw, int mode, const std::string &text) {
  SendCommitRequest(raw, mode, text);
}

void SetModeOrigin(ModeOrigin origin) {
  // IME サーバーは単一スレッドなので状態は 1 つで足りる（thread_local 不要）。
  g_mode_origin = origin;
}

ModeOrigin GetModeOrigin() { return g_mode_origin; }

void ClearModeOrigin() { g_mode_origin = ModeOrigin::kDefault; }

bool MaybeSwitchToEnglish(Composer *composer) {
  // 自分の書き換え（InsertCharacterPreedit）で再入しないための番人。
  static thread_local bool in_hook = false;
  if (in_hook || composer == nullptr) {
    return false;
  }
  if (composer->Empty()) {
    return false;
  }

  const transliteration::TransliterationType mode = composer->GetInputMode();
  const int mode_value = static_cast<int>(mode);
  const std::string romaji = composer->GetRawString();

  // 診断: フックが呼ばれた事実を1回だけサーバーとファイルの両方に残す。
  ReportFirstCall(romaji, mode_value);

  // ハイブリッド: 半角英数モードで日本語判定なら、ひらがなモードへ戻す。
  // 下の「英数モードなら即 return」ガードより前に置く必要があるため、ここで自前で判定を取る。
  //
  // 判定対象は「最後の空白以降のセグメント」だけ。半角英数で「web 」まで打った
  // 後の「web wo」は英字のみのゲートでは判定に一届かず、そのままだと
  // 文頭の英単語の後ろで日本語が打てなくなる（実測: スペース入り raw の判定依頼は 0 件）。
  // セグメントは 2 文字以上なら問い合わせる（助詞「に」など 2 打鍵語の救済）。
  // 対象キーは英字・数字・'-'（長音。de-ta = でーた 等が従来一切判定されなかった
  // 問題への対策・2026-10-05）。
  std::string prefix;
  std::string segment;
  if (mode == transliteration::HALF_ASCII &&
      SplitTrailingSegment(romaji, &prefix, &segment) &&
      segment.size() >= kMinSegmentLength && IsJudgeableKeys(segment)) {
    in_hook = true;
    const Verdict pre_verdict = QueryDecision(segment, mode_value);
    in_hook = false;
    if (pre_verdict.decision == "ja" &&
        pre_verdict.confidence >= kMinConfidenceJa) {
      // 末尾セグメントだけをかなへ組み直す。プレフィックス（"web "）は
      // そのまま半角英数のチャンクとして残す。
      //
      // CharChunk は生成時の transliterator を保持するため、モード切替の
      // あとから書き直したプレフィックスはひらがな変換
      // （LOCAL → HIRAGANA → 全角化）を受けてしまう。プレフィックスを
      // 半角のまま残すには、モード切替のあとからプレフィックスを
      // 書き直さず、組んだまま触らないことが必須。したがって順序は
      //   末尾セグメントだけ消す → モード変更 → SetNewInput → 末尾だけ再投入
      // とし、プレフィックスには触れない。
      //
      // プレフィックス無しのときは従来どおり全消ししてから組み直す
      // （順序: モード変更 → SetNewInput → DeleteRange → 再投入）。
      const size_t total_length = composer->GetLength();
      if (prefix.empty()) {
        composer->SetInputMode(transliteration::HIRAGANA);
        composer->SetNewInput();
        composer->DeleteRange(0, total_length);
      } else {
        // 半角英数モードでは生ローマ字 1 文字 = preedit 1 文字なので、
        // プレフィックスの長さはそのまま.DeleteRange の開始位置になる。
        const size_t prefix_length = prefix.size();
        if (prefix_length < total_length) {
          composer->DeleteRange(prefix_length, total_length - prefix_length);
        }
        composer->SetInputMode(transliteration::HIRAGANA);
        composer->SetNewInput();
      }
      for (std::string::size_type i = 0; i < segment.size(); ++i) {
        composer->InsertCharacter(segment.substr(i, 1));
      }
      SetModeOrigin(ModeOrigin::kAutoSwitchedByJudge);
      WriteDebugLog("switch to hiragana: raw=\"" + romaji + "\" segment=\"" +
                    segment + "\" conf=" +
                    std::to_string(pre_verdict.confidence) + " -> mode=" +
                    std::to_string(static_cast<int>(composer->GetInputMode())) +
                    " len=" + std::to_string(static_cast<int>(composer->GetLength())) +
                    " preedit=\"" + composer->GetStringForPreedit() + "\"\n");
      return true;
    }
  }
  if (mode == transliteration::HALF_ASCII ||
      mode == transliteration::FULL_ASCII ||
      mode == transliteration::HALF_ASCII_UPPER ||
      mode == transliteration::FULL_ASCII_UPPER) {
    return false;  // 既に英数素通し
  }
  if (romaji.empty()) {
    return false;
  }

  // 判定は「英字のみ」でなくても問い合わせる（実機テストで全データを見るため）。
  // 切り替えの適用キーは英字・数字・'-' に限定（'-' は長音入りの英語 e-mail 等を
  // かなモードから素通しへ戻す対称性のためにも使う。2026-10-05）。
  // 略語トークンの生挿入（2026-10-06 追加・選択肢2）。
  // ひらがなモードで「かな部分＋略語＋かな」の形（"korehaAPIdesu"、
  // "UImojissou" など）のとき、略語トークンだけを判定して en なら、かな部分と
  // 後続のかなはひらがなのまま・トークンは生の半角で組み直す。従来は打鍵列
  // まるごとが判定されて平坦化され、日本語部分が消えていた
  // （実測: "korehaAPIdesu" -> これはあぴです）。
  // モードはひらがなのまま維持するため、後続の "desu" 等は従来どおり変換される。
  {
    size_t acronym_prefix_len = 0;
    size_t acronym_token_len = 0;
    std::string acronym_tail;
    if (FindAcronymSplit(romaji, &acronym_prefix_len, &acronym_token_len,
                         &acronym_tail)) {
      const std::string token =
          romaji.substr(acronym_prefix_len, acronym_token_len);
      const std::string kana_part = romaji.substr(0, acronym_prefix_len);
      in_hook = true;
      const Verdict token_verdict = QueryDecision(token, mode_value);
      in_hook = false;
      if (token_verdict.decision == "en" &&
          token_verdict.confidence >= kMinConfidence) {
        const size_t total_length = composer->GetLength();
        composer->SetInputMode(transliteration::HIRAGANA);
        composer->SetNewInput();
        composer->DeleteRange(0, total_length);
        for (std::string::size_type i = 0; i < kana_part.size(); ++i) {
          composer->InsertCharacter(kana_part.substr(i, 1));
        }
        composer->InsertCharacterPreedit(token);
        for (std::string::size_type i = 0; i < acronym_tail.size(); ++i) {
          composer->InsertCharacter(acronym_tail.substr(i, 1));
        }
        SetModeOrigin(ModeOrigin::kAutoSwitchedByJudge);
        WriteDebugLog("switch acronym to half-ascii: raw=\"" + romaji +
                      "\" token=\"" + token + "\" tail=\"" + acronym_tail +
                      "\" conf=" + std::to_string(token_verdict.confidence) +
                      " preedit=\"" + composer->GetStringForPreedit() +
                      "\"\n");
        return true;
      }
    }
  }
  in_hook = true;
  const Verdict verdict = QueryDecision(romaji, mode_value);
  bool applied = false;
if (romaji.size() >= kMinLength && IsJudgeableKeys(romaji) &&
      verdict.decision == "en" && verdict.confidence >= kMinConfidence) {
    const size_t length = composer->GetLength();
    // Fork fix: モードを先に切り替えてから組み直す。順序が逆だと、既に入力済みの
    // 文字が全角のまま残り、「ｖｓｃode」のように先頭だけ全角になる。
    // 日本語切替パス（SetInputMode -> SetNewInput -> DeleteRange -> 再投入）と同じ順序に揃える。
    composer->SetInputMode(transliteration::HALF_ASCII);
    composer->SetNewInput();  // 一時モードは入力限りで失効するため恒久モードで切替
    composer->DeleteRange(0, length);          // かな組成をいったん消し
    composer->InsertCharacterPreedit(romaji);  // 生ローマ字をそのまま入れ直す
    WriteDebugLog("  after switch: mode=" +
                  std::to_string(static_cast<int>(composer->GetInputMode())) + " len=" +
                  std::to_string(static_cast<int>(composer->GetLength())) + " raw=\"" +
                  composer->GetRawString() + "\"\n");
    applied = true;
    WriteDebugLog("switch to half-ascii: raw=\"" + romaji + "\" conf=" +
                  std::to_string(verdict.confidence) + "\n");
  }
  in_hook = false;
  return applied;
}

}  // namespace jev
}  // namespace composer
}  // namespace mozc
