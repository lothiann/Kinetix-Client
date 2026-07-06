#include <game/client/components/kinetix/kinetix.h>
#include <game/client/components/kinetix/kinetix_internal.h>

void CBotNet::ConMacroLoad(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        const char *path = pResult->GetString(0);
        dbg_msg("botnet_macro", "ConMacroLoad called: %s", path);
        if(!path || !path[0])
                return;

        std::ifstream file(path);
        if(!file.is_open())
        {
                dbg_msg("botnet_macro", "Failed to open macro file: %s", path);
                if(p->Console())
                        p->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "botnet", "Failed to open macro file");
                return;
        }

        std::vector<std::string> lines;
        std::string line;
        while(std::getline(file, line))
        {
                line.erase(0, line.find_first_not_of(" \t\r\n"));
                line.erase(line.find_last_not_of(" \t\r\n") + 1);
                if(!line.empty())
                        lines.push_back(line);
        }
        file.close();

        // Load into all dummies
        for(int D = 0; D < MAX_DUMMIES; D++)
                p->m_aDummies[D].m_MacroPlayLines = lines;

        dbg_msg("botnet_macro", "Loaded %d macro lines", (int)lines.size());
        if(p->Console())
        {
                char aBuf[64];
                str_format(aBuf, sizeof(aBuf), "Loaded %d macro lines", (int)lines.size());
                p->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "botnet", aBuf);
        }
}

void CBotNet::ConMacroPlay(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int on = pResult->GetInteger(0);
        dbg_msg("botnet_macro", "ConMacroPlay called: %d", on);
        if(on != 0)
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        if(p->m_aDummies[D].m_MacroPlayLines.empty())
                                continue;
                        p->m_aDummies[D].m_MacroPlaying = true;
                        p->m_aDummies[D].m_MacroPlayIndex = 0;
                        p->m_aDummies[D].m_MacroSleepTicks = 0;
                }
                dbg_msg("botnet_macro", "Playback started.");
        }
        else
        {
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->m_aDummies[D].m_MacroPlaying = false;
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->ResetDummyInputs(D);
                p->GameClient()->m_BotControl.ActionStop(-1);
                dbg_msg("botnet_macro", "Playback stopped manually.");
        }
}

void CBotNet::ConMacroRecord(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int on = pResult->GetInteger(0);
        dbg_msg("botnet_macro", "ConMacroRecord called with on=%d", on);

        if(on != 0)
        {
                dbg_msg("botnet_macro", "Starting macro recording...");
                int64_t startTick = p->Client() ? p->Client()->GameTick(0) : 0;
                for(int D = 0; D < MAX_DUMMIES; D++)
                {
                        p->m_aDummies[D].m_MacroRecording = true;
                        p->m_aDummies[D].m_MacroRecordBuffer.clear();
                        p->m_aDummies[D].m_LastMacroRecordTick = startTick;
                        p->m_aDummies[D].m_LastRecordedDir = 0;
                        p->m_aDummies[D].m_LastRecordedJump = 0;
                        p->m_aDummies[D].m_LastRecordedHook = 0;
                        p->m_aDummies[D].m_LastRecordedFire = 0;
                        p->m_aDummies[D].m_LastRecordedAimX = 0;
                        p->m_aDummies[D].m_LastRecordedAimY = 0;
                        p->m_aDummies[D].m_LastRecordedWeapon = -1;
                }
                dbg_msg("botnet_macro", "Macro recording started successfully. Start tick: %lld", (long long)startTick);
        }
        else
        {
                dbg_msg("botnet_macro", "Stopping macro recording...");
                for(int D = 0; D < MAX_DUMMIES; D++)
                        p->m_aDummies[D].m_MacroRecording = false;
                dbg_msg("botnet_macro", "Macro recording stopped. Total lines in buffer[0]: %d", (int)p->m_aDummies[0].m_MacroRecordBuffer.size());
        }
}

void CBotNet::ConMacroSave(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        const char *path = pResult->GetString(0);
        dbg_msg("botnet_macro", "ConMacroSave called with path: %s", path ? path : "NULL");
        if(!path || !path[0])
                return;

        // Save from dummy 0's buffer
        std::ofstream file(path);
        if(!file.is_open())
        {
                dbg_msg("botnet_macro", "Failed to open file for writing: %s", path);
                if(p->Console())
                        p->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "botnet", "Failed to open file for writing");
                return;
        }

        dbg_msg("botnet_macro", "Writing %d lines to file...", (int)p->m_aDummies[0].m_MacroRecordBuffer.size());
        for(const std::string &line : p->m_aDummies[0].m_MacroRecordBuffer)
                file << line << "\n";
        file.close();

        dbg_msg("botnet_macro", "File saved successfully.");
        if(p->Console())
                p->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "botnet", "Macro saved");
}

void CBotNet::ConMacroCapture(IConsole::IResult *pResult, void *pUserData)
{
        CBotNet *p = (CBotNet *)pUserData;
        int id = pResult->GetInteger(0);
        for(int D = 0; D < MAX_DUMMIES; D++)
                p->m_aDummies[D].m_MacroCaptureID = id;
        dbg_msg("botnet_macro", "Capture ID set to: %d", id);
}
