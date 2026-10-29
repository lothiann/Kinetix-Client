#pragma once

#include <cstdint>
#include <vector>

// v6.7: now-playing media snapshot shared between the platform backend
// (Windows SMTC / Android media session) and the ArrayPlayer HUD.
struct SKxMediaInfo
{
	bool m_Valid = false;
	bool m_Playing = false;
	char m_aTitle[256] = {};
	char m_aArtist[256] = {};
	double m_Duration = 0.0;  // seconds
	double m_Position = 0.0;  // seconds at m_SampleTime
	double m_SampleTime = 0.0; // seconds (monotonic) when the snapshot was taken

	// Decoded 32-bit RGBA album art, ready for texture upload. The media
	// thread decodes it via WIC (PNG/JPEG/WebP/BMP all supported) — the old
	// encoded-bytes path broke on players that hand out JPEG covers. Empty
	// vector = no art.
	std::vector<uint8_t> m_vArt;
	int m_ArtW = 0;
	int m_ArtH = 0;
	uint64_t m_ArtSerial = 0;

	// Normalized 0..1 bar levels for the visualizer.
	static constexpr int NUM_BANDS = 48;
	float m_aBands[NUM_BANDS] = {};

	uint64_t m_Serial = 0;
};

namespace KxMedia
{
	// Starts the backend (once) and lets it refresh in the background.
	void Update();
	// Latest snapshot (copy under the internal lock).
	SKxMediaInfo Get();
	// Playback position extrapolated to "now", in seconds.
	double GetPosition();
	// Media transport buttons: 0 = previous, 1 = play/pause, 2 = next.
	// Queued to the media thread, which owns the SMTC session.
	void Command(int Cmd);
}