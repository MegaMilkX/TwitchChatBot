#ifndef AUDIO_MIXER_HPP
#define AUDIO_MIXER_HPP

#define NOMINMAX

#include <algorithm>
#include <thread>
#include <mutex>
#include <unordered_set>
#include <unordered_map>

#include "../handle/handle.hpp"

#include <stdint.h>
#include <xaudio2.h>
#pragma comment(lib, "xaudio2.lib")

#include "../math/gfxm.hpp"
#include "../log/log.hpp"

#define STB_VORBIS_HEADER_ONLY
extern "C" {
#include "../lib/stb_vorbis.c"
}

#define AUDIO_BUFFER_SZ 256

#include "audio_buffer.hpp"

static const int SHORT_MAX = std::numeric_limits<short>().max();

struct AudioChannel {
    size_t cursor = 0;
    AudioBuffer* buf = 0;
    float volume = 1.0f;
    float panning = 0.0f;
    float attenuation_radius = 10.0f;
    gfxm::vec3 pos;
    bool looping = false;

    void setPosition(const gfxm::vec3& p) {
        pos = p;
    }
};

struct AudioVoiceData {
    float* front;
    float* back;

    float buffer_f[AUDIO_BUFFER_SZ];
    float buffer[AUDIO_BUFFER_SZ];
    float buffer_back[AUDIO_BUFFER_SZ];

    float buffer_float[AUDIO_BUFFER_SZ];
    float buffer_back_float[AUDIO_BUFFER_SZ];

    IXAudio2SourceVoice* pSourceVoice;

    std::unordered_set<Handle<AudioChannel>> emitters;
    std::unordered_set<Handle<AudioChannel>> emitters3d;
};

class AudioMixer : public IXAudio2VoiceCallback {
    int sampleRate;
    int bitPerSample;
    int nChannels;

    // key - sample rate
    std::unordered_map<int, std::unique_ptr<AudioVoiceData>> voices;

    IXAudio2* pXAudio2;
    IXAudio2MasteringVoice* pMasteringVoice = 0;

    gfxm::mat4 lis_transform;
    std::mutex sync;
    
    bool createSourceVoice(AudioVoiceData* voice, int sampleRate) {
        const int blockAlign = (bitPerSample * nChannels) / 8;

        WAVEFORMATEX wfx = {
            WAVE_FORMAT_IEEE_FLOAT,
            (WORD)nChannels,
            (DWORD)sampleRate,
            (DWORD)(sampleRate * blockAlign),
            (WORD)blockAlign,
            (WORD)bitPerSample,
            0
        };
        HRESULT hr;
        if(FAILED(hr = pXAudio2->CreateSourceVoice(&voice->pSourceVoice, &wfx, 0, 1.0f, this)))
        {
            LOG_ERR("Failed to create source voice: " << hr);
            return false;
        }

        voice->front = &voice->buffer[0];
        voice->back = &voice->buffer_back[0];
        memset(voice->buffer, 0, sizeof(voice->buffer));
    
        
        XAUDIO2_BUFFER buf = { 0 };
        buf.AudioBytes = sizeof(voice->buffer);
        buf.pAudioData = (BYTE*)voice->front;
        buf.LoopCount = 0;
        buf.pContext = voice;

        hr = voice->pSourceVoice->SubmitSourceBuffer(&buf);
        voice->pSourceVoice->Start(0, 0);
    }
    AudioVoiceData* createVoiceForSampleRate(int sampleRate) {
        AudioVoiceData* voiceData = new AudioVoiceData;
        createSourceVoice(voiceData, sampleRate);
        voices.insert(std::make_pair(sampleRate, std::unique_ptr<AudioVoiceData>(voiceData)));
        LOG("Created source voice for sample rate of " << sampleRate);
        return voiceData;
    }
    AudioVoiceData* getVoiceDataBySampleRate(int sampleRate) {
        AudioVoiceData* voiceData = 0;
        auto it = voices.find(sampleRate);
        if (it == voices.end()) {
            voiceData = createVoiceForSampleRate(sampleRate);
        } else {
            voiceData = it->second.get();
        }
        return voiceData;
    }
public:
    AudioMixer() {}
    void setListenerTransform(const gfxm::mat4& t) {
        std::lock_guard<std::mutex> lock(sync);
        lis_transform = t;
    }

    Handle<AudioChannel> createChannel() {
        return HANDLE_MGR<AudioChannel>::acquire();
    }
    void freeChannel(Handle<AudioChannel> h) {
        stop(h);
        HANDLE_MGR<AudioChannel>::release(h);
    }

    void play(Handle<AudioChannel> ch) {
        auto vd = getVoiceDataBySampleRate(ch->buf->sampleRate());
        vd->emitters.insert(ch);
    }
    void play3d(Handle<AudioChannel> ch) {
        auto vd = getVoiceDataBySampleRate(ch->buf->sampleRate());
        vd->emitters3d.insert(ch);
    }
    void stop(Handle<AudioChannel> ch) {
        auto vd = getVoiceDataBySampleRate(ch->buf->sampleRate());
        vd->emitters.erase(ch);
        vd->emitters3d.erase(ch);
    }
    void resetCursor(Handle<AudioChannel> ch) {
        HANDLE_MGR<AudioChannel>::deref(ch)->cursor = 0;
    }

    void setBuffer(Handle<AudioChannel> ch, AudioBuffer* buf) {
        HANDLE_MGR<AudioChannel>::deref(ch)->buf = buf;
        HANDLE_MGR<AudioChannel>::deref(ch)->cursor = 0;
    }
    void setAttenuationRadius(Handle<AudioChannel> ch, float radius) {
        HANDLE_MGR<AudioChannel>::deref(ch)->attenuation_radius = radius;
    }
    void setGain(Handle<AudioChannel> ch, float gain) {
        HANDLE_MGR<AudioChannel>::deref(ch)->volume = gain;
    }
    void setLooping(Handle<AudioChannel> ch, bool v) {
        HANDLE_MGR<AudioChannel>::deref(ch)->looping = v;
    }
    void setPosition(Handle<AudioChannel> ch, const gfxm::vec3& pos) {
        std::lock_guard<std::mutex> lock(sync);
        HANDLE_MGR<AudioChannel>::deref(ch)->setPosition(pos);
    }

    bool isPlaying(Handle<AudioChannel> ch) {
        auto vd = getVoiceDataBySampleRate(ch->buf->sampleRate());
        return vd->emitters.count(ch) || vd->emitters3d.count(ch);
    }
    bool isLooping(Handle<AudioChannel> ch) {
        return HANDLE_MGR<AudioChannel>::deref(ch)->looping;
    }

    void playOnce(AudioBuffer* buf, float vol = 1.0f, float pan = .0f) {
        Handle<AudioChannel> em = HANDLE_MGR<AudioChannel>::acquire();
        HANDLE_MGR<AudioChannel>::deref(em)->buf = buf;
        HANDLE_MGR<AudioChannel>::deref(em)->volume = vol;
        HANDLE_MGR<AudioChannel>::deref(em)->panning = pan;
        auto vd = getVoiceDataBySampleRate(buf->sampleRate());
        vd->emitters.insert(em);
    }
    void playOnce3d(AudioBuffer* buf, const gfxm::vec3& pos, float vol = 1.0f, float attenuation_radius = 10.0f) {
        Handle<AudioChannel> em = HANDLE_MGR<AudioChannel>::acquire();
        auto emp = HANDLE_MGR<AudioChannel>::deref(em);
        emp->buf = buf;
        emp->volume = vol;
        emp->setPosition(pos);
        emp->attenuation_radius = attenuation_radius;
        auto vd = getVoiceDataBySampleRate(buf->sampleRate());
        vd->emitters3d.insert(em);
    }


    bool init(int sampleRate, int bps) {
        this->sampleRate = sampleRate;
        this->bitPerSample = 32;
        this->nChannels = 2;
        //memset(buffer, 0, sizeof(buffer));

        gfxm::mat4 lis_transform = gfxm::mat4(1.0f);

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if(FAILED(hr)) {
            // NOTE: It's ok to fail here, means someone else already did it
            //LOG_ERR("Failed to init COM: " << hr);
            //return false;
        }
        #if(_WIN32_WINNT < 0x602)
        #ifdef _DEBUG
            HMODULE xAudioDll = LoadLibraryExW(L"XAudioD2_7.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        #else
            HMODULE xAudioDll = LoadLibraryExW(L"XAudio2_7.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        #endif
            if(!xAudioDll) {
                LOG_ERR("Failed to find XAudio2.7 dll");
                CoUninitialize();
                return 1;
            }
        #endif
        UINT32 flags = 0;
        #if (_WIN32_WINNT < 0x0602 /*_WIN32_WINNT_WIN8*/) && defined(_DEBUG)
            flags |= XAUDIO2_DEBUG_ENGINE;
        #endif

        hr = XAudio2Create(&pXAudio2, flags);
        if(FAILED(hr)) {
            LOG_ERR("Failed to init XAudio2: " << hr);
            CoUninitialize();
            return false;
        }

        if(FAILED(hr = pXAudio2->CreateMasteringVoice(&pMasteringVoice)))
        {
            LOG_ERR("Failed to create mastering voice: " << hr);
            //pXAudio2.Reset();
            CoUninitialize();
            return false;
        }
        pMasteringVoice->SetVolume(1.00f);

        CoUninitialize();

        return true;
    }
    void cleanup() {
        pXAudio2->StopEngine();
        pXAudio2->Release();
    }

    void __stdcall OnStreamEnd() {   }

    //Unused methods are stubs
    void __stdcall OnVoiceProcessingPassEnd() { }
    void __stdcall OnVoiceProcessingPassStart(UINT32 SamplesRequired) {    }
    void __stdcall OnBufferEnd(void * pBufferContext) {
        AudioVoiceData* pVoiceData = (AudioVoiceData*)pBufferContext;

        gfxm::mat4 lis_trans_copy;
        {
            std::lock_guard<std::mutex> lock(sync);
            lis_trans_copy = lis_transform;
        }

        memset(pVoiceData->buffer_f, 0, sizeof(pVoiceData->buffer_f));
        memset(pVoiceData->back, 0, sizeof(pVoiceData->buffer));
        size_t buf_len = sizeof(pVoiceData->buffer_f) / sizeof(pVoiceData->buffer_f[0]);
        for(auto it = pVoiceData->emitters.begin(); it != pVoiceData->emitters.end();) {
            Handle<AudioChannel> ei = (*it);
            ++it;
            AudioChannel* em = HANDLE_MGR<AudioChannel>::deref(ei);
            if(!em->buf) {
                pVoiceData->emitters.erase(ei);
                HANDLE_MGR<AudioChannel>::release(ei);
                continue;
            }

            size_t src_cur = em->cursor;
            short* data = em->buf->getPtr();
            size_t src_len = em->buf->sampleCount();
            int sample_rate = em->buf->sampleRate();
            int channel_count = em->buf->channelCount();
            bool looping = em->looping;
            
            size_t advance = 0;
            if (channel_count == 2) {
                advance = mix(
                    pVoiceData->buffer_f, buf_len, data, src_len,
                    src_cur, em->volume, em->panning,
                    looping
                );
            } else if (channel_count == 1) {
                advance = mix_mono_(
                    pVoiceData->buffer_f, buf_len,
                    data, src_len, src_cur, sample_rate,
                    em->volume, looping
                );
                /*
                advance = mix_mono(
                    buffer_f, buf_len, data, src_len,
                    src_cur, em->volume, em->panning,
                    looping
                );*/
            }
            
            em->cursor += advance;
            if(em->cursor >= em->buf->sampleCount()) {
                if(!em->looping) {
                    pVoiceData->emitters.erase(ei);
                    HANDLE_MGR<AudioChannel>::release(ei);
                    continue;
                }
            } 
            em->cursor = em->cursor % src_len;
        }
        for(auto it = pVoiceData->emitters3d.begin(); it != pVoiceData->emitters3d.end();) {
            Handle<AudioChannel> ei = (*it);
            ++it;
            AudioChannel* em = HANDLE_MGR<AudioChannel>::deref(ei);
            if(!em->buf) {
                pVoiceData->emitters3d.erase(ei);
                continue;
            }

            gfxm::vec3 p_;
            {
                std::lock_guard<std::mutex> lock(sync);
                p_ = em->pos;
            }
            
            size_t src_cur = em->cursor;
            short* data = em->buf->getPtr();
            size_t src_len = em->buf->sampleCount();
            int src_sample_rate = em->buf->sampleRate();
            int src_channel_count = em->buf->channelCount();
            bool looping = em->looping;

            size_t advance = 0;
            if (src_channel_count == 2) {/*
                advance = mix3d(
                    buffer_f, buf_len,
                    em->buf->getPtr(),
                    em->buf->sampleCount(),
                    em->cursor, em->buf->channelCount(), em->volume,
                    p_,
                    lis_trans_copy
                );*/
                advance = mix3d_stereo(
                    pVoiceData->buffer_f, buf_len,
                    data, src_len, src_cur, src_sample_rate,
                    em->volume, em->attenuation_radius, looping, p_, lis_trans_copy
                );
            } else if(src_channel_count == 1) {
                advance = mix3d_mono(
                    pVoiceData->buffer_f, buf_len,
                    data, src_len, src_cur, src_sample_rate,
                    em->volume, em->attenuation_radius, looping, p_, lis_trans_copy
                );
            }

            em->cursor += advance;
            if(em->cursor >= em->buf->sampleCount()) {
                if(!em->looping) {
                    pVoiceData->emitters3d.erase(ei);
                }
            }
            em->cursor = em->cursor % em->buf->sampleCount();
        }

        for(int i = 0; i < AUDIO_BUFFER_SZ; ++i) {
            //samplef = pow(samplef, pow_) * sign;
            pVoiceData->back[i] = pVoiceData->buffer_f[i];// gfxm::_min(1.0f, (buffer_f[i] * 0.5f));
        }

        float* tmp = pVoiceData->front;
        pVoiceData->front = pVoiceData->back;
        pVoiceData->back = tmp;
        
        XAUDIO2_BUFFER buf = { 0 };
        buf.AudioBytes = sizeof(pVoiceData->buffer);
        buf.pAudioData = (BYTE*)pVoiceData->front;
        buf.LoopCount = 0;
        buf.pContext = pVoiceData;
        pVoiceData->pSourceVoice->SubmitSourceBuffer(&buf);
    }
    void __stdcall OnBufferStart(void * pBufferContext) {    }
    void __stdcall OnLoopEnd(void * pBufferContext) {
        
    }
    void __stdcall OnVoiceError(void * pBufferContext, HRESULT Error) {
        LOG("Voice error: " << Error);
     }
private:
    size_t mix(
        float* dest, 
        size_t dest_len, 
        short* src, 
        size_t src_len,
        size_t cur,
        float vol,
        float panning,
        bool looping
    ) {
        size_t sample_len = src_len < dest_len ? src_len : dest_len;
        size_t overflow = (cur + sample_len) > src_len ? (cur + sample_len) - src_len : 0;

        size_t tgt0sz = sample_len - overflow;
        size_t tgt1sz = overflow;
        short* tgt0 = src + cur;
        short* tgt1 = src;

        float mul = 1.0f / (float)SHORT_MAX;

        for(size_t i = 0; i < tgt0sz; ++i) {
            int lr = (i % 2) * 2 - 1;
            float pan = std::min(fabs(lr + panning), 1.0f);
            
            dest[i] += tgt0[i] * mul * vol * pan;
        }

        if (!looping) {
            return sample_len;
        }

        for(size_t i = 0; i < tgt1sz; ++i) {
            int lr = (i % 2) * 2 - 1;
            float pan = std::min(fabs(lr + panning), 1.0f);
            (dest + tgt0sz)[i] += tgt1[i] * mul * vol * pan;
        }

        return sample_len;
    }
    size_t mix_mono_(
        float* dst, size_t dst_len,
        short* src, size_t src_len,
        size_t src_cur, int src_sampleRate,
        float gain, bool looping
    ) {
        constexpr int dst_channelCount = 2;
        constexpr float flt_convert = 1.0f / (float)SHORT_MAX;
        float sampleRatio = src_sampleRate / (float)this->sampleRate;
        int invSampleRatio = this->sampleRate / src_sampleRate;
        
        // TODO: Handle non-looping
        // (don't emit wrapped around samples)
        if (dst_channelCount == 2) {
            for (int di = 0; di < dst_len; di += dst_channelCount) {
                int si = (src_cur + di / 2) % src_len;
                float s = src[si] * flt_convert * gain;
                dst[di    ] += s;
                dst[di + 1] += s;
            }
        } else {
            // TODO
        }

        return dst_len / dst_channelCount;
    }
    size_t mix_mono(
        float* dest, 
        size_t dest_len, 
        short* src, 
        size_t src_len,
        size_t cur,
        float vol,
        float panning,
        bool looping
    ) {
        size_t sample_len = src_len < (dest_len / 2) ? src_len : (dest_len / 2);
        size_t overflow = (cur + sample_len) > src_len ? (cur + sample_len) - src_len : 0;

        size_t src0sz = sample_len - overflow;
        size_t src1sz = overflow;
        short* src0 = src + cur;
        short* src1 = src;

        float mul = 1.0f / (float)SHORT_MAX;

        for(size_t i = 0; i < src0sz; ++i) {
            int lr = (i % 2) * 2 - 1;
            float pan = std::min(fabs(lr + panning), 1.0f);
            
            dest[i * 2] += src0[i] * mul * vol * pan;
            dest[i * 2 + 1] += src0[i] * mul * vol * pan;
        }

        if (!looping) {
            return sample_len;
        }

        for(size_t i = 0; i < src1sz; ++i) {
            int lr = (i % 2) * 2 - 1;
            float pan = std::min(fabs(lr + panning), 1.0f);
            (dest + src0sz * 2)[i * 2] += src1[i] * mul * vol * pan;
            (dest + src0sz * 2)[i * 2 + 1] += src1[i] * mul * vol * pan;
        }

        return sample_len;
    }
    size_t mix3d_stereo(
        float* dst, size_t dst_len,
        short* src, size_t src_len,
        size_t src_cur, int src_sampleRate,
        float gain, float atten_radius, bool looping,
        const gfxm::vec3& pos,
        const gfxm::mat4& listener_transform
    ) {
        constexpr int dst_channelCount = 2;
        constexpr float flt_convert = 1.0f / (float)SHORT_MAX;
        float sampleRatio = src_sampleRate / (float)this->sampleRate;
        int invSampleRatio = this->sampleRate / src_sampleRate;

        gfxm::vec3 ears[2] = {
            gfxm::vec3(-0.1075f, .0f, .0f),
            gfxm::vec3(0.1075f, .0f, .0f)
        };
        ears[0] = listener_transform * gfxm::vec4(ears[0], 1.0f);
        ears[1] = listener_transform * gfxm::vec4(ears[1], 1.0f);
        constexpr float EMITTER_RADIUS = 0.5f;
        float att = 1.0f / atten_radius;
        float falloff[2] = {
            std::min(1.0f / pow((gfxm::length(ears[0] - pos) * att), 2.0f), 1.0f),
            std::min(1.0f / pow((gfxm::length(ears[1] - pos) * att), 2.0f), 1.0f)
        };        

        for (int di = 0; di < dst_len; di += dst_channelCount) {
            int step = di;
            int si = (int)((src_cur + step / invSampleRatio)) % src_len;
            float s0 = src[si] * flt_convert * gain;
            float s1 = src[si + 1] * flt_convert * gain;
            dst[di] += (s0 + s1) * falloff[0]; // left
            dst[di + 1] += (s0 + s1) * falloff[1]; // right
        }
        // TODO: Handle non-looping

        return (int)(dst_len / invSampleRatio);
    }
    size_t mix3d_mono(
        float* dst, size_t dst_len,
        short* src, size_t src_len,
        size_t src_cur, int src_sampleRate,
        float gain, float atten_radius, bool looping,
        const gfxm::vec3& pos,
        const gfxm::mat4& listener_transform
    ) {
        constexpr int dst_channelCount = 2;
        constexpr float flt_convert = 1.0f / (float)SHORT_MAX;
        float sampleRatio = src_sampleRate / (float)this->sampleRate;
        int invSampleRatio = this->sampleRate / src_sampleRate;

        gfxm::vec3 ears[2] = {
            gfxm::vec3(-0.1075f, .0f, .0f),
            gfxm::vec3(0.1075f, .0f, .0f)
        };
        ears[0] = listener_transform * gfxm::vec4(ears[0], 1.0f);
        ears[1] = listener_transform * gfxm::vec4(ears[1], 1.0f);
        constexpr float EMITTER_RADIUS = 0.5f;
        float att = 1.0f / atten_radius;
        float falloff[2] = {
            std::min(1.0f / pow((gfxm::length(ears[0] - pos) * att), 2.0f), 1.0f),
            std::min(1.0f / pow((gfxm::length(ears[1] - pos) * att), 2.0f), 1.0f)
        };        

        int samples_till_end = src_len - src_cur;
        int len_to_sample = looping ? dst_len : std::min((int)dst_len, samples_till_end);
        for (int di = 0; di < len_to_sample; di += dst_channelCount) {
            int step = di / dst_channelCount;
            int si = (int)((src_cur + step / invSampleRatio)) % src_len;
            float s = src[si] * flt_convert * gain;
            dst[di] += s * falloff[0]; // left
            dst[di + 1] += s * falloff[1]; // right
        }

        return (int)(dst_len / dst_channelCount / invSampleRatio);
    }
};

inline AudioMixer& audio() {
    static AudioMixer mixer;
    return mixer;
}

#endif
