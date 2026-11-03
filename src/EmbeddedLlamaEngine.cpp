// EmbeddedLlamaEngine.cpp - written against llama.cpp v0.7.0 (see scripts/fetch_deps.ps1). It uses the
// current API names: llama_model_load_from_file / llama_init_from_model replace the deprecated
// llama_load_model_from_file / llama_new_context_with_model.
#include "EmbeddedLlamaEngine.h"

#include <algorithm>
#include <sstream>
#include <thread>
#include <vector>

#include "llama.h"

namespace appassist
{
    namespace
    {
        std::mutex g_logMutex;
        ProgressCallback g_logSink;

        void LlamaLog(enum ggml_log_level level, const char* text, void* /*user*/)
        {
            if (level < GGML_LOG_LEVEL_WARN || text == nullptr)
            {
                return;  // skip debug/info chatter
            }

            std::lock_guard<std::mutex> lock(g_logMutex);
            if (g_logSink)
            {
                std::string line = Trim(text);
                if (line.size() >= 4)
                {
                    g_logSink("llama.cpp: " + line);  // skip progress dots and fragments
                }
            }
        }

        // llama_backend_init/free once per process, regardless of how many engines exist.
        struct BackendGuard
        {
            BackendGuard()
            {
                llama_log_set(LlamaLog, nullptr);
                llama_backend_init();
            }

            ~BackendGuard()
            {
                llama_backend_free();
            }
        };

        void EnsureBackend()
        {
            static BackendGuard guard;
            (void)guard;
        }

        // Number of leading bytes of `text` that form complete UTF-8 characters (a token can end mid-character).
        size_t CompleteUtf8Prefix(const std::string& text)
        {
            size_t i = text.size();
            size_t back = 0;
            while (i > 0 && back < 4)
            {
                unsigned char c = static_cast<unsigned char>(text[i - 1]);
                if ((c & 0xC0) != 0x80)
                {
                    // lead byte (or ASCII)
                    size_t need = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
                    return (back + 1 >= need) ? text.size() : i - 1;
                }

                --i;
                ++back;
            }

            return text.size();
        }

        std::string BaseNameUtf8(const std::wstring& path)
        {
            size_t slash = path.find_last_of(L"\\/");
            return WideToUtf8(slash == std::wstring::npos ? path : path.substr(slash + 1));
        }

    }  // namespace

    void EmbeddedLlamaEngine::SetLogSink(const ProgressCallback& sink)
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_logSink = sink;
    }

    EmbeddedLlamaEngine::EmbeddedLlamaEngine()
        : model_(nullptr)
        , ctx_(nullptr)
        , vocab_(nullptr)
    {
    }

    EmbeddedLlamaEngine::~EmbeddedLlamaEngine()
    {
        Unload();
    }

    void EmbeddedLlamaEngine::Unload()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ctx_)
        {
            llama_free(ctx_);
            ctx_ = nullptr;
        }

        if (model_)
        {
            llama_model_free(model_);
            model_ = nullptr;
        }

        vocab_ = nullptr;
        description_.clear();
    }

    bool EmbeddedLlamaEngine::IsLoaded() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return model_ != nullptr && ctx_ != nullptr;
    }

    std::string EmbeddedLlamaEngine::Describe() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return description_;
    }

    bool EmbeddedLlamaEngine::Load(const LlamaSettings& settings, std::string& error)
    {
        Unload();
        if (!FileExists(settings.modelPath))
        {
            error = "Model file not found: " + WideToUtf8(settings.modelPath);
            return false;
        }

        EnsureBackend();
        std::lock_guard<std::mutex> lock(mutex_);
        settings_ = settings;

        llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = 0;  // CPU build: no GPU backend is linked

        // llama.cpp opens files with UTF-8 paths on Windows (converted to UTF-16 internally).
        model_ = llama_model_load_from_file(WideToUtf8(settings.modelPath).c_str(), mp);
        if (!model_)
        {
            error = "llama.cpp could not load the model (not a GGUF file, unsupported architecture, or not "
                "enough memory): " + WideToUtf8(settings.modelPath);
            return false;
        }

        vocab_ = llama_model_get_vocab(model_);

        int threads = settings.threads;
        if (threads <= 0)
        {
            threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) / 2);
        }

        int trained = llama_model_n_ctx_train(model_);
        int nctx = settings.contextSize;
        if (trained > 0 && nctx > trained)
        {
            nctx = trained;
        }

        if (nctx < 2048)
        {
            nctx = 2048;
        }

        llama_context_params cp = llama_context_default_params();
        cp.n_ctx = static_cast<uint32_t>(nctx);
        cp.n_batch = 512;
        cp.n_threads = threads;
        cp.n_threads_batch = threads;
        cp.no_perf = true;
        ctx_ = llama_init_from_model(model_, cp);
        if (!ctx_)
        {
            llama_model_free(model_);
            model_ = nullptr;
            vocab_ = nullptr;
            std::ostringstream os;
            os << "llama.cpp could not create a context of " << nctx << " tokens (not enough memory?).";
            error = os.str();
            return false;
        }

        std::ostringstream os;
        os << BaseNameUtf8(settings.modelPath) << ", embedded llama.cpp, " << llama_n_ctx(ctx_) << "-token context, "
            << threads << " threads";
        description_ = os.str();
        return true;
    }

    bool EmbeddedLlamaEngine::FormatChat(const std::string& systemPrompt, const std::string& userPrompt,
        std::string& out) const
    {
        llama_chat_message messages[2] =
        {
            {"system", systemPrompt.c_str()},
            {"user", userPrompt.c_str()}
        };

        const char* tmpl = llama_model_chat_template(model_, nullptr);  // the model's own template, if any
        std::vector<char> buffer(systemPrompt.size() + userPrompt.size() + 1024);
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const char* use = (attempt == 0 && tmpl) ? tmpl : "chatml";  // ChatML is understood by most instruct models
            int n = llama_chat_apply_template(use, messages, 2, true, buffer.data(), static_cast<int32_t>(buffer.size()));
            if (n > static_cast<int>(buffer.size()))
            {
                buffer.resize(static_cast<size_t>(n) + 1);
                n = llama_chat_apply_template(use, messages, 2, true, buffer.data(), static_cast<int32_t>(buffer.size()));
            }

            if (n > 0)
            {
                out.assign(buffer.data(), static_cast<size_t>(n));
                return true;
            }

            if (attempt == 0 && !tmpl)
            {
                break;
            }
        }
        return false;
    }

    bool EmbeddedLlamaEngine::Generate(const std::string& systemPrompt, const std::string& userPrompt,
        const TokenCallback& onText, std::string& error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!model_ || !ctx_)
        {
            error = "No model is loaded.";
            return false;
        }

        std::string prompt;
        if (!FormatChat(systemPrompt, userPrompt, prompt))
        {
            error = "Could not format the prompt with the model's chat template.";
            return false;
        }

        // Tokenize (the first call with too small a buffer returns -needed).
        std::vector<llama_token> tokens(prompt.size() + 16);
        int n = llama_tokenize(vocab_, prompt.c_str(), static_cast<int32_t>(prompt.size()), tokens.data(),
            static_cast<int32_t>(tokens.size()), true, true);
        if (n < 0)
        {
            tokens.resize(static_cast<size_t>(-n));
            n = llama_tokenize(vocab_, prompt.c_str(), static_cast<int32_t>(prompt.size()), tokens.data(),
                static_cast<int32_t>(tokens.size()), true, true);
        }

        if (n <= 0)
        {
            error = "Could not tokenize the prompt.";
            return false;
        }

        tokens.resize(static_cast<size_t>(n));

        const int nctx = static_cast<int>(llama_n_ctx(ctx_));
        const int room = nctx - n - 8;
        if (room < 64)
        {
            std::ostringstream os;
            os << "The prompt (" << n << " tokens) does not fit the " << nctx
                << "-token context. Use a larger context or fewer log lines.";
            error = os.str();
            return false;
        }

        const int maxNew = std::min(settings_.maxNewTokens, room);

        llama_memory_clear(llama_get_memory(ctx_), true);  // each diagnosis starts fresh

        // Feed the prompt in batches no larger than n_batch.
        const int batchSize = 512;
        for (int pos = 0; pos < n; pos += batchSize)
        {
            int count = std::min(batchSize, n - pos);
            llama_batch batch = llama_batch_get_one(tokens.data() + pos, count);
            if (llama_decode(ctx_, batch) != 0)
            {
                error = "llama.cpp failed while reading the prompt.";
                return false;
            }
        }

        // Sampler chain: discourage repetition, keep the likely tokens, low temperature.
        llama_sampler* sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(sampler, llama_sampler_init_penalties(llama_vocab_n_tokens(vocab_), 256, 1.15f, 0.0f, 0.0f));
        llama_sampler_chain_add(sampler, llama_sampler_init_top_k(40));
        llama_sampler_chain_add(sampler, llama_sampler_init_min_p(0.05f, 1));
        llama_sampler_chain_add(sampler, llama_sampler_init_temp(settings_.temperature));
        llama_sampler_chain_add(sampler, llama_sampler_init_dist(1234));

        std::string pending;  // bytes waiting for the rest of a UTF-8 character
        bool ok = true;
        char piece[256];
        for (int i = 0; i < maxNew; ++i)
        {
            llama_token token = llama_sampler_sample(sampler, ctx_, -1);
            if (llama_vocab_is_eog(vocab_, token))
            {
                break;
            }

            int len = llama_token_to_piece(vocab_, token, piece, sizeof(piece), 0, false);
            if (len > 0)
            {
                pending.append(piece, static_cast<size_t>(len));
                size_t ready = CompleteUtf8Prefix(pending);
                if (ready > 0)
                {
                    if (!onText(pending.substr(0, ready)))
                    {
                        break;  // cancelled
                    }

                    pending.erase(0, ready);
                }
            }

            llama_batch next = llama_batch_get_one(&token, 1);
            if (llama_decode(ctx_, next) != 0)
            {
                error = "llama.cpp failed while generating text.";
                ok = false;
                break;
            }
        }

        if (ok && !pending.empty())
        {
            onText(pending);
        }

        llama_sampler_free(sampler);
        return ok;
    }

}  // namespace appassist
