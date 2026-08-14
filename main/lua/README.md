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

## Display API

Devices with an LVGL display expose `xiaozhi.ui` and advertise the `display`
capability. The API reuses the display and LVGL task already owned by XiaoZhi;
scripts must not initialize the LCD or start another LVGL task.

```lua
function main(params)
    local ui = xiaozhi.ui
    local info = ui.info()
    if not info.available then
        error("display is unavailable")
    end

    local screen = ui.screen({ bg_color = "#101820" })
    local card = ui.container(screen, {
        width = info.width - 32,
        height = 100,
        align = "center",
        bg_color = "#F2F4F8",
        radius = 8,
        pad = 12,
        flex = "column",
    })
    ui.label(card, {
        text = params.text or "123",
        text_color = "#18212B",
        align = "center",
    })
    screen:load()
    return { width = info.width, height = info.height }
end
```

The custom screen remains visible after `main` returns. A later script can call
`xiaozhi.ui.restore()` to return to the XiaoZhi screen.

Available factories are `screen`, `container`, `label`, `button`, `bar`,
`slider`, `arc`, `switch`, `checkbox`, `dropdown`, `roller`, `textarea`,
`image`, `line`, `table`, `spinner`, `led`, and `chart`. Widget availability
also depends on the LVGL options selected by the board build. The generic
`create(type, parent, options)` function is equivalent to a named factory.

Every object supports:

```lua
object:set(options)
object:load()       -- screen objects only
object:delete()
```

Common options include `id`, `text`, `x`, `y`, `width`, `height`, `align`,
`bg_color`, `text_color`, `border_color`, `bg_opa`, `radius`, `border_width`,
`pad`, `pad_row`, `pad_column`, `hidden`, `clickable`, `scrollable`, `flex`,
`min`, `max`, `value`, `checked`, `options`, `src`, and `points`.

Set `events = true` when creating an interactive widget, then read bounded
events from the Lua task:

```lua
local button = ui.button(screen, { id = "confirm", text = "OK", events = true })
local event = ui.poll_event(1000)
if event and event.id == "confirm" and event.type == "clicked" then
    button:set({ text_color = "#00AA66" })
end
```

`poll_event` waits at most 1000 ms per call. Remote runs are still subject to
the device run timeout, so persistent application logic belongs in firmware;
the rendered screen itself can persist across runs.
