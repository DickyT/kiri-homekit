// Kiri Bridge — one file-selection path for clicks and firmware drag/drop.
import { useEffect, useRef, useState } from "preact/hooks";
export function FirmwarePicker({ disabled = false, onSelect }: {
  disabled?: boolean; onSelect: (file: File) => void | Promise<void>;
}) {
  const input = useRef<HTMLInputElement>(null);
  const pending = useRef(false);
  const [busy, setBusy] = useState(false);
  const [dragging, setDragging] = useState(false);
  const [error, setError] = useState("");
  const locked = disabled || busy;
  useEffect(() => {
    const preventFileNavigation = (event: DragEvent) => {
      if (event.dataTransfer?.types.includes("Files")) event.preventDefault();
    };
    window.addEventListener("dragover", preventFileNavigation);
    window.addEventListener("drop", preventFileNavigation);
    return () => {
      window.removeEventListener("dragover", preventFileNavigation);
      window.removeEventListener("drop", preventFileNavigation);
    };
  }, []);
  async function select(files: FileList | null) {
    if (disabled || pending.current || !files?.length) return;
    setError("");
    if (files.length !== 1) { setError("Choose one .kiri firmware package at a time."); return; }
    const file = files[0]!;
    if (!file.name.toLowerCase().endsWith(".kiri")) { setError("Only .kiri firmware packages are allowed."); return; }
    pending.current = true; setBusy(true);
    try { await onSelect(file); }
    catch (e) { setError(e instanceof Error ? e.message : String(e)); }
    finally { pending.current = false; setBusy(false); }
  }
  return <div class={`firmware-drop-zone${dragging && !locked ? " dragging" : ""}`} aria-disabled={locked}
    onDragOver={e => { if (e.dataTransfer?.types.includes("Files")) { e.preventDefault(); if (!locked) setDragging(true); } }}
    onDragLeave={e => { if (!(e.relatedTarget instanceof Node) || !e.currentTarget.contains(e.relatedTarget)) setDragging(false); }}
    onDrop={e => { e.preventDefault(); e.stopPropagation(); setDragging(false); void select(e.dataTransfer?.files ?? null); }}>
    <input ref={input} class="file-input-hidden" type="file" accept=".kiri" disabled={locked} onChange={e => {
      const files = e.currentTarget.files;
      void select(files);
      e.currentTarget.value = "";
    }} />
    <button class="btn primary file-pick-button" type="button" disabled={locked} onClick={() => input.current?.click()}>{busy ? "Uploading…" : "Choose Firmware"}</button>
    <div class="field-hint">Or drag and drop one .kiri firmware package here.</div>
    {error && <div class="text-bad" role="alert">{error}</div>}
  </div>;
}
