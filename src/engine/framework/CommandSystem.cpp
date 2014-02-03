/*
===========================================================================
Daemon BSD Source Code
Copyright (c) 2013-2014, Daemon Developers
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
===========================================================================
*/

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

#include "CommandSystem.h"

//TODO: use case-insensitive comparisons for commands (store the lower case version?)
namespace Cmd {

    Log::Logger commandLog("common.commands");

    /*
    ===============================================================================

    Cmd:: The command buffer

    ===============================================================================
    */

    // A buffered command and its environment
    struct BufferEntry {
        std::string text;
        Environment* env;
        bool parseCvars;
    };

    std::vector<BufferEntry> commandBuffer;

    void BufferCommandTextInternal(Str::StringRef text, bool parseCvars, Environment* env, bool insertAtTheEnd) {
        auto insertPoint = insertAtTheEnd ? commandBuffer.end() : commandBuffer.begin();

        // Iterates over the commands in the text
        const char* current = text.data();
        const char* end = text.data() + text.size();
        do {
            const char* next = SplitCommand(current, end);
            std::string command(current, next != end ? next - 1 : end);

            insertPoint = ++commandBuffer.insert(insertPoint, {std::move(command), env, parseCvars});

            current = next;
        } while (current != end);
    }

    void BufferCommandText(Str::StringRef text, bool parseCvars, Environment* env) {
        BufferCommandTextInternal(text, parseCvars, env, true);
    }

    void BufferCommandTextAfter(Str::StringRef text, bool parseCvars, Environment* env) {
        BufferCommandTextInternal(text, parseCvars, env, false);
    }

    void ExecuteCommandBuffer() {
        // Note that commands may be inserted into the buffer while running other commands
        while (not commandBuffer.empty()) {
            auto entry = std::move(commandBuffer.front());
            commandBuffer.erase(commandBuffer.begin());
            ExecuteCommand(entry.text, entry.parseCvars, entry.env);
        }
        commandBuffer.clear();
    }

    /*
    ===============================================================================

    Cmd:: Registration and execution

    ===============================================================================
    */

    // Commands are stored alongside their description
    struct commandRecord_t {
        std::string description;
        const CmdBase* cmd;
    };

    typedef std::unordered_map<std::string, commandRecord_t, Str::IHash, Str::IEqual> CommandMap;

    // The order in which static global variables are initialized is undefined and commands
    // can be registered before main. The first time this function is called the command map
    // is initialized so we are sure it is initialized as soon as we need it.
    CommandMap& GetCommandMap() {
        static CommandMap* commands = new CommandMap();
        return *commands;
    }

    // Used to emalute the C API
    //TODO: remove the need for this
    Args currentArgs;
    Args oldArgs;

    void AddCommand(std::string name, const CmdBase& cmd, std::string description) {
        CommandMap& commands = GetCommandMap();

        if (!IsValidCmdName(name)) {
            commandLog.Warn(_("Cmd::AddCommand: Invalid command name '%s'"), name);
            return;
        }

        if (!commands.insert({std::move(name), commandRecord_t{std::move(description), &cmd}}).second) {
            commandLog.Warn(_("Cmd::AddCommand: %s already defined"), name);
        }
    }

    void ChangeDescription(std::string name, std::string description) {
        CommandMap& commands = GetCommandMap();

        auto it = commands.find(name);
        if (it != commands.end()) {
            it->second.description = std::move(description);
        }
    }

    void RemoveCommand(const std::string& name) {
        CommandMap& commands = GetCommandMap();

        commands.erase(name);
    }

    void RemoveFlaggedCommands(int flag) {
        CommandMap& commands = GetCommandMap();

        for (auto it = commands.cbegin(); it != commands.cend();) {
            const commandRecord_t& record = it->second;

            if (record.cmd->GetFlags() & flag) {
                commands.erase(it ++);
            } else {
                ++ it;
            }
        }
    }

    bool CommandExists(const std::string& name) {
        CommandMap& commands = GetCommandMap();

        return commands.find(name) != commands.end();
    }

    DefaultEnvironment defaultEnv;

    // Command execution is sequential so we make their environment a global variable.
    Environment* storedEnvironment = &defaultEnv;



    void ExecuteCommand(Str::StringRef command, bool parseCvars, Environment* env) {
        CommandMap& commands = GetCommandMap();

        commandLog.Debug("Execing command '%s'", command);
        if (not env) {
            env = &defaultEnv;
        }

        std::string parsedString;
        if (parseCvars) {
            parsedString = SubstituteCvars(command);
            command = parsedString;
        }

        Args args(command);
        currentArgs = args;

        if (args.Argc() == 0) {
            return;
        }

        const std::string& cmdName = args.Argv(0);

        auto it = commands.find(cmdName);
        if (it != commands.end()) {
            storedEnvironment = env;
            it->second.cmd->Run(args);
            return;
        }

        //TODO: remove that and add default command handlers or something
        // send it as a server command if we are connected
        // (cvars are expanded locally)
        CL_ForwardCommandToServer(args.EscapedArgs(0).c_str());
    }

    CompletionResult CompleteArgument(const Args& args, int argNum) {
        CommandMap& commands = GetCommandMap();

        commandLog.Debug("Completing argument %i of '%s'", argNum, args.ConcatArgs(0));

        if (args.Argc() == 0) {
            return {};
        }

        if (argNum > 0) {
            const std::string& cmdName = args.Argv(0);

            auto it = commands.find(cmdName);
            if (it == commands.end()) {
                return {};
            }

            std::string prefix;
            if (argNum < args.Argc()) {
                prefix = args.Argv(argNum);
            }

            return it->second.cmd->Complete(argNum, args, prefix);
        } else {
            return CompleteCommandNames(args.Argv(0));
        }
    }

    CompletionResult CompleteCommandNames(Str::StringRef prefix) {
        CommandMap& commands = GetCommandMap();

        CompletionResult res;
        for (auto& entry: commands) {
            if (Str::IsIPrefix(prefix, entry.first)) {
                res.push_back({entry.first, entry.second.description});
            }
        }
        return res;
    }

    CompletionResult CompletionFilter(Str::StringRef prefix, std::initializer_list<CompletionItem> list) {
        return CompletionFilter({}, prefix, list);
    }

    CompletionResult CompletionFilter(CompletionResult &&res, Str::StringRef prefix, std::initializer_list<CompletionItem> list) {
        for (auto item: list) {
            if (Str::IsIPrefix(prefix, item.first)) {
                res.push_back({item.first, item.second});
            }
        }
        return res;
    }

    const Args& GetCurrentArgs() {
        return currentArgs;
    }

    void SetCurrentArgs(const Args& args) {
        currentArgs = args;
    }

    void SaveArgs() {
        oldArgs = currentArgs;
    }

    void LoadArgs() {
        currentArgs = oldArgs;
    }

    Environment* GetEnv() {
        return storedEnvironment;
    }

    void ResetEnv() {
        storedEnvironment = &defaultEnv;
    }

    /*
    ===============================================================================

    Cmd:: DefaultEnvironment

    ===============================================================================
    */

    void DefaultEnvironment::Print(Str::StringRef text) {
        Log::CodeSourceNotice(text);
    }

    void DefaultEnvironment::ExecuteAfter(Str::StringRef text, bool parseCvars) {
        BufferCommandTextAfter(text, parseCvars, this);
    }

    /*
    ===============================================================================

    Cmd:: /list<Subsystem>Commands

    ===============================================================================
    */


    class ListCmdsCmd: public StaticCmd {
        public:
            ListCmdsCmd(std::string name, int cmdFlags, std::string description, int showCmdFlags)
            :StaticCmd(std::move(name), cmdFlags, std::move(description)), showCmdFlags(showCmdFlags) {
            }

            void Run(const Cmd::Args& args) const OVERRIDE {
                CommandMap& commands = GetCommandMap();

                std::vector<const commandRecord_t*> matches;
                std::vector<const std::string*> matchesNames;
                unsigned long maxNameLength = 0;

                //Find all the matching commands and their names
                for (auto it = commands.cbegin(); it != commands.cend(); ++it) {
                    const commandRecord_t& record = it->second;

                    // /listCmds's arguement is used for prefix matching
                    if (args.Argc() >= 2 and not Str::IsIPrefix(args.Argv(1), it->first)) {
                        continue;
                    }

                    if (record.cmd->GetFlags() & showCmdFlags) {
                        matches.push_back(&it->second);
                        matchesNames.push_back(&it->first);
                        maxNameLength = std::max<size_t>(maxNameLength, it->first.size());
                    }
                }

                //Print the matches, keeping the description aligned
                for (unsigned i = 0; i < matches.size(); i++) {
                    int toFill = maxNameLength - matchesNames[i]->size();
                    Print("  %s%s %s", matchesNames[i]->c_str(), std::string(toFill, ' ').c_str(), matches[i]->description.c_str());
                }

                Print("%zu commands", matches.size());
            }

            Cmd::CompletionResult Complete(int argNum, const Cmd::Args& args, Str::StringRef prefix) const OVERRIDE {
                Q_UNUSED(args);

                if (argNum == 1) {
                    return ::Cmd::CompleteCommandNames(prefix);
                }

                return {};
            }

        private:
            int showCmdFlags;
    };

    static ListCmdsCmd listCmdsRegistration("listCmds", BASE, "lists all the commands", ~(CVAR|ALIAS));
    static ListCmdsCmd listBaseCmdsRegistration("listBaseCmds", BASE, "lists all the base commands", BASE);
    static ListCmdsCmd listSystemCmdsRegistration("listSystemCmds", BASE | SYSTEM, "lists all the system commands", SYSTEM);
    static ListCmdsCmd listRendererCmdsRegistration("listRendererCmds", BASE | RENDERER, "lists all the renderer commands", RENDERER);
    static ListCmdsCmd listAudioCmdsRegistration("listAudioCmds", BASE | AUDIO, "lists all the audio commands", AUDIO);
    static ListCmdsCmd listCGameCmdsRegistration("listCGameCmds", BASE | CGAME_VM, "lists all the client-side game commands", CGAME_VM);
    static ListCmdsCmd listGameCmdsRegistration("listGameCmds", BASE | GAME_VM, "lists all the server-side game commands", CGAME_VM);
    static ListCmdsCmd listUICmdsRegistration("listUICmds", BASE | UI_VM, "lists all the UI commands", CGAME_VM);
    static ListCmdsCmd listOldStyleCmdsRegistration("listOldStyleCmds", BASE, "lists all the commands registered through the C interface", PROXY_FOR_OLD);
}
