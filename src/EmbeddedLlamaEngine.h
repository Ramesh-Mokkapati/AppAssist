// EmbeddedLlamaEngine.h - runs a local GGUF model in-process through llama.cpp's C API (llama.h),
// statically linked (llama.lib, ggml*.lib). No server, no network.
#pragma once

#include <mutex>
#include <string>

#include "CoreEngine.h"

struct llama_model;
struct llama_context;
struct llama_vocab;

namespace appassist
{

    struct LlamaSettings
    {
        std::wstring modelPath;      // .gguf file
        int contextSize = 8192;      // tokens of prompt + answer; capped at the model's training context
        int maxNewTokens = 900;      // length limit of the answer
        int threads = 0;             // 0 = half the logical processors (at least 1)
        float temperature = 0.2f;    // low: diagnosis should be factual, not creative
    };

    class EmbeddedLlamaEngine : public ILanguageModel
    {
    public:
        EmbeddedLlamaEngine();
        ~EmbeddedLlamaEngine() override;

        // Loads the model (and frees any previously loaded one). Safe to call again with a new path.
        bool Load(const LlamaSettings& settings, std::string& error);
        void Unload();
        bool IsLoaded() const;
        const LlamaSettings& Settings() const
        {
            return settings_;
        }

        std::string Describe() const override;
        bool Generate(const std::string& systemPrompt, const std::string& userPrompt,
            const TokenCallback& onText, std::string& error) override;

        // llama.cpp diagnostics (load progress, errors) go here instead of stderr; may be called from
        // any thread. Set before Load().
        static void SetLogSink(const ProgressCallback& sink);

    private:
        EmbeddedLlamaEngine(const EmbeddedLlamaEngine&);
        EmbeddedLlamaEngine& operator=(const EmbeddedLlamaEngine&);

        bool FormatChat(const std::string& systemPrompt, const std::string& userPrompt, std::string& out) const;

        mutable std::mutex mutex_;
        LlamaSettings settings_;
        llama_model* model_;
        llama_context* ctx_;
        const llama_vocab* vocab_;
        std::string description_;
    };

}  // namespace appassist
