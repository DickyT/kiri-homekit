# Kiri Automation API

Kiri Bridge runs constrained Lua 5.4 scripts on real CN105 state changes. The
current API version is `2`. Use the public
[Kiri Automation Editor](https://kiri.dkt.moe/automation.html) for examples,
completion, and syntax preflight, then paste the result into **Admin >
Automation** on the device.

## Hooks

A script must define at least one hook:

```lua
function on_state_changed(state, previous)
end

function on_power_on(state, previous)
end

function on_power_off(state, previous)
end
```

`state` is the newest confirmed CN105 state. `previous` is the prior state or
`nil` when one is unavailable. `on_state_changed` runs for any meaningful
indoor-unit update; the power hooks run only for their respective transition.

## State

The hook argument and `state.current()` expose:

| Field | Type | Meaning |
| --- | --- | --- |
| `power` | boolean | Whether the indoor unit is on |
| `power_raw` | string | `ON` or `OFF` |
| `mode` | string | Current CN105 mode |
| `target_temp_f` / `target_temp_c` | number | Target temperature as the Mitsubishi Fahrenheit label or exact Celsius half-step |
| `room_temp_f` / `room_temp_c` | number | Reported room temperature in either unit |
| `fan` | string | `AUTO`, `QUIET`, or `1` through `4` |
| `up_down_airflow` / `vane` | string | Horizontal flap state |
| `left_right_airflow` / `wide_vane` | string | Vertical vane state |
| `operating` | boolean | Whether the unit is actively heating/cooling |
| `compressor_hz` | integer | Reported compressor frequency |
| `input_power_w` | integer | Reported input power |
| `connected` | boolean | Current CN105 connection state |

## Device information

`kiri.api_version` is `2`. `kiri.capabilities` contains boolean
`auto_mode`, `heat_mode`, `cool_mode`, `dry_mode`, `fan_mode`,
`up_down_airflow`, and `left_right_airflow` fields matching **Settings > AC
Capabilities**.

## AC actions

Each hook may request up to four actions:

```lua
ac.set_power(true)
ac.set_mode("COOL")              -- AUTO, HEAT, COOL, DRY, or FAN
ac.set_target_temp_f(72)         -- 50 through 88 F
ac.set_target_temp_c(21.5)       -- 10.0 through 31.0 C, in 0.5 C steps
ac.set_fan(3)                    -- AUTO, QUIET, or 1 through 4
ac.set_up_down_airflow("1")      -- AUTO, SWING, or 1 through 5
ac.set_left_right_airflow("|")  -- <<, <, |, >, >>, <>, SWING, AIRFLOW CONTROL
ac.set_swing(true)               -- every enabled airflow axis
```

Arguments and AC capabilities are checked immediately. No-op actions are
removed. Real actions are queued outside the state callback and considered
complete only after CN105 reports the requested state.

## Persistent values

```lua
local value = kv.get("key")
kv.set("key", value)
kv.delete("key")
```

Values are stored as strings and may originate from a string, number, integer,
or boolean. Storage is limited to 16 keys and 1 KiB total. Keys use letters,
numbers, `_`, or `-` and are at most 15 characters. Values are at most 95
bytes. Setting an unchanged value does not rewrite flash.

## Logging

`log.info(value)`, `log.warn(value)`, and `print(...)` write to the normal Kiri
diagnostic log. Avoid logging every poll unless actively debugging.

## Runtime limits and recovery

- Script size: 12 KiB.
- Lua arena: 32 KiB per run.
- AC actions: four per hook.
- Persistent values: 16 keys and 1 KiB.
- Filesystem, OS, package, and debug libraries are unavailable.
- Instruction limits stop runaway scripts.
- Repeated action storms and consecutive runtime failures disable automation.
- Automation-originated state echoes are suppressed to prevent feedback loops.
- Saves are validated and atomic; the previous valid script is retained as a
  last-known-good copy.
- The latest 12 runs and the newest CN105 confirmation result appear in Admin.

Examples are stored in [`site/automation-examples`](./site/automation-examples).
