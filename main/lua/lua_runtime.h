#ifndef XIAOZHI_LUA_RUNTIME_H
#define XIAOZHI_LUA_RUNTIME_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

struct lua_State;
struct lua_Debug;

class LuaRuntime {
public:
    static LuaRuntime& GetInstance();

    bool Start();
    void Stop();
    bool PostEvent(const char* name, const char* payload = "");
    bool PostNotification(const char* message, size_t length, int duration_ms);
    bool PostEmotion(const char* emotion, size_t length);
    bool PostListeningRequest(bool start);
    bool IsRunning() const { return running_.load(); }

private:
    enum class CommandType : uint8_t {
        Event,
        Stop,
    };

    struct Command {
        CommandType type;
        char name[32];
        char payload[256];
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
    std::atomic<bool> action_drain_scheduled_{false};
    int64_t deadline_us_ = 0;
};

#endif  // XIAOZHI_LUA_RUNTIME_H
