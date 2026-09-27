// The host's speech recognizer on Windows (SAPI's in-process recognizer),
// listening to the default microphone for the phrases of a small grammar.
// A Kinect's microphone array is an audio device like any other: made the
// default recording device, it is the one heard.
#include "voice_commands.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef _MSC_VER
// MinGW has no sapi.lib for the class and interface ids: let sapi.h define them.
#include <initguid.h>
#endif
#include <sapi.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "sapi.lib")
#endif

namespace sfr {
namespace {

std::wstring wide(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size);
    return out;
}

std::string narrow(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(size_t(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

template<class T> void release(T*& object) {
    if (object) object->Release();
    object = nullptr;
}

std::string hresult(HRESULT value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08lX", static_cast<unsigned long>(value));
    return text;
}

// The default token of a category (the recording device, or a recognizer of
// a language: "Language=409"), as sphelper.h's helpers find it.
ISpObjectToken* token_of(const wchar_t* category, const wchar_t* attributes) {
    ISpObjectTokenCategory* tokens = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory,
                                reinterpret_cast<void**>(&tokens))) ||
        FAILED(tokens->SetId(category, FALSE))) {
        release(tokens);
        return nullptr;
    }
    ISpObjectToken* token = nullptr;
    if (attributes) {
        IEnumSpObjectTokens* found = nullptr;
        if (SUCCEEDED(tokens->EnumTokens(attributes, nullptr, &found)) && found) found->Next(1, &token, nullptr);
        release(found);
    } else {
        wchar_t* id = nullptr;
        if (SUCCEEDED(tokens->GetDefaultTokenId(&id)) && id) {
            if (SUCCEEDED(CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL, IID_ISpObjectToken,
                                           reinterpret_cast<void**>(&token))) &&
                FAILED(token->SetId(nullptr, id, FALSE)))
                release(token);
            CoTaskMemFree(id);
        }
    }
    release(tokens);
    return token;
}

class SapiRecognizer final : public VoiceRecognizer {
public:
    ~SapiRecognizer() override {
        worker_.request_stop();
        if (worker_.joinable()) worker_.join();
    }

    bool start(const std::vector<std::string>& phrases, std::string* why) {
        // Everything COM is made and used on the worker's own apartment; the
        // first answer (started or why not) comes back before open returns.
        std::mutex ready_lock;
        std::condition_variable ready;
        bool answered = false, ok = false;
        std::string reason;
        worker_ = std::jthread([&, phrases](std::stop_token stop) {
            run(stop, phrases, [&](bool started, std::string because) {
                std::lock_guard guard(ready_lock);
                ok = started;
                reason = std::move(because);
                answered = true;
                ready.notify_one();
            });
        });
        std::unique_lock guard(ready_lock);
        ready.wait(guard, [&] { return answered; });
        if (!ok) {
            guard.unlock();
            worker_.join();
            if (why) *why = reason;
        }
        return ok;
    }

    bool next(std::string& phrase) override {
        std::lock_guard guard(lock_);
        if (heard_.empty()) return false;
        phrase = std::move(heard_.front());
        heard_.pop_front();
        return true;
    }

private:
    template<class Answer>
    void run(std::stop_token stop, std::vector<std::string> phrases, Answer answer) {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) { answer(false, "com"); return; }
        ISpRecognizer* recognizer = nullptr;
        ISpObjectToken* engine = nullptr;
        ISpObjectToken* microphone = nullptr;
        ISpRecoContext* context = nullptr;
        ISpRecoGrammar* grammar = nullptr;
        HANDLE event = nullptr;
        const auto finish = [&] {
            if (grammar) grammar->SetRuleState(nullptr, nullptr, SPRS_INACTIVE);
            release(grammar);
            release(context);
            release(microphone);
            release(engine);
            release(recognizer);
            CoUninitialize();
        };
        HRESULT result = CoCreateInstance(CLSID_SpInprocRecognizer, nullptr, CLSCTX_ALL, IID_ISpRecognizer,
                                          reinterpret_cast<void**>(&recognizer));
        if (FAILED(result)) { finish(); answer(false, "no-recognizer-" + hresult(result)); return; }
        // A recognizer of the language asked for, else the system's own.
        if (const char* language = std::getenv("SFR_VOICE_LANGUAGE"); language && *language) {
            engine = token_of(SPCAT_RECOGNIZERS, (L"Language=" + wide(language)).c_str());
            if (!engine) { finish(); answer(false, std::string("no-recognizer-for-language-") + language); return; }
            recognizer->SetRecognizer(engine);
        }
        microphone = token_of(SPCAT_AUDIOIN, nullptr);
        if (!microphone || FAILED(recognizer->SetInput(microphone, TRUE))) {
            finish();
            answer(false, "no-microphone");
            return;
        }
        if (FAILED(recognizer->CreateRecoContext(&context)) || FAILED(context->SetNotifyWin32Event()) ||
            FAILED(context->SetInterest(SPFEI(SPEI_RECOGNITION), SPFEI(SPEI_RECOGNITION))) ||
            !(event = context->GetNotifyEventHandle()) || FAILED(context->CreateGrammar(1, &grammar))) {
            finish();
            answer(false, "no-context");
            return;
        }
        // One rule: any single phrase. A phrase the recognizer cannot say
        // (an English word to a Chinese engine, say) is left out.
        SPSTATEHANDLE rule = nullptr;
        if (FAILED(grammar->GetRule(L"command", 1, SPRAF_TopLevel | SPRAF_Active, TRUE, &rule))) {
            finish();
            answer(false, "no-grammar");
            return;
        }
        size_t added = 0;
        for (const auto& phrase : phrases)
            if (SUCCEEDED(grammar->AddWordTransition(rule, nullptr, wide(phrase).c_str(), L" ", SPWT_LEXICAL, 1.0f,
                                                     nullptr)))
                ++added;
        if (!added || FAILED(grammar->Commit(0)) || FAILED(grammar->SetRuleState(nullptr, nullptr, SPRS_ACTIVE))) {
            finish();
            answer(false, "grammar-not-accepted");
            return;
        }
        std::cerr << "NATIVE_VOICE started phrases=" << added << '/' << phrases.size() << '\n';
        answer(true, {});
        while (!stop.stop_requested()) {
            if (WaitForSingleObject(event, 100) != WAIT_OBJECT_0) continue;
            SPEVENT heard{};
            ULONG fetched = 0;
            while (SUCCEEDED(context->GetEvents(1, &heard, &fetched)) && fetched) {
                if (heard.eEventId == SPEI_RECOGNITION && heard.elParamType == SPET_LPARAM_IS_OBJECT) {
                    ISpRecoResult* recognition = reinterpret_cast<ISpRecoResult*>(heard.lParam);
                    SPPHRASE* phrase = nullptr;
                    wchar_t* text = nullptr;
                    // Only what the recognizer is reasonably sure of: noise in
                    // a small grammar otherwise turns into commands.
                    if (SUCCEEDED(recognition->GetPhrase(&phrase)) && phrase &&
                        phrase->Rule.Confidence >= SP_NORMAL_CONFIDENCE &&
                        SUCCEEDED(recognition->GetText(SP_GETWHOLEPHRASE, SP_GETWHOLEPHRASE, FALSE, &text, nullptr)) &&
                        text) {
                        std::lock_guard guard(lock_);
                        heard_.push_back(narrow(text));
                        if (heard_.size() > 8) heard_.pop_front();
                    }
                    if (text) CoTaskMemFree(text);
                    if (phrase) CoTaskMemFree(phrase);
                }
                // An object parameter is the event's to release.
                if (heard.elParamType == SPET_LPARAM_IS_OBJECT && heard.lParam)
                    reinterpret_cast<IUnknown*>(heard.lParam)->Release();
                else if ((heard.elParamType == SPET_LPARAM_IS_POINTER || heard.elParamType == SPET_LPARAM_IS_STRING) &&
                         heard.lParam)
                    CoTaskMemFree(reinterpret_cast<void*>(heard.lParam));
                heard = {};
            }
        }
        finish();
    }

    std::mutex lock_;
    std::deque<std::string> heard_;
    std::jthread worker_;
};

}

bool VoiceRecognizer::supported() { return true; }

std::unique_ptr<VoiceRecognizer> VoiceRecognizer::open(const std::vector<std::string>& phrases, std::string* why) {
    auto recognizer = std::make_unique<SapiRecognizer>();
    if (!recognizer->start(phrases, why)) return nullptr;
    return recognizer;
}

}
