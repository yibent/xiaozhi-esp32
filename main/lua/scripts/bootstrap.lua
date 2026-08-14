function on_start()
    xiaozhi.log("Lua runtime started; device state=" .. xiaozhi.get_state())
end

function on_event(name, payload)
    if name == "state_changed" then
        xiaozhi.log("Device state changed to " .. payload)
    end
end
