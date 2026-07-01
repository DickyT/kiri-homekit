-- Use Quiet fan mode once the room is within 1 F of the target.

function on_state_changed(state, previous)
  if not state.power or not state.operating then
    return
  end

  local difference = math.abs(state.room_temp_f - state.target_temp_f)
  if difference <= 1 and state.fan ~= "QUIET" then
    ac.set_fan("QUIET")
    log.info("Target nearly reached; Quiet fan selected")
  end
end
