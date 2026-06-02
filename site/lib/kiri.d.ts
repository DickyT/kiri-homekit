// Kiri Bridge — TypeScript shape declarations for site/lib/kiri.js.
//
// The runtime module is plain JS so it can be served as-is to the public
// site and bundled by the firmware webui without a TS dependency. This file
// exists only to give Admin.tsx / installer.tsx call-site type hints.

export type KiriPart = {
  name: string;
  path: string;
  offset: number;
  size: number;
  sha256: string;
};

export type KiriManifest = {
  format: string;
  project: string;
  product?: string;
  displayName?: string;
  variant: 'app' | 'installer';
  build_app?: string;
  board?: string;
  boardName?: string;
  boardTarget?: string;
  project_name?: string;
  version?: string;
  target?: string;
  chipFamily?: string;
  flashSize?: number | null;
  flash_settings?: Record<string, unknown>;
  extra_esptool_args?: Record<string, unknown>;
  app: {
    offset: number;
    maxSize?: number;
    path: string;
    size: number;
    sha256: string;
  };
  parts: KiriPart[];
  sha256: Record<string, string>;
  partitions?: Record<string, { type?: string; subtype?: string; offset: number; size: number }>;
};

export type KiriPackage = {
  manifest: KiriManifest;
  parts: Map<string, Uint8Array>;
};

export function parseKiri(input: ArrayBuffer | Uint8Array | Blob): Promise<KiriPackage>;
export function sha256Hex(bytes: Uint8Array): Promise<string>;
