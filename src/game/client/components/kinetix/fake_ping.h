// (c) Kinetix. Fake Ping component — delays NETMSG_PING_REPLY to spoof the
// latency shown in TAB scoreboard and the 'ping' command.
//
// The server measures RTT by sending NETMSG_PING and waiting for
// NETMSG_PING_REPLY. By delaying the reply, the server sees a higher RTT
// and reports it back to all clients in the PlayerInfo snapshot (which is
// what TAB displays). Connection timeout is ~400s so short delays are safe.
//
// Modes:
//   Freeze  - fixed delay so displayed ping = KxFakePingValue (ms).
//   Dynamic - animated delay, target ping changes over time:
//     Random - pick a new random value in [0, KxFakePingValue] every
//              (101 - Speed) ticks.
//     Linear - triangle wave 0 -> KxFakePingValue -> 0 -> ... at Speed.
//
// The component computes the fake target latency every frame and calls
// CClient::FakePingProcess(realLatency, fakeLatency) which drains the
// pending-reply queue (filled in CClient::OnMessage NETMSG_PING handler).

#ifndef GAME_CLIENT_COMPONENTS_KINETIX_FAKE_PING_H
#define GAME_CLIENT_COMPONENTS_KINETIX_FAKE_PING_H

#include <game/client/component.h>

class CKinetixFakePing : public CComponent
{
public:
	CKinetixFakePing() = default;
	~CKinetixFakePing() override = default;

	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnUpdate() override;

private:
	// Compute the current fake target latency in ms based on mode/dynamic.
	int ComputeFakeLatency() const;

	// Dynamic state.
	int m_RandomLatency = 0; // current random value (Random mode)
	int m_RandomCounter = 0; // ticks until next random pick
	float m_LinearPhase = 0.0f; // 0..1 phase for Linear triangle wave
};

#endif // GAME_CLIENT_COMPONENTS_KINETIX_FAKE_PING_H
