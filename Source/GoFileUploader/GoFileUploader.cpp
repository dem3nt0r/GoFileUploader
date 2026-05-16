#include <windows.h>
#include <winhttp.h>
#include <iostream>
#include <string>
#include <cstdlib>
#include <ctime>

#pragma comment(lib, "winhttp.lib")

// ================= ERROR =================

std::string GetLastErrorAsString() {
    DWORD err = GetLastError();
    if (err == 0) return "No error";

    LPSTR msg = nullptr;

    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&msg,
        0,
        NULL
    );

    std::string result = msg ? msg : "Unknown error";
    if (msg) LocalFree(msg);

    return result;
}

// ================= UTF =================

std::wstring narrow_to_wide(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 1) return L"";

    std::wstring out(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &out[0], len);
    return out;
}

std::string wide_to_narrow(const std::wstring& str) {
    if (str.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, str.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "";

    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, str.c_str(), -1, &out[0], len, nullptr, nullptr);
    return out;
}

// ================= FILE =================

std::wstring getFilename(const std::wstring& path) {
    size_t pos = path.find_last_of(L"/\\");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

// ================= BOUNDARY =================

std::string generateBoundary() {
    static bool seeded = false;
    if (!seeded) {
        std::srand((unsigned)std::time(nullptr));
        seeded = true;
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf),
        "----gofile-%u%u",
        (unsigned)std::rand(),
        (unsigned)std::rand());

    return buf;
}

// ================= RESULT =================

struct UploadResult {
    bool ok = false;
    std::string url;
    std::string error;
};

// ================= UPLOAD =================

UploadResult uploadFile(const std::wstring& filePath) {
    UploadResult res;

    HANDLE hFile = INVALID_HANDLE_VALUE;
    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    HINTERNET hRequest = nullptr;

    do {
        std::wstring filename = getFilename(filePath);
        std::string filenameN = wide_to_narrow(filename);
        std::string boundary = generateBoundary();

        hFile = CreateFileW(
            filePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr
        );
        if (hFile == INVALID_HANDLE_VALUE) {
            res.error = "CreateFile failed: " + GetLastErrorAsString();
            break;
        }

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(hFile, &fileSize)) {
            res.error = "GetFileSizeEx failed: " + GetLastErrorAsString();
            break;
        }

        hSession = WinHttpOpen(
            L"WinHTTP-Client/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            nullptr, nullptr, 0
        );

        if (!hSession) {
            res.error = "WinHttpOpen failed: " + GetLastErrorAsString();
            break;
        }

        hConnect = WinHttpConnect(
            hSession,
            L"upload.gofile.io",
            INTERNET_DEFAULT_HTTPS_PORT,
            0
        );

        if (!hConnect) {
            res.error = "WinHttpConnect failed: " + GetLastErrorAsString();
            break;
        }

        hRequest = WinHttpOpenRequest(
            hConnect,
            L"POST",
            L"/uploadfile",
            nullptr,
            nullptr,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE
        );

        if (!hRequest) {
            res.error = "WinHttpOpenRequest failed: " + GetLastErrorAsString();
            break;
        }

        std::string header =
            "--" + boundary + "\r\n"
            "Content-Disposition: form-data; name=\"file\"; filename=\"" + filenameN + "\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n";

        std::string footer =
            "\r\n--" + boundary + "--\r\n";

        std::wstring contentType =
            narrow_to_wide("Content-Type: multipart/form-data; boundary=" + boundary);

        ULONGLONG totalSize =
            header.size() + footer.size() + (ULONGLONG)fileSize.QuadPart;

        if (!WinHttpSendRequest(
            hRequest,
            contentType.c_str(),
            -1,
            WINHTTP_NO_REQUEST_DATA,
            0,
            totalSize,
            0
        )) {
            res.error = "SendRequest failed: " + GetLastErrorAsString();
            break;
        }

        // ================= HEADER =================
        {
            DWORD written = 0;
            if (!WinHttpWriteData(hRequest, header.data(), (DWORD)header.size(), &written)) {
                res.error = "Header write failed: " + GetLastErrorAsString();
                break;
            }
        }

        // ================= STREAM =================
        {
            constexpr DWORD CHUNK_SIZE = 64 * 1024;
            char buffer[CHUNK_SIZE] = {};
            DWORD read = 0;

            while (true) {
                BOOL ok = ReadFile(hFile, buffer, CHUNK_SIZE, &read, nullptr);

                if (!ok) {
                    res.error = "ReadFile failed: " + GetLastErrorAsString();
                    break;
                }

                if (read == 0)
                    break;

                DWORD written = 0;

                if (!WinHttpWriteData(hRequest, buffer, read, &written)) {
                    res.error = "WriteData failed: " + GetLastErrorAsString();
                    break;
                }

                if (written != read) {
                    res.error = "Partial write";
                    break;
                }
            }

            if (!res.error.empty()) break;
        }

        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;

        // ================= FOOTER =================
        {
            DWORD written = 0;
            if (!WinHttpWriteData(hRequest, footer.data(), (DWORD)footer.size(), &written)) {
                res.error = "Footer write failed: " + GetLastErrorAsString();
                break;
            }
        }

        if (!WinHttpReceiveResponse(hRequest, nullptr)) {
            res.error = "ReceiveResponse failed: " + GetLastErrorAsString();
            break;
        }

        // ================= RESPONSE =================
        {
            std::string response;
            char buf[4096];
            DWORD r = 0;

            do {
                if (!WinHttpReadData(hRequest, buf, sizeof(buf), &r)) {
                    res.error = "ReadData failed: " + GetLastErrorAsString();
                    break;
                }

                if (r > 0)
                    response.append(buf, r);

            } while (r > 0);

            if (!res.error.empty()) break;

            const std::string key = "\"downloadPage\":\"";
            size_t pos = response.find(key);

            if (pos == std::string::npos) {
                res.error = "No download link found";
                break;
            }

            pos += key.size();
            size_t end = response.find('"', pos);

            if (end == std::string::npos) {
                res.error = "Invalid response";
                break;
            }

            res.url = response.substr(pos, end - pos);
            res.ok = true;
        }

    } while (false);

    // ================= CLEANUP =================

    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);

    if (hRequest)
        WinHttpCloseHandle(hRequest);

    if (hConnect)
        WinHttpCloseHandle(hConnect);

    if (hSession)
        WinHttpCloseHandle(hSession);

    return res;
}

// ================= MAIN =================

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: uploader.exe <file>\n";
        return ERROR_INVALID_PARAMETER;
    }

    UploadResult r = uploadFile(argv[1]);

    if (!r.ok) {
        std::cerr << "Upload failed: " << r.error << "\n";
        return 1;
    }

    std::cout << "Upload Success. Download link -> " << r.url << "\n";

    return 0;
}