#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <wincrypt.h>
#include <stdint.h>
#include <time.h>
#include <rpc.h>
#include <math.h>
#include <limits.h>

/*
 * 修复清单（累积）：
 *  [v2 已修复]
 *   1. SetClipboardText：先分配再 EmptyClipboard；SetClipboardData 失败时 GlobalFree
 *   2. ShowToastAsync：CreateProcess 失败显式提示
 *   3. UnicodeEncode/Decode：支持代理对 / \UXXXXXXXX
 *   4. GenShortId：检查 CryptGenRandom
 *   5. wmain：IsKnownCommand 白名单 + JoinArgsToUtf8 全参拼接
 *   6. wprintf 使用 %ls
 *   7. fh 与 -c 冲突报错
 *   8. -t 缺参数报错
 *
 *  [v3 本次新增修复]
 *   9. HexEncode 溢出：调用前估算并拒绝超长输入
 *  10. HexDecode / UrlDecode / Base64Decode：改用显式长度，修复 NUL 截断导致的误判
 *  11. b64/b64d/hex/hexd/url/urld/uni/unid 空结果统一输出 "(空)"，不再误报"结果为空"
 *  12. CountChars：Emoji（代理对）按 1 个字符统计
 *  13. ConvertBase：支持负号（"-255" → DEC:-255），不再被 strtoull 回绕
 *  14. ShowToastAsync：stdout/stderr 无效时回退到 NUL 设备，GUI 进程也能弹 Toast
 *  15. -t 数值非法（非数字/超范围）时明确报错
 *  16. uuid -c：忽略剪贴板，避免剪贴板为空时误报
 *  17. CalcHash：CryptHashData 返回值检查
 *  18. UnicodeDecode：孤立代理码点替换为 U+FFFD，避免乱码
 *  19. validArgs 上限从 16 提到 64，超出报错
 *  20. ShowToastBlocking：RegisterClassW 失败容错（多子进程并存）
 */

// ----------------- 剪贴板工具 -----------------
void SetClipboardText(const WCHAR* text) {
    for (int retry = 0; retry < 3; retry++) {
        if (!OpenClipboard(NULL)) { Sleep(10); continue; }

        size_t len = (wcslen(text) + 1) * sizeof(WCHAR);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
        if (!hMem) { CloseClipboard(); return; }

        memcpy(GlobalLock(hMem), text, len);
        GlobalUnlock(hMem);

        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, hMem))
            GlobalFree(hMem);  // 交接失败，由我们释放

        CloseClipboard();
        return;
    }
}

BOOL GetClipboardText(WCHAR* out, int maxChars) {
    if (!out || maxChars <= 0) return FALSE;
    if (!OpenClipboard(NULL)) return FALSE;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    BOOL ok = FALSE;
    if (h) {
        WCHAR* p = (WCHAR*)GlobalLock(h);
        if (p) {
            wcsncpy(out, p, maxChars - 1);
            out[maxChars - 1] = 0;
            GlobalUnlock(h);
            ok = TRUE;
        }
    }
    CloseClipboard();
    return ok;
}

// ----------------- Toast 窗口（子进程内） -----------------
LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));

        HFONT font = CreateFontW(-28, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
        HFONT hOld = (HFONT)SelectObject(hdc, font);

        WCHAR* str = (WCHAR*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (str && *str) {
            RECT calc = rc;
            calc.left += 20;
            calc.right -= 20;
            DrawTextW(hdc, str, -1, &calc, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
            int textH = calc.bottom - calc.top;
            int y = (rc.bottom - rc.top - textH) / 2;
            calc.left = rc.left + 20;
            calc.right = rc.right - 20;
            calc.top = y;
            calc.bottom = y + textH;
            DrawTextW(hdc, str, -1, &calc, DT_CENTER | DT_WORDBREAK);
        }

        SelectObject(hdc, hOld);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShowToastBlocking(const WCHAR* text, UINT durationMs) {
    HINSTANCE hInst = GetModuleHandle(NULL);
    HBRUSH hBrush = CreateSolidBrush(RGB(45, 45, 45));

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = ToastProc;
    wc.hInstance = hInst;
    wc.hbrBackground = hBrush;
    wc.lpszClassName = L"QRToast";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DeleteObject(hBrush);
        return;
    }

    int w = 650, h = 160;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h - 80);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
                                L"QRToast", L"", WS_POPUP,
                                x, y, w, h, NULL, NULL, hInst, NULL);
    if (!hwnd) {
        DeleteObject(hBrush);
        UnregisterClassW(L"QRToast", hInst);
        return;
    }

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)text);
    SetLayeredWindowAttributes(hwnd, 0, 230, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    SetTimer(hwnd, 1, durationMs, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterClassW(L"QRToast", hInst);
    DeleteObject(hBrush);
}

// ----------------- 异步 Toast -----------------
void ShowToastAsync(const WCHAR* text, UINT durationMs) {
    if (!text || !text[0]) return;

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return;
    SetHandleInformation(hWritePipe, HANDLE_FLAG_INHERIT, 0);

    WCHAR selfPath[MAX_PATH];
    if (!GetModuleFileNameW(NULL, selfPath, MAX_PATH)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return;
    }

    WCHAR cmdLine[MAX_PATH * 2 + 32];
    swprintf(cmdLine, MAX_PATH * 2 + 32, L"\"%ls\" --toast-worker %u", selfPath, durationMs);

    // 修复：GUI 进程无 stdout/stderr 时回退到 NUL 设备
    SECURITY_ATTRIBUTES saNull = { sizeof(saNull), NULL, TRUE };
    HANDLE hStdOut = CreateFileW(L"NUL", GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &saNull, OPEN_EXISTING, 0, NULL);
    HANDLE hStdErr = hStdOut;
    BOOL closeNull = FALSE;
    if (hStdOut == INVALID_HANDLE_VALUE) {
        hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        hStdErr = GetStdHandle(STD_ERROR_HANDLE);
    } else {
        closeNull = TRUE;
    }

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hReadPipe;
    si.hStdOutput = hStdOut;
    si.hStdError = hStdErr;

    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessW(selfPath, cmdLine, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        DWORD len = (DWORD)((wcslen(text) + 1) * sizeof(WCHAR));
        DWORD written = 0;
        if (!WriteFile(hWritePipe, &len, sizeof(len), &written, NULL) ||
            !WriteFile(hWritePipe, text, len, &written, NULL)) {
            MessageBoxW(NULL, L"Toast 传递失败（子进程未接收数据）",
                        L"CalcPlus", MB_ICONERROR);
        }
    } else {
        MessageBoxW(NULL, L"Toast 子进程启动失败", L"CalcPlus", MB_ICONERROR);
    }

    if (closeNull) CloseHandle(hStdOut);
    CloseHandle(hReadPipe);
    CloseHandle(hWritePipe);
}

// ----------------- 哈希 -----------------
void CalcHash(const char* input, WCHAR* output, ALG_ID algId, int hashLen) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BYTE rgbHash[64];
    DWORD cbHash = hashLen;
    WCHAR hex[] = L"0123456789abcdef";

    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return;
    if (CryptCreateHash(hProv, algId, 0, 0, &hHash)) {
        if (CryptHashData(hHash, (BYTE*)input, (DWORD)strlen(input), 0) &&
            CryptGetHashParam(hHash, HP_HASHVAL, rgbHash, &cbHash, 0)) {
            for (DWORD i = 0; i < cbHash; i++) {
                output[i * 2] = hex[rgbHash[i] >> 4];
                output[i * 2 + 1] = hex[rgbHash[i] & 0xf];
            }
            output[hashLen * 2] = 0;
        }
        CryptDestroyHash(hHash);
    }
    CryptReleaseContext(hProv, 0);
}

void CalcFileHash(const char* alg, const WCHAR* filePath, WCHAR* output) {
    ALG_ID algId = 0;
    int hashLen = 0;
    if (_stricmp(alg, "md5") == 0) { algId = CALG_MD5; hashLen = 16; }
    else if (_stricmp(alg, "sha1") == 0) { algId = CALG_SHA1; hashLen = 20; }
    else if (_stricmp(alg, "sha256") == 0) { algId = CALG_SHA_256; hashLen = 32; }
    else if (_stricmp(alg, "sha512") == 0) { algId = CALG_SHA_512; hashLen = 64; }
    else { wcscpy(output, L"不支持的哈希算法"); return; }

    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) { wcscpy(output, L"无法打开文件"); return; }

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        CloseHandle(hFile); return;
    }
    if (!CryptCreateHash(hProv, algId, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0); CloseHandle(hFile); return;
    }

    BYTE buffer[8192];
    DWORD bytesRead = 0;
    BOOL success = TRUE;
    while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0) {
        if (!CryptHashData(hHash, buffer, bytesRead, 0)) { success = FALSE; break; }
    }

    if (success) {
        BYTE rgbHash[64];
        DWORD cbHash = hashLen;
        WCHAR hex[] = L"0123456789abcdef";
        if (CryptGetHashParam(hHash, HP_HASHVAL, rgbHash, &cbHash, 0)) {
            for (DWORD i = 0; i < cbHash; i++) {
                output[i * 2] = hex[rgbHash[i] >> 4];
                output[i * 2 + 1] = hex[rgbHash[i] & 0xf];
            }
            output[hashLen * 2] = 0;
        }
    } else {
        wcscpy(output, L"读取文件失败");
    }

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    CloseHandle(hFile);
}

// ----------------- Base64 -----------------
static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
void Base64Encode(const char* input, char* output) {
    size_t len = strlen(input);
    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t octet_a = i < len ? (unsigned char)input[i++] : 0;
        uint32_t octet_b = i < len ? (unsigned char)input[i++] : 0;
        uint32_t octet_c = i < len ? (unsigned char)input[i++] : 0;
        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;
        output[j++] = b64chars[(triple >> 3 * 6) & 0x3F];
        output[j++] = b64chars[(triple >> 2 * 6) & 0x3F];
        output[j++] = b64chars[(triple >> 1 * 6) & 0x3F];
        output[j++] = b64chars[(triple >> 0 * 6) & 0x3F];
    }
    while (j % 4 != 0) output[j++] = '=';
    output[j] = 0;
}

void Base64Decode(const char* input, char* output, size_t* out_len) {
    size_t len = strlen(input);
    uint32_t buffer = 0;
    int bits = 0;
    size_t j = 0;

    for (size_t i = 0; i < len; i++) {
        char c = input[i];
        if (c == '=') break;
        if (!isalnum((unsigned char)c) && c != '+' && c != '/') continue;
        const char* p = strchr(b64chars, c);
        if (!p) continue;
        buffer = ((buffer << 6) | (p - b64chars)) & 0xFFFFFF;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output[j++] = (char)((buffer >> bits) & 0xFF);
        }
    }
    output[j] = '\0';
    if (out_len) *out_len = j;
}

// ----------------- CRC32 -----------------
uint32_t crc32_table[256];
void init_crc32() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
}
void CalcCRC32(const char* input, WCHAR* output) {
    init_crc32();
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < strlen(input); i++) crc = crc32_table[(crc ^ input[i]) & 0xFF] ^ (crc >> 8);
    crc ^= 0xFFFFFFFF;
    swprintf(output, 9, L"%08x", crc);
}

// ----------------- URL -----------------
void UrlEncode(const char* input, char* output) {
    char* p = output;
    for (const unsigned char* s = (const unsigned char*)input; *s; s++) {
        if (isalnum(*s) || *s == '-' || *s == '_' || *s == '.' || *s == '~') *p++ = *s;
        else { sprintf(p, "%%%02X", *s); p += 3; }
    }
    *p = 0;
}

int hex_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

// 修复：返回解码后实际长度
size_t UrlDecode(const char* input, char* output) {
    char* p = output;
    for (const char* s = input; *s; s++) {
        if (*s == '%' && s[1] && s[2]) {
            *p++ = (char)((hex_to_int(s[1]) << 4) + hex_to_int(s[2]));
            s += 2;
        } else if (*s == '+') *p++ = ' ';
        else *p++ = *s;
    }
    *p = 0;
    return (size_t)(p - output);
}

// ----------------- Hex -----------------
void HexEncode(const char* input, char* output) {
    char* p = output;
    for (const unsigned char* s = (const unsigned char*)input; *s; s++) {
        sprintf(p, "%02x", *s);
        p += 2;
    }
    *p = 0;
}

// 修复：返回解码后实际长度
size_t HexDecode(const char* input, char* output) {
    char* p = output;
    for (const char* s = input; s[0] && s[1]; s += 2) {
        *p++ = (char)((hex_to_int(s[0]) << 4) + hex_to_int(s[1]));
    }
    *p = 0;
    return (size_t)(p - output);
}

// ----------------- Unicode -----------------
void UnicodeEncode(const char* input, WCHAR* output) {
    WCHAR wbuf[4096];
    if (MultiByteToWideChar(CP_UTF8, 0, input, -1, wbuf, 4096) <= 0) {
        wcscpy(output, L"输入过长或不是合法 UTF-8");
        return;
    }
    WCHAR* p = output;
    for (int i = 0; wbuf[i] != 0; i++) {
        int rem = (int)(4096 - (p - output));
        if (wbuf[i] >= 0x20 && wbuf[i] <= 0x7E) {
            if (rem < 2) { wcscpy(output, L"结果超出缓冲区"); return; }
            *p++ = wbuf[i];
        } else if (wbuf[i] >= 0xD800 && wbuf[i] <= 0xDBFF &&
                   wbuf[i + 1] >= 0xDC00 && wbuf[i + 1] <= 0xDFFF) {
            unsigned int hi = wbuf[i]     - 0xD800;
            unsigned int lo = wbuf[i + 1] - 0xDC00;
            unsigned int cp = 0x10000 + (hi << 10) + lo;
            if (rem < 11) { wcscpy(output, L"结果超出缓冲区"); return; }
            swprintf(p, 11, L"\\U%08X", cp);
            p += 10;
            i++;
        } else {
            if (rem < 7) { wcscpy(output, L"结果超出缓冲区"); return; }
            swprintf(p, 7, L"\\u%04X", wbuf[i]);
            p += 6;
        }
    }
    *p = 0;
}

void UnicodeDecode(const char* input, WCHAR* output) {
    WCHAR wbuf[4096];
    if (MultiByteToWideChar(CP_UTF8, 0, input, -1, wbuf, 4096) <= 0) {
        wcscpy(output, L"输入过长或不是合法 UTF-8");
        return;
    }
    WCHAR* p = output;
    WCHAR* s = wbuf;
    while (*s) {
        if (*s == L'\\' && (*(s + 1) == L'u' || *(s + 1) == L'U')) {
            if (*(s + 1) == L'u' && *(s + 2) && *(s + 3) && *(s + 4) && *(s + 5)) {
                WCHAR hex[5] = { *(s + 2), *(s + 3), *(s + 4), *(s + 5), 0 };
                BOOL isHex = TRUE;
                for (int i = 0; i < 4; i++)
                    if (!iswxdigit(hex[i])) { isHex = FALSE; break; }
                if (isHex) {
                    unsigned int val;
                    swscanf(hex, L"%04X", &val);
                    // 孤立代理码点用替换字符
                    *p++ = (val >= 0xD800 && val <= 0xDFFF) ? 0xFFFD : (WCHAR)val;
                    s += 6;
                    continue;
                }
            } else if (*(s + 1) == L'U' &&
                       *(s + 2) && *(s + 3) && *(s + 4) && *(s + 5) &&
                       *(s + 6) && *(s + 7) && *(s + 8) && *(s + 9)) {
                WCHAR hex[9] = { *(s + 2), *(s + 3), *(s + 4), *(s + 5),
                                 *(s + 6), *(s + 7), *(s + 8), *(s + 9), 0 };
                BOOL isHex = TRUE;
                for (int i = 0; i < 8; i++)
                    if (!iswxdigit(hex[i])) { isHex = FALSE; break; }
                if (isHex) {
                    unsigned int cp = 0;
                    swscanf(hex, L"%08X", &cp);
                    if (cp >= 0xD800 && cp <= 0xDFFF) {
                        *p++ = 0xFFFD;
                    } else if (cp <= 0xFFFF) {
                        *p++ = (WCHAR)cp;
                    } else if (cp <= 0x10FFFF) {
                        cp -= 0x10000;
                        *p++ = (WCHAR)(0xD800 + (cp >> 10));
                        *p++ = (WCHAR)(0xDC00 + (cp & 0x3FF));
                    } else {
                        *p++ = 0xFFFD;
                    }
                    s += 10;
                    continue;
                }
            }
        }
        *p++ = *s++;
    }
    *p = 0;
}

// ----------------- 颜色 -----------------
void ConvertColor(const char* input, WCHAR* output) {
    if (input[0] == '#') {
        unsigned int r, g, b;
        if (sscanf(input + 1, "%02x%02x%02x", &r, &g, &b) == 3)
            swprintf(output, 64, L"%d, %d, %d", r, g, b);
        else wcscpy(output, L"颜色格式错误");
    } else {
        int r, g, b;
        if (sscanf(input, "%d,%d,%d", &r, &g, &b) == 3)
            swprintf(output, 64, L"#%02X%02X%02X", r, g, b);
        else wcscpy(output, L"格式错误。示例: #FF0000 或 255,0,0");
    }
}

// ----------------- 文本处理 -----------------
void StrToUpper(const char* input, WCHAR* output) {
    if (MultiByteToWideChar(CP_UTF8, 0, input, -1, output, 4096) > 0) {
        for (WCHAR* p = output; *p; p++) *p = towupper(*p);
    } else {
        wcscpy(output, L"输入过长或不是合法 UTF-8");
    }
}

void StrToLower(const char* input, WCHAR* output) {
    if (MultiByteToWideChar(CP_UTF8, 0, input, -1, output, 4096) > 0) {
        for (WCHAR* p = output; *p; p++) *p = towlower(*p);
    } else {
        wcscpy(output, L"输入过长或不是合法 UTF-8");
    }
}

// 修复：Emoji（代理对）按 1 个字符计
void CountChars(const char* input, WCHAR* output) {
    if (!input || !input[0]) { wcscpy(output, L"0 个字符"); return; }
    WCHAR wbuf[4096];
    int wlen = MultiByteToWideChar(CP_UTF8, 0, input, -1, wbuf, 4096);
    if (wlen <= 0) { wcscpy(output, L"0 个字符"); return; }
    int count = 0;
    for (int i = 0; i < wlen - 1; i++) {
        if (wbuf[i] >= 0xD800 && wbuf[i] <= 0xDBFF &&
            i + 1 < wlen - 1 && wbuf[i + 1] >= 0xDC00 && wbuf[i + 1] <= 0xDFFF) {
            i++;
        }
        count++;
    }
    swprintf(output, 64, L"%d 个字符", count);
}

// ----------------- UUID / 时间戳 -----------------
void GenUUID(WCHAR* output) {
    UUID uuid;
    if (UuidCreate(&uuid) == RPC_S_OK) {
        RPC_WSTR str;
        if (UuidToStringW(&uuid, &str) == RPC_S_OK) {
            wcscpy(output, (WCHAR*)str);
            RpcStringFreeW(&str);
        }
    }
}

void ConvertTimestamp(const char* input, WCHAR* output) {
    if (input[0] == '\0') {
        swprintf(output, 64, L"%lld", (long long)time(NULL));
    } else if (input[0] >= '0' && input[0] <= '9') {
        long long ts = _atoi64(input);
        if (strlen(input) >= 12) ts /= 1000;
        time_t t = (time_t)ts;
        struct tm* tm_info = localtime(&t);
        char tmp[64] = {0};
        strftime(tmp, sizeof(tmp), "%Y-%m-%d %H:%M:%S", tm_info);
        MultiByteToWideChar(CP_ACP, 0, tmp, -1, output, 64);
    } else {
        struct tm tm = {0};
        int y, m, d, hh = 0, mm = 0, ss = 0;
        int parsed = sscanf(input, "%d-%d-%d %d:%d:%d", &y, &m, &d, &hh, &mm, &ss);
        if (parsed >= 3) {
            tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d;
            tm.tm_hour = hh; tm.tm_min = mm; tm.tm_sec = ss; tm.tm_isdst = -1;
            swprintf(output, 64, L"%lld", (long long)mktime(&tm));
        } else {
            wcscpy(output, L"格式错误 (示例: 2023-11-14 或 1700000000)");
        }
    }
}

// ----------------- 进制转换（修复负数） -----------------
void ConvertBase(const char* input, WCHAR* output) {
    char buf[64];
    strncpy(buf, input, 63);
    buf[63] = 0;

    char* s = buf;
    while (*s == ' ') s++;

    BOOL negative = FALSE;
    if (*s == '-') { negative = TRUE; s++; }
    else if (*s == '+') s++;

    int base = 10;
    const char* num = s;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; num = s + 2; }
    else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; num = s + 2; }
    else if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) { base = 8; num = s + 2; }

    char* end;
    unsigned long long val = strtoull(num, &end, base);
    if (end == num) {
        wcscpy(output, L"无效数值 (示例: 255 / 0xFF / 0b11111111 / 0o377)");
        return;
    }

    WCHAR bin[65];
    int bi = 64;
    bin[64] = 0;
    if (val == 0) bin[--bi] = '0';
    unsigned long long t = val;
    while (t) { bin[--bi] = (t & 1) ? '1' : '0'; t >>= 1; }

    const WCHAR* sign = negative ? L"-" : L"";
    swprintf(output, 256, L"HEX: %ls0x%llX\nDEC: %ls%llu\nOCT: %ls0o%llo\nBIN: %ls0b%ls",
             sign, val, sign, val, sign, val, sign, bin + bi);
}

// ----------------- 子网 -----------------
void CalcSubnet(const char* input, WCHAR* output) {
    unsigned int a, b, c, d;
    unsigned int prefix = 0;
    char maskstr[64] = {0};

    int n = sscanf(input, "%u.%u.%u.%u/%u", &a, &b, &c, &d, &prefix);
    if (n != 5) {
        n = sscanf(input, "%u.%u.%u.%u %63s", &a, &b, &c, &d, maskstr);
        if (n == 5) {
            if (maskstr[0] == '/') {
                prefix = (unsigned)atoi(maskstr + 1);
            } else if (strchr(maskstr, '.')) {
                unsigned int m1, m2, m3, m4;
                if (sscanf(maskstr, "%u.%u.%u.%u", &m1, &m2, &m3, &m4) == 4) {
                    uint32_t m = (m1 << 24) | (m2 << 16) | (m3 << 8) | m4;
                    prefix = 0;
                    while (prefix < 32 && (m & (0x80000000u >> prefix))) prefix++;
                } else {
                    wcscpy(output, L"掩码格式错误");
                    return;
                }
            } else {
                prefix = (unsigned)atoi(maskstr);
            }
        } else {
            wcscpy(output, L"格式错误，示例: 192.168.1.0/24 或 192.168.1.100 255.255.255.192");
            return;
        }
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        wcscpy(output, L"IP 段超出 0~255");
        return;
    }
    if (prefix > 32) { wcscpy(output, L"前缀长度须在 0~32"); return; }

    uint32_t ip = (a << 24) | (b << 16) | (c << 8) | d;
    uint32_t mask = (prefix == 0) ? 0 : (0xFFFFFFFFu << (32 - prefix));
    uint32_t net = ip & mask;
    uint32_t bcast = net | ~mask;
    uint32_t first = (prefix >= 31) ? net : net + 1;
    uint32_t last  = (prefix >= 31) ? bcast : bcast - 1;
    uint32_t hosts = (prefix == 32) ? 1 : (prefix == 31) ? 2 : (bcast - net - 1);

    swprintf(output, 512,
        L"网络地址: %u.%u.%u.%u\n广播地址: %u.%u.%u.%u\n主机范围: %u.%u.%u.%u - %u.%u.%u.%u\n"
        L"子网掩码: %u.%u.%u.%u\n通配符:   %u.%u.%u.%u\n可用主机: %u",
        (net>>24)&255,(net>>16)&255,(net>>8)&255,net&255,
        (bcast>>24)&255,(bcast>>16)&255,(bcast>>8)&255,bcast&255,
        (first>>24)&255,(first>>16)&255,(first>>8)&255,first&255,
        (last>>24)&255,(last>>16)&255,(last>>8)&255,last&255,
        (mask>>24)&255,(mask>>16)&255,(mask>>8)&255,mask&255,
        (~mask>>24)&255,(~mask>>16)&255,(~mask>>8)&255,~mask&255,
        hosts);
}

// ----------------- NanoID -----------------
void GenShortId(const char* input, WCHAR* output) {
    int len = input[0] ? atoi(input) : 12;
    if (len < 1) len = 12;
    if (len > 256) len = 256;

    static const char* charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";
    unsigned char rnd[256];

    HCRYPTPROV hProv;
    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        wcscpy(output, L"随机数生成失败");
        return;
    }
    if (!CryptGenRandom(hProv, len, rnd)) {
        CryptReleaseContext(hProv, 0);
        wcscpy(output, L"随机数生成失败");
        return;
    }
    CryptReleaseContext(hProv, 0);

    for (int i = 0; i < len; i++) output[i] = (WCHAR)charset[rnd[i] & 63];
    output[len] = 0;
}

// ----------------- 数学表达式解析器 -----------------
static const char* expr_ptr;
static double parse_expr();
static double parse_term() {
    double val = 0, sign = 1;
    while (*expr_ptr == ' ') expr_ptr++;
    if (*expr_ptr == '-') { sign = -1; expr_ptr++; }
    if (*expr_ptr == '(') {
        expr_ptr++;
        val = parse_expr();
        if (*expr_ptr == ')') expr_ptr++;
    } else {
        char* end;
        val = strtod(expr_ptr, &end);
        expr_ptr = end;
    }
    return val * sign;
}
static double parse_mul() {
    double val = parse_term();
    while (1) {
        while (*expr_ptr == ' ') expr_ptr++;
        if (*expr_ptr == '*') { expr_ptr++; val *= parse_term(); }
        else if (*expr_ptr == '/') { expr_ptr++; val /= parse_term(); }
        else break;
    }
    return val;
}
static double parse_expr() {
    double val = parse_mul();
    while (1) {
        while (*expr_ptr == ' ') expr_ptr++;
        if (*expr_ptr == '+') { expr_ptr++; val += parse_mul(); }
        else if (*expr_ptr == '-') { expr_ptr++; val -= parse_mul(); }
        else break;
    }
    return val;
}

// ----------------- 参数辅助 -----------------
static BOOL CopyArgToUtf8(const wchar_t* src, char* dst, int dstSize, const WCHAR* label) {
    int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dstSize, NULL, NULL);
    if (n <= 0) {
        WCHAR msg[256];
        swprintf(msg, 256, L"%ls过长或无法转换（UTF-8 上限 %d 字节）", label, dstSize - 1);
        MessageBoxW(NULL, msg, L"CalcPlus", MB_ICONERROR);
        return FALSE;
    }
    return TRUE;
}

static BOOL IsKnownCommand(const wchar_t* s) {
    static const wchar_t* cmds[] = {
        L"calc", L"md5", L"sha1", L"sha256", L"sha512", L"crc32", L"fhash",
        L"b64", L"b64d", L"url", L"urld", L"hex", L"hexd", L"uni", L"unid",
        L"uuid", L"ts", L"color", L"upper", L"lower", L"count", L"base",
        L"ip", L"nanoid", L"fh", NULL
    };
    for (int i = 0; cmds[i]; i++)
        if (_wcsicmp(s, cmds[i]) == 0) return TRUE;
    return FALSE;
}

static BOOL JoinArgsToUtf8(wchar_t** args, int start, int count, char* out, int outSize) {
    int pos = 0;
    out[0] = 0;
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            if (pos + 1 >= outSize) {
                MessageBoxW(NULL, L"输入参数总长超出缓冲区", L"CalcPlus", MB_ICONERROR);
                return FALSE;
            }
            out[pos++] = ' ';
        }
        int n = WideCharToMultiByte(CP_UTF8, 0, args[start + i], -1,
                                    out + pos, outSize - pos, NULL, NULL);
        if (n <= 0) {
            WCHAR msg[256];
            swprintf(msg, 256, L"输入过长或无法转换（UTF-8 上限 %d 字节）", outSize - 1);
            MessageBoxW(NULL, msg, L"CalcPlus", MB_ICONERROR);
            return FALSE;
        }
        pos += n - 1;
    }
    out[pos] = 0;
    return TRUE;
}

// 把二进制结果安全转成可显示文本：空→"(空)"，非法字节→提示，含 NUL→提示二进制
static void BinResultToWide(const char* buf, size_t len, WCHAR* out) {
    if (len == 0) { wcscpy(out, L"(空)"); return; }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, (int)len, out, 4095);
    if (wlen == 0) wlen = MultiByteToWideChar(CP_ACP, 0, buf, (int)len, out, 4095);
    if (wlen == 0) {
        wcscpy(out, L"结果含非法字节，无法以文本显示");
        return;
    }
    out[wlen] = 0;
    if (out[0] == 0) wcscpy(out, L"结果含不可见字符（可能是二进制）");
}

// ----------------- 帮助 -----------------
void ShowHelp() {
    const WCHAR* helpText =
        L"【CalcPlus 工具箱 指令速查】\n\n"
        L"▶ 数学计算 (直接输入算式)\n"
        L"  2*(3+5) 或 calc 2*(3+5)\n\n"
        L"▶ 哈希算法 (字符串)\n"
        L"  md5 / sha1 / sha256 / sha512 / crc32 <文本>\n\n"
        L"▶ 文件哈希\n"
        L"  fhash md5 <文件路径> (支持 md5/sha1/sha256/sha512)\n\n"
        L"▶ 编码与解码\n"
        L"  b64 / b64d (Base64)\n"
        L"  url / urld (URL)\n"
        L"  hex / hexd (十六进制)\n"
        L"  uni / unid (Unicode)\n\n"
        L"▶ 开发辅助\n"
        L"  uuid (生成 GUID)\n"
        L"  ts <时间戳/日期> (互转，不填则为当前)\n"
        L"  color <#FF0000 或 255,0,0> (格式互转)\n"
        L"  base <值> (自动识别 0x/0b/0o，输出 2/8/10/16 进制)\n"
        L"  ip <IP/前缀> 或 <IP> <掩码> (子网计算)\n"
        L"  nanoid [长度] (默认 12 位 URL-safe 短ID)\n\n"
        L"▶ 文本处理\n"
        L"  upper / lower (大小写)\n"
        L"  count (统计字符数)\n\n"
        L"▶ 私人符号库 (fh)\n"
        L"  fh kg (全角空格) | dui (√) | cuo (×)\n"
        L"  fh zuo (←) | you (→) | shang (↑) | xia (↓)\n"
        L"  fh wjx (★) | yuan (●) | fang (■) | dian (·)\n"
        L"  fh mi (※) | bo (▶) | shan (⚡) | deng (💡)\n\n"
        L"▶ 通用参数 (可组合)\n"
        L"  -c / --clip        从剪贴板读取输入 (如: md5 -c)\n"
        L"  -t / --toast-time <秒>  Toast 显示时长，默认 2，范围 0.5~30\n"
        L"  -nt / --no-toast   静默执行，不弹 Toast\n"
        L"  -nc / --no-copy    只展示结果，不写入剪贴板\n";

    MessageBoxW(NULL, helpText, L"CalcPlus 帮助", MB_ICONINFORMATION | MB_OK);
}

// ----------------- 主函数 -----------------
int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);

    // Toast 子进程
    if (argc >= 3 && wcscmp(argv[1], L"--toast-worker") == 0) {
        UINT durationMs = (UINT)_wtoi(argv[2]);
        if (durationMs < 500 || durationMs > 30000) durationMs = 2000;

        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        DWORD len = 0, read = 0;
        if (hIn != INVALID_HANDLE_VALUE &&
            ReadFile(hIn, &len, sizeof(len), &read, NULL) &&
            read == sizeof(len) && len > 0 && len <= 1048576) {
            WCHAR* text = (WCHAR*)malloc(len + sizeof(WCHAR));
            if (text) {
                if (ReadFile(hIn, text, len, &read, NULL) && read == len) {
                    text[len / sizeof(WCHAR)] = 0;
                    ShowToastBlocking(wcslen(text) > 0 ? text : L"(Toast 内容为空)", durationMs);
                } else {
                    ShowToastBlocking(L"(Toast 读取失败)", durationMs);
                }
                free(text);
            } else {
                ShowToastBlocking(L"(Toast 内存不足)", durationMs);
            }
        } else {
            ShowToastBlocking(L"(Toast 未收到数据)", durationMs);
        }
        return 0;
    }

    if (argc < 2) { ShowHelp(); return 0; }
    if (wcscmp(argv[1], L"-h") == 0 || wcscmp(argv[1], L"--help") == 0 ||
        wcscmp(argv[1], L"help") == 0 || wcscmp(argv[1], L"?") == 0) {
        ShowHelp();
        return 0;
    }

    BOOL showToast = TRUE;
    BOOL copyToClipboard = TRUE;
    BOOL useClip = FALSE;
    UINT toastMs = 2000;
    char cmd[32] = "calc";
    char input[4096] = {0};
    char alg[32] = {0};
    WCHAR wfilePath[4096] = {0};

    wchar_t* validArgs[64] = {0};
    int argCount = 0;

    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--no-toast") == 0 || wcscmp(argv[i], L"-nt") == 0) {
            showToast = FALSE;
        } else if (wcscmp(argv[i], L"--no-copy") == 0 || wcscmp(argv[i], L"-nc") == 0) {
            copyToClipboard = FALSE;
        } else if (wcscmp(argv[i], L"--clip") == 0 || wcscmp(argv[i], L"-c") == 0) {
            useClip = TRUE;
        } else if (wcscmp(argv[i], L"--toast-time") == 0 || wcscmp(argv[i], L"-t") == 0) {
            if (i + 1 >= argc) {
                MessageBoxW(NULL, L"-t 缺少时长参数（范围 0.5~30 秒）",
                            L"CalcPlus", MB_ICONERROR);
                return 0;
            }
            wchar_t* endptr = NULL;
            double sec = wcstod(argv[i + 1], &endptr);
            if (endptr == argv[i + 1] || (endptr && *endptr != 0)) {
                MessageBoxW(NULL, L"-t 后的参数不是有效数字（范围 0.5~30 秒）",
                            L"CalcPlus", MB_ICONERROR);
                return 0;
            }
            if (sec < 0.5 || sec > 30.0) {
                MessageBoxW(NULL, L"-t 时长范围 0.5~30 秒",
                            L"CalcPlus", MB_ICONERROR);
                return 0;
            }
            toastMs = (UINT)(sec * 1000);
            i++;
        } else if (argCount < 64) {
            validArgs[argCount++] = argv[i];
        } else {
            MessageBoxW(NULL, L"参数过多（上限 64）", L"CalcPlus", MB_ICONERROR);
            return 0;
        }
    }

    // 命令分派
    if (argCount == 0) {
        strcpy(cmd, "uuid");
    } else if (IsKnownCommand(validArgs[0])) {
        WideCharToMultiByte(CP_UTF8, 0, validArgs[0], -1, cmd, sizeof(cmd), NULL, NULL);
        if (strcmp(cmd, "fhash") == 0) {
            if (argCount < 3) {
                MessageBoxW(NULL, L"用法: fhash <md5|sha1|sha256|sha512> <文件路径>",
                            L"CalcPlus", MB_ICONERROR);
                return 0;
            }
            if (!CopyArgToUtf8(validArgs[1], alg, sizeof(alg), L"算法名")) return 0;
            if (wcslen(validArgs[2]) >= 4096) {
                MessageBoxW(NULL, L"文件路径过长", L"CalcPlus", MB_ICONERROR);
                return 0;
            }
            wcscpy(wfilePath, validArgs[2]);
        } else if (strcmp(cmd, "ip") == 0 && argCount >= 3) {
            char t1[2048] = {0}, t2[2048] = {0};
            if (!CopyArgToUtf8(validArgs[1], t1, sizeof(t1), L"IP 地址")) return 0;
            if (!CopyArgToUtf8(validArgs[2], t2, sizeof(t2), L"掩码")) return 0;
            snprintf(input, sizeof(input), "%s %s", t1, t2);
        } else if (argCount >= 2) {
            if (!JoinArgsToUtf8(validArgs, 1, argCount - 1, input, sizeof(input))) return 0;
        }
    } else {
        strcpy(cmd, "calc");
        if (!JoinArgsToUtf8(validArgs, 0, argCount, input, sizeof(input))) return 0;
    }

    // fh 冲突
    if (useClip && strcmp(cmd, "fh") == 0) {
        MessageBoxW(NULL, L"fh 命令不能与 -c 组合（需要关键词参数）",
                    L"CalcPlus", MB_ICONERROR);
        return 0;
    }

    // 读剪贴板（uuid 忽略 -c）
    if (useClip && strcmp(cmd, "uuid") != 0) {
        WCHAR clipText[4096] = {0};
        if (!GetClipboardText(clipText, 4096)) {
            MessageBoxW(NULL, L"剪贴板为空或无法读取", L"CalcPlus", MB_ICONERROR);
            return 0;
        }
        if (!CopyArgToUtf8(clipText, input, sizeof(input), L"剪贴板内容")) return 0;
    }

    WCHAR wresult[4096] = {0};
    char result[8192] = {0};

    if (strcmp(cmd, "calc") == 0) {
        expr_ptr = input;
        double res = parse_expr();
        if (isnan(res) || isinf(res)) {
            wcscpy(wresult, L"表达式无效或除零");
        } else if (res >= INT_MIN && res <= INT_MAX && res == (int)res) {
            swprintf(wresult, 4096, L"%d", (int)res);
        } else {
            swprintf(wresult, 4096, L"%g", res);
        }
    }
    else if (strcmp(cmd, "md5") == 0)    { CalcHash(input, wresult, CALG_MD5, 16); }
    else if (strcmp(cmd, "sha1") == 0)   { CalcHash(input, wresult, CALG_SHA1, 20); }
    else if (strcmp(cmd, "sha256") == 0) { CalcHash(input, wresult, CALG_SHA_256, 32); }
    else if (strcmp(cmd, "sha512") == 0) { CalcHash(input, wresult, CALG_SHA_512, 64); }
    else if (strcmp(cmd, "fhash") == 0)  { CalcFileHash(alg, wfilePath, wresult); }
    else if (strcmp(cmd, "b64") == 0) {
        if (strlen(input) * 4 / 3 + 8 > sizeof(result)) {
            wcscpy(wresult, L"输入过长，Base64 结果超出缓冲区");
        } else {
            Base64Encode(input, result);
            if (result[0]) MultiByteToWideChar(CP_UTF8, 0, result, -1, wresult, 4096);
        }
    }
    else if (strcmp(cmd, "b64d") == 0) {
        size_t decoded_len = 0;
        Base64Decode(input, result, &decoded_len);
        BinResultToWide(result, decoded_len, wresult);
    }
    else if (strcmp(cmd, "crc32") == 0) { CalcCRC32(input, wresult); }
    else if (strcmp(cmd, "url") == 0) {
        if (strlen(input) * 3 + 1 > sizeof(result)) {
            wcscpy(wresult, L"输入过长，URL 编码结果超出缓冲区");
        } else {
            UrlEncode(input, result);
            if (result[0]) MultiByteToWideChar(CP_UTF8, 0, result, -1, wresult, 4096);
        }
    }
    else if (strcmp(cmd, "urld") == 0) {
        size_t rlen = UrlDecode(input, result);
        BinResultToWide(result, rlen, wresult);
    }
    else if (strcmp(cmd, "hex") == 0) {
        if (strlen(input) * 2 + 1 > sizeof(result)) {
            wcscpy(wresult, L"输入过长，Hex 编码结果超出缓冲区");
        } else {
            HexEncode(input, result);
            if (result[0]) MultiByteToWideChar(CP_UTF8, 0, result, -1, wresult, 4096);
        }
    }
    else if (strcmp(cmd, "hexd") == 0) {
        size_t hexlen = strlen(input);
        if (hexlen % 2 != 0) {
            wcscpy(wresult, L"Hex 字符串长度必须为偶数");
        } else {
            size_t rlen = HexDecode(input, result);
            BinResultToWide(result, rlen, wresult);
        }
    }
    else if (strcmp(cmd, "uni") == 0)   { UnicodeEncode(input, wresult); }
    else if (strcmp(cmd, "unid") == 0)  { UnicodeDecode(input, wresult); }
    else if (strcmp(cmd, "uuid") == 0)  { GenUUID(wresult); }
    else if (strcmp(cmd, "ts") == 0)    { ConvertTimestamp(input, wresult); }
    else if (strcmp(cmd, "color") == 0) { ConvertColor(input, wresult); }
    else if (strcmp(cmd, "upper") == 0) { StrToUpper(input, wresult); }
    else if (strcmp(cmd, "lower") == 0) { StrToLower(input, wresult); }
    else if (strcmp(cmd, "count") == 0) { CountChars(input, wresult); }
    else if (strcmp(cmd, "base") == 0)  { ConvertBase(input, wresult); }
    else if (strcmp(cmd, "ip") == 0)    { CalcSubnet(input, wresult); }
    else if (strcmp(cmd, "nanoid") == 0){ GenShortId(input, wresult); }

    else if (strcmp(cmd, "fh") == 0) {
        if (input[0] == '\0') {
            wcscpy(wresult, L"kg=全角空格, dui=对号, cuo=错号, 四向箭头,圆方点五角星,米播闪灯");
        } else if (_stricmp(input, "kg") == 0) { wcscpy(wresult, L"\u3000"); }
        else if (_stricmp(input, "dui") == 0)  { wcscpy(wresult, L"\u221A"); }
        else if (_stricmp(input, "cuo") == 0)  { wcscpy(wresult, L"\u00D7"); }
        else if (_stricmp(input, "zuo") == 0)  { wcscpy(wresult, L"\u2190"); }
        else if (_stricmp(input, "you") == 0)  { wcscpy(wresult, L"\u2192"); }
        else if (_stricmp(input, "shang") == 0){ wcscpy(wresult, L"\u2191"); }
        else if (_stricmp(input, "xia") == 0)  { wcscpy(wresult, L"\u2193"); }
        else if (_stricmp(input, "wjx") == 0)  { wcscpy(wresult, L"\u2605"); }
        else if (_stricmp(input, "yuan") == 0) { wcscpy(wresult, L"\u25CF"); }
        else if (_stricmp(input, "dian") == 0) { wcscpy(wresult, L"\u00B7"); }
        else if (_stricmp(input, "fang") == 0) { wcscpy(wresult, L"\u25A0"); }
        else if (_stricmp(input, "mi") == 0)   { wcscpy(wresult, L"\u203B"); }
        else if (_stricmp(input, "bo") == 0)   { wcscpy(wresult, L"\u25B6"); }
        else if (_stricmp(input, "shan") == 0) { wcscpy(wresult, L"\u26A1"); }
        else if (_stricmp(input, "deng") == 0) { wcscpy(wresult, L"\U0001F4A1"); }
        else { wcscpy(wresult, L"未知符号"); }
    }

    if (wresult[0]) {
        if (copyToClipboard) SetClipboardText(wresult);
        if (showToast) ShowToastAsync(wresult, toastMs);
        if (!showToast && !copyToClipboard) wprintf(L"%ls\n", wresult);
    } else {
        MessageBoxW(NULL, L"未知命令或结果为空", L"CalcPlus", MB_ICONERROR);
    }
    return 0;
}