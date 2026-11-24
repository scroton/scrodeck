// windows/src/mouse.hpp — MOVED: the mouse encoder is core/input/mouse.hpp (shared by every shell; its test is
// core/test/input). This forwarding header keeps the old path working (the Mac draft reaches it by a symlink).
#pragma once
#include "../../core/input/mouse.hpp"
