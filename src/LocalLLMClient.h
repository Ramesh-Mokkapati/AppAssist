// LocalLLMClient.h - optional fallback: talks to a local LLM server over HTTP with WinHTTP, using the
// OpenAI-compatible /v1/chat/completions API that LM Studio (port 1234) and Ollama (port 11434) provide.
// Used when no GGUF model is embedded. Requests stay on this computer unless you point the URL elsewhere.
#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "CoreEngine.h"

namespace appassist
{
    struct HttpEndpoint
    {
        std::wstring url = L"http://localhost:1234/v1/chat/completions";  // LM Studio default
        std::string model;                // e.g. "qwen2.5-7b-instruct" (LM Studio) or "llama3.1" (Ollama)
        int maxTokens = 900;
        float temperature = 0.2f;
        int timeoutSeconds = 600;         // generation on a CPU can be slow
    };

    class LocalLLMClient : public ILanguageModel
    {
    public:
        explicit LocalLLMClient(const HttpEndpoint& endpoint);
        ~LocalLLMClient() override;

        std::string Describe() const override;
        bool Generate(const std::string& systemPrompt, const std::string& userPrompt,
            const TokenCallback& onText, std::string& error) override;

        // Aborts a request in progress from another thread (closes its WinHTTP handle).
        void Cancel();

    private:
        LocalLLMClient(const LocalLLMClient&);
        LocalLLMClient& operator=(const LocalLLMClient&);

        HttpEndpoint endpoint_;
        std::mutex mutex_;
        void* request_;                   // HINTERNET of the request in flight, for Cancel()
        std::atomic<bool> cancelled_;
    };

}  // namespace appassist
