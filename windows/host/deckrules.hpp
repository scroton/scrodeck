// windows/host/deckrules.hpp — the deck's pure rules moved to the shared core (core/deck/rules.hpp, 2026-11-18):
// the spawn environment, newTileSpec, THE SEAM claudeLaunch(), the alias-file accounts, DEFAULT_SETTINGS and
// applySettings. This header keeps the names under deck::host for the callers that had them there.
#pragma once
#include "../../core/deck/rules.hpp"

namespace deck { namespace host {

using json = nlohmann::json;
using core::EnvList;
using core::Account;
using core::TileSpec;
using core::ClaudeLaunchInput;
using core::ClaudeLaunch;
using core::baseEnv;
using core::envFor;
using core::parseAccounts;
using core::accountById;
using core::publicAccounts;
using core::accountRows;
using core::normalizeWinPath;
using core::newTileSpec;
using core::claudeLaunch;
using core::defaultSettings;
using core::mergeSettings;
using core::applySettings;
using core::cleanPages;
using core::hibernateConfigJson;

}}  // namespace deck::host
