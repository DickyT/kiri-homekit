// Kiri Bridge — server JSON contracts.
// Hand-maintained mirror of the C++ web_routes.cpp shapes.

export type WifiInfo = {
  connected: boolean;
  ssid?: string;
  ip?: string;
  rssi?: number;
  bssid?: string;
  mac?: string;
  ap_ssid?: string;
  ap_ip?: string;
};

export type Cn105MockState = {
  power: "ON" | "OFF";
  mode: "COOL" | "HEAT" | "DRY" | "FAN" | "AUTO";
  target_temperature_f: number;
  room_temperature_f: number;
  fan: string;
  vane: string;
  wide_vane: string;
};

export type TransportStatus = {
  phase: string;
  connected: boolean;
  connect_attempts: number;
  poll_cycles: number;
  rx_packets: number;
  rx_errors: number;
  tx_packets: number;
  sets_pending: number;
  last_error?: string;
};

export type Cn105Block = {
  transport: "real" | "mock";
  transport_status: TransportStatus;
  mock_state: Cn105MockState;
};

export type HomekitBlock = {
  started: boolean;
  paired_controllers: number;
  setup_code?: string;
  setup_payload?: string;
  accessory_name?: string;
  model?: string;
  firmware_revision?: string;
};

export type ProvisioningBlock = {
  active: boolean;
  stage?: string;
  service_name?: string;
  remaining_ms?: number;
  last_result?: string;
  pending_ssid?: string;
  button_gpio?: number;
};

export type DeviceConfig = {
  device_name?: string;
  wifi_ssid?: string;
  wifi_password_set?: boolean;
  homekit_code?: string;
  homekit_setup_id?: string;
  homekit_manufacturer?: string;
  homekit_model?: string;
  homekit_serial?: string;
  homekit_separate_airflow_tile?: boolean;
  homekit_advanced_mapping?: number;
  ac_capabilities?: number;
  homekit_hvac_modes?: string;
  led_pin?: number;
  cn105_mode?: "real" | "mock";
  cn105_rx_pin?: number;
  cn105_tx_pin?: number;
  cn105_baud?: number;
  cn105_data_bits?: number;
  cn105_parity?: "E" | "N" | "O";
  cn105_stop_bits?: number;
  cn105_rx_pullup?: boolean;
  cn105_tx_open_drain?: boolean;
  log_level?: string;
  poll_active_ms?: number;
  poll_off_ms?: number;
};

export type BoardInfo = {
  id?: string;
  name?: string;
  target?: string;
  defaults?: {
    led_pin?: number;
    cn105_rx_pin?: number;
    cn105_tx_pin?: number;
    provisioning_button_gpio?: number;
  };
};

export type FilesystemInfo = {
  used_bytes: number;
  total_bytes: number;
};

export type Status = {
  ok?: boolean;
  device: string;
  version: string;
  board?: BoardInfo;
  uptime_ms: number;
  wifi: WifiInfo;
  cn105: Cn105Block;
  homekit: HomekitBlock;
  provisioning: ProvisioningBlock;
  filesystem: FilesystemInfo;
  config?: DeviceConfig;
};

export type AutomationAction = {
  type: string;
  value?: string;
  int_value?: number;
};

export type AutomationStatus = {
  ok?: boolean;
  enabled: boolean;
  script_exists: boolean;
  script_size: number;
  script_max_bytes: number;
  last_set: boolean;
  last_ok: boolean;
  last_script_loaded: boolean;
  last_hook_found: boolean;
  last_instruction_limit_hit: boolean;
  last_peak_bytes: number;
  last_hook?: string;
  last_message?: string;
  run_count: number;
  last_actions: AutomationAction[];
};

export type LogFile = { name: string; size: number; current: boolean };
export type LogList = {
  active: boolean;
  current?: string;
  current_bytes?: number;
  level?: string;
  logs: LogFile[];
};
