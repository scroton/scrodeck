// core/deck/platform.hpp — lib/platform.js, the parts that are decisions rather than OS calls: which Claude
// binary, which shell, which bash, where userswitch and the alias file are, the window keys and their help, the
// font stack. PURE over Env (process.env, home, the platform's name) and Files (does this file exist).
// The OS calls of lib/platform.js are seams: killTree / processParents / consoleHosts -> ProcessList,
// readOAuth / credentialsStamp -> Credentials (core/deck/seams.hpp).
#pragma once
#include "seams.hpp"
#include <string>
#include <vector>

namespace deck { namespace core { namespace platform {

struct Shell { std::string file; std::vector<std::string> args; std::string name; };

std::string claudeExe(Env& env, Files& files, const std::string& override_ = "");     // CLAUDE_DECK_CLAUDE first
Shell shellCommand(Env& env);                                                          // PowerShell on Windows, the login $SHELL elsewhere
std::string bashPath(Env& env, Files& files, const std::string& override_ = "");      // Git Bash on Windows
// the user's copy in ~/.local/bin, else <resources>/bin/userswitch (a packaged build), else <appDir>/bin/userswitch
std::string userswitchPath(Env& env, Files& files, const std::string& appDir, const std::string& resourcesDir = "");
struct AliasFiles { std::string primary; std::vector<std::string> extra; };
AliasFiles aliasFiles(Env& env, Files& files);                                         // USERSWITCH_ZSHRC, else ~/.claude-code-account-aliases
json keyHelp(const std::string& os);
std::string defaultFontStack(const std::string& os);
// The window keys (before-input-event). input: { key, code, control, shift, alt, meta }. An action name or "".
std::string keyAction(const std::string& os, const json& input);
// Not here: the macOS Keychain names (keychainServiceNames) — the Swift shell's Credentials seam owns that read.

}}}  // namespace deck::core::platform
