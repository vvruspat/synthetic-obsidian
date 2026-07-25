type ProjectStatusBarProps = {
  status: string;
};

export function ProjectStatusBar({ status }: ProjectStatusBarProps) {
  return (
    <footer className="project-status-bar" aria-live="polite">
      <output className="project-status" title={status}>
        {status || "Ready"}
      </output>
    </footer>
  );
}
