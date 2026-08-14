#include "lua_runtime.h"

#include <algorithm>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "esp_random.h"

#include "application.h"
#include "board.h"
#include "display.h"
#include "lua_bindings.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

static_assert(LUA_EXTRASPACE >= sizeof(LuaRuntime*));

namespace {

constexpr char kTag[] = "LuaRuntime";

extern const uint8_t bootstrap_lua_start[] asm("_binary_bootstrap_lua_start");
extern const uint8_t bootstrap_lua_end[] asm("_binary_bootstrap_lua_end");

struct SafeLibrary {
    const char* name;
    lua_CFunction open;
};

constexpr SafeLibrary kSafeLibraries[] = {
    {LUA_GNAME, luaopen_base},       {LUA_COLIBNAME, luaopen_coroutine},
    {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
    {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8},
};

}  // namespace

LuaRuntime& LuaRuntime::GetInstance() {
    static LuaRuntime instance;
    return instance;
}

bool LuaRuntime::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return true;
    }

    stop_requested_.store(false);
    if (queue_ == nullptr) {
        queue_ = xQueueCreate(CONFIG_XIAOZHI_LUA_EVENT_QUEUE_LENGTH, sizeof(Command));
    } else {
        xQueueReset(queue_);
    }
    if (queue_ == nullptr) {
        ESP_LOGE(kTag, "Failed to create event queue");
        running_.store(false);
        return false;
    }
    if (action_queue_ == nullptr) {
        action_queue_ = xQueueCreate(CONFIG_XIAOZHI_LUA_EVENT_QUEUE_LENGTH, sizeof(Action));
    } else {
        xQueueReset(action_queue_);
    }
    if (action_queue_ == nullptr) {
        ESP_LOGE(kTag, "Failed to create action queue");
        running_.store(false);
        return false;
    }
    action_drain_scheduled_.store(false);

    BaseType_t result = xTaskCreate(TaskEntry, "lua_runtime", CONFIG_XIAOZHI_LUA_TASK_STACK_SIZE,
                                    this, CONFIG_XIAOZHI_LUA_TASK_PRIORITY, nullptr);
    if (result != pdPASS) {
        ESP_LOGE(kTag, "Failed to create runtime task");
        running_.store(false);
        return false;
    }
    return true;
}

void LuaRuntime::Stop() {
    if (!running_.load() || queue_ == nullptr) {
        return;
    }
    stop_requested_.store(true);
    Command command{};
    command.type = CommandType::Stop;
    xQueueSend(queue_, &command, 0);
}

bool LuaRuntime::PostEvent(const char* name, const char* payload) {
    if (!running_.load() || queue_ == nullptr || name == nullptr || payload == nullptr) {
        return false;
    }
    if (std::strlen(name) >= sizeof(Command::name) ||
        std::strlen(payload) >= sizeof(Command::payload)) {
        ESP_LOGW(kTag, "Dropping oversized Lua event");
        return false;
    }

    Command command{};
    command.type = CommandType::Event;
    std::strcpy(command.name, name);
    std::strcpy(command.payload, payload);
    if (xQueueSend(queue_, &command, 0) != pdTRUE) {
        ESP_LOGW(kTag, "Dropping Lua event because the queue is full");
        return false;
    }
    return true;
}

bool LuaRuntime::PostNotification(const char* message, size_t length, int duration_ms) {
    if (message == nullptr || length >= sizeof(Action::text)) {
        return false;
    }
    Action action{};
    action.type = ActionType::Notify;
    action.value = duration_ms;
    std::memcpy(action.text, message, length);
    action.text[length] = '\0';
    return PostAction(action);
}

bool LuaRuntime::PostEmotion(const char* emotion, size_t length) {
    if (emotion == nullptr || length >= sizeof(Action::text)) {
        return false;
    }
    Action action{};
    action.type = ActionType::SetEmotion;
    std::memcpy(action.text, emotion, length);
    action.text[length] = '\0';
    return PostAction(action);
}

bool LuaRuntime::PostListeningRequest(bool start) {
    Action action{};
    action.type = start ? ActionType::StartListening : ActionType::StopListening;
    return PostAction(action);
}

void LuaRuntime::TaskEntry(void* arg) { static_cast<LuaRuntime*>(arg)->Run(); }

void* LuaRuntime::Allocate(void* user_data, void* ptr, size_t old_size, size_t new_size) {
    auto* allocator = static_cast<AllocatorContext*>(user_data);
    size_t allocated_size = ptr == nullptr ? 0 : old_size;

    if (new_size == 0) {
        heap_caps_free(ptr);
        allocator->used -= std::min(allocator->used, allocated_size);
        return nullptr;
    }

    size_t base_usage = allocator->used - std::min(allocator->used, allocated_size);
    if (new_size > allocator->limit - std::min(allocator->limit, base_usage)) {
        return nullptr;
    }

    void* result = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (result != nullptr) {
        allocator->used = base_usage + new_size;
        allocator->peak = std::max(allocator->peak, allocator->used);
    }
    return result;
}

void LuaRuntime::InstructionHook(lua_State* state, lua_Debug* debug) {
    (void)debug;
    auto* runtime = *static_cast<LuaRuntime**>(lua_getextraspace(state));
    if (runtime == nullptr) {
        return;
    }
    if (runtime->stop_requested_.load()) {
        luaL_error(state, "Lua execution cancelled");
    }
    if (runtime->deadline_us_ > 0 && esp_timer_get_time() > runtime->deadline_us_) {
        luaL_error(state, "Lua callback timed out");
    }
}

void LuaRuntime::Run() {
    ESP_LOGI(kTag, "Runtime task started");
    if (!InitializeState() || !LoadBootstrapScript()) {
        CloseState();
        running_.store(false);
        ESP_LOGE(kTag, "Runtime task stopped during initialization");
        vTaskDelete(nullptr);
        return;
    }

    lua_getglobal(state_, "on_start");
    if (lua_isfunction(state_, -1)) {
        CallProtected("on_start", 0);
    } else {
        lua_pop(state_, 1);
    }

    Command command{};
    while (!stop_requested_.load()) {
        if (xQueueReceive(queue_, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (command.type == CommandType::Stop) {
            break;
        }
        if (state_ != nullptr) {
            HandleEvent(command);
        }
    }

    CloseState();
    running_.store(false);
    ESP_LOGI(kTag, "Runtime task stopped");
    vTaskDelete(nullptr);
}

bool LuaRuntime::InitializeState() {
    allocator_.used = 0;
    allocator_.limit = CONFIG_XIAOZHI_LUA_HEAP_LIMIT_KB * 1024;
    state_ = lua_newstate(Allocate, &allocator_, esp_random());
    if (state_ == nullptr) {
        ESP_LOGE(kTag, "Failed to create Lua state");
        return false;
    }

    *static_cast<LuaRuntime**>(lua_getextraspace(state_)) = this;
    for (const auto& library : kSafeLibraries) {
        luaL_requiref(state_, library.name, library.open, 1);
        lua_pop(state_, 1);
    }
    RegisterXiaozhiLuaBindings(state_);

    const char* blocked_globals[] = {"dofile", "load", "loadfile", "print"};
    for (const char* name : blocked_globals) {
        lua_pushnil(state_);
        lua_setglobal(state_, name);
    }
    return true;
}

bool LuaRuntime::LoadBootstrapScript() {
    size_t script_size = bootstrap_lua_end - bootstrap_lua_start;
    int result = luaL_loadbufferx(state_, reinterpret_cast<const char*>(bootstrap_lua_start),
                                  script_size, "@bootstrap.lua", "t");
    if (result != LUA_OK) {
        ESP_LOGE(kTag, "Failed to load bootstrap.lua: %s", lua_tostring(state_, -1));
        lua_pop(state_, 1);
        return false;
    }
    return CallProtected("bootstrap.lua", 0);
}

bool LuaRuntime::CallProtected(const char* context, int argument_count) {
    deadline_us_ = esp_timer_get_time() + CONFIG_XIAOZHI_LUA_CALLBACK_TIMEOUT_MS * 1000LL;
    lua_sethook(state_, InstructionHook, LUA_MASKCOUNT, CONFIG_XIAOZHI_LUA_HOOK_INSTRUCTION_COUNT);
    int result = lua_pcall(state_, argument_count, 0, 0);
    lua_sethook(state_, nullptr, 0, 0);
    deadline_us_ = 0;
    if (result != LUA_OK) {
        ESP_LOGE(kTag, "%s failed: %s", context, lua_tostring(state_, -1));
        lua_pop(state_, 1);
        return false;
    }
    return true;
}

void LuaRuntime::HandleEvent(const Command& command) {
    lua_getglobal(state_, "on_event");
    if (!lua_isfunction(state_, -1)) {
        lua_pop(state_, 1);
        return;
    }
    lua_pushstring(state_, command.name);
    lua_pushstring(state_, command.payload);
    CallProtected("on_event", 2);
}

bool LuaRuntime::PostAction(const Action& action) {
    if (!running_.load() || action_queue_ == nullptr) {
        return false;
    }
    if (xQueueSend(action_queue_, &action, 0) != pdTRUE) {
        ESP_LOGW(kTag, "Dropping Lua action because the queue is full");
        return false;
    }
    ScheduleActionDrain();
    return true;
}

void LuaRuntime::ScheduleActionDrain() {
    bool expected = false;
    if (!action_drain_scheduled_.compare_exchange_strong(expected, true)) {
        return;
    }
    Application::GetInstance().Schedule([this]() { DrainActions(); });
}

void LuaRuntime::DrainActions() {
    constexpr int kMaxActionsPerDrain = 4;
    Action action{};
    for (int i = 0; i < kMaxActionsPerDrain; ++i) {
        if (xQueueReceive(action_queue_, &action, 0) != pdTRUE) {
            break;
        }
        switch (action.type) {
            case ActionType::Notify:
                Board::GetInstance().GetDisplay()->ShowNotification(action.text, action.value);
                break;
            case ActionType::SetEmotion:
                Board::GetInstance().GetDisplay()->SetEmotion(action.text);
                break;
            case ActionType::StartListening:
                Application::GetInstance().StartListening();
                break;
            case ActionType::StopListening:
                Application::GetInstance().StopListening();
                break;
        }
    }

    action_drain_scheduled_.store(false);
    if (uxQueueMessagesWaiting(action_queue_) > 0) {
        ScheduleActionDrain();
    }
}

void LuaRuntime::CloseState() {
    size_t peak = allocator_.peak;
    if (state_ != nullptr) {
        lua_close(state_);
        state_ = nullptr;
    }
    ESP_LOGI(kTag, "Lua heap peak: %u bytes", static_cast<unsigned>(peak));
    allocator_ = {};
}
