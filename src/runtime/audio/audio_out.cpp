#include "runtime/audio/audio_out.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <cstring>

#include "runtime/log.h"

namespace rt::audio {
namespace {

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
const GUID kFloatSubtype = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

IAudioClient* g_client = nullptr;
IAudioRenderClient* g_render = nullptr;
UINT32 g_buffer_frames = 0;
bool g_ok = false;

}  // namespace

bool out_init() {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  (void)hr;
  IMMDeviceEnumerator* en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&en)))) {
    LOG_W("audio", "no MMDeviceEnumerator; audio output disabled");
    return false;
  }
  IMMDevice* dev = nullptr;
  if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
    LOG_W("audio", "no default audio endpoint; audio output disabled");
    en->Release();
    return false;
  }
  en->Release();
  if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&g_client)))) {
    dev->Release();
    return false;
  }
  dev->Release();
  WAVEFORMATEXTENSIBLE wf{};
  wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wf.Format.nChannels = 2;
  wf.Format.nSamplesPerSec = 48000;
  wf.Format.wBitsPerSample = 32;
  wf.Format.nBlockAlign = 8;
  wf.Format.nAvgBytesPerSec = 48000 * 8;
  wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  wf.Samples.wValidBitsPerSample = 32;
  wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  wf.SubFormat = kFloatSubtype;
  REFERENCE_TIME dur = 1000000;  // 100 ms buffer
  hr = g_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, dur, 0,
                            reinterpret_cast<WAVEFORMATEX*>(&wf), nullptr);
  if (FAILED(hr)) {
    LOG_W("audio", "IAudioClient::Initialize failed 0x%08lx; audio output disabled", hr);
    return false;
  }
  g_client->GetBufferSize(&g_buffer_frames);
  if (FAILED(g_client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&g_render)))) return false;
  g_client->Start();
  g_ok = true;
  LOG_I("audio", "WASAPI output: 48 kHz float stereo, device buffer %u frames", g_buffer_frames);
  return true;
}

bool out_available() { return g_ok; }

u32 out_queued_frames() {
  if (!g_ok) return 0;
  UINT32 pad = 0;
  g_client->GetCurrentPadding(&pad);
  return pad;
}

void out_write(const float* stereo, u32 frames) {
  if (!g_ok) return;
  UINT32 pad = 0;
  g_client->GetCurrentPadding(&pad);
  if (pad + frames > g_buffer_frames) return;  // device full: drop (should not happen with pacing)
  BYTE* p = nullptr;
  if (FAILED(g_render->GetBuffer(frames, &p))) return;
  std::memcpy(p, stereo, size_t(frames) * 8);
  g_render->ReleaseBuffer(frames, 0);
}

}  // namespace rt::audio
