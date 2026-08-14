#ifndef XIAOZHI_LUA_DEVICE_GATEWAY_H
#define XIAOZHI_LUA_DEVICE_GATEWAY_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

class WebSocket;
struct cJSON;
#include "lua_runtime.h"

class LuaDeviceGateway {
public:
    static LuaDeviceGateway& GetInstance();

    ~LuaDeviceGateway();
    bool Start();
    void Stop();

private:
    struct Transfer {
        std::string run_id;
        std::string source_sha256;
        std::string params_sha256;
        std::string params_json;
        std::string source;
        size_t byte_length = 0;
        size_t chunk_bytes = 0;
        int total_chunks = 0;
        int next_chunk_index = 0;
        int timeout_ms = 0;
    };

    LuaDeviceGateway() = default;
    static void TaskEntry(void* arg);
    void Run();
    void ConfigureCallbacks();
    bool Connect();
    void Disconnect();
    void HandleIncoming(const char* data, size_t length, bool binary);
    void SendHello();
    void HandleWelcome(const cJSON* data);
    void HandlePrepare(const cJSON* root, const cJSON* data);
    void HandleChunk(const cJSON* root, const cJSON* data);
    void HandleCommit(const cJSON* root, const cJSON* data);
    void HandleStop(const cJSON* root, const cJSON* data);
    void HandleFinishedAck(const cJSON* root, const cJSON* data);
    void SendStatus();
    void SendLog(const std::string& run_id, const std::string& message);
    void SendFinished(const LuaRuntime::RunResult& result);
    void SendError(const char* reply_to, const char* code, const char* message, bool retryable);
    bool SendEnvelope(const char* type, const char* reply_to, cJSON* data,
                      const std::string* message_id = nullptr);
    bool IsOnline() const;
    void PersistTerminal(const LuaRuntime::RunResult& result, const std::string& message_id);
    bool LoadTerminal(const std::string& run_id, LuaRuntime::RunResult* result,
                      std::string* message_id);

    std::atomic<bool> running_{false};
    std::atomic<bool> online_{false};
    std::mutex mutex_;
    std::unique_ptr<WebSocket> websocket_;
    Transfer transfer_;
    std::string boot_id_;
    std::string device_id_;
    std::string last_finished_id_;
    std::string last_finished_run_id_;
    int heartbeat_interval_ms_ = 20000;
    int64_t last_status_us_ = 0;
    int64_t last_finished_us_ = 0;
    int64_t log_window_start_us_ = 0;
    uint32_t log_sequence_ = 0;
    uint32_t log_window_count_ = 0;
    uint32_t dropped_logs_ = 0;
};

#endif
