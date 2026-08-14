#include "lua_device_gateway.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <inttypes.h>

extern "C" {
#include <lua.h>
}

#include "board.h"
#include "settings.h"

namespace {
constexpr char kTag[] = "LuaGateway";
constexpr char kGatewayUrl[] = "wss://max.sh.creativone.cn/api/device-ws/v1";
constexpr size_t kMaxScriptBytes = 65536;
constexpr size_t kMaxParamsBytes = 4096;
constexpr size_t kMaxChunkBytes = 1024;
constexpr size_t kMaxMessageBytes = 24576;
constexpr int kHelloTimeoutMs = 10000;

std::string NewUuid() {
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    char output[37];
    snprintf(output, sizeof(output),
         "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
         bytes[0], bytes[1], bytes[2], bytes[3],
         bytes[4], bytes[5],
         bytes[6], bytes[7],
         bytes[8], bytes[9],
         bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return output;
}

std::string Timestamp() {
    time_t now = time(nullptr);
    struct tm value{};
    gmtime_r(&now, &value);
    char text[32];
    strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S.000Z", &value);
    return text;
}

bool GetString(const cJSON* object, const char* name, std::string* output) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == nullptr)
        return false;
    *output = item->valuestring;
    return true;
}

bool GetInt(const cJSON* object, const char* name, int* output) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item))
        return false;
    *output = item->valueint;
    return true;
}

std::string Sha256(const std::string& input) {
    std::array<unsigned char, 32> digest{};
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr || mbedtls_md(info, reinterpret_cast<const unsigned char*>(input.data()),
                                      input.size(), digest.data()) != 0)
        return "";
    static const char hex[] = "0123456789abcdef";
    std::string output(64, '0');
    for (size_t i = 0; i < digest.size(); ++i) {
        output[i * 2] = hex[digest[i] >> 4];
        output[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    return output;
}

bool DecodeBase64(const std::string& input, std::string* output) {
    size_t size = 0;
    if (mbedtls_base64_decode(nullptr, 0, &size,
                              reinterpret_cast<const unsigned char*>(input.data()),
                              input.size()) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL &&
        size == 0) {
        return false;
    }
    output->resize(size);
    return mbedtls_base64_decode(reinterpret_cast<unsigned char*>(output->data()), output->size(),
                                 &size, reinterpret_cast<const unsigned char*>(input.data()),
                                 input.size()) == 0 &&
           (output->resize(size), true);
}

uint32_t Crc32(const std::string& input) {
    uint32_t crc = 0xffffffff;
    for (unsigned char byte : input) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320 & -(crc & 1));
    }
    return ~crc;
}

bool HasCapabilities(const cJSON* value) {
    if (!cJSON_IsArray(value))
        return false;
    cJSON* item = nullptr;
    cJSON_ArrayForEach (item, value) {
        if (!cJSON_IsString(item) ||
            (strcmp(item->valuestring, "lua") != 0 && strcmp(item->valuestring, "xiaozhi") != 0))
            return false;
    }
    return true;
}
}  // namespace

LuaDeviceGateway& LuaDeviceGateway::GetInstance() {
    static LuaDeviceGateway instance;
    return instance;
}

LuaDeviceGateway::~LuaDeviceGateway() { Stop(); }

bool LuaDeviceGateway::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
        return true;
    device_id_ = Board::GetInstance().GetUuid();
    boot_id_ = NewUuid();
    ConfigureCallbacks();
    if (xTaskCreate(TaskEntry, "lua_gateway", 8192, this, 3, nullptr) != pdPASS) {
        running_.store(false);
        return false;
    }
    return true;
}

void LuaDeviceGateway::Stop() {
    running_.store(false);
    std::lock_guard<std::mutex> lock(mutex_);
    if (websocket_ != nullptr)
        websocket_->Close();
}

void LuaDeviceGateway::TaskEntry(void* arg) { static_cast<LuaDeviceGateway*>(arg)->Run(); }

void LuaDeviceGateway::ConfigureCallbacks() {
    LuaRuntime::GetInstance().SetCallbacks(
        [this](const std::string& run_id, const std::string& text) { SendLog(run_id, text); },
        [this](const LuaRuntime::RunResult& result) { SendFinished(result); });
}

void LuaDeviceGateway::Run() {
    int delay_seconds = 1;
    while (running_.load()) {
        if (!Connect()) {
            vTaskDelay(pdMS_TO_TICKS(delay_seconds * 1000));
            delay_seconds = std::min(delay_seconds * 2, 30);
            continue;
        }
        delay_seconds = 1;
        while (running_.load() && IsOnline()) {
            const int64_t now = esp_timer_get_time();
            if (now - last_status_us_ >= heartbeat_interval_ms_ * 1000LL)
                SendStatus();
            if (!last_finished_id_.empty() && now - last_finished_us_ >= 5000000) {
                LuaRuntime::RunResult terminal;
                std::string message_id;
                if (LoadTerminal(last_finished_run_id_, &terminal, &message_id))
                    SendFinished(terminal);
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        Disconnect();
        if (running_.load())
            vTaskDelay(pdMS_TO_TICKS(1000));
    }
    LuaRuntime::GetInstance().SetCallbacks({}, {});
    vTaskDelete(nullptr);
}

bool LuaDeviceGateway::Connect() {
    online_.store(false);
    auto socket = Board::GetInstance().GetNetwork()->CreateWebSocket(2);
    if (socket == nullptr)
        return false;
    socket->SetReceiveBufferSize(kMaxMessageBytes);
    socket->OnData([this](const char* data, size_t length, bool binary) {
        HandleIncoming(data, length, binary);
    });
    socket->OnDisconnected([this]() {
        ESP_LOGW(kTag, "WebSocket disconnected");
        online_.store(false);
    });
    socket->OnError([this](int error) {
        ESP_LOGW(kTag, "WebSocket error %d", error);
        online_.store(false);
    });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        websocket_ = std::move(socket);
    }
    WebSocket* socket_ptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        socket_ptr = websocket_.get();
    }
    ESP_LOGI(kTag, "Connecting to %s", kGatewayUrl);
    if (!socket_ptr->Connect(kGatewayUrl)) {
        ESP_LOGW(kTag, "WebSocket connection failed");
        Disconnect();
        return false;
    }
    if (!SendHello()) {
        ESP_LOGW(kTag, "Failed to send hello");
        Disconnect();
        return false;
    }

    const int64_t deadline_us = esp_timer_get_time() + kHelloTimeoutMs * 1000LL;
    while (running_.load() && socket_ptr->IsConnected() && !online_.load() &&
           esp_timer_get_time() < deadline_us) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!online_.load()) {
        if (socket_ptr->IsConnected())
            ESP_LOGW(kTag, "Server welcome timed out");
        else
            ESP_LOGW(kTag, "Disconnected before server welcome");
        Disconnect();
        return false;
    }
    return true;
}

void LuaDeviceGateway::Disconnect() {
    online_.store(false);
    std::lock_guard<std::mutex> lock(mutex_);
    if (websocket_ != nullptr) {
        websocket_->Close();
        websocket_.reset();
    }
}

bool LuaDeviceGateway::IsOnline() const { return online_.load(); }

bool LuaDeviceGateway::SendEnvelope(const char* type, const char* reply_to, cJSON* data,
                                    const std::string* message_id) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddStringToObject(root, "id",
                            message_id == nullptr ? NewUuid().c_str() : message_id->c_str());
    if (reply_to != nullptr)
        cJSON_AddStringToObject(root, "reply_to", reply_to);
    cJSON_AddStringToObject(root, "ts", Timestamp().c_str());
    cJSON_AddItemToObject(root, "data", data);
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == nullptr)
        return false;
    std::string message(text);
    cJSON_free(text);
    std::lock_guard<std::mutex> lock(mutex_);
    return websocket_ != nullptr && websocket_->IsConnected() && websocket_->Send(message);
}

void LuaDeviceGateway::SendError(const char* reply_to, const char* code, const char* message,
                                 bool retryable) {
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "code", code);
    cJSON_AddStringToObject(data, "message", message);
    cJSON_AddBoolToObject(data, "retryable", retryable);
    SendEnvelope("error", reply_to, data);
}

void LuaDeviceGateway::HandleIncoming(const char* text, size_t length, bool binary) {
    if (binary || length == 0 || length > kMaxMessageBytes)
        return;
    cJSON* root = cJSON_ParseWithLength(text, length);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }
    int version = 0;
    std::string type;
    if (!GetInt(root, "v", &version) || version != 1 || !GetString(root, "type", &type) ||
        !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "data"))) {
        cJSON_Delete(root);
        return;
    }
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (type == "hello.welcome")
        HandleWelcome(data);
    else if (online_.load() && type == "run.prepare")
        HandlePrepare(root, data);
    else if (online_.load() && type == "run.chunk")
        HandleChunk(root, data);
    else if (online_.load() && type == "run.commit")
        HandleCommit(root, data);
    else if (online_.load() && type == "run.stop")
        HandleStop(root, data);
    else if (online_.load() && type == "run.finished.ack")
        HandleFinishedAck(root, data);
    cJSON_Delete(root);
}

bool LuaDeviceGateway::SendHello() {
    const esp_app_desc_t* app = esp_app_get_description();
    std::string firmware = app->version;
    cJSON* hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "device_id", device_id_.c_str());
    cJSON_AddStringToObject(hello, "boot_id", boot_id_.c_str());
    cJSON_AddStringToObject(hello, "firmware_version", firmware.c_str());
    cJSON_AddStringToObject(hello, "lua_runtime", LUA_VERSION);
    cJSON* limits = cJSON_AddObjectToObject(hello, "limits");
    cJSON_AddNumberToObject(limits, "max_script_bytes", kMaxScriptBytes);
    cJSON_AddNumberToObject(limits, "max_params_bytes", kMaxParamsBytes);
    cJSON_AddNumberToObject(limits, "max_chunk_bytes", kMaxChunkBytes);
    cJSON_AddNumberToObject(limits, "max_message_bytes", kMaxMessageBytes);
    cJSON_AddNumberToObject(limits, "max_log_bytes", 1024);
    cJSON* capabilities = cJSON_AddArrayToObject(hello, "capabilities");
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("lua"));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("xiaozhi"));
    cJSON* runtime = cJSON_AddObjectToObject(hello, "runtime");
    cJSON_AddStringToObject(runtime, "execution_model", "main_once");
    cJSON_AddStringToObject(runtime, "api_version", "xiaozhi.v1");
    cJSON_AddStringToObject(runtime, "transfer_storage", "ram");
    cJSON_AddNumberToObject(runtime, "max_run_timeout_ms", CONFIG_XIAOZHI_LUA_RUN_TIMEOUT_MS);
    cJSON* state = cJSON_AddObjectToObject(hello, "runtime_state");
    std::string active = LuaRuntime::GetInstance().ActiveRunId();
    if (active.empty())
        cJSON_AddNullToObject(state, "active_run_id");
    else
        cJSON_AddStringToObject(state, "active_run_id", active.c_str());
    if (transfer_.run_id.empty()) {
        cJSON_AddNullToObject(state, "transfer");
    } else {
        cJSON* transfer = cJSON_AddObjectToObject(state, "transfer");
        cJSON_AddStringToObject(transfer, "run_id", transfer_.run_id.c_str());
        cJSON_AddStringToObject(transfer, "sha256", transfer_.source_sha256.c_str());
        cJSON_AddNumberToObject(transfer, "next_chunk_index", transfer_.next_chunk_index);
        cJSON_AddNumberToObject(transfer, "received_bytes", transfer_.source.size());
        cJSON_AddNumberToObject(transfer, "total_chunks", transfer_.total_chunks);
    }
    Settings terminal_settings("lua_run", false);
    std::string terminal_run_id = terminal_settings.GetString("last_run");
    if (terminal_run_id.empty()) {
        cJSON_AddNullToObject(state, "last_terminal_run");
    } else {
        cJSON* terminal = cJSON_AddObjectToObject(state, "last_terminal_run");
        cJSON_AddStringToObject(terminal, "run_id", terminal_run_id.c_str());
        cJSON_AddStringToObject(terminal, "status",
                                terminal_settings.GetString("last_stat").c_str());
    }
    return SendEnvelope("hello", nullptr, hello);
}

void LuaDeviceGateway::HandleWelcome(const cJSON* data) {
    int interval = 0;
    if (GetInt(data, "heartbeat_interval_ms", &interval))
        heartbeat_interval_ms_ = std::clamp(interval, 5000, 60000);
    online_.store(true);
    ESP_LOGI(kTag, "Connected, heartbeat interval %d ms", heartbeat_interval_ms_);
    SendStatus();
    Settings settings("lua_run", false);
    if (!settings.GetBool("last_acked", true)) {
        LuaRuntime::RunResult terminal;
        std::string message_id;
        if (LoadTerminal(settings.GetString("last_run"), &terminal, &message_id))
            SendFinished(terminal);
    }
}

void LuaDeviceGateway::SendStatus() {
    if (!online_.load())
        return;
    cJSON* data = cJSON_CreateObject();
    std::string active = LuaRuntime::GetInstance().ActiveRunId();
    cJSON_AddStringToObject(data, "state",
                            !active.empty()            ? "running"
                            : transfer_.run_id.empty() ? "idle"
                                                       : "receiving");
    cJSON_AddNumberToObject(data, "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddNumberToObject(data, "free_heap_bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    if (active.empty())
        cJSON_AddNullToObject(data, "active_run_id");
    else
        cJSON_AddStringToObject(data, "active_run_id", active.c_str());
    if (transfer_.run_id.empty())
        cJSON_AddNullToObject(data, "transfer");
    else {
        cJSON* transfer = cJSON_AddObjectToObject(data, "transfer");
        cJSON_AddStringToObject(transfer, "run_id", transfer_.run_id.c_str());
        cJSON_AddStringToObject(transfer, "sha256", transfer_.source_sha256.c_str());
        cJSON_AddNumberToObject(transfer, "next_chunk_index", transfer_.next_chunk_index);
        cJSON_AddNumberToObject(transfer, "received_bytes", transfer_.source.size());
        cJSON_AddNumberToObject(transfer, "total_chunks", transfer_.total_chunks);
    }
    last_status_us_ = esp_timer_get_time();
    SendEnvelope("device.status", nullptr, data);
}

void LuaDeviceGateway::HandlePrepare(const cJSON* root, const cJSON* data) {
    std::string run_id, params_sha, entry, mode, message_id;
    int timeout = 0;
    const cJSON* script = cJSON_GetObjectItemCaseSensitive(data, "script");
    const cJSON* params = cJSON_GetObjectItemCaseSensitive(data, "params");
    if (!GetString(data, "run_id", &run_id) || !GetString(data, "params_sha256", &params_sha) ||
        !GetString(data, "entry", &entry) || !GetString(data, "run_mode", &mode) ||
        !GetString(root, "id", &message_id) || !GetInt(data, "timeout_ms", &timeout) ||
        !cJSON_IsObject(script) || params == nullptr) {
        SendError(message_id.c_str(), "INVALID_MESSAGE", "invalid run.prepare", false);
        return;
    }
    std::string source_sha;
    int length = 0, chunk_bytes = 0, total_chunks = 0;
    if (entry != "main" || mode != "replace" || !GetString(script, "sha256", &source_sha) ||
        !GetInt(script, "byte_length", &length) || !GetInt(script, "chunk_bytes", &chunk_bytes) ||
        !GetInt(script, "total_chunks", &total_chunks) || length < 1 ||
        length > static_cast<int>(kMaxScriptBytes) || chunk_bytes < 1 ||
        chunk_bytes > static_cast<int>(kMaxChunkBytes) ||
        total_chunks != (length + chunk_bytes - 1) / chunk_bytes || timeout < 1000 ||
        timeout > CONFIG_XIAOZHI_LUA_RUN_TIMEOUT_MS ||
        !HasCapabilities(cJSON_GetObjectItemCaseSensitive(data, "required_capabilities"))) {
        SendError(message_id.c_str(), "UNSUPPORTED_CAPABILITY", "unsupported run settings", false);
        return;
    }
    char* params_text = cJSON_PrintUnformatted(params);
    if (params_text == nullptr) {
        SendError(message_id.c_str(), "INVALID_MESSAGE", "unable to encode params", false);
        return;
    }
    std::string params_json(params_text);
    cJSON_free(params_text);
    if (params_json.size() > kMaxParamsBytes) {
        SendError(message_id.c_str(), "PARAMS_TOO_LARGE", "params exceeds device limit", false);
        return;
    }
    LuaRuntime::RunResult terminal;
    std::string terminal_id;
    if (LoadTerminal(run_id, &terminal, &terminal_id)) {
        SendFinished(terminal);
        return;
    }
    if (!transfer_.run_id.empty() && transfer_.run_id != run_id) {
        SendError(message_id.c_str(), "DEVICE_BUSY", "another transfer is active", true);
        return;
    }
    if (transfer_.run_id == run_id &&
        (transfer_.source_sha256 != source_sha || transfer_.params_sha256 != params_sha)) {
        SendError(message_id.c_str(), "RUN_ID_CONFLICT", "run_id content changed", false);
        return;
    }
    if (transfer_.run_id.empty()) {
        transfer_.run_id = run_id;
        transfer_.source_sha256 = source_sha;
        transfer_.params_sha256 = params_sha;
        transfer_.params_json = std::move(params_json);
        transfer_.byte_length = length;
        transfer_.chunk_bytes = chunk_bytes;
        transfer_.total_chunks = total_chunks;
        transfer_.timeout_ms = timeout;
        transfer_.source.reserve(length);
    }
    cJSON* ready = cJSON_CreateObject();
    cJSON_AddStringToObject(ready, "run_id", run_id.c_str());
    cJSON_AddNumberToObject(ready, "next_chunk_index", transfer_.next_chunk_index);
    cJSON_AddNumberToObject(ready, "received_bytes", transfer_.source.size());
    SendEnvelope("run.ready", message_id.c_str(), ready);
}

void LuaDeviceGateway::HandleChunk(const cJSON* root, const cJSON* data) {
    std::string run_id, b64, crc, message_id;
    int index = 0, total = 0, offset = 0;
    if (!GetString(data, "run_id", &run_id) || !GetString(data, "data_b64", &b64) ||
        !GetString(data, "crc32", &crc) || !GetString(root, "id", &message_id) ||
        !GetInt(data, "index", &index) || !GetInt(data, "total_chunks", &total) ||
        !GetInt(data, "offset", &offset) || run_id != transfer_.run_id ||
        total != transfer_.total_chunks) {
        SendError(message_id.c_str(), "RUN_NOT_FOUND", "transfer was not found", false);
        return;
    }
    if (index > transfer_.next_chunk_index) {
        SendError(message_id.c_str(), "OUT_OF_ORDER_CHUNK", "future chunk", true);
        return;
    }
    if (index == transfer_.next_chunk_index) {
        std::string chunk;
        char expected_crc[9];
        if (!DecodeBase64(b64, &chunk) || chunk.empty() || chunk.size() > kMaxChunkBytes ||
            offset != index * static_cast<int>(transfer_.chunk_bytes) ||
            (snprintf(expected_crc, sizeof(expected_crc), "%08" PRIx32, Crc32(chunk)),
             crc != expected_crc) ||
            transfer_.source.size() + chunk.size() > transfer_.byte_length) {
            SendError(message_id.c_str(), "CHUNK_INVALID", "invalid chunk", true);
            return;
        }
        transfer_.source.append(chunk);
        ++transfer_.next_chunk_index;
    }
    cJSON* ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "run_id", transfer_.run_id.c_str());
    cJSON_AddNumberToObject(ack, "index", index);
    cJSON_AddNumberToObject(ack, "next_chunk_index", transfer_.next_chunk_index);
    cJSON_AddNumberToObject(ack, "received_bytes", transfer_.source.size());
    SendEnvelope("run.chunk.ack", message_id.c_str(), ack);
}

void LuaDeviceGateway::HandleCommit(const cJSON* root, const cJSON* data) {
    std::string run_id, sha, message_id;
    int length = 0;
    if (!GetString(data, "run_id", &run_id) || !GetString(data, "sha256", &sha) ||
        !GetString(root, "id", &message_id) || !GetInt(data, "byte_length", &length))
        return;
    LuaRuntime::RunResult terminal;
    std::string terminal_id;
    if (LoadTerminal(run_id, &terminal, &terminal_id)) {
        SendFinished(terminal);
        return;
    }
    if (LuaRuntime::GetInstance().ActiveRunId() == run_id) {
        cJSON* accepted = cJSON_CreateObject();
        cJSON_AddStringToObject(accepted, "run_id", run_id.c_str());
        cJSON_AddStringToObject(accepted, "state", "running");
        cJSON_AddStringToObject(accepted, "started_at", Timestamp().c_str());
        SendEnvelope("run.accepted", message_id.c_str(), accepted);
        return;
    }
    if (run_id != transfer_.run_id || length != static_cast<int>(transfer_.byte_length) ||
        sha != transfer_.source_sha256 || transfer_.next_chunk_index != transfer_.total_chunks ||
        transfer_.source.size() != transfer_.byte_length || Sha256(transfer_.source) != sha) {
        SendError(message_id.c_str(), "HASH_MISMATCH", "source verification failed", true);
        return;
    }
    if (!LuaRuntime::GetInstance().RunScript(run_id, transfer_.source, transfer_.params_json,
                                             transfer_.timeout_ms)) {
        SendError(message_id.c_str(), "DEVICE_BUSY", "Lua runtime is busy", true);
        return;
    }
    transfer_ = {};
    cJSON* accepted = cJSON_CreateObject();
    cJSON_AddStringToObject(accepted, "run_id", run_id.c_str());
    cJSON_AddStringToObject(accepted, "state", "running");
    cJSON_AddStringToObject(accepted, "started_at", Timestamp().c_str());
    SendEnvelope("run.accepted", message_id.c_str(), accepted);
}

void LuaDeviceGateway::HandleStop(const cJSON* root, const cJSON* data) {
    std::string run_id, message_id;
    if (!GetString(data, "run_id", &run_id) || !GetString(root, "id", &message_id))
        return;
    LuaRuntime::RunResult terminal;
    std::string terminal_id;
    if (LoadTerminal(run_id, &terminal, &terminal_id)) {
        SendFinished(terminal);
        return;
    }
    if (run_id == transfer_.run_id) {
        transfer_ = {};
        terminal.run_id = run_id;
        terminal.status = "stopped";
        terminal.error_code = "RUN_STOPPED";
        terminal.error_message = "cancelled before execution";
        cJSON* stopping = cJSON_CreateObject();
        cJSON_AddStringToObject(stopping, "run_id", run_id.c_str());
        SendEnvelope("run.stopping", message_id.c_str(), stopping);
        SendFinished(terminal);
        return;
    }
    if (LuaRuntime::GetInstance().CancelRun(run_id)) {
        cJSON* stopping = cJSON_CreateObject();
        cJSON_AddStringToObject(stopping, "run_id", run_id.c_str());
        SendEnvelope("run.stopping", message_id.c_str(), stopping);
        return;
    }
    SendError(message_id.c_str(), "RUN_NOT_FOUND", "run was not found", false);
}

void LuaDeviceGateway::SendLog(const std::string& run_id, const std::string& text) {
    if (!online_.load())
        return;
    const int64_t now = esp_timer_get_time();
    if (now - log_window_start_us_ >= 1000000) {
        log_window_start_us_ = now;
        log_window_count_ = 0;
    }
    if (log_window_count_ >= 20) {
        ++dropped_logs_;
        return;
    }
    ++log_window_count_;
    std::string output = text;
    if (dropped_logs_ > 0) {
        output = "[dropped " + std::to_string(dropped_logs_) + " logs] " + output;
        dropped_logs_ = 0;
    }
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "run_id", run_id.c_str());
    cJSON_AddNumberToObject(data, "sequence", ++log_sequence_);
    cJSON_AddStringToObject(data, "level", "info");
    cJSON_AddStringToObject(data, "text", output.substr(0, 1024).c_str());
    SendEnvelope("run.log", nullptr, data);
}

void LuaDeviceGateway::PersistTerminal(const LuaRuntime::RunResult& result,
                                       const std::string& message_id) {
    Settings settings("lua_run", true);
    settings.SetString("last_run", result.run_id);
    settings.SetString("last_stat", result.status);
    settings.SetString("last_result", result.result_json);
    settings.SetString("last_error", result.error_code + "\n" + result.error_message);
    settings.SetInt("last_duration", result.duration_ms);
    settings.SetString("last_msgid", message_id);
    settings.SetBool("last_acked", false);
}

bool LuaDeviceGateway::LoadTerminal(const std::string& run_id, LuaRuntime::RunResult* result,
                                    std::string* message_id) {
    Settings settings("lua_run", false);
    if (settings.GetString("last_run") != run_id)
        return false;
    result->run_id = run_id;
    result->status = settings.GetString("last_stat");
    result->result_json = settings.GetString("last_result");
    std::string error = settings.GetString("last_error");
    size_t separator = error.find('\n');
    result->error_code = error.substr(0, separator);
    result->error_message = separator == std::string::npos ? "" : error.substr(separator + 1);
    result->duration_ms = settings.GetInt("last_duration");
    *message_id = settings.GetString("last_msgid");
    return !result->status.empty();
}

void LuaDeviceGateway::SendFinished(const LuaRuntime::RunResult& result) {
    LuaRuntime::RunResult existing;
    std::string id;
    if (LoadTerminal(result.run_id, &existing, &id)) {
        if (existing.status != result.status && !LuaRuntime::GetInstance().ActiveRunId().empty())
            return;
        id = id.empty() ? NewUuid() : id;
    } else {
        id = NewUuid();
        PersistTerminal(result, id);
    }
    last_finished_id_ = id;
    last_finished_run_id_ = result.run_id;
    last_finished_us_ = esp_timer_get_time();
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "run_id", result.run_id.c_str());
    cJSON_AddStringToObject(data, "status", result.status.c_str());
    cJSON_AddNumberToObject(data, "duration_ms", result.duration_ms);
    if (result.status == "succeeded" && !result.result_json.empty()) {
        cJSON* value = cJSON_Parse(result.result_json.c_str());
        if (value != nullptr)
            cJSON_AddItemToObject(data, "result", value);
        else
            cJSON_AddNullToObject(data, "result");
        cJSON_AddNullToObject(data, "error");
    } else {
        cJSON_AddNullToObject(data, "result");
        cJSON* error = cJSON_AddObjectToObject(data, "error");
        cJSON_AddStringToObject(error, "code", result.error_code.c_str());
        cJSON_AddStringToObject(error, "message", result.error_message.c_str());
        if (result.error_line > 0)
            cJSON_AddNumberToObject(error, "line", result.error_line);
    }
    SendEnvelope("run.finished", nullptr, data, &id);
}

void LuaDeviceGateway::HandleFinishedAck(const cJSON* root, const cJSON* data) {
    std::string run_id, reply;
    if (GetString(data, "run_id", &run_id) && GetString(root, "reply_to", &reply) &&
        run_id == last_finished_run_id_ && reply == last_finished_id_) {
        last_finished_id_.clear();
        last_finished_run_id_.clear();
        Settings settings("lua_run", true);
        settings.SetBool("last_acked", true);
    }
}
