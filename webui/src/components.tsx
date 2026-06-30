// Kiri Bridge — shared UI primitives. All components are dumb/stateless;
// page-level state lives in signals (store.ts) or local component state.

import type { ComponentChildren, JSX } from "preact";
import { useEffect, useState } from "preact/hooks";
import type { OtaUploadResult } from "./lib/ota";

export function Section({ title, action, children }: { title?: string; action?: ComponentChildren; children: ComponentChildren }): JSX.Element {
  return (
    <section class="section">
      {title && (
        <div class="section-head">
          <h3>{title}</h3>
          {action}
        </div>
      )}
      {children}
    </section>
  );
}

export function SpecRow({ k, children, valueClass }: { k: string; children: ComponentChildren; valueClass?: string }): JSX.Element {
  return (
    <div class="spec-row">
      <span class="key">{k}</span>
      <span class={"val " + (valueClass ?? "")}>{children}</span>
    </div>
  );
}

export function Field({ label, help, children }: { label: string; help?: string; children: ComponentChildren }): JSX.Element {
  return (
    <label class="field">
      <span class="field-label">
        {label}
        {help && <span class="info-icon" data-tooltip={help} aria-label={help} tabIndex={0}>i</span>}
      </span>
      {children}
    </label>
  );
}

export function Btn({
  variant = "default",
  compact = false,
  type = "button",
  class: extraClass = "",
  ...rest
}: {
  variant?: "default" | "primary" | "danger";
  compact?: boolean;
  type?: "button" | "submit";
  children?: ComponentChildren;
} & Omit<JSX.ButtonHTMLAttributes<HTMLButtonElement>, "size">): JSX.Element {
  const cls = ["btn", variant !== "default" ? variant : "", compact ? "compact" : "", extraClass].filter(Boolean).join(" ");
  return <button {...rest} type={type} class={cls} />;
}

export function Modal({
  open,
  onClose,
  title,
  subtitle,
  size,
  children,
  actions,
  closable = true,
}: {
  open: boolean;
  onClose?: () => void;
  title: string;
  subtitle?: ComponentChildren;
  size?: "narrow" | "wide";
  children: ComponentChildren;
  actions?: ComponentChildren;
  closable?: boolean;
}): JSX.Element | null {
  if (!open) return null;
  return (
    <div class="modal" role="dialog" aria-modal="true">
      <div class="modal-backdrop" onClick={() => closable && onClose?.()} />
      <div class={"modal-panel " + (size ?? "")}>
        <div class="modal-header">
          <div>
            <h2>{title}</h2>
            {subtitle && <div class="modal-subtitle subtitle">{subtitle}</div>}
          </div>
        </div>
        {children}
        {actions && <div class="modal-actions">{actions}</div>}
      </div>
    </div>
  );
}

export function RebootingModal({ open }: { open: boolean }): JSX.Element | null {
  const [secondsLeft, setSecondsLeft] = useState(5);

  useEffect(() => {
    if (!open) {
      setSecondsLeft(5);
      return;
    }
    setSecondsLeft(5);
    const timer = window.setInterval(() => {
      setSecondsLeft((value) => Math.max(0, value - 1));
    }, 1000);
    return () => window.clearInterval(timer);
  }, [open]);

  const message = secondsLeft > 0
    ? `Device is rebooting. This page will refresh in ${secondsLeft} second${secondsLeft === 1 ? "" : "s"}.`
    : "Rebooting...";

  return (
    <Modal
      open={open}
      title="Rebooting"
      size="narrow"
      closable={false}
    >
      <div class="subtitle">{message}</div>
    </Modal>
  );
}

function formatOtaBytes(value: number | undefined): string {
  if (typeof value !== "number" || !Number.isFinite(value)) return "--";
  if (value < 1024) return `${value} B`;
  const kib = value / 1024;
  if (kib < 1024) return `${kib.toFixed(1)} KiB`;
  return `${(kib / 1024).toFixed(2)} MiB`;
}

function formatOtaVariant(value: string | undefined): string {
  if (value === "app") return "Production app";
  if (value === "installer") return "Installer";
  return value || "--";
}

function formatOtaBoard(result: OtaUploadResult): string {
  const board = result.board_name || result.board || "--";
  const target = result.target || "--";
  return `${board} (${target})`;
}

export function OtaConfirmModal({
  result,
  applying = false,
  status,
  subtitle = "Upload complete. Confirm to reboot into the uploaded firmware.",
  onConfirm,
  onCancel,
}: {
  result: OtaUploadResult | null;
  applying?: boolean;
  status?: ComponentChildren;
  subtitle?: ComponentChildren;
  onConfirm: () => void;
  onCancel: () => void;
}): JSX.Element | null {
  if (!result) return null;
  const warning = result.warning ??
    ((result.same_or_older || result.rollback)
      ? "The uploaded firmware is not newer than the running firmware. Reinstalling is allowed."
      : "");
  return (
    <Modal
      open={true}
      onClose={() => { if (!applying) onCancel(); }}
      title="Confirm OTA"
      subtitle={status ?? subtitle}
      closable={!applying}
      actions={(
        <>
          <Btn variant="primary" disabled={applying} onClick={onConfirm}>Confirm and Reboot</Btn>
          <Btn disabled={applying} onClick={onCancel}>Cancel</Btn>
        </>
      )}
    >
      <div class="modal-grid">
        <div class="modal-card">
          <div class="section-label">Firmware</div>
          <SpecRow k="Current">{result.current_version ?? "--"}</SpecRow>
          <SpecRow k="Uploaded">{result.uploaded_version ?? "--"}</SpecRow>
          <SpecRow k="Type">{formatOtaVariant(result.variant)}</SpecRow>
        </div>
        <div class="modal-card">
          <div class="section-label">Target</div>
          <SpecRow k="Board">{formatOtaBoard(result)}</SpecRow>
          <SpecRow k="Partition">{result.partition ?? "--"}</SpecRow>
          <SpecRow k="Size">{formatOtaBytes(result.bytes)}</SpecRow>
          <SpecRow k="Status">{applying ? "Applying" : "Ready to reboot"}</SpecRow>
        </div>
      </div>
      {warning && <div class="warn-box">{warning}</div>}
    </Modal>
  );
}
