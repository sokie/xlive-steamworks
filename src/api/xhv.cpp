// #5008 XHVCreateEngine: Steam voice capture and decode, waveOut playback
#include "xlive/xfuncs.h"

#include "core/config.h"
#include "core/log.h"
#include "core/steam.h"
#include "core/users.h"

#include <mmsystem.h>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace {

#pragma pack(push, 1)
struct VoiceFrameHeader {
	uint16_t size;
};
#pragma pack(pop)

const int kPlaybackBuffers = 8;
const DWORD kTalkingWindowMs = 300;

class Playback {
public:
	explicit Playback(uint32_t sampleRate) : m_sampleRate(sampleRate) {}

	~Playback()
	{
		if (m_device) {
			waveOutReset(m_device);
			for (WAVEHDR& header : m_headers) {
				if (header.dwFlags & WHDR_PREPARED) {
					waveOutUnprepareHeader(m_device, &header, sizeof(header));
				}
			}
			waveOutClose(m_device);
		}
	}

	void Submit(const int16_t* samples, size_t count)
	{
		if (!m_device && !Open()) {
			return;
		}
		for (size_t i = 0; i < m_headers.size(); i++) {
			if ((m_headers[i].dwFlags & WHDR_DONE) || !(m_headers[i].dwFlags & WHDR_PREPARED)) {
				if (m_headers[i].dwFlags & WHDR_PREPARED) {
					waveOutUnprepareHeader(m_device, &m_headers[i], sizeof(WAVEHDR));
				}
				m_buffers[i].assign(samples, samples + count);
				memset(&m_headers[i], 0, sizeof(WAVEHDR));
				m_headers[i].lpData = (LPSTR)m_buffers[i].data();
				m_headers[i].dwBufferLength = (DWORD)(count * sizeof(int16_t));
				if (waveOutPrepareHeader(m_device, &m_headers[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
					waveOutWrite(m_device, &m_headers[i], sizeof(WAVEHDR));
				}
				return;
			}
		}
	}

	void SetVolume(float volume)
	{
		if (m_device) {
			DWORD level = (DWORD)(volume * 0xFFFF);
			waveOutSetVolume(m_device, (level << 16) | level);
		}
	}

private:
	bool Open()
	{
		WAVEFORMATEX format = {};
		format.wFormatTag = WAVE_FORMAT_PCM;
		format.nChannels = 1;
		format.nSamplesPerSec = m_sampleRate;
		format.wBitsPerSample = 16;
		format.nBlockAlign = 2;
		format.nAvgBytesPerSec = m_sampleRate * 2;
		if (waveOutOpen(&m_device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
			m_device = nullptr;
			return false;
		}
		m_headers.resize(kPlaybackBuffers);
		m_buffers.resize(kPlaybackBuffers);
		for (WAVEHDR& header : m_headers) {
			memset(&header, 0, sizeof(header));
		}
		return true;
	}

	uint32_t m_sampleRate;
	HWAVEOUT m_device = nullptr;
	std::vector<WAVEHDR> m_headers;
	std::vector<std::vector<int16_t>> m_buffers;
};

struct RemoteTalker {
	std::unique_ptr<Playback> playback;
	DWORD lastDataTick = 0;
	XHV_PLAYBACK_PRIORITY priority[XUSER_MAX_COUNT] = { 0, 0, 0, 0 };
	bool processing = false;
};

class SteamVoiceEngine : public IXHVEngine {
public:
	SteamVoiceEngine()
	{
		InitializeCriticalSection(&m_lock);
		m_sampleRate = xls::SteamReady() ? xls::SteamUser()->GetVoiceOptimalSampleRate() : 16000;
	}

	~SteamVoiceEngine()
	{
		StopRecording();
		DeleteCriticalSection(&m_lock);
	}

	LONG __stdcall AddRef() override { return InterlockedIncrement(&m_refs); }

	LONG __stdcall Release() override
	{
		LONG refs = InterlockedDecrement(&m_refs);
		if (refs == 0) {
			delete this;
		}
		return refs;
	}

	HRESULT __stdcall Lock(XHV_LOCK_TYPE lockType) override
	{
		switch (lockType) {
			case XHV_LOCK_TYPE_LOCK: EnterCriticalSection(&m_lock); return S_OK;
			case XHV_LOCK_TYPE_TRYLOCK: return TryEnterCriticalSection(&m_lock) ? S_OK : E_FAIL;
			case XHV_LOCK_TYPE_UNLOCK: LeaveCriticalSection(&m_lock); return S_OK;
			default: return E_INVALIDARG;
		}
	}

	HRESULT __stdcall StartLocalProcessingModes(DWORD userIndex, const XHV_PROCESSING_MODE* modes, DWORD count) override
	{
		if (userIndex >= XUSER_MAX_COUNT || !modes) return E_INVALIDARG;
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		for (DWORD i = 0; i < count; i++) {
			if (modes[i] == XHV_VOICECHAT_MODE) m_localChat[userIndex] = true;
			if (modes[i] == XHV_LOOPBACK_MODE) m_localLoopback[userIndex] = true;
		}
		if (userIndex == 0) {
			StartRecording();
		}
		return S_OK;
	}

	HRESULT __stdcall StopLocalProcessingModes(DWORD userIndex, const XHV_PROCESSING_MODE* modes, DWORD count) override
	{
		if (userIndex >= XUSER_MAX_COUNT || !modes) return E_INVALIDARG;
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		for (DWORD i = 0; i < count; i++) {
			if (modes[i] == XHV_VOICECHAT_MODE) m_localChat[userIndex] = false;
			if (modes[i] == XHV_LOOPBACK_MODE) m_localLoopback[userIndex] = false;
		}
		if (userIndex == 0 && !m_localChat[0] && !m_localLoopback[0]) {
			StopRecording();
		}
		return S_OK;
	}

	HRESULT __stdcall StartRemoteProcessingModes(XUID xuid, const XHV_PROCESSING_MODE* modes, DWORD count) override
	{
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		auto it = m_remote.find(xuid);
		if (it == m_remote.end()) return E_INVALIDARG;
		it->second.processing = true;
		return S_OK;
	}

	HRESULT __stdcall StopRemoteProcessingModes(XUID xuid, const XHV_PROCESSING_MODE* modes, DWORD count) override
	{
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		auto it = m_remote.find(xuid);
		if (it == m_remote.end()) return E_INVALIDARG;
		it->second.processing = false;
		return S_OK;
	}

	HRESULT __stdcall SetMaxDecodePackets(DWORD maxDecodePackets) override { return S_OK; }

	HRESULT __stdcall RegisterLocalTalker(DWORD userIndex) override
	{
		if (userIndex >= XUSER_MAX_COUNT) return E_INVALIDARG;
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		m_localRegistered[userIndex] = true;
		return S_OK;
	}

	HRESULT __stdcall UnregisterLocalTalker(DWORD userIndex) override
	{
		if (userIndex >= XUSER_MAX_COUNT) return E_INVALIDARG;
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		m_localRegistered[userIndex] = false;
		m_localChat[userIndex] = false;
		m_localLoopback[userIndex] = false;
		if (userIndex == 0) {
			StopRecording();
		}
		return S_OK;
	}

	HRESULT __stdcall RegisterRemoteTalker(XUID xuid, XAUDIOVOICEFXCHAIN*, XAUDIOVOICEFXCHAIN*, XAUDIOSUBMIXVOICE*) override
	{
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		if (m_remote.size() >= XHV_MAX_REMOTE_TALKERS) return E_OUTOFMEMORY;
		RemoteTalker& talker = m_remote[xuid];
		talker.processing = true;
		return S_OK;
	}

	HRESULT __stdcall UnregisterRemoteTalker(XUID xuid) override
	{
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		return m_remote.erase(xuid) ? S_OK : E_INVALIDARG;
	}

	HRESULT __stdcall GetRemoteTalkers(DWORD* count, XUID* xuids) override
	{
		if (!count) return E_POINTER;
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		DWORD i = 0;
		for (const auto& entry : m_remote) {
			if (xuids && i < *count) {
				xuids[i] = entry.first;
			}
			i++;
		}
		*count = (DWORD)m_remote.size();
		return S_OK;
	}

	BOOL __stdcall IsHeadsetPresent(DWORD userIndex) override
	{
		return userIndex == 0 && xls::Cfg().voiceEnabled && xls::SteamReady() ? TRUE : FALSE;
	}

	BOOL __stdcall IsLocalTalking(DWORD userIndex) override
	{
		return userIndex == 0 && GetTickCount() - m_lastLocalDataTick < kTalkingWindowMs ? TRUE : FALSE;
	}

	BOOL __stdcall IsRemoteTalking(XUID xuid) override
	{
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		auto it = m_remote.find(xuid);
		return it != m_remote.end() && GetTickCount() - it->second.lastDataTick < kTalkingWindowMs ? TRUE : FALSE;
	}

	DWORD __stdcall GetDataReadyFlags() override
	{
		if (!m_recording || !xls::SteamReady()) {
			return 0;
		}
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		PullVoice();
		return m_pendingLocal.empty() ? 0 : 1;
	}

	HRESULT __stdcall GetLocalChatData(DWORD userIndex, uint8_t* data, DWORD* size, DWORD* packets) override
	{
		if (userIndex != 0 || !data || !size) return E_INVALIDARG;
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		PullVoice();
		DWORD written = 0;
		DWORD count = 0;
		while (!m_pendingLocal.empty()) {
			const std::vector<uint8_t>& frame = m_pendingLocal.front();
			DWORD needed = (DWORD)(sizeof(VoiceFrameHeader) + frame.size());
			if (written + needed > *size) {
				break;
			}
			VoiceFrameHeader header = { (uint16_t)frame.size() };
			memcpy(data + written, &header, sizeof(header));
			memcpy(data + written + sizeof(header), frame.data(), frame.size());
			written += needed;
			count++;
			if (m_localLoopback[0]) {
				Play(xls::UserXuid(0), frame.data(), (uint32_t)frame.size());
			}
			m_pendingLocal.pop_front();
		}
		*size = written;
		if (packets) {
			*packets = count;
		}
		return written ? S_OK : E_PENDING;
	}

	HRESULT __stdcall SetPlaybackPriority(XUID xuid, DWORD userIndex, XHV_PLAYBACK_PRIORITY priority) override
	{
		if (userIndex >= XUSER_MAX_COUNT) return E_INVALIDARG;
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		auto it = m_remote.find(xuid);
		if (it == m_remote.end()) return E_INVALIDARG;
		it->second.priority[userIndex] = priority;
		return S_OK;
	}

	HRESULT __stdcall SubmitIncomingChatData(XUID xuid, const uint8_t* data, DWORD* size) override
	{
		if (!data || !size) return E_POINTER;
		std::lock_guard<std::recursive_mutex> guard(m_mutex);
		auto it = m_remote.find(xuid);
		if (it == m_remote.end()) return E_INVALIDARG;
		DWORD consumed = 0;
		while (consumed + sizeof(VoiceFrameHeader) <= *size) {
			VoiceFrameHeader header;
			memcpy(&header, data + consumed, sizeof(header));
			if (consumed + sizeof(header) + header.size > *size) {
				break;
			}
			it->second.lastDataTick = GetTickCount();
			bool muted = it->second.priority[0] == XHV_PLAYBACK_PRIORITY_NEVER;
			if (it->second.processing && !muted) {
				Play(xuid, data + consumed + sizeof(header), header.size);
			}
			consumed += (DWORD)(sizeof(header) + header.size);
		}
		*size = consumed;
		return S_OK;
	}

private:
	void StartRecording()
	{
		if (m_recording || !xls::SteamReady() || !xls::Cfg().voiceEnabled) {
			return;
		}
		xls::SteamUser()->StartVoiceRecording();
		xls::SteamFriends()->SetInGameVoiceSpeaking(xls::SteamLocalId(), true);
		m_recording = true;
	}

	void StopRecording()
	{
		if (!m_recording) {
			return;
		}
		if (xls::SteamReady()) {
			xls::SteamUser()->StopVoiceRecording();
			xls::SteamFriends()->SetInGameVoiceSpeaking(xls::SteamLocalId(), false);
		}
		m_recording = false;
	}

	// Moves whatever Steam has compressed into the pending frame queue.
	void PullVoice()
	{
		if (!m_recording || !xls::SteamReady()) {
			return;
		}
		uint32 available = 0;
		while (xls::SteamUser()->GetAvailableVoice(&available) == k_EVoiceResultOK && available) {
			std::vector<uint8_t> frame(available > 8192 ? 8192 : available);
			uint32 written = 0;
			EVoiceResult result = xls::SteamUser()->GetVoice(true, frame.data(), (uint32)frame.size(), &written);
			if (result != k_EVoiceResultOK || !written) {
				break;
			}
			frame.resize(written);
			// Frames larger than the wire header allows are split.
			size_t offset = 0;
			while (offset < frame.size()) {
				size_t chunk = std::min<size_t>(frame.size() - offset, 0xFFFF);
				m_pendingLocal.emplace_back(frame.begin() + offset, frame.begin() + offset + chunk);
				offset += chunk;
			}
			m_lastLocalDataTick = GetTickCount();
			if (m_pendingLocal.size() > 64) {
				m_pendingLocal.pop_front();
			}
		}
	}

	void Play(XUID xuid, const uint8_t* compressed, uint32_t size)
	{
		if (!xls::SteamReady()) {
			return;
		}
		std::vector<int16_t> pcm(m_sampleRate); // Up to one second per frame.
		uint32 written = 0;
		EVoiceResult result = xls::SteamUser()->DecompressVoice(compressed, size, pcm.data(), (uint32)(pcm.size() * sizeof(int16_t)), &written, m_sampleRate);
		if (result != k_EVoiceResultOK || !written) {
			return;
		}
		RemoteTalker& talker = m_remote[xuid];
		if (!talker.playback) {
			talker.playback = std::make_unique<Playback>(m_sampleRate);
		}
		talker.playback->Submit(pcm.data(), written / sizeof(int16_t));
	}

	LONG m_refs = 1;
	CRITICAL_SECTION m_lock;
	std::recursive_mutex m_mutex;
	uint32_t m_sampleRate;
	bool m_recording = false;
	bool m_localRegistered[XUSER_MAX_COUNT] = {};
	bool m_localChat[XUSER_MAX_COUNT] = {};
	bool m_localLoopback[XUSER_MAX_COUNT] = {};
	std::deque<std::vector<uint8_t>> m_pendingLocal;
	DWORD m_lastLocalDataTick = 0;
	std::map<XUID, RemoteTalker> m_remote;
};

}

// #5008
HRESULT WINAPI XHVCreateEngine(XHV_INIT_PARAMS* pParams, HANDLE* phWorkerThread, IXHVEngine** ppEngine)
{
	XLS_TRACE_FN();
	if (!pParams || !ppEngine) {
		return E_POINTER;
	}
	if (pParams->dwMaxRemoteTalkers > XHV_MAX_REMOTE_TALKERS || pParams->dwMaxLocalTalkers > XHV_MAX_LOCAL_TALKERS) {
		return E_INVALIDARG;
	}
	*ppEngine = new SteamVoiceEngine();
	if (phWorkerThread) {
		// No worker thread: Steam does the capture. A signalled event stands in for a running thread.
		*phWorkerThread = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	}
	XLS_LOG_INFO("xhv: engine created (%u local, %u remote talkers).", pParams->dwMaxLocalTalkers, pParams->dwMaxRemoteTalkers);
	return S_OK;
}
