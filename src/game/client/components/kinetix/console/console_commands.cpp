#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

std::string CBotNet::ReplacePlaceholders(const std::string &Cmd, int DummyIndex, CGameClient *pGame)
{
        std::string Result = Cmd;

        // {i} — rank of this dummy among connected dummies (1-based)
        // Original: online = sorted(control_clients.keys()); rank = online.index(client_index) + 1
        {
                // Collect connected dummy indices (sorted ascending, which they already are)
                int Online[MAX_DUMMIES];
                int Count = 0;
                for(int d = 0; d < MAX_DUMMIES; ++d)
                {
                        if(d != 0 && !pGame->Client()->DummyConnected(d))
                                continue;
                        if(pGame->m_aLocalIds[d] < 0)
                                continue;
                        Online[Count++] = d;
                }
                int Rank = DummyIndex + 1; // fallback if not found
                for(int a = 0; a < Count; ++a)
                {
                        if(Online[a] == DummyIndex)
                        {
                                Rank = a + 1;
                                break;
                        }
                }
                size_t Pos;
                while((Pos = Result.find("{i}")) != std::string::npos)
                        Result.replace(Pos, 3, std::to_string(Rank));
        }

        // {r} — random char from a-zA-Z0-9._-
        // Original: random.choice(string.ascii_letters + string.digits + "._-")
        {
                static const char RAND_CHARS[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-";
                static const int RAND_CHARS_LEN = sizeof(RAND_CHARS) - 1;
                size_t Pos;
                while((Pos = Result.find("{r}")) != std::string::npos)
                {
                        char Ch = RAND_CHARS[rand() % RAND_CHARS_LEN];
                        Result.replace(Pos, 3, 1, Ch);
                }
        }

        // {ri-N} — random integer from 0 to N (inclusive)
        // Original: re.sub(r'{ri-(\d+)}', lambda m: str(random.randint(0, int(m.group(1)))), cmd)
        {
                size_t Pos;
                while((Pos = Result.find("{ri-")) != std::string::npos)
                {
                        size_t End = Result.find('}', Pos);
                        if(End != std::string::npos)
                        {
                                std::string NumStr = Result.substr(Pos + 4, End - Pos - 4);
                                int MaxVal = atoi(NumStr.c_str());
                                if(MaxVal < 1) MaxVal = 1;
                                int Val = rand() % (MaxVal + 1);
                                Result.replace(Pos, End - Pos + 1, std::to_string(Val));
                        }
                        else
                        {
                                break;
                        }
                }
        }

        // {c} — random CJK character U+4E00–U+9FFF
        // Original: chr(random.randint(0x4E00, 0x9FFF))
        {
                size_t Pos;
                while((Pos = Result.find("{c}")) != std::string::npos)
                {
                        int Codepoint = 0x4E00 + (rand() % (0x9FFF - 0x4E00 + 1));
                        char Utf8[5];
                        Utf8[0] = (char)(0xE0 | ((Codepoint >> 12) & 0x0F));
                        Utf8[1] = (char)(0x80 | ((Codepoint >> 6) & 0x3F));
                        Utf8[2] = (char)(0x80 | (Codepoint & 0x3F));
                        Utf8[3] = '\0';
                        Result.replace(Pos, 3, Utf8);
                }
        }

        return Result;
}

void CBotNet::ConSendDummy(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *pSelf = (CBotNet *)pUserData;
        const char *pIds = pResult->GetString(0);
        const char *pCmd = pResult->GetString(1);

        if(!pCmd[0])
                return;

        // Parse dummy IDs
        bool aTargets[MAX_DUMMIES] = {};
        bool AllDummies = false;

        if(str_comp(pIds, "-1") == 0)
        {
                AllDummies = true;
        }
        else
        {
                // Parse comma-separated IDs: "0,1,2,3"
                char aBuf[64];
                str_copy(aBuf, pIds, sizeof(aBuf));
                char *pTok = strtok(aBuf, ",");
                while(pTok)
                {
                        int Id = atoi(pTok);
                        if(Id >= 0 && Id < MAX_DUMMIES)
                                aTargets[Id] = true;
                        pTok = strtok(nullptr, ",");
                }
        }

        // Save current dummy
        int SavedDummy = g_Config.m_ClDummy;

        // Execute command on each target dummy
        for(int D = 0; D < MAX_DUMMIES; D++)
        {
                if(!AllDummies && !aTargets[D])
                        continue;
                // Skip disconnected dummies (0=main, always connected if online)
                if(D != 0 && !pSelf->Client()->DummyConnected(D))
                        continue;

                // Replace placeholders per-dummy
                std::string Cmd = ReplacePlaceholders(pCmd, D, pSelf->GameClient());

                g_Config.m_ClDummy = D;
                pSelf->Console()->ExecuteLine(Cmd.c_str(), IConsole::CLIENT_ID_UNSPECIFIED);
        }

        // Restore original dummy
        g_Config.m_ClDummy = SavedDummy;
}

// =========================================================
// KINODYNAMIC A* — CONSOLE COMMANDS
// =========================================================

void CBotNet::ConPfLive(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int s = pResult->GetInteger(0);
        if(s < 0 || s > 2)
                return;
        if(s == PF_STATE_RUNNING && p->m_PfState != PF_STATE_RUNNING)
                p->PfResetRun();
        p->m_PfState = s;
        if(s == PF_STATE_IDLE)
                p->m_PfVPath.clear();
}

void CBotNet::ConPfGo(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int s = pResult->GetInteger(0);
        if(s == 1)
        {
                // Start playback when path is ready (m_PfAPathReady=true).
                // This covers both: pathfinder finished on its own (reached goal)
                // AND user pressed Stop (clickgui.cpp reconstructs best partial path).
                if(!p->m_PfAPathReady)
                {
                        dbg_msg("pathfinder", "kx_pf_play 1: path not ready (run pathfinder first, or press Stop after it finds a path)");
                        return;
                }
                if(p->m_PfFullInputs.empty())
                {
                        dbg_msg("pathfinder", "kx_pf_play 1: m_PfFullInputs is empty (path found but no inputs?)");
                        return;
                }
                p->m_PfGoActive = true;
                p->m_PfGoIdx = 0;
                dbg_msg("pathfinder", "kx_pf_play 1: playback started (%d ticks)", (int)p->m_PfFullInputs.size());
        }
        else
        {
                p->m_PfGoActive = false;
                dbg_msg("pathfinder", "kx_pf_play 0: playback stopped at tick %d/%d", (int)p->m_PfGoIdx, (int)p->m_PfFullInputs.size());
        }
}

void CBotNet::ConPfAlgorithm(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 0 && v < PF_TAB_ALGORITHM_COUNT)
                g_Config.m_KxPfAlgorithm = v;
}

void CBotNet::ConPfChunkSize(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= CConfig::PF_CHUNK_SIZE_MIN && v <= PF_TAB_MAX_CHUNK_TICKS)
                g_Config.m_KxPfChunkSize = v;
}

void CBotNet::ConPfCandidates(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 50 && v <= 2000)
                g_Config.m_KxPfCandidates = v;
}

void CBotNet::ConPfHookAngles(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 4 && v <= 32)
                g_Config.m_KxPfHookAngles = v;
}

void CBotNet::ConPfHorizon(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int v = pResult->GetInteger(0);
        if(v >= 1 && v <= PF_TAB_MAX_CHUNK_TICKS)
                g_Config.m_KxPfHorizon = v;
}

// Reset run: anchor to active player's predicted core, build flow field.
