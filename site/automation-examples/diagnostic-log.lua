-- Log meaningful CN105 changes without changing the indoor unit.

function on_state_changed(state, previous)
  if not previous then
    log.info("Initial CN105 state received")
    return
  end

  if state.power ~= previous.power then
    log.info("Power changed to " .. tostring(state.power_raw))
  end
  if state.mode ~= previous.mode then
    log.info("Mode changed to " .. state.mode)
  end
  if state.target_temp_f ~= previous.target_temp_f then
    log.info("Target changed to " .. tostring(state.target_temp_f) .. " F")
  end
end
