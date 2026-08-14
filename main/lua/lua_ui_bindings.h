#ifndef XIAOZHI_LUA_UI_BINDINGS_H
#define XIAOZHI_LUA_UI_BINDINGS_H

struct lua_State;

// Adds the xiaozhi.ui table to the xiaozhi module table at the stack top.
void RegisterLuaUiBindings(lua_State* state);

// Used during the device handshake and run capability validation.
bool IsLuaUiAvailable();

#endif  // XIAOZHI_LUA_UI_BINDINGS_H
