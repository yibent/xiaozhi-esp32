#include "lua_bindings.h"

#include <esp_log.h>

#include "application.h"
#include "device_state_machine.h"
#include "lua_runtime.h"
#include "lua_ui_bindings.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace {

constexpr size_t kMaxLogLength = 512;
constexpr size_t kMaxNotificationLength = 255;
constexpr size_t kMaxEmotionLength = 32;

int Log(lua_State* state) {
    size_t length = 0;
    const char* message = luaL_checklstring(state, 1, &length);
    luaL_argcheck(state, length <= kMaxLogLength, 1, "message is too long");
    ESP_LOGI("Lua", "%.*s", static_cast<int>(length), message);
    LuaRuntime::GetInstance().EmitLog(message, length);
    return 0;
}

int GetState(lua_State* state) {
    auto device_state = Application::GetInstance().GetDeviceState();
    lua_pushstring(state, DeviceStateMachine::GetStateName(device_state));
    return 1;
}

int Notify(lua_State* state) {
    size_t length = 0;
    const char* message = luaL_checklstring(state, 1, &length);
    luaL_argcheck(state, length <= kMaxNotificationLength, 1, "notification is too long");
    int duration_ms = static_cast<int>(luaL_optinteger(state, 2, 3000));
    luaL_argcheck(state, duration_ms >= 100 && duration_ms <= 30000, 2,
                  "duration must be between 100 and 30000 ms");

    lua_pushboolean(state,
                    LuaRuntime::GetInstance().PostNotification(message, length, duration_ms));
    return 1;
}

int SetEmotion(lua_State* state) {
    size_t length = 0;
    const char* emotion = luaL_checklstring(state, 1, &length);
    luaL_argcheck(state, length > 0 && length <= kMaxEmotionLength, 1, "emotion name is invalid");

    lua_pushboolean(state, LuaRuntime::GetInstance().PostEmotion(emotion, length));
    return 1;
}

int StartListening(lua_State* state) {
    lua_pushboolean(state, LuaRuntime::GetInstance().PostListeningRequest(true));
    return 1;
}

int StopListening(lua_State* state) {
    lua_pushboolean(state, LuaRuntime::GetInstance().PostListeningRequest(false));
    return 1;
}

const luaL_Reg kXiaozhiFunctions[] = {
    {"log", Log},
    {"get_state", GetState},
    {"notify", Notify},
    {"set_emotion", SetEmotion},
    {"start_listening", StartListening},
    {"stop_listening", StopListening},
    {nullptr, nullptr},
};

}  // namespace

void RegisterXiaozhiLuaBindings(lua_State* state) {
    luaL_newlib(state, kXiaozhiFunctions);
    RegisterLuaUiBindings(state);
    lua_setglobal(state, "xiaozhi");
}
