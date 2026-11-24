// core/deck/transcript_page.hpp — lib/transcript-page.js, ported: pages of a session's transcript for the phone's
// text view. It reads like the terminal, not like the file: tool calls collapse into one "Called …" line with
// their results under it, tool results posted back as "user" records are dropped, and the machinery's own
// postings (system reminders, command output, skill bodies) are kept but folded under a label, never shown as you.
// PURE: the caller reads the file. Checked against core/test/fixtures/deck/transcript-page.json.
#pragma once
#include "../third_party/nlohmann/json.hpp"
#include <string>

namespace deck { namespace core {

using json = nlohmann::json;

// Every entry of the conversation, in order: { role: 'user' | 'assistant' | 'tool' | 'meta', at, text, i, … }
json transcriptEntries(const std::string& jsonl);
// A page ending just before `before` (the newest page when it is null): { total, from, to, entries }
json transcriptPage(const std::string& jsonl, const json& opts);

}}  // namespace deck::core
