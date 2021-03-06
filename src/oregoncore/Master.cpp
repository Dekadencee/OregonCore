/*
 * Copyright (C) 2010-2012 OregonCore <http://www.oregoncore.com/>
 * Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2012 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <ace/OS_NS_signal.h>
#include "WorldSocketMgr.h"
#include "Common.h"
#include "Master.h"
#include "WorldSocket.h"
#include "WorldRunnable.h"
#include "World.h"
#include "Log.h"
#include "Timer.h"
#include "Policies/SingletonImp.h"
#include "SystemConfig.h"
#include "Config/Config.h"
#include "Database/DatabaseEnv.h"
#include "DBCStores.h"
#include "CliRunnable.h"
#include "RARunnable.h"
#include "Util.h"
#include "OCSoap.h"

#ifdef _WIN32
#   include "ServiceWin32.h"
extern int ServiceStatus;
#endif

INSTANTIATE_SINGLETON_1(Master);

class FreezeDetectorRunnable : public ACE_Based::Runnable
{
public:
    FreezeDetectorRunnable() : loops_(0), lastchange_(0), delaytime_(0) { }

    void run()
    {
        sLog.outString("Starting up anti-freeze thread (%u seconds max stuck time)...", delaytime_ / 1000);

        while (!World::IsStopped())
        {
            ACE_Based::Thread::Sleep(1000);
            uint32 curtime = getMSTime();

            // Normal work
            if (loops_ != World::m_worldLoopCounter)
            {
                lastchange_ = curtime;
                loops_ = World::m_worldLoopCounter;
            }
            // Possible freeze
            else if (getMSTimeDiff(lastchange_, curtime) > delaytime_)
            {
                sLog.outError("World thread is stuck. Terminating world server!");
                *((uint32 volatile*)NULL) = 0; // Bang crash
            }
        }

        sLog.outString("Anti-freeze thread exiting without problems.");
    }

    void SetDelayTime(const uint32 t) { delaytime_ = t; }

private:
    uint32 loops_, lastchange_;
    uint32 delaytime_;
};

// Main function
int Master::Run()
{
    sLog.outString("%s (core-daemon)", _FULLVERSION);
    sLog.outString("<Ctrl-C> to stop.\n");
    sLog.outString("  _____                                          ");
    sLog.outString(" /\\  __`\\                                        ");
    sLog.outString(" \\ \\ \\/\\ \\  _ __   __     __     ___    ___      ");
    sLog.outString("  \\ \\ \\ \\ \\/\\`'__\\'__`\\ /'_ `\\  / __`\\/' _ `\\    ");
    sLog.outString("   \\ \\ \\_\\ \\ \\ \\/\\  __//\\ \\L\\ \\/\\ \\L\\ \\\\ \\/\\ \\   ");
    sLog.outString("    \\ \\_____\\ \\_\\ \\____\\ \\____ \\ \\____/ \\_\\ \\_\\  ");
    sLog.outString("     \\/_____/\\/_/\\/____/\\/___L\\ \\/___/ \\/_/\\/_/  ");
    sLog.outString("                          /\\____/                ");
    sLog.outString("                          \\_/__/                 ");
    sLog.outString(" http://www.oregoncore.com                    \n ");

    // worldd PID file creation
    std::string pidfile = sConfig.GetStringDefault("PidFile", "");
    if (!pidfile.empty())
    {
        uint32 pid = CreatePIDFile(pidfile);
        if (!pid)
        {
            sLog.outError("Cannot create PID file %s.\n", pidfile.c_str());
            return 1;
        }
        sLog.outString("Daemon PID: %u\n", pid);
    }

    // Start the databases
    if (!StartDatabase())
        return 1;

    // Set server offline (not connectable)
    LoginDatabase.DirectPExecute("UPDATE realmlist SET flag = (flag & ~%u) | %u WHERE id = '%d'", REALM_FLAG_OFFLINE, REALM_FLAG_INVALID, realmID);

    // Initialize the World
    sWorld.SetInitialWorldSettings();

    // Catch termination signals
    this->HookSignals();

    // Launch WorldRunnable thread
    ACE_Based::Thread world_thread(new WorldRunnable);
    world_thread.setPriority(ACE_Based::Highest);

	// Launch CliRunnable thread
    ACE_Based::Thread* cliThread = NULL;
    #ifdef _WIN32
    if (sConfig.GetBoolDefault("Console.Enable", true) && (ServiceStatus == -1)/* need disable console in service mode*/)
    #else
    if (sConfig.GetBoolDefault("Console.Enable", true))
    #endif 
        cliThread = new ACE_Based::Thread(new CliRunnable);

    ACE_Based::Thread rar_thread(new RARunnable);

    // Handle affinity for multiple processors and process priority on Windows
    #ifdef _WIN32
    {
        HANDLE hProcess = GetCurrentProcess();

        uint32 Aff = sConfig.GetIntDefault("UseProcessors", 0);
        if (Aff > 0)
        {
            ULONG_PTR appAff;
            ULONG_PTR sysAff;

            if (GetProcessAffinityMask(hProcess,&appAff,&sysAff))
            {
                ULONG_PTR curAff = Aff & appAff; // remove non accessible processors

                if (!curAff)
                {
                    sLog.outError("Processors marked in UseProcessors bitmask (hex) %x not accessible for OregonCore. Accessible processors bitmask (hex): %x",Aff,appAff);
                }
                else
                {
                    if (SetProcessAffinityMask(hProcess,curAff))
                        sLog.outString("Using processors (bitmask, hex): %x", curAff);
                    else
                        sLog.outError("Can't set used processors (hex): %x",curAff);
                }
            }
            sLog.outString();
        }

        if (sConfig.GetBoolDefault("ProcessPriority", false))
        {
            if (SetPriorityClass(hProcess,HIGH_PRIORITY_CLASS))
                sLog.outString("OregonCore process priority class set to HIGH");
            else
                sLog.outError("ERROR: Can't set OregonCore process priority class.");
            sLog.outString();
        }
    }
    #endif

    // Start soap serving thread
    ACE_Based::Thread* soap_thread = NULL;
    if (sConfig.GetBoolDefault("SOAP.Enabled", false))
    {
        OCSoapRunnable* runnable = new OCSoapRunnable();
        runnable->setListenArguments(sConfig.GetStringDefault("SOAP.IP", "127.0.0.1"), sConfig.GetIntDefault("SOAP.Port", 7878));
        soap_thread = new ACE_Based::Thread(runnable);
    }

    // Start up freeze catcher thread
    ACE_Based::Thread* freeze_thread = NULL;
    if (uint32 freeze_delay = sConfig.GetIntDefault("MaxCoreStuckTime", 0))
    {
        FreezeDetectorRunnable* fdr = new FreezeDetectorRunnable();
        fdr->SetDelayTime(freeze_delay * 1000);
        freeze_thread = new ACE_Based::Thread(fdr);
        freeze_thread->setPriority(ACE_Based::Highest);
    }

    // Launch the world listener socket
    uint16 wsport = sWorld.getConfig(CONFIG_PORT_WORLD);
    std::string bind_ip = sConfig.GetStringDefault ("BindIP", "0.0.0.0");
    if (sWorldSocketMgr->StartNetwork (wsport, bind_ip.c_str ()) == -1)
    {
        sLog.outError("Failed to start network");
        World::StopNow(ERROR_EXIT_CODE);
    }

    // Set server online (allow connecting now)
    LoginDatabase.DirectPExecute("UPDATE realmlist SET flag = flag & ~%u, population = 0 WHERE id = '%u'", REALM_FLAG_INVALID, realmID);

    sLog.outString("%s (worldserver-daemon) ready...", _FULLVERSION);

    sWorldSocketMgr->Wait();

    // Stop freeze protection before shutdown tasks
    if (freeze_thread)
    {
        freeze_thread->destroy();
        delete freeze_thread;
    }

    // Stop soap thread
    if (soap_thread)
    {
        soap_thread->wait();
        soap_thread->destroy();
        delete soap_thread;
    }

    // Set server offline in realmlist
    LoginDatabase.PExecute("UPDATE realmlist SET flag = flag | %u WHERE id = '%d'", REALM_FLAG_OFFLINE, realmID);

    // Remove signal handling before leaving
    this->UnhookSignals();

    // When the main thread closes the singletons get unloaded
    // Since worldrunnable uses them, it will crash if unloaded after master
    world_thread.wait();
    rar_thread.wait ();

    // Clean account database before leaving
    this->ClearOnlineAccounts();

    // Wait for delay threads to end
    CharacterDatabase.HaltDelayThread();
    WorldDatabase.HaltDelayThread();
    LoginDatabase.HaltDelayThread();

    sLog.outString("Halting process...");

    if (cliThread)
    {
        #ifdef _WIN32
        // this only way to terminate CLI thread exist at Win32 (alt. way exist only in Windows Vista API)
        //_exit(1);
        // send keyboard input to safely unblock the CLI thread
        INPUT_RECORD b[5];
        HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
        b[0].EventType = KEY_EVENT;
        b[0].Event.KeyEvent.bKeyDown = TRUE;
        b[0].Event.KeyEvent.uChar.AsciiChar = 'X';
        b[0].Event.KeyEvent.wVirtualKeyCode = 'X';
        b[0].Event.KeyEvent.wRepeatCount = 1;

        b[1].EventType = KEY_EVENT;
        b[1].Event.KeyEvent.bKeyDown = FALSE;
        b[1].Event.KeyEvent.uChar.AsciiChar = 'X';
        b[1].Event.KeyEvent.wVirtualKeyCode = 'X';
        b[1].Event.KeyEvent.wRepeatCount = 1;

        b[2].EventType = KEY_EVENT;
        b[2].Event.KeyEvent.bKeyDown = TRUE;
        b[2].Event.KeyEvent.dwControlKeyState = 0;
        b[2].Event.KeyEvent.uChar.AsciiChar = '\r';
        b[2].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        b[2].Event.KeyEvent.wRepeatCount = 1;
        b[2].Event.KeyEvent.wVirtualScanCode = 0x1c;

        b[3].EventType = KEY_EVENT;
        b[3].Event.KeyEvent.bKeyDown = FALSE;
        b[3].Event.KeyEvent.dwControlKeyState = 0;
        b[3].Event.KeyEvent.uChar.AsciiChar = '\r';
        b[3].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        b[3].Event.KeyEvent.wVirtualScanCode = 0x1c;
        b[3].Event.KeyEvent.wRepeatCount = 1;
        DWORD numb;
        WriteConsoleInput(hStdIn, b, 4, &numb);
        cliThread->wait();
        #else
        cliThread->destroy();
        #endif
        delete cliThread;
    }

    // for some unknown reason, unloading scripts here and not in worldrunnable
    // fixes a memory leak related to detaching threads from the module
    //UnloadScriptingModule();

    // Exit the process with specified return value
    return World::GetExitCode();
}

// Initialize connection to the databases
bool Master::StartDatabase()
{
    sLog.SetLogDB(false);

    // Get world database info from configuration file
    std::string dbstring = sConfig.GetStringDefault("WorldDatabaseInfo", "");
    if (dbstring.empty())
    {
        sLog.outError("World database not specified in configuration file");
        return false;
    }

    // Initialise the world database
    if (!WorldDatabase.Initialize(dbstring.c_str()))
    {
        sLog.outError("Cannot connect to world database %s",dbstring.c_str());
        return false;
    }

    // Get character database info from configuration file
    dbstring = sConfig.GetStringDefault("CharacterDatabaseInfo", "");
    if (dbstring.empty())
    {
        sLog.outError("Character database not specified in configuration file");
        return false;
    }

    // Initialise the Character database
    if (!CharacterDatabase.Initialize(dbstring.c_str()))
    {
        sLog.outError("Cannot connect to Character database %s",dbstring.c_str());
        return false;
    }

    // Get login database info from configuration file
    dbstring = sConfig.GetStringDefault("LoginDatabaseInfo", "");
    if (dbstring.empty())
    {
        sLog.outError("Login database not specified in configuration file");
        return false;
    }

    // Initialise the login database
    if (!LoginDatabase.Initialize(dbstring.c_str()))
    {
        sLog.outError("Cannot connect to login database %s",dbstring.c_str());
        return false;
    }

    // Get the realm Id from the configuration file
    realmID = sConfig.GetIntDefault("RealmID", 0);
    if (!realmID)
    {
        sLog.outError("Realm ID not defined in configuration file");
        return false;
    }
    sLog.outString("Realm running as realm ID %d", realmID);

    // Initialize the DB logging system
    sLog.SetLogDBLater(sConfig.GetBoolDefault("EnableLogDB", false)); // set var to enable DB logging once startup finished.
    sLog.SetLogDB(false);
    sLog.SetRealmID(realmID);

    // Clean the database before starting
    this->ClearOnlineAccounts();

    // Insert version info into DB
    WorldDatabase.PExecute("UPDATE version SET core_version = '%s', core_revision = '%s'", _FULLVERSION, _REVISION);

    sWorld.LoadDBVersion();

    sLog.outString("Using World Database: %s", sWorld.GetDBVersion());
    return true;
}

// Clear 'online' status for all accounts with characters in this realm
void Master::ClearOnlineAccounts()
{
    // Cleanup online status for characters hosted at current realm
    // todo - Only accounts with characters logged on *this* realm should have online status reset. Move the online column from 'account' to 'realmcharacters'?
    LoginDatabase.PExecute("UPDATE account SET online = 0 WHERE online = '%d'", realmID);
    CharacterDatabase.Execute("UPDATE characters SET online = 0 WHERE online<>0");
}

// Handle termination signals
void Master::OnSignal(int s)
{
    switch (s)
    {
    case SIGINT:
        World::StopNow(RESTART_EXIT_CODE);
        break;
    case SIGTERM:
    #ifdef _WIN32
    case SIGBREAK:
    #endif
        World::StopNow(SHUTDOWN_EXIT_CODE);
        break;
    }
}

// Define hook 'OnSignal' for all termination signals
void Master::HookSignals()
{
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
    #ifdef _WIN32
    signal(SIGBREAK, OnSignal);
    #endif
}

// Unhook the signals before leaving
void Master::UnhookSignals()
{
    signal(SIGINT, 0);
    signal(SIGTERM, 0);
    #ifdef _WIN32
    signal(SIGBREAK, 0);
    #endif
}