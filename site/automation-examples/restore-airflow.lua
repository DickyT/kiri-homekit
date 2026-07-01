-- Remember the last up/down flap position and restore it after power-on.

function on_power_off(state, previous)
  if previous and previous.up_down_airflow then
    kv.set("last_up_down", previous.up_down_airflow)
  end
end

function on_power_on(state, previous)
  if not kiri.capabilities.up_down_airflow then
    return
  end

  local position = kv.get("last_up_down")
  if position and position ~= "" then
    ac.set_up_down_airflow(position)
    log.info("Restored up/down airflow to " .. position)
  end
end
