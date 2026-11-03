// ModelDownloader.h - downloads a catalog model with WinHTTP: resumable (HTTP range requests), retried on
// stalls and dropped connections, checked for free disk space, and verified against the published
// SHA-256 (Windows CNG) before it is moved into place.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "ModelAdvisor.h"

namespace appassist
{
    struct DownloadProgress
    {
        std::string phase;                  // "Downloading", "Verifying"
        unsigned long long done = 0;
        unsigned long long total = 0;
        double bytesPerSecond = 0.0;
    };

    typedef std::function<void(const DownloadProgress&)> DownloadProgressCallback;

    // SHA-256 of a file as lowercase hex.
    bool Sha256File(const std::wstring& path, std::string& hexDigest, const DownloadProgressCallback& progress,
        const std::atomic<bool>* cancel, std::string& error);

    class ModelDownloader
    {
    public:
        ModelDownloader();

        // Downloads `model` into `directory` (created if needed). A partial file from an earlier attempt,
        // ours (.part) or another tool's (the final name, smaller than expected), is resumed.
        bool Download(const CatalogModel& model, const std::wstring& directory, const DownloadProgressCallback& progress,
            std::wstring& finalPath, std::string& error);

        // Stops a download in progress from another thread; the partial file is kept for resuming.
        void Cancel();

    private:
        ModelDownloader(const ModelDownloader&);
        ModelDownloader& operator=(const ModelDownloader&);

        bool FetchRange(const CatalogModel& model, const std::wstring& partPath, unsigned long long offset,
            const DownloadProgressCallback& progress, std::string& error);

        std::atomic<bool> cancel_;
        std::mutex mutex_;
        void* request_;     // HINTERNET in flight, closed by Cancel()
    };

}  // namespace appassist
