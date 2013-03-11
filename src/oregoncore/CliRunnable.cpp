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

#include "Common.h"
#include "ObjectMgr.h"
#include "World.h"
#include "WorldSession.h"
#include "Config/Config.h"

#include "AccountMgr.h"
#include "Chat.h"
#include "CliRunnable.h"
#include "Language.h"
#include "Log.h"
#include "MapManager.h"
#include "Player.h"
#include "Util.h"

#if PLATFORM != WINDOWS
#   include <readline/readline.h>
#   include <readline/history.h>

char* command_finder(const char* text, int state)
{
    static int idx,len;
    const char* ret;
    ChatCommand* cmd = ChatHandler::getCommandTable();

    if (!state)
    {
        idx = 0;
        len = strlen(text);
    }

    while((ret = cmd[idx].Name))
    {
        if (!cmd[idx].AllowConsole)
        {
            idx++;
            continue;
        }

        idx++;
        //printf("Checking %s \n", cmd[idx].Name);
        if (strncmp(ret, text, len) == 0)
            return strdup(ret);
        if (cmd[idx].Name == NULL)
            break;
    }

    return ((char*)NULL);
}

char** cli_completion(const char* text, int start, int /*end*/)
{
    char** matches;
    matches = (char**)NULL;

    if (start == 0)
        matches = rl_completion_matches((char*)text,&command_finder);
    else
        rl_bind_key('\t',rl_abort);
    return (matches);
}
#endif

void utf8print(void* /*arg*/, const char* str)
{
    #if PLATFORM == PLATFORM_WINDOWS
    wchar_t wtemp_buffer[6000];
    size_t wtemp_length = 6000-1;
    if (!Utf8toWStr(str,strlen(str), wtemp_buffer, wtemp_length))
        return;

    char temp_buffer[6000];
    CharToOemBuffW(&wtemp_buffer[0], &temp_buffer[0], wtemp_length + 1);
    printf(temp_buffer);
    #else
    printf("%s", str);
    #endif
}

void commandFinished(void*, bool /*success*/)
{
    printf("Oregon>");
    fflush(stdout);
}

// Delete a user account and all associated characters in this realm
// Todo - This function has to be enhanced to respect the login/realm split (delete char, delete account chars in realm, delete account chars in realm then delete account
bool ChatHandler::HandleAccountDeleteCommand(const char* args)
{
    if (!*args)
        return false;

    // Get the account name from the command line
    char* account_name_str=strtok ((char*)args," ");
    if (!account_name_str)
        return false;

    std::string account_name = account_name_str;
    if (!AccountMgr::normalizeString(account_name))
    {
        PSendSysMessage(LANG_ACCOUNT_NOT_EXIST, account_name.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    uint32 account_id = sAccountMgr->GetId(account_name);
    if (!account_id)
    {
        PSendSysMessage(LANG_ACCOUNT_NOT_EXIST, account_name.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    // Commands not recommended call from chat, but support anyway
    if (m_session)
    {
        uint32 target_security = sAccountMgr->GetSecurity(account_id);

        // Can delete only for account with less security
        // This is also reject self apply in fact
        if (target_security >= m_session->GetSecurity())
        {
            SendSysMessage (LANG_YOURS_SECURITY_IS_LOW);
            SetSentErrorMessage (true);
            return false;
        }
    }

    AccountOpResult result = sAccountMgr->DeleteAccount(account_id);
    switch (result)
    {
    case AOR_OK:
        PSendSysMessage(LANG_ACCOUNT_DELETED, account_name.c_str());
        break;
    case AOR_NAME_NOT_EXIST:
        PSendSysMessage(LANG_ACCOUNT_NOT_EXIST, account_name.c_str());
        SetSentErrorMessage(true);
        return false;
    case AOR_DB_INTERNAL_ERROR:
        PSendSysMessage(LANG_ACCOUNT_NOT_DELETED_SQL_ERROR, account_name.c_str());
        SetSentErrorMessage(true);
        return false;
    default:
        PSendSysMessage(LANG_ACCOUNT_NOT_DELETED, account_name.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    return true;
}

/**
 * Collects all GUIDs (and related info) from deleted characters which are still in the database.
 *
 * @param foundList    a reference to an std::list which will be filled with info data
 * @param searchString the search string which either contains a player GUID or a part fo the character-name
 * @return             returns false if there was a problem while selecting the characters (e.g. player name not normalizeable)
 */
bool ChatHandler::GetDeletedCharacterInfoList(DeletedInfoList& found_list, std::string search_string)
{
    QueryResult_AutoPtr result_character;
    if (!search_string.empty())
    {
        // Search by GUID
        if (isNumeric(search_string.c_str()))
            result_character = CharacterDatabase.PQuery("SELECT guid, deleteInfos_Name, deleteInfos_Account, deleteDate FROM characters WHERE deleteDate IS NOT NULL AND guid = %llu", uint64(atoi(search_string.c_str())));
        // Search by name
        else
        {
            if (!normalizePlayerName(search_string))
                return false;

            result_character = CharacterDatabase.PQuery("SELECT guid, deleteInfos_Name, deleteInfos_Account, deleteDate FROM characters WHERE deleteDate IS NOT NULL AND deleteInfos_Name " _LIKE_ " " _CONCAT3_("'%%'", "'%s'", "'%%'"), search_string.c_str());
        }
    }
    else
        result_character = CharacterDatabase.Query("SELECT guid, deleteInfos_Name, deleteInfos_Account, deleteDate FROM characters WHERE deleteDate IS NOT NULL");

    if (result_character)
    {
        do
        {
            Field* fields = result_character->Fetch();

            DeletedInfo delete_info;

            delete_info.lowguid = fields[0].GetUInt32();
            delete_info.name = fields[1].GetCppString();
            delete_info.accountId  = fields[2].GetUInt32();

            // Account name will be empty for not existed account
            sAccountMgr->GetName(delete_info.accountId, delete_info.accountName);

            delete_info.deleteDate = time_t(fields[3].GetUInt64());

            found_list.push_back(delete_info);
        } while (result_character->NextRow());
    }

    return true;
}

/**
 * Generate WHERE guids list by deleted info in way preventing return too long where list for existed query string length limit.
 *
 * @param itr          A reference to an deleted info list iterator, it updated in function for possible next function call if list to long
 * @param itr_end      A reference to an deleted info list iterator end()
 * @return             Returns generated where list string in form: 'guid IN (gui1, guid2, ...)'
 */
std::string ChatHandler::GenerateDeletedCharacterGUIDsWhereStr(DeletedInfoList::const_iterator& itr, DeletedInfoList::const_iterator const& itr_end)
{
    std::ostringstream where_string;
    where_string << "guid IN ('";
    for(; itr != itr_end; ++itr)
    {
        where_string << itr->lowguid;

        if (where_string.str().size() > MAX_QUERY_LEN - 50)     // near to max query
        {
            ++itr;
            break;
        }

        DeletedInfoList::const_iterator itr2 = itr;
        if(++itr2 != itr_end)
            where_string << "','";
    }
    where_string << "')";
    return where_string.str();
}

/**
 * Shows all deleted characters which matches the given search string, expected non empty list
 *
 * @see ChatHandler::HandleCharacterDeletedListCommand
 * @see ChatHandler::HandleCharacterDeletedRestoreCommand
 * @see ChatHandler::HandleCharacterDeletedDeleteCommand
 * @see ChatHandler::DeletedInfoList
 *
 * @param foundList Contains a list with all found deleted characters
 */
void ChatHandler::HandleCharacterDeletedListHelper(DeletedInfoList const& found_list)
{
    if (!m_session)
    {
        SendSysMessage(LANG_CHARACTER_DELETED_LIST_BAR);
        SendSysMessage(LANG_CHARACTER_DELETED_LIST_HEADER);
        SendSysMessage(LANG_CHARACTER_DELETED_LIST_BAR);
    }

    for (DeletedInfoList::const_iterator itr = found_list.begin(); itr != found_list.end(); ++itr)
    {
        std::string date_string = TimeToTimestampStr(itr->deleteDate);

        if (!m_session)
            PSendSysMessage(LANG_CHARACTER_DELETED_LIST_LINE_CONSOLE,
                itr->lowguid, itr->name.c_str(), itr->accountName.empty() ? "<Not existed>" : itr->accountName.c_str(),
                itr->accountId, date_string.c_str());
        else
            PSendSysMessage(LANG_CHARACTER_DELETED_LIST_LINE_CHAT,
                itr->lowguid, itr->name.c_str(), itr->accountName.empty() ? "<Not existed>" : itr->accountName.c_str(),
                itr->accountId, date_string.c_str());
    }

    if (!m_session)
        SendSysMessage(LANG_CHARACTER_DELETED_LIST_BAR);
}

/**
 * Handles the '.character deleted list' command, which shows all deleted characters which matches the given search string
 *
 * @see ChatHandler::HandleCharacterDeletedListHelper
 * @see ChatHandler::HandleCharacterDeletedRestoreCommand
 * @see ChatHandler::HandleCharacterDeletedDeleteCommand
 * @see ChatHandler::DeletedInfoList
 *
 * @param args The search string which either contains a player GUID or a part fo the character-name
 */
bool ChatHandler::HandleCharacterDeletedListCommand(const char* args)
{
    DeletedInfoList found_list;
    if (!GetDeletedCharacterInfoList(found_list, args))
        return false;

    // If no characters have been found, output a warning
    if (found_list.empty())
    {
        SendSysMessage(LANG_CHARACTER_DELETED_LIST_EMPTY);
        return false;
    }

    HandleCharacterDeletedListHelper(found_list);
    return true;
}

/**
 * Restore a previously deleted character
 *
 * @see ChatHandler::HandleCharacterDeletedListHelper
 * @see ChatHandler::HandleCharacterDeletedRestoreCommand
 * @see ChatHandler::HandleCharacterDeletedDeleteCommand
 * @see ChatHandler::DeletedInfoList
 *
 * @param delInfo The informations about the character which will be restored
 */
void ChatHandler::HandleCharacterDeletedRestoreHelper(DeletedInfo const& delete_info)
{
    if (delete_info.accountName.empty()) // Account not exist
    {
        PSendSysMessage(LANG_CHARACTER_DELETED_SKIP_ACCOUNT, delete_info.name.c_str(), delete_info.lowguid, delete_info.accountId);
        return;
    }

    // check character count
    uint32 charcount = sAccountMgr->GetCharactersCount(delete_info.accountId);
    if (charcount >= 10)
    {
        PSendSysMessage(LANG_CHARACTER_DELETED_SKIP_FULL, delete_info.name.c_str(), delete_info.lowguid, delete_info.accountId);
        return;
    }

    if (objmgr.GetPlayerGUIDByName(delete_info.name))
    {
        PSendSysMessage(LANG_CHARACTER_DELETED_SKIP_NAME, delete_info.name.c_str(), delete_info.lowguid, delete_info.accountId);
        return;
    }

    CharacterDatabase.PExecute("UPDATE characters SET name='%s', account='%u', deleteDate=NULL, deleteInfos_Name=NULL, deleteInfos_Account=NULL WHERE deleteDate IS NOT NULL AND guid = %u",
        delete_info.name.c_str(), delete_info.accountId, delete_info.lowguid);
}

/**
 * Handles the '.character deleted restore' command, which restores all deleted characters which matches the given search string
 *
 * The command automatically calls '.character deleted list' command with the search string to show all restored characters.
 *
 * @see ChatHandler::HandleCharacterDeletedRestoreHelper
 * @see ChatHandler::HandleCharacterDeletedListCommand
 * @see ChatHandler::HandleCharacterDeletedDeleteCommand
 *
 * @param args The search string which either contains a player GUID or a part of the character-name
 */
bool ChatHandler::HandleCharacterDeletedRestoreCommand(const char* args)
{
    // It is required to submit at least one argument
    if (!*args)
        return false;

    std::string search_string;
    std::string new_character_name;
    uint32 new_account = 0;

    // GCC by some strange reason fail build code without temporary variable
    std::istringstream params(args);
    params >> search_string >> new_character_name >> new_account;

    DeletedInfoList found_list;
    if (!GetDeletedCharacterInfoList(found_list, search_string))
        return false;

    if (found_list.empty())
    {
        SendSysMessage(LANG_CHARACTER_DELETED_LIST_EMPTY);
        return false;
    }

    SendSysMessage(LANG_CHARACTER_DELETED_RESTORE);
    HandleCharacterDeletedListHelper(found_list);

    if (new_character_name.empty())
    {
        // Drop not existed account cases
        for (DeletedInfoList::iterator itr = found_list.begin(); itr != found_list.end(); ++itr)
            HandleCharacterDeletedRestoreHelper(*itr);
    }
    else if (found_list.size() == 1 && normalizePlayerName(new_character_name))
    {
        DeletedInfo delete_info = found_list.front();

        // update name
        delete_info.name = new_character_name;

        // if new account provided update deleted info
        if (new_account && new_account != delete_info.accountId)
        {
            delete_info.accountId = new_account;
            sAccountMgr->GetName(new_account, delete_info.accountName);
        }

        HandleCharacterDeletedRestoreHelper(delete_info);
    }
    else
        SendSysMessage(LANG_CHARACTER_DELETED_ERR_RENAME);

    return true;
}

/**
 * Handles the '.character deleted delete' command, which completely deletes all deleted characters which matches the given search string
 *
 * @see Player::GetDeletedCharacterGUIDs
 * @see Player::DeleteFromDB
 * @see ChatHandler::HandleCharacterDeletedListCommand
 * @see ChatHandler::HandleCharacterDeletedRestoreCommand
 *
 * @param args The search string which either contains a player GUID or a part fo the character-name
 */
bool ChatHandler::HandleCharacterDeletedDeleteCommand(const char* args)
{
    // It is required to submit at least one argument
    if (!*args)
        return false;

    DeletedInfoList found_list;
    if (!GetDeletedCharacterInfoList(found_list, args))
        return false;

    if (found_list.empty())
    {
        SendSysMessage(LANG_CHARACTER_DELETED_LIST_EMPTY);
        return false;
    }

    SendSysMessage(LANG_CHARACTER_DELETED_DELETE);
    HandleCharacterDeletedListHelper(found_list);

    // Call the appropriate function to delete them (current account for deleted characters is 0)
    for(DeletedInfoList::const_iterator itr = found_list.begin(); itr != found_list.end(); ++itr)
        Player::DeleteFromDB(itr->lowguid, 0, false, true);

    return true;
}

/**
 * Handles the '.character deleted old' command, which completely deletes all deleted characters deleted with some days ago
 *
 * @see Player::DeleteOldCharacters
 * @see Player::DeleteFromDB
 * @see ChatHandler::HandleCharacterDeletedDeleteCommand
 * @see ChatHandler::HandleCharacterDeletedListCommand
 * @see ChatHandler::HandleCharacterDeletedRestoreCommand
 *
 * @param args The search string which either contains a player GUID or a part fo the character-name
 */
bool ChatHandler::HandleCharacterDeletedOldCommand(const char* args)
{
    int32 keep_days = sWorld.getConfig(CONFIG_CHARDELETE_KEEP_DAYS);

    char* px = strtok((char*)args, " ");
    if (px)
    {
        if (!isNumeric(px))
            return false;

        keep_days = atoi(px);
        if (keep_days < 0)
            return false;
    }
    // Config option value 0 -> disabled and can't be used
    else if (keep_days <= 0)
        return false;

    Player::DeleteOldCharacters((uint32)keep_days);
    return true;
}

bool ChatHandler::HandleCharacterEraseCommand(const char* args)
{
    if (!*args)
        return false;

    char* character_name_str = strtok((char*)args," ");
    if (!character_name_str)
        return false;

    std::string character_name = character_name_str;
    if (!normalizePlayerName(character_name))
        return false;

    uint64 character_guid;
    uint32 account_id;

    Player* player = objmgr.GetPlayer(character_name.c_str());
    if (player)
    {
        character_guid = player->GetGUID();
        account_id = player->GetSession()->GetAccountId();
        player->GetSession()->KickPlayer();
    }
    else
    {
        character_guid = objmgr.GetPlayerGUIDByName(character_name);
        if (!character_guid)
        {
            PSendSysMessage(LANG_NO_PLAYER,character_name.c_str());
            SetSentErrorMessage(true);
            return false;
        }

        account_id = objmgr.GetPlayerAccountIdByGUID(character_guid);
    }

    std::string account_name;
    sAccountMgr->GetName(account_id, account_name);

    Player::DeleteFromDB(character_guid, account_id, true);
    PSendSysMessage(LANG_CHARACTER_DELETED,character_name.c_str(),GUID_LOPART(character_guid), account_name.c_str(), account_id);
    return true;
}

// Exit the realm
bool ChatHandler::HandleServerExitCommand(const char* /*args*/)
{
    SendSysMessage(LANG_COMMAND_EXIT);
    World::StopNow(SHUTDOWN_EXIT_CODE);
    return true;
}

// Display info on users currently in the realm
bool ChatHandler::HandleAccountOnlineListCommand(const char* /*args*/)
{
    // Get the list of accounts ID logged to the realm
    QueryResult_AutoPtr result_database = CharacterDatabase.Query("SELECT name,account FROM characters WHERE online > 0");
    if (!result_database)
        return true;

    // Display the list of account/characters online
    SendSysMessage("=====================================================================");
    SendSysMessage(LANG_ACCOUNT_LIST_HEADER);
    SendSysMessage("=====================================================================");

    // Circle through accounts
    do
    {
        Field* fields_database = result_database->Fetch();
        std::string name = fields_database[0].GetCppString();
        uint32 account = fields_database[1].GetUInt32();

        // Get the username, last IP and GM level of each account
        // No SQL injection. account is uint32.
        QueryResult_AutoPtr result_login = LoginDatabase.PQuery("SELECT a.username, a.last_ip, aa.gmlevel, a.expansion FROM account a "
            "LEFT JOIN account_access aa ON (a.id = aa.id) WHERE a.id = '%u'", account);
        if (result_login)
        {
            Field* fields_login = result_login->Fetch();
            PSendSysMessage("|%15s| %20s | %15s |%4d|%5d|",
                fields_login[0].GetString(),name.c_str(),fields_login[1].GetString(),fields_login[2].GetUInt32(),fields_login[3].GetUInt32());
        }
        else
            PSendSysMessage(LANG_ACCOUNT_LIST_ERROR,name.c_str());

    } while (result_database->NextRow());

    SendSysMessage("=====================================================================");
    return true;
}

// Create an account
bool ChatHandler::HandleAccountCreateCommand(const char* args)
{
    if (!*args)
        return false;

    // Parse the command line arguments
    char* szAcc = strtok((char*)args, " ");
    char* szPassword = strtok(NULL, " ");
    if (!szAcc || !szPassword)
        return false;

    // Normalized in sAccountMgr->CreateAccount
    std::string account_name = szAcc;
    std::string password = szPassword;

    AccountOpResult result = sAccountMgr->CreateAccount(account_name, password);
    switch(result)
    {
    case AOR_OK:
        PSendSysMessage(LANG_ACCOUNT_CREATED,account_name.c_str());
        break;
    case AOR_NAME_TOO_LONG:
        SendSysMessage(LANG_ACCOUNT_TOO_LONG);
        SetSentErrorMessage(true);
        return false;
    case AOR_NAME_ALREDY_EXIST:
        SendSysMessage(LANG_ACCOUNT_ALREADY_EXIST);
        SetSentErrorMessage(true);
        return false;
    case AOR_DB_INTERNAL_ERROR:
        PSendSysMessage(LANG_ACCOUNT_NOT_CREATED_SQL_ERROR,account_name.c_str());
        SetSentErrorMessage(true);
        return false;
    default:
        PSendSysMessage(LANG_ACCOUNT_NOT_CREATED,account_name.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    return true;
}

// Set the level of logging
bool ChatHandler::HandleServerSetLogLevelCommand(const char* args)
{
    if (!*args)
        return false;

    char* new_level = strtok((char*)args, " ");
    if (!new_level)
        return false;

    sLog.SetLogLevel(new_level);
    return true;
}

// Set diff time record interval
bool ChatHandler::HandleServerSetDiffTimeCommand(const char* args)
{
    if (!*args)
        return false;

    char* new_time_string = strtok((char*)args, " ");
    if (!new_time_string)
        return false;

    int32 new_time = atoi(new_time_string);
    if (new_time < 0)
        return false;

    sWorld.SetRecordDiffInterval(new_time);
    printf( "Record diff every %u ms\n", new_time);
    return true;
}

#ifdef linux
// Non-blocking keypress detector, when return pressed, return 1, else always return 0
int kb_hit_return()
{
    struct timeval tv;
    fd_set fds;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    select(STDIN_FILENO+1, &fds, NULL, NULL, &tv);
    return FD_ISSET(STDIN_FILENO, &fds);
}
#endif

// Thread start
void CliRunnable::run()
{
    // Init new SQL thread for the world database (one connection call enough)
    WorldDatabase.ThreadStart(); // Let thread do safe mySQL requests

    #if PLATFORM == WINDOWS
    char command_buffer[256];
    #endif
    // Display the list of available CLI functions then beep
    sLog.outString();
    #if PLATFORM != WINDOWS
    rl_attempted_completion_function = cli_completion;
    #endif
    if (sConfig.GetBoolDefault("BeepAtStart", true))
        printf("\a");
    printf("Oregon>");

    // As long as the World is running (no World::m_stopEvent), get the command line and handle it
    while (!World::IsStopped())
    {
        fflush(stdout);
        char* command_string; // = fgets(commandbuf,sizeof(commandbuf),stdin);
        #if PLATFORM == WINDOWS
        command_string = fgets(command_buffer, sizeof(command_buffer), stdin);
        #else
        command_str = readline("Oregon>");
        rl_bind_key('\t',rl_complete);
        #endif
        if (command_string != NULL)
        {
            for (int x=0; command_string[x]; x++)
                if (command_string[x]=='\r'||command_string[x]=='\n')
                {
                    command_string[x]=0;
                    break;
                }

            if (!*command_string)
            {
                #if PLATFORM == WINDOWS
                printf("Oregon>");
                #endif
                continue;
            }

            std::string command;
            if (!consoleToUtf8(command_string, command)) // Convert from console encoding to utf8
            {
                #if PLATFORM == WINDOWS
                printf("Oregon>");
                #endif
                continue;
            }
            fflush(stdout);
            sWorld.QueueCliCommand(new CliCommandHolder(NULL, command.c_str(), &utf8print, &commandFinished));
            #if PLATFORM != WINDOWS
            add_history(command.c_str());
            #endif
        }
        else if (feof(stdin))
            World::StopNow(SHUTDOWN_EXIT_CODE);
    }

    // End the database thread
    WorldDatabase.ThreadEnd(); // Free MySQL thread resources
}

