#include "tts.h"

#include "log/log.hpp"


extern CComModule _Module;

static ISpVoice* pVoice = 0;

bool ttsSetFirstFitNarrator(ISpVoice* voice, const WCHAR* lang) {
    HRESULT hr;
    ISpObjectToken* token = 0;
    CComPtr<IEnumSpObjectTokens> cpEnum;

    hr = SpEnumTokens(SPCAT_VOICES, lang, 0, &cpEnum);
    if (hr != S_OK) {
        return false;
    }

    while (cpEnum->Next(1, &token, 0) == S_OK) {
        HRESULT hr = voice->SetVoice(token);
        if (hr == S_OK) {
            hr = voice->Speak(0, SPF_PURGEBEFORESPEAK, 0);
            if (hr == S_OK) {
                hr = voice->Speak(L" ", SPF_ASYNC | SPF_IS_NOT_XML, 0);
                if (hr == S_OK) {
                    LOG("Narrator OK");
                    return true;
                }
            }
        }
    }
    return false;
}

bool ttsInit() {
    if (FAILED(::CoInitialize(NULL))) {
        LOG_ERR("Failed to initialize COM");
        return false;
    }

    HRESULT hr = CoCreateInstance(CLSID_SpVoice, NULL, CLSCTX_ALL, IID_ISpVoice, (void**)&pVoice);
    if (!SUCCEEDED(hr)) {
        LOG_ERR("Failed to create TTS voice");
        return false;
    }

    if (!ttsSetFirstFitNarrator(pVoice, L"Language=419")) {
        LOG_ERR("Failed to select TTS narrator");
        pVoice->Release();
        pVoice = 0;
        return false;
    }

    pVoice->SetVolume(100);

    LOG("TTS initialized successfully");
    return true;
}

void ttsCleanup() {
    pVoice->Release();
    pVoice = 0;

    ::CoUninitialize();
}

void ttsSay(const char* str) {
    std::string s = str;
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), s.size(), 0, 0);
    std::wstring ws;
    ws.resize(len);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), s.size(), (wchar_t*)ws.c_str(), len);
    pVoice->Speak(ws.c_str(), SPF_ASYNC | SPF_IS_NOT_XML, 0);
}