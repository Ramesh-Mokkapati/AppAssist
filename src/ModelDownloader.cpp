// ModelDownloader.cpp - see ModelDownloader.h.
#include "ModelDownloader.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")

namespace appassist
{
    namespace
    {
        typedef std::chrono::steady_clock Clock;

        unsigned long long FileSize(const std::wstring& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA data;
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            {
                return 0;
            }
            return (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        }

        bool CreateDirectories(const std::wstring& dir)
        {
            if (dir.empty() || DirectoryExists(dir))
            {
                return true;
            }
            size_t slash = dir.find_last_of(L"\\/");
            if (slash != std::wstring::npos && slash > 2 && !CreateDirectories(dir.substr(0, slash)))
            {
                return false;
            }
            return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
        }

        std::string WinError(const char* what)
        {
            DWORD code = GetLastError();
            std::ostringstream os;
            os << what << " (error " << code;
            if (code == ERROR_WINHTTP_TIMEOUT)
            {
                os << ": timed out";
            }
            else if (code == ERROR_WINHTTP_CANNOT_CONNECT || code == ERROR_WINHTTP_NAME_NOT_RESOLVED)
            {
                os << ": no connection to the download server";
            }
            else if (code == ERROR_WINHTTP_CONNECTION_ERROR)
            {
                os << ": the connection dropped";
            }
            os << ")";
            return os.str();
        }

        // Closes a WinHTTP handle when it goes out of scope.
        class InternetHandle
        {
        public:
            explicit InternetHandle(HINTERNET h = nullptr)
                : h_(h)
            {
            }
            ~InternetHandle()
            {
                if (h_)
                {
                    WinHttpCloseHandle(h_);
                }
            }
            HINTERNET get() const
            {
                return h_;
            }
            HINTERNET release()
            {
                HINTERNET h = h_;
                h_ = nullptr;
                return h;
            }

        private:
            InternetHandle(const InternetHandle&);
            InternetHandle& operator=(const InternetHandle&);
            HINTERNET h_;
        };

        // Sleeps in small steps so a cancel is noticed quickly.
        void WaitOrCancel(int seconds, const std::atomic<bool>& cancel)
        {
            for (int i = 0; i < seconds * 5 && !cancel; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
    }  // namespace

    bool Sha256File(const std::wstring& path, std::string& hexDigest, const DownloadProgressCallback& progress,
        const std::atomic<bool>* cancel, std::string& error)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = WinError(("Cannot open " + WideToUtf8(path)).c_str());
            return false;
        }
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
                  BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));
        if (!ok)
        {
            error = "Windows could not start a SHA-256 hash.";
        }

        DownloadProgress p;
        p.phase = "Verifying";
        p.total = FileSize(path);
        std::vector<unsigned char> buffer(4 * 1024 * 1024);
        Clock::time_point lastReport = Clock::now();
        while (ok)
        {
            DWORD got = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr))
            {
                error = WinError("Cannot read the downloaded file");
                ok = false;
                break;
            }
            if (got == 0)
            {
                break;
            }
            ok = BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), got, 0));
            p.done += got;
            if (cancel && cancel->load())
            {
                error = "Cancelled.";
                ok = false;
            }
            if (progress && Clock::now() - lastReport > std::chrono::milliseconds(250))
            {
                progress(p);
                lastReport = Clock::now();
            }
        }
        unsigned char digest[32] = { 0 };
        if (ok && !BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0)))
        {
            error = "Windows could not finish the SHA-256 hash.";
            ok = false;
        }
        if (hash)
        {
            BCryptDestroyHash(hash);
        }
        if (algorithm)
        {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        CloseHandle(file);
        if (ok)
        {
            static const char hex[] = "0123456789abcdef";
            hexDigest.clear();
            for (size_t i = 0; i < sizeof(digest); ++i)
            {
                hexDigest += hex[digest[i] >> 4];
                hexDigest += hex[digest[i] & 0x0F];
            }
        }
        return ok;
    }

    ModelDownloader::ModelDownloader()
        : cancel_(false)
        , request_(nullptr)
    {
    }

    void ModelDownloader::Cancel()
    {
        cancel_ = true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (request_)
        {
            WinHttpCloseHandle(static_cast<HINTERNET>(request_));  // makes a blocked read fail at once
            request_ = nullptr;
        }
    }

    bool ModelDownloader::Download(const CatalogModel& model, const std::wstring& directory,
        const DownloadProgressCallback& progress, std::wstring& finalPath, std::string& error)
    {
        cancel_ = false;
        if (!CreateDirectories(directory))
        {
            error = WinError(("Cannot create the folder " + WideToUtf8(directory)).c_str());
            return false;
        }
        finalPath = JoinPath(directory, model.fileName);
        const std::wstring partPath = finalPath + L".part";

        // Already there, or a partial file under the final name (another tool's interrupted download).
        unsigned long long existing = FileSize(finalPath);
        if (existing > model.sizeBytes)
        {
            error = WideToUtf8(finalPath) + " is larger than the published model; move it away and try again.";
            return false;
        }
        if (existing > 0 && existing < model.sizeBytes && FileSize(partPath) == 0)
        {
            MoveFileExW(finalPath.c_str(), partPath.c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        if (FileSize(finalPath) != model.sizeBytes)
        {
            // Enough disk for the rest of the file, plus a little room.
            const unsigned long long have = FileSize(partPath);
            ULARGE_INTEGER freeBytes;
            if (GetDiskFreeSpaceExW(directory.c_str(), &freeBytes, nullptr, nullptr) &&
                freeBytes.QuadPart < model.sizeBytes - have + 256ull * 1024 * 1024)
            {
                error = "Not enough free disk space in " + WideToUtf8(directory) + ": need " +
                        FormatSize(model.sizeBytes - have) + ", have " + FormatSize(freeBytes.QuadPart) + ".";
                return false;
            }

            // Download, resuming after every failure; give up only after many attempts in a row.
            int failures = 0;
            while (FileSize(partPath) < model.sizeBytes)
            {
                std::string attemptError;
                const unsigned long long before = FileSize(partPath);
                if (FetchRange(model, partPath, before, progress, attemptError))
                {
                    failures = 0;
                    continue;
                }
                if (cancel_)
                {
                    error = "Cancelled. The partial download is kept; Download resumes it.";
                    return false;
                }
                failures = FileSize(partPath) > before ? 0 : failures + 1;  // progress resets the count
                if (failures >= 8)
                {
                    error = attemptError + ". The partial download is kept; try again later to resume.";
                    return false;
                }
                WaitOrCancel(std::min(5 * failures, 30), cancel_);
            }
            if (FileSize(partPath) != model.sizeBytes)
            {
                error = "The download ended with an unexpected size; delete " + WideToUtf8(partPath) + " and try again.";
                return false;
            }
        }

        // Check the file is exactly the published one before using it. A file that was already complete is
        // checked where it is, so a model in use never disappears while it is being verified.
        const bool alreadyComplete = FileSize(finalPath) == model.sizeBytes;
        const std::wstring& checkPath = alreadyComplete ? finalPath : partPath;
        std::string digest;
        if (!Sha256File(checkPath, digest, progress, &cancel_, error))
        {
            if (cancel_)
            {
                error = "Cancelled while verifying. The file is kept; Download checks it again.";
            }
            return false;
        }
        if (digest != model.sha256)
        {
            std::wstring bad = finalPath + L".corrupt";
            MoveFileExW(checkPath.c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING);
            error = "The downloaded file is damaged (SHA-256 does not match the published one). It was renamed to " +
                    WideToUtf8(bad) + "; download it again.";
            return false;
        }
        if (!alreadyComplete && !MoveFileExW(partPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            error = WinError(("Cannot rename the download to " + WideToUtf8(finalPath)).c_str());
            return false;
        }
        return true;
    }

    bool ModelDownloader::FetchRange(const CatalogModel& model, const std::wstring& partPath, unsigned long long offset,
        const DownloadProgressCallback& progress, std::string& error)
    {
        std::wstring url = Utf8ToWide(model.url);
        URL_COMPONENTS parts;
        ZeroMemory(&parts, sizeof(parts));
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256] = { 0 };
        wchar_t path[2048] = { 0 };
        parts.lpszHostName = host;
        parts.dwHostNameLength = static_cast<DWORD>(sizeof(host) / sizeof(host[0]));
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
        {
            error = "Invalid download address: " + std::string(model.url);
            return false;
        }

        InternetHandle session(WinHttpOpen(L"AppAssist/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session.get())
        {
            error = WinError("Cannot start WinHTTP");
            return false;
        }
        // A stalled transfer (no data for 60 s) fails, and the caller resumes it.
        WinHttpSetTimeouts(session.get(), 15000, 15000, 30000, 60000);
        InternetHandle connection(WinHttpConnect(session.get(), host, parts.nPort, 0));
        InternetHandle request(connection.get()
                ? WinHttpOpenRequest(connection.get(), L"GET", path, nullptr, WINHTTP_NO_REFERER,
                      WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                : nullptr);
        if (!request.get())
        {
            error = WinError("Cannot connect to the download server");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            request_ = request.get();
        }

        std::wstring headers;
        if (offset > 0)
        {
            headers = L"Range: bytes=" + std::to_wstring(offset) + L"-\r\n";
        }
        BOOL ok = WinHttpSendRequest(request.get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                      headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                  WinHttpReceiveResponse(request.get(), nullptr);
        DWORD status = 0;
        if (ok)
        {
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        }

        bool result = false;
        if (!ok)
        {
            error = cancel_ ? std::string("Cancelled.") : WinError("The download request failed");
        }
        else if (status == 416 && offset >= model.sizeBytes)
        {
            result = true;  // nothing left to fetch
        }
        else if (status != 200 && status != 206)
        {
            error = "The download server answered HTTP " + std::to_string(status);
        }
        else
        {
            // 200 means the server ignored the range: start the file again from the beginning.
            const bool restart = status == 200 && offset > 0;
            HANDLE file = CreateFileW(partPath.c_str(), restart ? GENERIC_WRITE : FILE_APPEND_DATA, FILE_SHARE_READ,
                nullptr, restart ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                error = WinError(("Cannot write " + WideToUtf8(partPath)).c_str());
            }
            else
            {
                DownloadProgress p;
                p.phase = "Downloading";
                p.total = model.sizeBytes;
                p.done = restart ? 0 : offset;
                const unsigned long long startDone = p.done;
                const Clock::time_point started = Clock::now();
                Clock::time_point lastReport = started;
                std::vector<char> buffer(1024 * 1024);
                result = true;
                for (;;)
                {
                    DWORD got = 0;
                    if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &got))
                    {
                        error = cancel_ ? std::string("Cancelled.") : WinError("The download stopped");
                        result = false;
                        break;
                    }
                    if (got == 0)
                    {
                        break;  // the server finished sending
                    }
                    DWORD wrote = 0;
                    if (!WriteFile(file, buffer.data(), got, &wrote, nullptr) || wrote != got)
                    {
                        error = WinError("Cannot write the download (disk full?)");
                        result = false;
                        break;
                    }
                    p.done += got;
                    const Clock::time_point now = Clock::now();
                    if (progress && now - lastReport > std::chrono::milliseconds(250))
                    {
                        double seconds = std::chrono::duration<double>(now - started).count();
                        p.bytesPerSecond = seconds > 0 ? (p.done - startDone) / seconds : 0.0;
                        progress(p);
                        lastReport = now;
                    }
                }
                CloseHandle(file);
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (request_ == nullptr)
        {
            request.release();  // Cancel() already closed it
        }
        request_ = nullptr;
        return result && !cancel_;
    }

}  // namespace appassist
