// LocalLLMClient.cpp - WinHTTP client for an OpenAI-compatible local chat endpoint.
#include "LocalLLMClient.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <sstream>
#include <vector>

#include "nlohmann/json.hpp"

#pragma comment(lib, "winhttp.lib")

namespace appassist
{
    using nlohmann::json;

    namespace
    {
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
                reset();
            }

            void reset(HINTERNET h = nullptr)
            {
                if (h_)
                {
                    WinHttpCloseHandle(h_);
                }

                h_ = h;
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

        std::string WinHttpError(const char* what)
        {
            DWORD code = GetLastError();
            std::ostringstream os;
            os << what << " (WinHTTP error " << code;
            switch (code)
            {
            case ERROR_WINHTTP_CANNOT_CONNECT:
                os << ": nothing is listening at that address - is the server running?";
                break;
            case ERROR_WINHTTP_TIMEOUT:
                os << ": timed out";
                break;
            case ERROR_WINHTTP_NAME_NOT_RESOLVED:
                os << ": host name not found";
                break;
            case ERROR_WINHTTP_OPERATION_CANCELLED:
                os << ": cancelled";
                break;
            default:
                break;
            }

            os << ")";
            return os.str();
        }

    }  // namespace

    LocalLLMClient::LocalLLMClient(const HttpEndpoint& endpoint)
        : endpoint_(endpoint)
        , request_(nullptr)
        , cancelled_(false)
    {
    }

    LocalLLMClient::~LocalLLMClient()
    {
    }

    std::string LocalLLMClient::Describe() const
    {
        return (endpoint_.model.empty() ? std::string("default model") : endpoint_.model) + " via " +
               WideToUtf8(endpoint_.url);
    }

    void LocalLLMClient::Cancel()
    {
        cancelled_ = true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (request_)
        {
            WinHttpCloseHandle(static_cast<HINTERNET>(request_));  // makes the blocked call fail at once
            request_ = nullptr;
        }
    }

    bool LocalLLMClient::Generate(const std::string& systemPrompt, const std::string& userPrompt,
        const TokenCallback& onText, std::string& error)
    {
        cancelled_ = false;

        // Split the URL into host / port / path.
        URL_COMPONENTS parts;
        ZeroMemory(&parts, sizeof(parts));
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256] = { 0 }, path[2048] = { 0 };
        parts.lpszHostName = host;
        parts.dwHostNameLength = static_cast<DWORD>(sizeof(host) / sizeof(host[0]));
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
        if (!WinHttpCrackUrl(endpoint_.url.c_str(), 0, 0, &parts))
        {
            error = "Invalid server URL: " + WideToUtf8(endpoint_.url) +
                    " (expected e.g. http://localhost:1234/v1/chat/completions)";
            return false;
        }

        const bool https = parts.nScheme == INTERNET_SCHEME_HTTPS;

        // Request body (OpenAI chat completions format, non-streaming).
        json body;
        if (!endpoint_.model.empty())
        {
            body["model"] = endpoint_.model;
        }

        body["messages"] = json::array({ { { "role", "system" }, { "content", systemPrompt } },
            { { "role", "user" }, { "content", userPrompt } } });
        body["temperature"] = endpoint_.temperature;
        body["max_tokens"] = endpoint_.maxTokens;
        body["stream"] = false;
        const std::string payload = body.dump(-1, ' ', false, json::error_handler_t::replace);

        InternetHandle session(WinHttpOpen(L"AppAssist/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session.get())
        {
            error = WinHttpError("WinHttpOpen failed");
            return false;
        }

        const int receiveMs = endpoint_.timeoutSeconds * 1000;
        WinHttpSetTimeouts(session.get(), 10000, 10000, 30000, receiveMs);

        InternetHandle connection(WinHttpConnect(session.get(), host, parts.nPort, 0));
        if (!connection.get())
        {
            error = WinHttpError("Cannot connect to the server");
            return false;
        }

        InternetHandle request(WinHttpOpenRequest(connection.get(), L"POST", path, nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0));
        if (!request.get())
        {
            error = WinHttpError("Cannot create the HTTP request");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            request_ = request.get();
        }

        const wchar_t* headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
        BOOL sent = WinHttpSendRequest(request.get(), headers, static_cast<DWORD>(-1L),
            const_cast<char*>(payload.data()), static_cast<DWORD>(payload.size()),
            static_cast<DWORD>(payload.size()), 0);
        BOOL received = sent && WinHttpReceiveResponse(request.get(), nullptr);

        std::string response;
        DWORD status = 0;
        if (received)
        {
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);

            for (;;)
            {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request.get(), &available))
                {
                    received = FALSE;
                    break;
                }

                if (available == 0)
                {
                    break;
                }

                std::vector<char> chunk(available);
                DWORD read = 0;
                if (!WinHttpReadData(request.get(), chunk.data(), available, &read))
                {
                    received = FALSE;
                    break;
                }

                response.append(chunk.data(), read);
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (request_ == nullptr)
            {
                request.release();  // Cancel() already closed it
            }

            request_ = nullptr;
        }

        if (cancelled_)
        {
            error = "Cancelled.";
            return false;
        }

        if (!sent)
        {
            error = WinHttpError(("Could not send the request to " + WideToUtf8(endpoint_.url)).c_str());
            return false;
        }

        if (!received)
        {
            error = WinHttpError("No response from the server");
            return false;
        }

        try
        {
            json reply = json::parse(response);
            if (status != 200)
            {
                std::string message = reply.contains("error")
                                          ? (reply["error"].is_object() ? reply["error"].value("message", reply["error"].dump())
                                                                        : reply["error"].dump())
                                          : response.substr(0, 400);
                std::ostringstream os;
                os << "The server answered HTTP " << status << ": " << message;
                error = os.str();
                return false;
            }

            const json& choices = reply.at("choices");
            if (!choices.is_array() || choices.empty())
            {
                error = "The server's reply has no choices.";
                return false;
            }

            std::string content = choices[0].at("message").value("content", std::string());
            onText(content);
            return true;
        }
        catch (const std::exception& e)
        {
            std::ostringstream os;
            os << "Unexpected reply from the server (HTTP " << status << "): " << e.what() << ": "
               << response.substr(0, 300);
            error = os.str();
            return false;
        }
    }

}  // namespace appassist
