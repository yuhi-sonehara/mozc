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

}  // namespace

int Judge::GetTimeoutMsec() {
#ifdef _WIN32
  return static_cast<int>(kIpcTimeoutMsec);
#else
  return 0;
#endif  // _WIN32
}

bool Judge::IsEnglish(const std::string &romaji) {
#ifdef _WIN32
  const std::string request =
      std::string("{\"keys\":\"") + JsonEscape(romaji) +
      "\",\"source\":\"mozc\",\"version\":1}";
  const std::string response = Transact(request);
  if (response.empty()) {
    return false;  // サーバー不通・無応答 → 現状動作のまま
  }
  std::string decision;
  double confidence = 0.0;
  if (!FindStringValue(response, "decision", &decision)) {
    return false;
  }
  FindNumberValue(response, "confidence", &confidence);
  return decision == "en" && confidence >= kMinConfidence;
#else
  (void)romaji;
  return false;
#endif  // _WIN32
}

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
  if (mode == transliteration::HALF_ASCII ||
      mode == transliteration::FULL_ASCII ||
      mode == transliteration::HALF_ASCII_UPPER ||
      mode == transliteration::FULL_ASCII_UPPER) {
    return false;  // 既に英数素通し
  }

  const std::string romaji = composer->GetRawString();
  if (romaji.size() < kMinLength || !IsAsciiLetters(romaji)) {
    return false;
  }

  in_hook = true;
  const bool english = Judge::IsEnglish(romaji);
  if (english) {
    const size_t length = composer->GetLength();
    composer->DeleteRange(0, length);       // かな組成をいったん消し
    composer->InsertCharacterPreedit(romaji);  // 生ローマ字をそのまま入れ直す
    composer->SetTemporaryInputMode(transliteration::HALF_ASCII);
  }
  in_hook = false;
  return english;
}

}  // namespace jev
}  // namespace composer
}  // namespace mozc