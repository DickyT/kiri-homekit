-- Raise fan speed when the room drifts above target, then restore Auto.
-- The two thresholds provide hysteresis so the rule does not chatter.

function on_state_changed(state, previous)
  if not state.power or state.mode ~= "COOL" then
    return
  end

  local delta = state.room_temp_f - state.target_temp_f
  local boosted = kv.get("fan_boosted") == "true"

  if delta >= 3 and not boosted then
    ac.set_fan(4)
    kv.set("fan_boosted", true)
    log.info("Room is warm; fan boost enabled")
  elseif delta <= 1 and boosted then
    ac.set_fan("AUTO")
    kv.set("fan_boosted", false)
    log.info("Room is near target; fan returned to Auto")
  end
end
