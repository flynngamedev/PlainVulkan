// audio.cpp -- Pv::LoadSound / PlaySound / ... via miniaudio (vendored in
// runtime/third_party/). WAV/MP3/FLAC decode via miniaudio's built-in
// decoders with zero extra wiring; OGG decodes via a small custom
// ma_decoding_backend_vtable that wraps miniaudio's built-in
// ma_stbvorbis_* helpers around the vendored stb_vorbis.c -- this is
// miniaudio's own documented extension point (see the "Custom Decoders"
// section of miniaudio.h), not a hack.
//
// music vs. sound: Pv::LoadMusic just sets `isMusic` on the same
// ma_sound-backed record as Pv::LoadSound (miniaudio's ma_sound already
// handles streaming vs. fully-decoded-in-memory internally based on
// flags), so PlayMusic/StopMusic/SetMusicVolume reuse the sound path.

// 1) stb_vorbis declarations only (implementation included again at the
//    bottom of this file, after miniaudio.h has seen the declarations).
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

// 2) miniaudio, with STB_VORBIS_INCLUDE_STB_VORBIS_H telling it the
//    stb_vorbis API above is available so it compiles its ma_stbvorbis_*
//    wrapper functions around it.
#define MINIAUDIO_IMPLEMENTATION
#define STB_VORBIS_INCLUDE_STB_VORBIS_H
#include "miniaudio.h"

// 3) stb_vorbis implementation.
#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#include "pv/pv_internal.h"
#include "pv/pv_runtime.h"

namespace pv {

// ---- custom OGG/Vorbis decoding backend for miniaudio's resource manager ----
static ma_result vorbis_init(void* pUserData, ma_read_proc onRead, ma_seek_proc onSeek, ma_tell_proc onTell,
                              void* pReadSeekTellUserData, const ma_decoding_backend_config* pConfig,
                              const ma_allocation_callbacks* pAllocationCallbacks, ma_data_source** ppBackend) {
    (void)pUserData;
    auto* pVorbis = static_cast<ma_stbvorbis*>(ma_malloc(sizeof(ma_stbvorbis), pAllocationCallbacks));
    if (!pVorbis) return MA_OUT_OF_MEMORY;
    ma_result result =
        ma_stbvorbis_init(onRead, onSeek, onTell, pReadSeekTellUserData, pConfig, pAllocationCallbacks, pVorbis);
    if (result != MA_SUCCESS) {
        ma_free(pVorbis, pAllocationCallbacks);
        return result;
    }
    *ppBackend = reinterpret_cast<ma_data_source*>(pVorbis);
    return MA_SUCCESS;
}
static ma_result vorbis_init_file(void* pUserData, const char* pFilePath, const ma_decoding_backend_config* pConfig,
                                   const ma_allocation_callbacks* pAllocationCallbacks, ma_data_source** ppBackend) {
    (void)pUserData;
    auto* pVorbis = static_cast<ma_stbvorbis*>(ma_malloc(sizeof(ma_stbvorbis), pAllocationCallbacks));
    if (!pVorbis) return MA_OUT_OF_MEMORY;
    ma_result result = ma_stbvorbis_init_file(pFilePath, pConfig, pAllocationCallbacks, pVorbis);
    if (result != MA_SUCCESS) {
        ma_free(pVorbis, pAllocationCallbacks);
        return result;
    }
    *ppBackend = reinterpret_cast<ma_data_source*>(pVorbis);
    return MA_SUCCESS;
}
static void vorbis_uninit(void* pUserData, ma_data_source* pBackend,
                           const ma_allocation_callbacks* pAllocationCallbacks) {
    (void)pUserData;
    auto* pVorbis = reinterpret_cast<ma_stbvorbis*>(pBackend);
    ma_stbvorbis_uninit(pVorbis, pAllocationCallbacks);
    ma_free(pVorbis, pAllocationCallbacks);
}
static ma_decoding_backend_vtable g_vorbisVTable = {vorbis_init, vorbis_init_file, nullptr, nullptr, vorbis_uninit};

static ma_engine* maEngine() { return static_cast<ma_engine*>(engine().audioEngine); }

static void ensureAudioInit() {
    auto& e = engine();
    if (e.audioInitialized) return;

    auto* rm = new ma_resource_manager();
    ma_resource_manager_config rmConfig = ma_resource_manager_config_init();
    static ma_decoding_backend_vtable* vtables[] = {&g_vorbisVTable};
    rmConfig.ppCustomDecodingBackendVTables = vtables;
    rmConfig.customDecodingBackendCount = 1;
    if (ma_resource_manager_init(&rmConfig, rm) != MA_SUCCESS) {
        logLine("ERROR", "audio: ma_resource_manager_init failed; OGG playback won't be available");
        delete rm;
        rm = nullptr;
    }
    e.audioResourceManager = rm;

    auto* eng = new ma_engine();
    ma_engine_config engConfig = ma_engine_config_init();
    if (rm) engConfig.pResourceManager = rm;
    if (ma_engine_init(&engConfig, eng) != MA_SUCCESS) {
        logLine("ERROR", "audio: ma_engine_init failed; Pv::LoadSound/PlaySound/... will be no-ops");
        delete eng;
        eng = nullptr;
    }
    e.audioEngine = eng;
    e.audioInitialized = true;
}

namespace rt {

static Value loadSoundImpl(const std::string& path, bool isMusic) {
    ensureAudioInit();
    if (!maEngine()) return Value::MakeHandle(0, "sound");
    auto* sound = new ma_sound();
    ma_result r = ma_sound_init_from_file(maEngine(), path.c_str(),
                                           MA_SOUND_FLAG_DECODE | (isMusic ? MA_SOUND_FLAG_STREAM : 0), nullptr,
                                           nullptr, sound);
    if (r != MA_SUCCESS) {
        logLine("ERROR", "Pv::Load" + std::string(isMusic ? "Music" : "Sound") + " failed for '" + path +
                              "' (unsupported format, or file not found)");
        delete sound;
        return Value::MakeHandle(0, "sound");
    }
    SoundRecord rec;
    rec.path = path;
    rec.isMusic = isMusic;
    rec.maSound = sound;
    return Value::MakeHandle(engine().sounds.add(rec), "sound");
}

Value LoadSound(const Value& filepath) { return loadSoundImpl(filepath.asString(), false); }
Value LoadMusic(const Value& filepath) { return loadSoundImpl(filepath.asString(), true); }

static ma_sound* getSound(const Value& handle) {
    auto* rec = engine().sounds.get(static_cast<uint64_t>(handle.asInt()));
    return rec ? static_cast<ma_sound*>(rec->maSound) : nullptr;
}

Value UnloadSound(const Value& sound) {
    uint64_t id = static_cast<uint64_t>(sound.asInt());
    if (auto* rec = engine().sounds.get(id)) {
        auto* s = static_cast<ma_sound*>(rec->maSound);
        if (s) {
            ma_sound_uninit(s);
            delete s;
        }
        engine().sounds.remove(id);
    }
    return Value();
}
Value PlaySound(const Value& sound) {
    if (auto* s = getSound(sound)) {
        ma_sound_seek_to_pcm_frame(s, 0);
        ma_sound_start(s);
    }
    return Value();
}
Value StopSound(const Value& sound) {
    if (auto* s = getSound(sound)) ma_sound_stop(s);
    return Value();
}
Value PauseSound(const Value& sound) {
    if (auto* s = getSound(sound)) ma_sound_stop(s); // miniaudio resumes from position on start; matches "pause"
    return Value();
}
Value SetSoundVolume(const Value& sound, const Value& volume) {
    if (auto* s = getSound(sound)) ma_sound_set_volume(s, static_cast<float>(volume.asFloat()));
    return Value();
}
Value SetSoundPitch(const Value& sound, const Value& pitch) {
    if (auto* s = getSound(sound)) ma_sound_set_pitch(s, static_cast<float>(pitch.asFloat()));
    return Value();
}
Value PlayMusic(const Value& music) { return PlaySound(music); }
Value StopMusic(const Value& music) { return StopSound(music); }
Value SetMusicVolume(const Value& music, const Value& volume) { return SetSoundVolume(music, volume); }
Value SetSound3D(const Value& sound, const Value& x, const Value& y, const Value& z) {
    if (auto* s = getSound(sound)) {
        ma_sound_set_position(s, static_cast<float>(x.asFloat()), static_cast<float>(y.asFloat()),
                               static_cast<float>(z.asFloat()));
        ma_sound_set_spatialization_enabled(s, MA_TRUE);
    }
    return Value();
}

} // namespace rt

void shutdownAudio() {
    auto& e = engine();
    if (!e.audioInitialized) return;
    for (auto& kv : e.sounds) {
        if (kv.second.maSound) {
            ma_sound_uninit(static_cast<ma_sound*>(kv.second.maSound));
            delete static_cast<ma_sound*>(kv.second.maSound);
        }
    }
    if (e.audioEngine) {
        ma_engine_uninit(static_cast<ma_engine*>(e.audioEngine));
        delete static_cast<ma_engine*>(e.audioEngine);
        e.audioEngine = nullptr;
    }
    if (e.audioResourceManager) {
        ma_resource_manager_uninit(static_cast<ma_resource_manager*>(e.audioResourceManager));
        delete static_cast<ma_resource_manager*>(e.audioResourceManager);
        e.audioResourceManager = nullptr;
    }
    e.audioInitialized = false;
}

} // namespace pv
