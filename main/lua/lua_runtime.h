#ifndef XIAOZHI_LUA_RUNTIME_H
#define XIAOZHI_LUA_RUNTIME_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

struct lua_State;
struct lua_Debug;

class LuaRuntime {
public:
    struct RunResult {
        std::string run_id;
        std::string status;
        std::string result_json;
        std::string error_code;
        std::string error_message;
        int error_line = 0;
        int64_t duration_ms = 0;
    };

    using LogCallback = std::function<void(const std::string&, const std::string&)>;
    using RunFinishedCallback = std::function<void(const RunResult&)>;

    static LuaRuntime& GetInstance();

    bool Start();
    void Stop();
    bool PostEvent(const char* name, const char* payload = "");
    bool PostNotification(const char* message, size_t length, int duration_ms);
    bool PostEmotion(const char* emotion, size_t length);
    bool PostListeningRequest(bool start);
    bool RunScript(const std::string& run_id, const std::string& source,
                   const std::string& params_json, int timeout_ms);
    bool CancelRun(const std::string& run_id);
    std::string ActiveRunId() const;
    void SetCallbacks(LogCallback on_log, RunFinishedCallback on_finished);
    void EmitLog(const char* message, size_t length);
    bool IsRunning() const { return running_.load(); }

private:
    enum class CommandType : uint8_t {
        Event,
        Run,
        Stop,
    };

    struct Command {
        CommandType type;
        char name[32];
        char payload[256];
        void* run_request;
    };

    struct RunRequest {
        std::string run_id;
        std::string source;
        std::string params_json;
        int timeout_ms = 0;
    };

    enum class ActionType : uint8_t {
        Notify,
        SetEmotion,
        StartListening,
        StopListening,
    };

    struct Action {
        ActionType type;
        int value;
        char text[256];
    };

    struct AllocatorContext {
        size_t used = 0;
        size_t peak = 0;
        size_t limit = 0;
    };

    LuaRuntime() = default;
    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    static void TaskEntry(void* arg);
    static void* Allocate(void* user_data, void* ptr, size_t old_size, size_t new_size);
    static void InstructionHook(lua_State* state, lua_Debug* debug);

    void Run();
    bool InitializeState();
    bool LoadBootstrapScript();
    bool CallProtected(const char* context, int argument_count);
    void HandleEvent(const Command& command);
    void HandleRun(RunRequest* request);
    bool PushJsonValue(lua_State* state, const char* json, std::string* error);
    bool LuaValueToJson(lua_State* state, int index, std::string* output, std::string* error,
                        int depth = 0);
    bool PostAction(const Action& action);
    void ScheduleActionDrain();
    void DrainActions();
    void CloseState();

    QueueHandle_t queue_ = nullptr;
    QueueHandle_t action_queue_ = nullptr;
    lua_State* state_ = nullptr;
    AllocatorContext allocator_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> action_drain_scheduled_{false};
    int64_t deadline_us_ = 0;
    mutable std::mutex callback_mutex_;
    LogCallback on_log_;
    RunFinishedCallback on_finished_;
    mutable std::mutex active_run_mutex_;
    std::string active_run_id_;
};

#endif  // XIAOZHI_LUA_RUNTIME_H
