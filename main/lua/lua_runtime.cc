#include "lua_runtime.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "esp_random.h"
#include <cJSON.h>

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

bool LuaRuntime::RunScript(const std::string& run_id, const std::string& source,
                           const std::string& params_json, int timeout_ms) {
    if (!running_.load() || queue_ == nullptr || run_id.empty() || source.empty()) {
        return false;
    }
    auto request = std::make_unique<RunRequest>();
    request->run_id = run_id;
    request->source = source;
    request->params_json = params_json;
    request->timeout_ms = timeout_ms;
    Command command{};
    command.type = CommandType::Run;
    command.run_request = request.get();
    if (xQueueSend(queue_, &command, 0) != pdTRUE) {
        ESP_LOGW(kTag, "Lua run queue is full");
        return false;
    }
    request.release();
    return true;
}

bool LuaRuntime::CancelRun(const std::string& run_id) {
    std::lock_guard<std::mutex> lock(active_run_mutex_);
    if (active_run_id_ != run_id || run_id.empty()) {
        return false;
    }
    cancel_requested_.store(true);
    return true;
}

std::string LuaRuntime::ActiveRunId() const {
    std::lock_guard<std::mutex> lock(active_run_mutex_);
    return active_run_id_;
}

void LuaRuntime::SetCallbacks(LogCallback on_log, RunFinishedCallback on_finished) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_log_ = std::move(on_log);
    on_finished_ = std::move(on_finished);
}

void LuaRuntime::EmitLog(const char* message, size_t length) {
    std::string run_id = ActiveRunId();
    if (run_id.empty()) {
        return;
    }
    LogCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = on_log_;
    }
    if (callback != nullptr) {
        callback(run_id, std::string(message, length));
    }
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
    if (runtime->stop_requested_.load() || runtime->cancel_requested_.load()) {
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
        if (command.type == CommandType::Run) {
            HandleRun(static_cast<RunRequest*>(command.run_request));
            continue;
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

bool LuaRuntime::PushJsonValue(lua_State* state, const char* json, std::string* error) {
    cJSON* value = cJSON_Parse(json);
    if (value == nullptr) {
        *error = "params is not valid JSON";
        return false;
    }
    std::function<bool(const cJSON*)> push = [&](const cJSON* item) {
        if (cJSON_IsNull(item)) {
            lua_pushnil(state);
        } else if (cJSON_IsBool(item)) {
            lua_pushboolean(state, cJSON_IsTrue(item));
        } else if (cJSON_IsNumber(item)) {
            lua_pushnumber(state, item->valuedouble);
        } else if (cJSON_IsString(item)) {
            lua_pushstring(state, item->valuestring);
        } else if (cJSON_IsArray(item)) {
            lua_createtable(state, cJSON_GetArraySize(item), 0);
            int index = 1;
            cJSON* child = nullptr;
            cJSON_ArrayForEach (child, item) {
                if (!push(child))
                    return false;
                lua_rawseti(state, -2, index++);
            }
        } else if (cJSON_IsObject(item)) {
            lua_createtable(state, 0, 8);
            cJSON* child = nullptr;
            cJSON_ArrayForEach (child, item) {
                if (child->string == nullptr || !push(child))
                    return false;
                lua_setfield(state, -2, child->string);
            }
        } else {
            *error = "unsupported params value";
            return false;
        }
        return true;
    };
    bool ok = push(value);
    cJSON_Delete(value);
    return ok;
}

bool LuaRuntime::LuaValueToJson(lua_State* state, int index, std::string* output,
                                std::string* error, int depth) {
    if (depth > 8) {
        *error = "result nesting is too deep";
        return false;
    }
    index = lua_absindex(state, index);
    cJSON* root = nullptr;
    std::function<cJSON*(int, int)> convert = [&](int value_index, int level) -> cJSON* {
        if (level > 8)
            return nullptr;
        value_index = lua_absindex(state, value_index);
        switch (lua_type(state, value_index)) {
            case LUA_TNIL:
                return cJSON_CreateNull();
            case LUA_TBOOLEAN:
                return cJSON_CreateBool(lua_toboolean(state, value_index));
            case LUA_TNUMBER:
                return cJSON_CreateNumber(lua_tonumber(state, value_index));
            case LUA_TSTRING:
                return cJSON_CreateString(lua_tostring(state, value_index));
            case LUA_TTABLE: {
                size_t length = lua_rawlen(state, value_index);
                bool array = true;
                size_t count = 0;
                lua_pushnil(state);
                while (lua_next(state, value_index) != 0) {
                    ++count;
                    bool numeric = lua_isinteger(state, -2) && lua_tointeger(state, -2) >= 1 &&
                                   static_cast<size_t>(lua_tointeger(state, -2)) <= length;
                    lua_pop(state, 1);
                    if (!numeric)
                        array = false;
                }
                cJSON* result =
                    array && count == length ? cJSON_CreateArray() : cJSON_CreateObject();
                if (result == nullptr)
                    return nullptr;
                if (array && count == length) {
                    for (size_t i = 1; i <= length; ++i) {
                        lua_rawgeti(state, value_index, i);
                        cJSON* child = convert(-1, level + 1);
                        lua_pop(state, 1);
                        if (child == nullptr) {
                            cJSON_Delete(result);
                            return nullptr;
                        }
                        cJSON_AddItemToArray(result, child);
                    }
                } else {
                    lua_pushnil(state);
                    while (lua_next(state, value_index) != 0) {
                        if (!lua_isstring(state, -2)) {
                            lua_pop(state, 2);
                            cJSON_Delete(result);
                            return nullptr;
                        }
                        const char* key = lua_tostring(state, -2);
                        cJSON* child = convert(-1, level + 1);
                        lua_pop(state, 1);
                        if (child == nullptr) {
                            cJSON_Delete(result);
                            return nullptr;
                        }
                        cJSON_AddItemToObject(result, key, child);
                    }
                }
                return result;
            }
            default:
                return nullptr;
        }
    };
    root = convert(index, depth);
    if (root == nullptr) {
        *error = "result must be JSON-compatible";
        return false;
    }
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == nullptr) {
        *error = "unable to encode result";
        return false;
    }
    *output = text;
    cJSON_free(text);
    if (output->size() > 16384) {
        *error = "result is too large";
        return false;
    }
    return true;
}

void LuaRuntime::HandleRun(RunRequest* request) {
    std::unique_ptr<RunRequest> holder(request);
    RunResult result;
    result.run_id = request->run_id;
    int64_t started_at = esp_timer_get_time();
    {
        std::lock_guard<std::mutex> lock(active_run_mutex_);
        active_run_id_ = request->run_id;
    }
    cancel_requested_.store(false);

    AllocatorContext remote_allocator{};
    remote_allocator.limit = CONFIG_XIAOZHI_LUA_HEAP_LIMIT_KB * 1024;
    lua_State* remote = lua_newstate(Allocate, &remote_allocator, esp_random());
    if (remote == nullptr) {
        result.status = "failed";
        result.error_code = "LUA_MEMORY_ERROR";
        result.error_message = "failed to create Lua state";
    } else {
        *static_cast<LuaRuntime**>(lua_getextraspace(remote)) = this;
        for (const auto& library : kSafeLibraries) {
            luaL_requiref(remote, library.name, library.open, 1);
            lua_pop(remote, 1);
        }
        RegisterXiaozhiLuaBindings(remote);
        const char* blocked[] = {"dofile", "load", "loadfile", "print"};
        for (const char* name : blocked) {
            lua_pushnil(remote);
            lua_setglobal(remote, name);
        }
        int load_result = luaL_loadbufferx(remote, request->source.data(), request->source.size(),
                                           "@remote.lua", "t");
        if (load_result != LUA_OK) {
            result.status = "failed";
            result.error_code = "LUA_COMPILE_ERROR";
            result.error_message = lua_tostring(remote, -1);
        } else {
            deadline_us_ = started_at + static_cast<int64_t>(request->timeout_ms) * 1000;
            lua_sethook(remote, InstructionHook, LUA_MASKCOUNT,
                        CONFIG_XIAOZHI_LUA_HOOK_INSTRUCTION_COUNT);
            int call_result = lua_pcall(remote, 0, 0, 0);
            lua_sethook(remote, nullptr, 0, 0);
            deadline_us_ = 0;
            if (call_result != LUA_OK) {
                result.status = cancel_requested_.load() ? "stopped" : "failed";
                result.error_code = cancel_requested_.load() ? "RUN_STOPPED" : "LUA_RUNTIME_ERROR";
                result.error_message = lua_tostring(remote, -1);
            } else {
                lua_getglobal(remote, "main");
                std::string params_error;
                if (!lua_isfunction(remote, -1) ||
                    !PushJsonValue(remote, request->params_json.c_str(), &params_error)) {
                    lua_settop(remote, 0);
                    result.status = "failed";
                    result.error_code = "LUA_ENTRY_ERROR";
                    result.error_message =
                        params_error.empty() ? "main(params) is required" : params_error;
                } else {
                    deadline_us_ = started_at + static_cast<int64_t>(request->timeout_ms) * 1000;
                    lua_sethook(remote, InstructionHook, LUA_MASKCOUNT,
                                CONFIG_XIAOZHI_LUA_HOOK_INSTRUCTION_COUNT);
                    call_result = lua_pcall(remote, 1, 1, 0);
                    lua_sethook(remote, nullptr, 0, 0);
                    deadline_us_ = 0;
                    if (call_result != LUA_OK) {
                        result.status = cancel_requested_.load() ? "stopped" : "failed";
                        result.error_code =
                            cancel_requested_.load()
                                ? "RUN_STOPPED"
                                : (esp_timer_get_time() >
                                           started_at +
                                               static_cast<int64_t>(request->timeout_ms) * 1000
                                       ? "RUN_TIMEOUT"
                                       : "LUA_RUNTIME_ERROR");
                        result.error_message = lua_tostring(remote, -1);
                    } else {
                        std::string encode_error;
                        if (!LuaValueToJson(remote, -1, &result.result_json, &encode_error)) {
                            result.status = "failed";
                            result.error_code = "LUA_RESULT_ERROR";
                            result.error_message = encode_error;
                        } else {
                            result.status = "succeeded";
                        }
                    }
                }
            }
        }
        lua_close(remote);
    }
    result.duration_ms = (esp_timer_get_time() - started_at) / 1000;
    if (result.status == "failed" && result.error_code == "LUA_RUNTIME_ERROR" &&
        result.duration_ms >= request->timeout_ms) {
        result.status = "timed_out";
        result.error_code = "RUN_TIMEOUT";
    }
    {
        std::lock_guard<std::mutex> lock(active_run_mutex_);
        active_run_id_.clear();
    }
    cancel_requested_.store(false);
    RunFinishedCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = on_finished_;
    }
    if (callback != nullptr)
        callback(result);
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
