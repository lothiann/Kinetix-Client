// (c) Kinetix. Fake Ping component — see fake_ping.h for overview.

#include <game/client/components/kinetix/fake_ping.h>

#include <engine/client.h>
#include <engine/client/client.h>
#include <engine/shared/config.h>
#include <game/client/gameclient.h>
#include <stdlib.h>

void CKinetixFakePing::OnReset()
{
        m_RandomLatency = 0;
        m_RandomCounter = 0;
        m_LinearPhase = 0.0f;
}

int CKinetixFakePing::ComputeFakeLatency() const
{
        int value = g_Config.m_KxFakePingValue; // 0..1000, default 100

        if(g_Config.m_KxFakePingMode == 1) // Dynamic
        {
                if(g_Config.m_KxFakePingDynamic == 0) // Random
                {
                        // Use the current random value (updated in OnUpdate).
                        return m_RandomLatency;
                }
                else // Linear (triangle wave)
                {
                        // m_LinearPhase is 0..1, triangle: 0->1->0->1...
                        // value at phase p: 2*p for p<0.5, 2*(1-p) for p>=0.5
                        float p = m_LinearPhase;
                        float tri = (p < 0.5f) ? (2.0f * p) : (2.0f * (1.0f - p));
                        return (int)(tri * (float)value);
                }
        }

        // Freeze: fixed value.
        return value;
}

void CKinetixFakePing::OnUpdate()
{
        if(!g_Config.m_KxFakePing)
        {
                // Fake ping off — let CClient send replies immediately (no queueing
                // happens when KxFakePing is off). Clear any stale state.
                m_RandomLatency = 0;
                m_RandomCounter = 0;
                m_LinearPhase = 0.0f;
                return;
        }

        // Advance dynamic state.
        if(g_Config.m_KxFakePingMode == 1) // Dynamic
        {
                int speed = g_Config.m_KxFakePingSpeed; // 0..100
                if(g_Config.m_KxFakePingDynamic == 0) // Random
                {
                        // Pick new random value every (101 - speed) ticks.
                        // speed=0 -> 101 ticks (~2s), speed=100 -> 1 tick (50/s).
                        int period = 101 - speed;
                        if(period < 1)
                                period = 1;
                        if(m_RandomCounter <= 0)
                        {
                                m_RandomLatency = rand() % (g_Config.m_KxFakePingValue + 1);
                                m_RandomCounter = period;
                        }
                        m_RandomCounter--;
                }
                else // Linear
                {
                        // Advance phase. Full cycle (0->1->0) takes (200 - speed*2) ticks.
                        // speed=0 -> 200 ticks (~4s), speed=100 -> 1 tick (fast).
                        int cycleTicks = 200 - speed * 2;
                        if(cycleTicks < 1)
                                cycleTicks = 1;
                        float advance = 1.0f / (float)cycleTicks;
                        m_LinearPhase += advance;
                        if(m_LinearPhase >= 1.0f)
                                m_LinearPhase -= 1.0f;
                }
        }

        // Read real latency from snapshot.
        int realLatency = 0;
        if(GameClient()->m_Snap.m_pLocalInfo)
                realLatency = GameClient()->m_Snap.m_pLocalInfo->m_Latency;
        if(realLatency < 0)
                realLatency = 0;

        int fakeLatency = ComputeFakeLatency();

        // Drain the pending-reply queue in CClient (cast IClient* to CClient*).
        static_cast<CClient *>(Client())->FakePingProcess(realLatency, fakeLatency);
}
