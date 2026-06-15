// lua.hpp — C++ wrapper for Lua headers
// This file allows including Lua from C++ code (used by sol2)

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}
