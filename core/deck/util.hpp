// core/deck/util.hpp — small pure helpers the deck shares: paths (UTF-8, either separator read, the platform's
// written), Date -> ISO text, and "is this text a JSON state".
#pragma once
#include "../third_party/nlohmann/json.hpp"
#include <string>

namespace deck { namespace core {

using json = nlohmann::json;

// The separator join() writes. '\\' unless a shell on another platform says otherwise (setPathSep('/')), once,
// before it makes a deck.
char pathSep();
void setPathSep(char sep);
std::string joinPath(const std::string& a, const std::string& b);
std::string dirName(const std::string& path);
std::string baseName(const std::string& path);
std::string extName(const std::string& path);                             // path.extname, lower-cased: ".html"
std::string isoTime(double ms);                                           // new Date(ms).toISOString()
std::string isoStamp(double ms);                                          // … with ':' and '.' as '-'
// JSON text -> an object/array value, or a discarded value: nothing, whitespace or NUL bytes are not JSON here
json parseState(const std::string& text);
std::string lowerAscii(std::string s);

}}  // namespace deck::core
