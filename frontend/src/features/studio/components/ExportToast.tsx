import { CheckCircleIcon, XMarkIcon } from "@heroicons/react/24/outline";

export type ExportToastState = {
  id: number;
  message: string;
  directory: string;
};

export function ExportToast({ toast, onDismiss }: { toast: ExportToastState; onDismiss(): void }) {
  return (
    <aside className="export-toast" role="status" aria-live="polite">
      <CheckCircleIcon className="export-toast-icon" strokeWidth={1.8} aria-hidden="true" />
      <div>
        <strong>Export complete</strong>
        <span>{toast.message}</span>
        <small title={toast.directory}>{toast.directory}</small>
      </div>
      <button type="button" aria-label="Dismiss export notification" onClick={onDismiss}>
        <XMarkIcon className="export-toast-close-icon" strokeWidth={1.8} aria-hidden="true" />
      </button>
    </aside>
  );
}
