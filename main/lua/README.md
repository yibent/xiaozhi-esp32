# Lua Runtime

This directory contains the ESP-device Lua runtime. It is not built for PC or
Linux simulation targets.

Enable `CONFIG_XIAOZHI_LUA_RUNTIME` on an ESP32-S3 or ESP32-P4 build with PSRAM.
The built-in `bootstrap.lua` script may define these callbacks:

- `on_start()` runs once after the runtime has loaded the script.
- `on_event(name, payload)` receives bounded device events.

The `xiaozhi` module currently provides `log`, `get_state`, `notify`,
`set_emotion`, `start_listening`, and `stop_listening`. UI and application
mutations are scheduled onto the XiaoZhi main task.
