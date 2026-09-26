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
constexpr double kMinConfidence = 0.85;
// これ未満の長さでは問い合わせない（誤爆を避ける）。
constexpr size_t kMinLength = 3;
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

// 生ローマ字列が英字のみかどうか。
bool IsAsciiLetters(const std::string &text) {
  if (text.empty()) {
    return false;
  }
  for (const char c : text) {
    const bool is_alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (!is_alpha) {
      return false;
    }
  }
  return true;
}

#ifdef _WIN32
// 診断用: フックが呼ばれた事実をファイルにも残す。
// パイプ不通（サーバーに届かない）とフック未発火を区別するために使う。
void WriteDebugLog(const std::string &line) {
  // 診断専用: 低整合性プロセスでも書ける場所を含め、複数へ出力する。
  static bool banner_done = false;
  const std::string banner = "=== jev_judge debug log (instrumented build) ===\n";
  const std::string body = banner_done ? line : (banner + line);
  wchar_t tmp[MAX_PATH] = {};
  if (::GetTempPathW(MAX_PATH, tmp) > 0) {
    AppendLogLine(JoinPath(tmp, L"jev_judge_hook.log"), body);
  }
  const wchar_t *const kEnvNames[] = {L"USERPROFILE", L"PUBLIC", L"ProgramData"};
  for (int e = 0; e < 3; ++e) {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = ::GetEnvironmentVariableW(kEnvNames[e], buf, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) continue;
    AppendLogLine(JoinPath(buf, L"jev_judge_hook.log"), body);
  }
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
#endif  // _WIN32

}  // namespace

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
  if (mode == transliteration::HALF_ASCII && romaji.size() >= kMinLength &&
      IsAsciiLetters(romaji)) {
    in_hook = true;
    const Verdict pre_verdict = QueryDecision(romaji, mode_value);
    in_hook = false;
    if (pre_verdict.decision == "ja" && pre_verdict.confidence >= kMinConfidence) {
      const size_t length = composer->GetLength();
      composer->SetInputMode(transliteration::HIRAGANA);
      composer->SetNewInput();
      // 組成を「かな」で組み直す（生ローマ字を削除して1文字ずつ再投入 → かなへ変換される）
      composer->DeleteRange(0, length);
      for (std::string::size_type i = 0; i < romaji.size(); ++i) {
        composer->InsertCharacter(romaji.substr(i, 1));
      }
      WriteDebugLog("switch to hiragana: raw=\"" + romaji + "\" conf=" +
                    std::to_string(pre_verdict.confidence) + " -> mode=" +
                    std::to_string(static_cast<int>(composer->GetInputMode())) +
                    " len=" + std::to_string(static_cast<int>(composer->GetLength())) +
                    " rebuilt=\"" + composer->GetRawString() + "\"\n");
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
  // 切り替えの適用条件は従来どおり厳しく保つ。
  in_hook = true;
  const Verdict verdict = QueryDecision(romaji, mode_value);
  bool applied = false;
if (romaji.size() >= kMinLength && IsAsciiLetters(romaji) &&
      verdict.decision == "en" && verdict.confidence >= kMinConfidence) {
    const size_t length = composer->GetLength();
    composer->DeleteRange(0, length);          // かな組成をいったん消し
    composer->InsertCharacterPreedit(romaji);  // 生ローマ字をそのまま入れ直す
    composer->SetInputMode(transliteration::HALF_ASCII);
      composer->SetNewInput();  // 一時モードは入力限りで失効するため恒久モードで切替
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
