# Lua VM component

This component vendors Lua 5.4.8 from https://www.lua.org/ftp/lua-5.4.8.tar.gz.

It is only linked into the production Kiri Bridge firmware. The installer firmware
does not include Lua.

The embedded VM intentionally omits the `io`, `os`, `package`, and `debug`
libraries. Script loading is controlled by firmware code, and runtime scripts get
a bounded heap arena plus an instruction limit so a bad automation cannot grow
without limit.

Lua is MIT-licensed. Upstream copyright notices are retained in the vendored
source files.

Airflow naming in the Lua API keeps the original CN105 field names for
compatibility:

- `state.vane` / `ac.set_vane()` control up/down airflow via the horizontal flap.
- `state.wide_vane` / `ac.set_wide_vane()` control left/right airflow via the vertical vanes.

Prefer the clearer aliases in new scripts:

- `state.up_down_airflow` / `ac.set_up_down_airflow()`
- `state.left_right_airflow` / `ac.set_left_right_airflow()`
