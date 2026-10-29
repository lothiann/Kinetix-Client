#include "media_info.h"

#include <base/detect.h>
#include <base/str.h>
#include <base/time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Leaked on purpose: the polling thread is detached and may still touch these
// during process exit, after normal static destruction would have run.
static SKxMediaInfo &s_Info = *new SKxMediaInfo();
static std::mutex &s_Mutex = *new std::mutex();
static std::atomic<bool> s_Started{false};
static std::atomic<int> s_PendingMediaCmd{-1}; // 0 = prev, 1 = play/pause, 2 = next

// Monotonic clock used by KxMedia::GetPosition() on every platform. Must live
// outside the Windows-only block: on Android/Linux the function body below is
// not compiled, but GetPosition() still calls it.
static double KxNowSeconds()
{
	return (double)time_get() / (double)time_freq();
}

// ----------------------------------------------------------------------------
// Windows backend: SMTC for metadata/timeline/art + a WASAPI loopback capture
// of the default render endpoint for the visualizer spectrum bars.
// ----------------------------------------------------------------------------
#if defined(CONF_FAMILY_WINDOWS) && __has_include(<winrt/base.h>)
#define KX_MEDIA_WINRT 1

// The project defines NOGDI globally, which hides BITMAPINFOHEADER from
// mmreg.h (pulled in by audioclient.h). Include the GDI-dependent SDK
// headers once with NOGDI lifted, like code_exec/file_dialog.cpp does.
#pragma push_macro("NOGDI")
#undef NOGDI
#include <windows.h>
#include <ole2.h> // CreateStreamOnHGlobal
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wincodec.h>
#pragma pop_macro("NOGDI")

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt::Windows::Storage::Streams;

namespace
{
	constexpr int KX_FFT_SIZE = 2048;
	constexpr float KX_BAND_MIN_HZ = 30.0f;
	constexpr float KX_BAND_MAX_HZ = 16000.0f;

	// In-place iterative radix-2 Cooley-Tukey FFT.
	void KxFft(std::complex<double> *pData, int N)
	{
		for(int i = 1, j = 0; i < N; ++i)
		{
			int bit = N >> 1;
			for(; j & bit; bit >>= 1)
				j ^= bit;
			j ^= bit;
			if(i < j)
				std::swap(pData[i], pData[j]);
		}
		for(int Len = 2; Len <= N; Len <<= 1)
		{
			const double Angle = -2.0 * 3.14159265358979323846264 / Len;
			const std::complex<double> WLen(std::cos(Angle), std::sin(Angle));
			for(int i = 0; i < N; i += Len)
			{
				std::complex<double> W(1.0, 0.0);
				for(int k = 0; k < Len / 2; ++k)
				{
					const std::complex<double> U = pData[i + k];
					const std::complex<double> V = pData[i + k + Len / 2] * W;
					pData[i + k] = U + V;
					pData[i + k + Len / 2] = U - V;
					W *= WLen;
				}
			}
		}
	}
}

// v6.7.1: real spectrum visualizer. Captures whatever Windows plays on the
// default render endpoint via WASAPI loopback and folds the newest ~43 ms of
// samples into NUM_BANDS log-spaced level bars with an FFT (Hann window,
// 50% frame overlap). Replaces the old single peak-meter wobble.
class CKxLoopback
{
public:
	~CKxLoopback()
	{
		Stop();
	}

	bool Ok() const { return m_pCapture != nullptr; }

	bool TryStart()
	{
		Stop();
		winrt::com_ptr<IMMDeviceEnumerator> pEnum;
		if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
			       __uuidof(IMMDeviceEnumerator), pEnum.put_void())))
			return false;
		winrt::com_ptr<IMMDevice> pDevice;
		if(FAILED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, pDevice.put())))
			return false;
		if(FAILED(pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, m_pClient.put_void())))
			return false;
		WAVEFORMATEX *pFormat = nullptr;
		if(FAILED(m_pClient->GetMixFormat(&pFormat)) || !pFormat)
			return false;
		m_SampleRate = std::max<uint32_t>(8000, pFormat->nSamplesPerSec);
		m_Channels = std::max<uint32_t>(1, pFormat->nChannels);
		m_Float = pFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
		if(pFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
		{
			static const GUID FloatGuid = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
			m_Float = IsEqualGUID(((const WAVEFORMATEXTENSIBLE *)pFormat)->SubFormat, FloatGuid);
		}
		const HRESULT Hr = m_pClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, pFormat, nullptr);
		CoTaskMemFree(pFormat);
		if(FAILED(Hr) || FAILED(m_pClient->GetService(__uuidof(IAudioCaptureClient), m_pCapture.put_void())))
		{
			Stop();
			return false;
		}
		m_pClient->Start();
		return true;
	}

	void Stop()
	{
		if(m_pClient)
			m_pClient->Stop();
		m_pCapture = nullptr;
		m_pClient = nullptr;
		m_vRing.clear();
		for(float &Peak : m_aBandMax)
			Peak = 0.0f;
	}

	// Moves new loopback audio into the ring buffer. Returns false when the
	// capture broke and a restart is needed.
	bool Drain()
	{
		if(!m_pCapture)
			return true;
		UINT32 Packet = 0;
		while(SUCCEEDED(m_pCapture->GetNextPacketSize(&Packet)) && Packet > 0)
		{
			BYTE *pData = nullptr;
			UINT32 Frames = 0;
			DWORD Flags = 0;
			if(FAILED(m_pCapture->GetBuffer(&pData, &Frames, &Flags, nullptr, nullptr)))
				return false;
			if(Flags & AUDCLNT_BUFFERFLAGS_SILENT)
			{
				m_vRing.insert(m_vRing.end(), Frames, 0.0f);
			}
			else if(m_Float)
			{
				const float *pSamples = (const float *)pData;
				for(UINT32 f = 0; f < Frames; ++f)
				{
					float Sum = 0.0f;
					for(UINT32 c = 0; c < m_Channels; ++c)
						Sum += pSamples[f * m_Channels + c];
					m_vRing.push_back(Sum / m_Channels);
				}
			}
			else
			{
				const int16_t *pSamples = (const int16_t *)pData;
				for(UINT32 f = 0; f < Frames; ++f)
				{
					int Sum = 0;
					for(UINT32 c = 0; c < m_Channels; ++c)
						Sum += pSamples[f * m_Channels + c];
					m_vRing.push_back((Sum / (int)m_Channels) / 32768.0f);
				}
			}
			m_pCapture->ReleaseBuffer(Frames);
			// Cap the ring so a stalled consumer cannot grow it unboundedly.
			if(m_vRing.size() > (size_t)KX_FFT_SIZE * 4)
				m_vRing.erase(m_vRing.begin(), m_vRing.end() - KX_FFT_SIZE * 2);
		}
		return true;
	}

	// Folds the newest KX_FFT_SIZE samples into pBands (0..1 levels).
	// pSmoothed carries the decay between calls (may be the same array).
	// Returns false while there is not enough audio buffered yet.
	bool ComputeBands(float *pBands, float *pSmoothed)
	{
		if((int)m_vRing.size() < KX_FFT_SIZE)
			return false;
		static std::complex<double> aData[KX_FFT_SIZE];
		const int N = KX_FFT_SIZE;
		for(int i = 0; i < N; ++i)
		{
			const double Sample = m_vRing[m_vRing.size() - N + i];
			const double Window = 0.5 * (1.0 - std::cos(2.0 * 3.14159265358979323846264 * i / (N - 1)));
			aData[i] = Sample * Window;
		}
		// 50% overlap keeps consecutive frames smooth.
		m_vRing.erase(m_vRing.begin(), m_vRing.end() - N / 2);
		KxFft(aData, N);

		const double BinHz = (double)m_SampleRate / N;
		int Bin0 = std::max(1, (int)std::floor(KX_BAND_MIN_HZ / BinHz));
		const double Norm = N / 4.0;
		const int Bands = SKxMediaInfo::NUM_BANDS;
		for(int b = 0; b < Bands; ++b)
		{
			const double Hz1 = KX_BAND_MIN_HZ * std::pow(KX_BAND_MAX_HZ / KX_BAND_MIN_HZ, (b + 1) / (double)Bands);
			const int Bin1 = std::max(Bin0 + 1, (int)std::ceil(Hz1 / BinHz));
			double Mag = 0.0;
			int Count = 0;
			for(int k = Bin0; k < Bin1 && k < N / 2; ++k)
			{
				Mag += std::abs(aData[k]);
				++Count;
			}
			Bin0 = Bin1;
			Mag = Count > 0 ? Mag / Count / Norm : 0.0; // ≈ linear amplitude, 1.0 ≈ 0 dBFS

			// Per-band auto-gain: normalize against a slowly decaying running
			// peak (halflife ~7 s at 50 ms ticks) so the bars fill the same
			// way at ANY playback volume. A silence gate keeps DAC/idle noise
			// from lighting the bars up.
			m_aBandMax[b] = std::max((float)Mag, m_aBandMax[b] * 0.995f);
			float Level = 0.0f;
			if(Mag > 5e-4)
			{
				const float Ref = std::max(m_aBandMax[b] * 0.6f, 2e-3f);
				Level = std::clamp((float)Mag / Ref, 0.0f, 1.0f);
			}
			pBands[b] = std::max(Level, pSmoothed[b] * 0.80f);
			pSmoothed[b] = pBands[b];
		}
		return true;
	}

private:
	winrt::com_ptr<IAudioClient> m_pClient;
	winrt::com_ptr<IAudioCaptureClient> m_pCapture;
	uint32_t m_SampleRate = 48000;
	uint32_t m_Channels = 2;
	bool m_Float = true;
	float m_aBandMax[SKxMediaInfo::NUM_BANDS] = {};
	std::vector<float> m_vRing;
};

// Shared state of the polling thread. Leaked like s_Info — the detached
// thread outlives static destruction.
struct KxMediaState
{
	bool Valid = false;
	bool Playing = false;
	std::string Title;
	std::string Artist;
	double Duration = 0.0;
	// Position base: BasePos is valid at BaseTime; the HUD extrapolates from
	// there while the player reports "playing".
	double BasePos = 0.0;
	double BaseTime = 0.0;
	// Poll bookkeeping for the extrapolation logic.
	std::string LastTrack;
	std::string LastArtTitle;
	double LastReported = -1.0;
	bool WasPlaying = false;
	// Cached SMTC session manager (first RequestAsync is expensive). Lives
	// in this never-destructed heap struct because WinRT projection types
	// delete operator new and a function-local static would be destructed
	// while the detached thread may still use it.
	winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager Manager{nullptr};
	// WIC factory for the album art decode (created lazily).
	winrt::com_ptr<IWICImagingFactory> pWic;
	std::vector<uint8_t> vArt;
	int ArtW = 0;
	int ArtH = 0;
	uint64_t ArtSerial = 0;
	float aBands[SKxMediaInfo::NUM_BANDS] = {};
};
KxMediaState &s_State = *new KxMediaState();

// v6.7: WIC is the proper Windows way to decode a thumbnail of unknown
// container — SMTC covers come back as PNG, JPEG or WebP depending on the
// player, and the old PNG-only path left the HUD without art.
static bool KxDecodeImageToRgba(IWICImagingFactory *pFactory, const std::vector<uint8_t> &vIn, std::vector<uint8_t> &vOut, int &W, int &H)
{
	// CreateDecoderFromMemory only exists on IWICImagingFactory2, which the
	// project's NTDDI_VERSION (Win7) hides. Wrap the bytes in an IStream and
	// use CreateDecoderFromStream on the base factory instead.
	HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, vIn.size());
	if(!hMem)
		return false;
	void *pMem = GlobalLock(hMem);
	if(!pMem)
	{
		GlobalFree(hMem);
		return false;
	}
	memcpy(pMem, vIn.data(), vIn.size());
	GlobalUnlock(hMem);
	winrt::com_ptr<IStream> pStream;
	if(FAILED(CreateStreamOnHGlobal(hMem, TRUE, pStream.put())))
	{
		GlobalFree(hMem); // stream not attached — free manually
		return false;
	}
	// stream owns hMem from here on
	winrt::com_ptr<IWICBitmapDecoder> pDecoder;
	if(FAILED(pFactory->CreateDecoderFromStream(pStream.get(), nullptr,
		       WICDecodeMetadataCacheOnDemand, pDecoder.put())))
		return false;
	winrt::com_ptr<IWICBitmapFrameDecode> pFrame;
	if(FAILED(pDecoder->GetFrame(0, pFrame.put())))
		return false;
	winrt::com_ptr<IWICFormatConverter> pConverter;
	if(FAILED(pFactory->CreateFormatConverter(pConverter.put())))
		return false;
	// 32bppRGBA, non-premultiplied (symbol from wincodec.h, links via uuid.lib).
	if(FAILED(pConverter->Initialize(pFrame.get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
		return false;
	UINT Width = 0, Height = 0;
	if(FAILED(pFrame->GetSize(&Width, &Height)) || Width == 0 || Height == 0 || Width > 4096 || Height > 4096)
		return false;
	vOut.resize((size_t)Width * Height * 4);
	if(FAILED(pConverter->CopyPixels(nullptr, Width * 4, (UINT)vOut.size(), vOut.data())))
	{
		vOut.clear();
		return false;
	}
	W = (int)Width;
	H = (int)Height;
	return true;
}

// Polls SMTC for metadata/timeline/art. Called every ~250 ms.
void KxPollSmtc()
{
	using namespace winrt::Windows::Media::Control;
	try
	{
		if(!s_State.Manager)
			s_State.Manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
		auto Session = s_State.Manager.GetCurrentSession();
		if(!Session)
		{
			s_State.Valid = false;
			return;
		}

		auto Props = Session.TryGetMediaPropertiesAsync().get();
		const std::string Title = winrt::to_string(Props.Title());
		const std::string Artist = winrt::to_string(Props.Artist());
		auto TL = Session.GetTimelineProperties();
		auto Playback = Session.GetPlaybackInfo();
		const bool Playing = Playback.PlaybackStatus() ==
			GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
		const double Reported = (double)TL.Position().count() / 10000000.0;
		const double Duration = (double)(TL.EndTime() - TL.StartTime()).count() / 10000000.0;
		const double Now = KxNowSeconds();
		const bool TrackChanged = Title != s_State.LastTrack;
		if(TrackChanged)
			s_State.LastTrack = Title;

		// SMTC tells us WHEN the reported position was valid (LastUpdatedTime).
		// Players push timeline updates with a lag — extrapolating the report
		// to "now" by its own timestamp removes that lag, so periodic pushes
		// no longer yank the displayed time backwards (v6.7 bug).
		double Effective = Reported;
		try
		{
			double Age = std::chrono::duration_cast<std::chrono::duration<double>>(
				winrt::clock::now() - TL.LastUpdatedTime())
				.count();
			if(Age >= 0.0 && Age < 3600.0)
				Effective = Reported + Age;
		}
		catch(...)
		{
		}

		// Many players (all browsers included) push the timeline only once per
		// track, so treat the SMTC report as a position BASE and extrapolate
		// locally. Reset the base only on real events — a re-poll of the same
		// stale value must NOT restart the clock, otherwise the HUD freezes
		// at the track start / resume timecode (v6.7 bug).
		if(!Playing)
		{
			// Paused: follow only fresh reports (seek, track switch). A stale
			// re-poll keeps our extrapolated pause position.
			if(Reported != s_State.LastReported)
			{
				s_State.BasePos = Reported;
				s_State.BaseTime = Now;
			}
		}
		else if(TrackChanged)
		{
			// New track — resync to the (lag-corrected) report.
			s_State.BasePos = Effective;
			s_State.BaseTime = Now;
		}
		else if(!s_State.WasPlaying)
		{
			// Just resumed: keep the last known position, restart the clock.
			s_State.BaseTime = Now;
		}
		else if(std::fabs(Effective - (s_State.BasePos + (Now - s_State.BaseTime))) > 1.0)
		{
			// Real jump (seek) or large drift — resync. Periodic timeline
			// pushes now arrive lag-corrected, so their deltas stay tiny and
			// never reset the clock.
			s_State.BasePos = Effective;
			s_State.BaseTime = Now;
		}
		s_State.LastReported = Reported;
		s_State.WasPlaying = Playing;

		s_State.Valid = true;
		s_State.Playing = Playing;
		s_State.Title = Title;
		s_State.Artist = Artist;
		s_State.Duration = Duration;

		// Art: fetch + WIC-decode once per track, retrying every poll until
		// it succeeds — the thumbnail stream is often not ready right after a
		// change, and a single failed attempt used to leave the HUD without
		// art forever.
		if(s_State.LastArtTitle != Title)
		{
			if(!s_State.pWic &&
				FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
					__uuidof(IWICImagingFactory), s_State.pWic.put_void())))
			{
			}
			auto Thumb = Props.Thumbnail();
			bool Done = false;
			if(Thumb)
			{
				try
				{
					auto Stream = Thumb.OpenReadAsync().get();
					const uint32_t Size = (uint32_t)Stream.Size();
					if(Size > 0 && Size < 8u * 1024u * 1024u)
					{
						DataReader Reader(Stream.GetInputStreamAt(0));
						Reader.LoadAsync(Size).get();
						std::vector<uint8_t> Data(Size);
						Reader.ReadBytes(winrt::array_view<uint8_t>(Data.data(), Data.data() + Data.size()));
						if(s_State.pWic && KxDecodeImageToRgba(s_State.pWic.get(), Data, s_State.vArt, s_State.ArtW, s_State.ArtH))
						{
							++s_State.ArtSerial;
							Done = true;
						}
					}
					else if(Size == 0)
					{
						s_State.vArt.clear();
						s_State.ArtW = 0;
						s_State.ArtH = 0;
						++s_State.ArtSerial;
						Done = true;
					}
				}
				catch(...)
				{
				}
			}
			else
			{
				s_State.vArt.clear();
				s_State.ArtW = 0;
				s_State.ArtH = 0;
				++s_State.ArtSerial;
				Done = true;
			}
			if(Done)
				s_State.LastArtTitle = Title;
		}
	}
	catch(...)
	{
	}
}

static void KxMediaThread()
{
	winrt::init_apartment(winrt::apartment_type::multi_threaded);

	CKxLoopback Loop;
	double LastLoopTry = 0.0;
	int Tick = 0;

	while(true)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		const double Now = KxNowSeconds();

		// Media transport commands (prev / play-pause / next) — the SMTC
		// session object belongs to this thread's apartment.
		const int Cmd = s_PendingMediaCmd.exchange(-1);
		if(Cmd >= 0)
		{
			try
			{
				auto Session = s_State.Manager.GetCurrentSession();
				if(Session)
				{
					if(Cmd == 0)
						Session.TrySkipPreviousAsync().get();
					else if(Cmd == 1)
						Session.TryTogglePlayPauseAsync().get();
					else if(Cmd == 2)
						Session.TrySkipNextAsync().get();
				}
			}
			catch(...)
			{
			}
		}

		// Visualizer: ~20 fps spectrum from the audio endpoint.
		if(!Loop.Ok() && Now - LastLoopTry > 2.0)
		{
			LastLoopTry = Now;
			Loop.TryStart();
		}
		if(Loop.Ok() && !Loop.Drain())
			Loop.Stop();
		if(!Loop.ComputeBands(s_State.aBands, s_State.aBands))
		{
			for(float &Level : s_State.aBands)
				Level *= 0.85f;
		}

		// Metadata / timeline / art every ~250 ms.
		if(Tick % 5 == 0)
			KxPollSmtc();

		// Publish a full snapshot. The art bytes ride along on EVERY publish —
		// previously they were only present on the single tick that fetched
		// them, so a render thread that missed that snapshot never decoded
		// the cover (v6.7 bug).
		{
			SKxMediaInfo Info;
			Info.m_Valid = s_State.Valid;
			Info.m_Playing = s_State.Valid && s_State.Playing;
			str_copy(Info.m_aTitle, s_State.Title.c_str(), sizeof(Info.m_aTitle));
			str_copy(Info.m_aArtist, s_State.Artist.c_str(), sizeof(Info.m_aArtist));
			Info.m_Duration = s_State.Duration;
			Info.m_Position = s_State.BasePos;
			Info.m_SampleTime = s_State.BaseTime;
			Info.m_vArt = s_State.vArt;
			Info.m_ArtW = s_State.ArtW;
			Info.m_ArtH = s_State.ArtH;
			Info.m_ArtSerial = s_State.ArtSerial;
			for(int i = 0; i < SKxMediaInfo::NUM_BANDS; ++i)
				Info.m_aBands[i] = s_State.aBands[i];
			std::lock_guard<std::mutex> Lock(s_Mutex);
			Info.m_Serial = s_Info.m_Serial + 1;
			s_Info = std::move(Info);
		}
		++Tick;
	}
}
#endif

void KxMedia::Update()
{
#if defined(KX_MEDIA_WINRT)
	if(!s_Started.exchange(true))
	{
		std::thread(KxMediaThread).detach();
	}
#endif
}

SKxMediaInfo KxMedia::Get()
{
	std::lock_guard<std::mutex> Lock(s_Mutex);
	return s_Info;
}

double KxMedia::GetPosition()
{
	std::lock_guard<std::mutex> Lock(s_Mutex);
	if(!s_Info.m_Valid)
		return 0.0;
	double Pos = s_Info.m_Position;
	if(s_Info.m_Playing)
		Pos += std::max(0.0, KxNowSeconds() - s_Info.m_SampleTime);
	if(s_Info.m_Duration > 0.0)
		Pos = std::min(Pos, s_Info.m_Duration);
	return std::max(0.0, Pos);
}

void KxMedia::Command(int Cmd)
{
	if(Cmd >= 0 && Cmd <= 2)
		s_PendingMediaCmd.store(Cmd);
}
