import {
  CheckCircleIcon,
  CpuChipIcon,
  FolderOpenIcon,
  MicrophoneIcon,
  PlusIcon,
  SparklesIcon,
  TrashIcon,
  XMarkIcon,
} from "@heroicons/react/24/outline";
import { useEffect, useState } from "react";
import type { PluginBridge, VoiceTrainingState } from "@/bridge/plugin-bridge";

export const EMPTY_VOICE_TRAINING_STATE: VoiceTrainingState = {
  phase: "idle",
  message: "Add clean, dry vocal recordings to build a reusable voice profile.",
  progress: 0,
  device: "Apple Silicon · Local",
  isAppleSilicon: true,
  sources: [],
  totalDurationSeconds: 0,
  minimumDurationSeconds: 30,
  recommendedDurationSeconds: 180,
  canCreateProfile: false,
  activeProfileName: "",
  outputDirectory: "",
  fineTuningAvailable: true,
  canStartTraining: false,
  jobs: [],
};

function formatDuration(seconds: number) {
  if (seconds < 60) return `${Math.round(seconds)} sec`;
  const minutes = Math.floor(seconds / 60);
  const remainder = Math.round(seconds % 60);
  return `${minutes}:${remainder.toString().padStart(2, "0")}`;
}

export function VoiceTrainingDialog({
  bridge,
  state = EMPTY_VOICE_TRAINING_STATE,
  open,
  onClose,
}: {
  bridge: PluginBridge;
  state?: VoiceTrainingState;
  open: boolean;
  onClose(): void;
}) {
  const [presetName, setPresetName] = useState("My Voice");
  const [quality, setQuality] = useState<"balanced" | "high">("balanced");
  const [mode, setMode] = useState<"reference" | "lora">("lora");

  useEffect(() => {
    if (state.activeProfileName) {
      setPresetName(state.activeProfileName);
    }
  }, [state.activeProfileName]);

  useEffect(() => {
    if (!open) return;
    bridge.send({ type: "voice-training", action: "request-state" });
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === "Escape") onClose();
    };
    window.addEventListener("keydown", closeOnEscape);
    return () => window.removeEventListener("keydown", closeOnEscape);
  }, [bridge, onClose, open]);

  if (!open) return null;

  const isWorking = state.phase === "preparing" || state.phase === "cancelling";
  const coverage = Math.min(
    100,
    (state.totalDurationSeconds / Math.max(1, state.recommendedDurationSeconds)) * 100,
  );

  return (
    <div className="voice-training-backdrop">
      <section
        className="voice-training-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="voice-training-title"
      >
        <header className="voice-training-header">
          <span className="voice-training-mark" aria-hidden="true">
            <MicrophoneIcon />
          </span>
          <div>
            <small>VOICE LAB</small>
            <h2 id="voice-training-title">Train a voice</h2>
          </div>
          <span className="voice-training-device">
            <CpuChipIcon aria-hidden="true" />
            {state.device}
          </span>
          <button
            className="voice-training-close"
            type="button"
            aria-label="Close voice training"
            disabled={isWorking}
            onClick={onClose}
          >
            <XMarkIcon aria-hidden="true" />
          </button>
        </header>

        <div className="voice-training-body">
          <aside className="voice-training-steps" aria-label="Training steps">
            <span className="is-active">
              <b className="voice-training-step-number">01</b>
              <i />
              <strong>Voice data</strong>
            </span>
            <span className={state.sources.length > 0 ? "is-active" : ""}>
              <b className="voice-training-step-number">02</b>
              <i />
              <strong>Quality check</strong>
            </span>
            <span className={state.phase === "complete" ? "is-active" : ""}>
              <b className="voice-training-step-number">03</b>
              <i />
              <strong>Train adapter</strong>
            </span>
          </aside>

          <div className="voice-training-content">
            <div className="voice-training-mode-grid">
              <button
                className={`voice-training-mode ${mode === "reference" ? "is-selected" : ""}`}
                type="button"
                onClick={() => setMode("reference")}
              >
                <SparklesIcon aria-hidden="true" />
                <span>
                  <strong>Voice profile</strong>
                  <small>Instant · SoulX reference only</small>
                </span>
                <i>{mode === "reference" ? "SELECTED" : "QUICK"}</i>
              </button>
              <button
                className={`voice-training-mode ${mode === "lora" ? "is-selected" : ""}`}
                type="button"
                disabled={!state.fineTuningAvailable}
                onClick={() => setMode("lora")}
              >
                <CpuChipIcon aria-hidden="true" />
                <span>
                  <strong>SoulX LoRA</strong>
                  <small>Real training · survives app restart</small>
                </span>
                <i>{mode === "lora" ? "SELECTED" : "TRAIN"}</i>
              </button>
            </div>

            <p className="voice-training-explainer">
              {mode === "lora"
                ? "Train a small singer-specific adapter while the SoulX base model stays frozen. The independent process continues even when Synthetic Obsidian is closed."
                : "Create a reusable zero-shot voice reference immediately. This copies and indexes recordings but does not change model weights."}
            </p>

            <div className="voice-training-settings">
              <label>
                <span>PRESET NAME</span>
                <input
                  value={presetName}
                  maxLength={64}
                  disabled={isWorking}
                  onChange={(event) => setPresetName(event.target.value)}
                />
              </label>
              <fieldset>
                <legend>{mode === "lora" ? "TRAINING QUALITY" : "PROFILE QUALITY"}</legend>
                <button
                  className={`voice-training-quality-button ${
                    quality === "balanced" ? "is-selected" : ""
                  }`}
                  type="button"
                  disabled={isWorking}
                  onClick={() => setQuality("balanced")}
                >
                  Balanced
                </button>
                <button
                  className={`voice-training-quality-button ${
                    quality === "high" ? "is-selected" : ""
                  }`}
                  type="button"
                  disabled={isWorking}
                  onClick={() => setQuality("high")}
                >
                  High
                </button>
              </fieldset>
            </div>

            <div className="voice-training-source-header">
              <div>
                <strong>Training recordings</strong>
                <small>Dry solo vocals, without reverb, tuning, or backing music.</small>
              </div>
              <div>
                <button
                  type="button"
                  disabled={isWorking}
                  onClick={() =>
                    bridge.send({ type: "voice-training", action: "add-current-track" })
                  }
                >
                  <MicrophoneIcon aria-hidden="true" />
                  Current vocal
                </button>
                <button
                  type="button"
                  disabled={isWorking}
                  onClick={() => bridge.send({ type: "voice-training", action: "add-files" })}
                >
                  <PlusIcon aria-hidden="true" />
                  Add files
                </button>
              </div>
            </div>

            {state.sources.length > 0 ? (
              <div className="voice-training-files">
                {state.sources.map((source) => (
                  <article key={source.id} className={`is-${source.status}`}>
                    <span>
                      {source.status === "ready" ? (
                        <CheckCircleIcon aria-hidden="true" />
                      ) : (
                        <MicrophoneIcon aria-hidden="true" />
                      )}
                    </span>
                    <div>
                      <strong title={source.name}>{source.name}</strong>
                      <small>
                        {formatDuration(source.durationSeconds)} ·{" "}
                        {Math.round(source.sampleRate / 100) / 10} kHz ·{" "}
                        {source.channels === 1 ? "Mono" : `${source.channels} ch`}
                      </small>
                      <em className="voice-training-file-message">{source.message}</em>
                    </div>
                    <button
                      type="button"
                      aria-label={`Remove ${source.name}`}
                      disabled={isWorking}
                      onClick={() =>
                        bridge.send({
                          type: "voice-training",
                          action: "remove-file",
                          sourceId: source.id,
                        })
                      }
                    >
                      <TrashIcon aria-hidden="true" />
                    </button>
                  </article>
                ))}
              </div>
            ) : (
              <button
                className="voice-training-dropzone"
                type="button"
                disabled={isWorking}
                onClick={() => bridge.send({ type: "voice-training", action: "add-files" })}
              >
                <FolderOpenIcon aria-hidden="true" />
                <strong>Add clean vocal recordings</strong>
                <span>WAV, AIFF, FLAC, or MP3 · 30 seconds minimum</span>
              </button>
            )}

            <div className="voice-training-coverage">
              <span>
                <strong>{formatDuration(state.totalDurationSeconds)} usable</strong>
                <small>
                  {formatDuration(state.minimumDurationSeconds)} minimum ·{" "}
                  {formatDuration(state.recommendedDurationSeconds)} recommended
                </small>
              </span>
              <i>
                <b style={{ width: `${coverage}%` }} />
              </i>
            </div>

            <div
              className={`voice-training-status is-${state.phase}`}
              role="status"
              aria-live="polite"
            >
              {state.phase === "complete" ? <CheckCircleIcon aria-hidden="true" /> : null}
              <span>
                <strong>
                  {state.phase === "complete"
                    ? `${state.activeProfileName} is ready`
                    : state.message}
                </strong>
                {state.outputDirectory ? (
                  <small title={state.outputDirectory}>{state.outputDirectory}</small>
                ) : null}
              </span>
              {state.outputDirectory ? (
                <button
                  type="button"
                  onClick={() => bridge.send({ type: "voice-training", action: "reveal-profile" })}
                >
                  Show
                </button>
              ) : null}
            </div>

            {state.jobs.length > 0 ? (
              <section className="voice-training-jobs" aria-label="Voice training processes">
                <header>
                  <strong>Training processes</strong>
                  <small>Persistent jobs reconnect automatically after app restart.</small>
                </header>
                <div>
                  {state.jobs.map((job) => {
                    const percentage = Math.round(job.progress * 100);
                    return (
                      <article key={job.jobId} className={`is-${job.status}`}>
                        <span className="voice-training-job-icon">
                          {job.status === "complete" ? (
                            <CheckCircleIcon aria-hidden="true" />
                          ) : (
                            <CpuChipIcon aria-hidden="true" />
                          )}
                        </span>
                        <div className="voice-training-job-details">
                          <span>
                            <strong>{job.presetName}</strong>
                            <small>{job.stage}</small>
                          </span>
                          <p>{job.message}</p>
                          <i>
                            <b style={{ width: `${percentage}%` }} />
                          </i>
                          <small>
                            {percentage}%
                            {job.totalSteps > 0
                              ? ` · step ${job.currentStep}/${job.totalSteps}`
                              : ""}
                            {typeof job.loss === "number" ? ` · loss ${job.loss.toFixed(4)}` : ""}
                          </small>
                        </div>
                        <div className="voice-training-job-actions">
                          {job.canCancel ? (
                            <button
                              type="button"
                              onClick={() =>
                                bridge.send({
                                  type: "voice-training",
                                  action: "cancel-training",
                                  jobId: job.jobId,
                                })
                              }
                            >
                              Cancel
                            </button>
                          ) : (
                            <button
                              type="button"
                              onClick={() =>
                                bridge.send({
                                  type: "voice-training",
                                  action: "reveal-training",
                                  jobId: job.jobId,
                                })
                              }
                            >
                              Show
                            </button>
                          )}
                        </div>
                      </article>
                    );
                  })}
                </div>
              </section>
            ) : null}

            {isWorking ? (
              <div className="voice-training-progress">
                <i>
                  <b style={{ width: `${Math.round(state.progress * 100)}%` }} />
                </i>
                <strong>{Math.round(state.progress * 100)}%</strong>
              </div>
            ) : null}
          </div>
        </div>

        <footer className="voice-training-footer">
          <small>
            {mode === "lora"
              ? "Training stays on this Mac and continues after the app closes. It stops only when Cancel is pressed next to the process."
              : "Profile preparation stays on this Mac and never runs on the realtime audio thread."}
          </small>
          {isWorking ? (
            <button
              className="voice-training-secondary-action"
              type="button"
              onClick={() => bridge.send({ type: "voice-training", action: "cancel" })}
            >
              Cancel
            </button>
          ) : (
            <>
              {state.sources.length > 0 ? (
                <button
                  className="voice-training-secondary-action"
                  type="button"
                  onClick={() => bridge.send({ type: "voice-training", action: "clear-files" })}
                >
                  Clear
                </button>
              ) : null}
              <button
                className="voice-training-primary-action"
                type="button"
                disabled={
                  presetName.trim().length === 0 ||
                  (mode === "lora" ? !state.canStartTraining : !state.canCreateProfile)
                }
                onClick={() =>
                  bridge.send({
                    type: "voice-training",
                    action: mode === "lora" ? "start-training" : "create-profile",
                    presetName: presetName.trim(),
                    quality,
                  })
                }
              >
                {mode === "lora" ? (
                  <CpuChipIcon aria-hidden="true" />
                ) : (
                  <SparklesIcon aria-hidden="true" />
                )}
                {mode === "lora" ? "Start LoRA training" : "Create voice profile"}
              </button>
            </>
          )}
        </footer>
      </section>
    </div>
  );
}
