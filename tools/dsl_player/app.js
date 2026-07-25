const state = {
  song: null,
  fileName: "",
  tracks: [],
  tempo: 120,
  totalBeats: 1,
  pxPerBeat: 42,
  isPlaying: false,
  startAudioTime: 0,
  startBeat: 0,
  currentBeat: 0,
  scheduledUntilBeat: 0,
  schedulerId: null,
  animationId: null,
  audio: null,
  activeNodes: [],
  syncOffsetMs: 0,
};

const els = {
  fileInput: document.getElementById("fileInput"),
  metadata: document.getElementById("metadata"),
  playButton: document.getElementById("playButton"),
  stopButton: document.getElementById("stopButton"),
  positionSlider: document.getElementById("positionSlider"),
  timeReadout: document.getElementById("timeReadout"),
  beatReadout: document.getElementById("beatReadout"),
  trackControls: document.getElementById("trackControls"),
  zoomSlider: document.getElementById("zoomSlider"),
  syncSlider: document.getElementById("syncSlider"),
  syncReadout: document.getElementById("syncReadout"),
  followToggle: document.getElementById("followToggle"),
  songTitle: document.getElementById("songTitle"),
  songSubtitle: document.getElementById("songSubtitle"),
  stats: document.getElementById("stats"),
  ruler: document.getElementById("ruler"),
  scroll: document.getElementById("timelineScroll"),
  spacer: document.getElementById("timelineSpacer"),
  canvas: document.getElementById("timelineCanvas"),
  playhead: document.getElementById("playhead"),
  chordList: document.getElementById("chordList"),
  warnings: document.getElementById("warnings"),
};

const ctx = els.canvas.getContext("2d");

els.fileInput.addEventListener("change", async (event) => {
  const file = event.target.files?.[0];
  if (!file) return;
  const text = await file.text();
  loadSong(parseDslYaml(text), file.name);
});

els.playButton.addEventListener("click", () => {
  if (state.isPlaying) {
    pause();
  } else {
    play();
  }
});

els.stopButton.addEventListener("click", stop);

els.positionSlider.addEventListener("input", () => {
  const beat = Number(els.positionSlider.value) * state.totalBeats;
  seek(beat, state.isPlaying);
});

els.scroll.addEventListener("click", (event) => {
  if (!state.song) return;
  const beat = pointerEventToBeat(event);
  seek(beat, state.isPlaying);
});

els.scroll.addEventListener("scroll", () => {
  requestAnimationFrame(renderViewport);
});

els.zoomSlider.addEventListener("input", () => {
  state.pxPerBeat = Number(els.zoomSlider.value);
  render();
});

els.syncSlider.addEventListener("input", () => {
  state.syncOffsetMs = Number(els.syncSlider.value);
  els.syncReadout.textContent = `${state.syncOffsetMs} ms`;
  if (state.isPlaying) {
    state.currentBeat = audiblePlaybackBeat();
    updateReadout();
    updatePlayhead();
  }
});

window.addEventListener("resize", render);
renderTrackControls();
loadSongFromUrlParam();

async function loadSongFromUrlParam() {
  const params = new URLSearchParams(window.location.search);
  const file = params.get("file");
  if (!file) return;
  try {
    const response = await fetch(file, { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const text = await response.text();
    loadSong(parseDslYaml(text), file.split("/").pop() || file);
  } catch (error) {
    els.metadata.innerHTML = `<div class="empty-state">Could not auto-load ${escapeHtml(file)}: ${escapeHtml(error.message)}</div>`;
  }
}

function parseDslYaml(text) {
  const song = {
    meta: {},
    tracks: { chords: [], lead_vocal: [], backing_vocals: [] },
    analysis: { warnings: [], lead_detection: {}, chord_detection: {} },
  };
  let section = "";
  let subsection = "";
  let current = null;
  let currentBacking = null;
  let currentPart = null;
  let currentBackingNote = null;

  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.replace(/\t/g, "  ");
    if (!line.trim() || line.trim().startsWith("#")) continue;
    const indent = line.match(/^ */)[0].length;
    const trimmed = line.trim();

    if (indent === 0 && trimmed.endsWith(":")) {
      section = trimmed.slice(0, -1);
      subsection = "";
      current = null;
      continue;
    }

    if (section === "meta" && indent === 2) {
      const [key, value] = splitPair(trimmed);
      song.meta[key] = parseScalar(value);
      continue;
    }

    if (section === "tracks" && indent === 2 && trimmed.endsWith(":")) {
      subsection = trimmed.slice(0, -1);
      current = null;
      currentBacking = null;
      currentPart = null;
      currentBackingNote = null;
      continue;
    }

    if (section === "tracks" && subsection === "backing_vocals") {
      if (indent === 4 && trimmed.startsWith("- ")) {
        currentBacking = { parts: [] };
        song.tracks.backing_vocals.push(currentBacking);
        currentPart = null;
        currentBackingNote = null;
        const rest = trimmed.slice(2);
        if (rest.includes(":")) {
          const [key, value] = splitPair(rest);
          currentBacking[key] = parseScalar(value);
        }
        continue;
      }
      if (currentBacking && indent === 6 && trimmed.includes(":") && trimmed !== "parts:") {
        const [key, value] = splitPair(trimmed);
        currentBacking[key] = parseScalar(value);
        continue;
      }
      if (currentBacking && indent === 8 && trimmed.startsWith("- ")) {
        currentPart = { notes: [] };
        currentBacking.parts.push(currentPart);
        currentBackingNote = null;
        const rest = trimmed.slice(2);
        if (rest.includes(":")) {
          const [key, value] = splitPair(rest);
          currentPart[key] = parseScalar(value);
        }
        continue;
      }
      if (currentPart && indent === 10 && trimmed.includes(":") && trimmed !== "notes:") {
        const [key, value] = splitPair(trimmed);
        currentPart[key] = parseScalar(value);
        continue;
      }
      if (currentPart && indent === 12 && trimmed.startsWith("- ")) {
        currentBackingNote = {};
        currentPart.notes.push(currentBackingNote);
        const rest = trimmed.slice(2);
        if (rest.includes(":")) {
          const [key, value] = splitPair(rest);
          currentBackingNote[key] = parseScalar(value);
        }
        continue;
      }
      if (currentBackingNote && indent === 14 && trimmed.includes(":")) {
        const [key, value] = splitPair(trimmed);
        currentBackingNote[key] = parseScalar(value);
        continue;
      }
    }

    if (section === "tracks" && indent === 4 && trimmed.startsWith("- ")) {
      current = {};
      song.tracks[subsection] ||= [];
      song.tracks[subsection].push(current);
      const rest = trimmed.slice(2);
      if (rest.includes(":")) {
        const [key, value] = splitPair(rest);
        current[key] = parseScalar(value);
      }
      continue;
    }

    if (section === "tracks" && current && indent >= 6 && trimmed.includes(":")) {
      const [key, value] = splitPair(trimmed);
      current[key] = parseScalar(value);
      continue;
    }

    if (section === "analysis" && indent === 2 && trimmed.endsWith(":")) {
      subsection = trimmed.slice(0, -1);
      current = null;
      continue;
    }

    if (section === "analysis" && indent === 2 && trimmed.startsWith("warnings:")) {
      song.analysis.warnings = parseScalar(trimmed.slice("warnings:".length).trim()) || [];
      continue;
    }

    if (section === "analysis" && subsection && indent === 4 && trimmed.includes(":")) {
      const [key, value] = splitPair(trimmed);
      song.analysis[subsection][key] = parseScalar(value);
    }
  }

  return normalizeSong(song);
}

function splitPair(text) {
  const index = text.indexOf(":");
  return [text.slice(0, index).trim(), text.slice(index + 1).trim()];
}

function parseScalar(value) {
  if (value === "" || value === undefined) return "";
  if (value === "null") return null;
  if (value === "true") return true;
  if (value === "false") return false;
  if (value.startsWith("[") && value.endsWith("]")) return parseList(value);
  if (/^-?\d+(\.\d+)?$/.test(value)) return Number(value);
  if ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith("'") && value.endsWith("'"))) {
    return value.slice(1, -1).replace(/\\"/g, '"').replace(/\\\\/g, "\\");
  }
  return value;
}

function parseList(value) {
  const body = value.slice(1, -1).trim();
  if (!body) return [];
  const result = [];
  let token = "";
  let quote = "";
  for (let i = 0; i < body.length; i += 1) {
    const char = body[i];
    if (quote) {
      if (char === quote && body[i - 1] !== "\\") quote = "";
      token += char;
    } else if (char === '"' || char === "'") {
      quote = char;
      token += char;
    } else if (char === ",") {
      result.push(parseScalar(token.trim()));
      token = "";
    } else {
      token += char;
    }
  }
  if (token.trim()) result.push(parseScalar(token.trim()));
  return result;
}

function normalizeSong(song) {
  const tempo = Number(song.meta.tempo) || 120;
  const chords = (song.tracks.chords || []).map((chord) => ({
    ...chord,
    start: Number(chord.start) || 0,
    duration: Math.max(Number(chord.duration) || 0, 0.05),
    notes: Array.isArray(chord.notes) ? chord.notes : [],
  }));
  const lead = (song.tracks.lead_vocal || []).map((note) => ({
    ...note,
    start: Number(note.start) || 0,
    duration: Math.max(Number(note.duration) || 0, 0.05),
    midi_note: Number(note.midi_note) || noteNameToMidi(note.pitch) || 60,
    velocity: Number(note.velocity) || 80,
  }));
  const backing = (song.tracks.backing_vocals || []).map((track) => ({
    ...track,
    parts: (track.parts || []).map((part) => ({
      ...part,
      notes: (part.notes || []).map((note) => ({
        ...note,
        start: Number(note.start) || 0,
        duration: Math.max(Number(note.duration) || 0, 0.05),
        midi_note: Number(note.midi_note) || noteNameToMidi(note.pitch) || 60,
        velocity: Number(note.velocity) || 72,
      })),
    })),
  }));
  const maxChordBeat = chords.reduce((max, chord) => Math.max(max, chord.start + chord.duration), 0);
  const maxLeadBeat = lead.reduce((max, note) => Math.max(max, note.start + note.duration), 0);
  const maxBackingBeat = backing.reduce(
    (trackMax, track) =>
      Math.max(
        trackMax,
        ...track.parts.map((part) => part.notes.reduce((partMax, note) => Math.max(partMax, note.start + note.duration), 0))
      ),
    0
  );
  return {
    ...song,
    meta: { ...song.meta, tempo },
    tracks: { chords, lead_vocal: lead, backing_vocals: backing },
    totalBeats: Math.max(maxChordBeat, maxLeadBeat, maxBackingBeat, 1),
  };
}

function loadSong(song, fileName) {
  stop();
  state.song = song;
  state.fileName = fileName;
  state.tracks = buildTrackList(song);
  state.tempo = song.meta.tempo;
  state.totalBeats = song.totalBeats;
  state.currentBeat = 0;
  state.startBeat = 0;
  state.scheduledUntilBeat = 0;
  els.playButton.disabled = false;
  els.stopButton.disabled = false;
  els.positionSlider.disabled = false;
  els.positionSlider.value = "0";
  els.songTitle.textContent = song.meta.title || fileName;
  els.songSubtitle.textContent = fileName;
  renderMetadata();
  renderStats();
  renderTrackControls();
  renderChordList();
  renderWarnings();
  render();
  updateReadout();
}

function buildTrackList(song) {
  const previous = new Map(state.tracks.map((track) => [track.id, track]));
  const tracks = [
    makeTrack("chords", "Chords", "#f0bd62", "chords", null, previous),
    makeTrack("lead", "Lead Vocal", "#65a9ff", "lead", null, previous),
  ];
  const palette = ["#55c7a3", "#ff8e72", "#c792ea", "#8bd3dd", "#f78fb3", "#b8e986", "#f5d76e", "#9aa7ff"];
  let colorIndex = 0;
  for (const backing of song.tracks.backing_vocals || []) {
    for (const part of backing.parts || []) {
      const id = `backing:${backing.id}:${part.id}`;
      const role = part.role ? ` ${part.role}` : "";
      const name = `${backing.name || backing.id}${role}`;
      tracks.push(makeTrack(id, name, palette[colorIndex % palette.length], "backing", { backing, part }, previous));
      colorIndex += 1;
    }
  }
  return tracks;
}

function makeTrack(id, name, color, type, source, previous) {
  const old = previous.get(id);
  return {
    id,
    name,
    color,
    type,
    source,
    muted: old?.muted || false,
    solo: old?.solo || false,
  };
}

function renderMetadata() {
  const meta = state.song?.meta || {};
  els.metadata.innerHTML = `
    <dl>
      <dt>Tempo</dt><dd>${escapeHtml(meta.tempo ?? "")} BPM</dd>
      <dt>Meter</dt><dd>${escapeHtml(meta.meter ?? "")}</dd>
      <dt>Key</dt><dd>${escapeHtml(meta.key ?? "")}</dd>
      <dt>Ticks</dt><dd>${escapeHtml(meta.ticks_per_beat ?? "")}</dd>
      <dt>Resolution</dt><dd>${escapeHtml(meta.source_resolution ?? "")}</dd>
    </dl>
  `;
}

function renderStats() {
  const chords = state.song?.tracks.chords.length || 0;
  const lead = state.song?.tracks.lead_vocal.length || 0;
  const backingTracks = state.song?.tracks.backing_vocals.length || 0;
  const backingParts = state.tracks.filter((track) => track.type === "backing").length;
  const seconds = beatsToSeconds(state.totalBeats);
  els.stats.innerHTML = `
    <span class="stat">${chords} chords</span>
    <span class="stat">${lead} lead notes</span>
    <span class="stat">${backingTracks} backing sets</span>
    <span class="stat">${backingParts} backing parts</span>
    <span class="stat">${formatTime(seconds)}</span>
  `;
}

function renderTrackControls() {
  els.trackControls.innerHTML = "";
  for (const track of state.tracks) {
    const row = document.createElement("div");
    row.className = "track-row";
    row.innerHTML = `
      <div class="track-name">
        <span class="track-swatch" style="background:${track.color}"></span>
        <span class="track-label">${escapeHtml(track.name)}</span>
      </div>
      <button class="mini-button ${track.muted ? "active" : ""}" title="Mute ${escapeHtml(track.name)}">M</button>
      <button class="mini-button ${track.solo ? "active" : ""}" title="Solo ${escapeHtml(track.name)}">S</button>
    `;
    const [muteButton, soloButton] = row.querySelectorAll("button");
    muteButton.addEventListener("click", () => {
      track.muted = !track.muted;
      renderTrackControls();
      render();
      restartPlaybackIfNeeded();
    });
    soloButton.addEventListener("click", () => {
      track.solo = !track.solo;
      renderTrackControls();
      render();
      restartPlaybackIfNeeded();
    });
    els.trackControls.appendChild(row);
  }
}

function renderChordList() {
  const chords = state.song?.tracks.chords || [];
  els.chordList.innerHTML = chords
    .slice(0, 160)
    .map(
      (chord) =>
        `<div class="chord-chip"><strong>${escapeHtml(chord.chord)}</strong> · bar ${escapeHtml(chord.bar)} · ${escapeHtml(
          chord.duration
        )} beats</div>`
    )
    .join("");
  if (chords.length > 160) {
    els.chordList.insertAdjacentHTML("beforeend", `<div class="chord-chip">+${chords.length - 160} more</div>`);
  }
}

function renderWarnings() {
  const warnings = state.song?.analysis.warnings || [];
  els.warnings.innerHTML = warnings.length
    ? warnings.map((warning) => `<div class="warning">${escapeHtml(warning)}</div>`).join("")
    : `<div class="empty-state">No warnings.</div>`;
}

function timelineLanes() {
  const tracks = state.tracks.length
    ? state.tracks
    : [
        { id: "chords", name: "Chords", color: "#f0bd62", type: "chords" },
        { id: "lead", name: "Lead Vocal", color: "#65a9ff", type: "lead" },
      ];
  let y = 0;
  return tracks.map((track) => {
    const height = track.type === "chords" ? 96 : 54;
    const lane = { track, y, height };
    y += height;
    return lane;
  });
}

function timelineHeight() {
  const lanes = timelineLanes();
  return Math.max(520, lanes.reduce((height, lane) => Math.max(height, lane.y + lane.height), 0));
}

function render() {
  const width = contentWidth();
  const height = timelineHeight();
  els.spacer.style.width = `${width}px`;
  els.spacer.style.height = `${height}px`;
  els.playhead.style.height = `${height}px`;
  renderViewport();
  drawRuler(width);
  updatePlayhead();
}

function renderViewport() {
  const width = contentWidth();
  const height = timelineHeight();
  const viewportWidth = Math.max(els.scroll.clientWidth || 800, 1);
  const viewportHeight = Math.max(els.scroll.clientHeight || 520, 1);
  const ratio = canvasScaleRatio();
  els.canvas.style.width = `${viewportWidth}px`;
  els.canvas.style.height = `${viewportHeight}px`;
  els.canvas.style.transform = `translate(${els.scroll.scrollLeft}px, ${els.scroll.scrollTop}px)`;
  els.canvas.width = Math.round(viewportWidth * ratio);
  els.canvas.height = Math.round(viewportHeight * ratio);
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  ctx.imageSmoothingEnabled = false;
  drawTimeline(viewportWidth, viewportHeight, width, height, els.scroll.scrollLeft, els.scroll.scrollTop);
}

function contentWidth() {
  return Math.max(Math.ceil(state.totalBeats * state.pxPerBeat) + 240, els.scroll.clientWidth || 800);
}

function canvasScaleRatio() {
  const dpr = window.devicePixelRatio || 1;
  return Math.max(1, Math.min(dpr, 2));
}

function hexToRgba(hex, alpha) {
  const normalized = hex.replace("#", "");
  const value = Number.parseInt(normalized.length === 3 ? normalized.replace(/(.)/g, "$1$1") : normalized, 16);
  const r = (value >> 16) & 255;
  const g = (value >> 8) & 255;
  const b = value & 255;
  return `rgba(${r}, ${g}, ${b}, ${alpha})`;
}

function drawTimeline(viewportWidth, viewportHeight, width, height, scrollLeft, scrollTop) {
  ctx.clearRect(0, 0, viewportWidth, viewportHeight);
  ctx.fillStyle = "#0d0f12";
  ctx.fillRect(0, 0, viewportWidth, viewportHeight);
  ctx.save();
  ctx.translate(-scrollLeft, -scrollTop);
  drawGrid(width, height);
  drawTrackLabels(scrollLeft);
  if (state.song) {
    for (const lane of timelineLanes()) {
      if (lane.track.type === "chords") drawChords(lane);
      if (lane.track.type === "lead") drawNoteTrack(lane, state.song.tracks.lead_vocal, "rgba(101, 169, 255, 0.86)");
      if (lane.track.type === "backing") drawNoteTrack(lane, lane.track.source.part.notes, hexToRgba(lane.track.color, 0.78));
    }
  }
  ctx.restore();
}

function drawGrid(width, height) {
  const barBeats = meterBeats();
  ctx.strokeStyle = "#242934";
  ctx.lineWidth = 1;
  for (let beat = 0; beat <= state.totalBeats + 1; beat += 1) {
    const x = beatToX(beat);
    ctx.strokeStyle = beat % barBeats === 0 ? "#3a414f" : "#222730";
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, height);
    ctx.stroke();
  }
  ctx.strokeStyle = "#313641";
  for (const lane of timelineLanes()) {
    ctx.fillStyle = "#14171b";
    ctx.fillRect(0, lane.y, width, 34);
    ctx.beginPath();
    ctx.moveTo(0, lane.y);
    ctx.lineTo(width, lane.y);
    ctx.moveTo(0, lane.y + 34);
    ctx.lineTo(width, lane.y + 34);
    ctx.moveTo(0, lane.y + lane.height);
    ctx.lineTo(width, lane.y + lane.height);
    ctx.stroke();
  }
}

function drawTrackLabels(scrollLeft) {
  ctx.font = "700 13px Inter, sans-serif";
  for (const lane of timelineLanes()) {
    ctx.fillStyle = lane.track.color;
    ctx.fillRect(scrollLeft + 10, lane.y + 10, 8, 14);
    ctx.fillStyle = "#a5adba";
    ctx.fillText(lane.track.name, scrollLeft + 26, lane.y + 23);
  }
}

function drawChords(lane) {
  if (!isAudible("chords")) return;
  const chords = state.song.tracks.chords;
  ctx.font = "700 13px Inter, sans-serif";
  for (const chord of chords) {
    const x = beatToX(chord.start);
    const w = Math.max(chord.duration * state.pxPerBeat - 2, 4);
    ctx.fillStyle = "rgba(240, 189, 98, 0.26)";
    ctx.fillRect(x, lane.y + 42, w, lane.height - 50);
    ctx.strokeStyle = "rgba(240, 189, 98, 0.65)";
    ctx.strokeRect(x, lane.y + 42, w, lane.height - 50);
    ctx.fillStyle = "#f0bd62";
    ctx.fillText(String(chord.chord || ""), x + 7, lane.y + 62, Math.max(w - 12, 20));
  }
}

function drawNoteTrack(lane, notes, fill) {
  if (!isAudible(lane.track.id)) return;
  if (!notes.length) return;
  const pitches = notes.map((note) => note.midi_note);
  const minPitch = Math.min(...pitches) - 2;
  const maxPitch = Math.max(...pitches) + 2;
  const range = Math.max(maxPitch - minPitch, 1);
  const noteArea = Math.max(lane.height - 42, 8);
  for (const note of notes) {
    const x = beatToX(note.start);
    const w = Math.max(note.duration * state.pxPerBeat - 1, 3);
    const y = lane.y + lane.height - 13 - ((note.midi_note - minPitch) / range) * noteArea;
    ctx.fillStyle = fill;
    ctx.fillRect(x, y, w, 7);
    if (note.syllable && lane.height >= 80) {
      ctx.fillStyle = "#d8e7ff";
      ctx.font = "11px Inter, sans-serif";
      ctx.fillText(String(note.syllable), x + 3, y - 4, Math.max(w + 30, 40));
    }
  }
}

function drawRuler(width) {
  const barBeats = meterBeats();
  const barCount = Math.ceil(state.totalBeats / barBeats);
  els.ruler.innerHTML = "";
  els.ruler.style.width = `${width}px`;
  for (let bar = 0; bar <= barCount; bar += 1) {
    const marker = document.createElement("div");
    marker.style.position = "absolute";
    marker.style.left = `${beatToX(bar * barBeats)}px`;
    marker.style.top = "0";
    marker.style.height = "32px";
    marker.style.borderLeft = "1px solid #49515f";
    marker.style.paddingLeft = "6px";
    marker.style.color = "#a5adba";
    marker.style.fontSize = "12px";
    marker.style.lineHeight = "32px";
    marker.textContent = String(bar + 1);
    els.ruler.appendChild(marker);
  }
}

async function play() {
  if (!state.song) return;
  state.audio ||= new AudioContext();
  if (state.audio.state === "suspended") await state.audio.resume();
  state.isPlaying = true;
  state.startBeat = state.currentBeat;
  state.startAudioTime = state.audio.currentTime;
  state.scheduledUntilBeat = state.currentBeat;
  els.playButton.textContent = "Pause";
  scheduleAhead();
  state.schedulerId = window.setInterval(scheduleAhead, 90);
  animate();
}

function pause() {
  state.isPlaying = false;
  clearInterval(state.schedulerId);
  state.schedulerId = null;
  cancelAnimationFrame(state.animationId);
  state.currentBeat = audiblePlaybackBeat();
  stopActiveNodes();
  els.playButton.textContent = "Play";
  updateReadout();
  updatePlayhead();
}

function stop() {
  state.isPlaying = false;
  clearInterval(state.schedulerId);
  state.schedulerId = null;
  cancelAnimationFrame(state.animationId);
  stopActiveNodes();
  state.currentBeat = 0;
  state.startBeat = 0;
  state.scheduledUntilBeat = 0;
  els.playButton.textContent = "Play";
  els.positionSlider.value = "0";
  updateReadout();
  updatePlayhead();
}

function seek(beat, resume) {
  const wasPlaying = resume && state.isPlaying;
  if (state.isPlaying) pause();
  state.currentBeat = clamp(beat, 0, state.totalBeats);
  state.startBeat = state.currentBeat;
  state.scheduledUntilBeat = state.currentBeat;
  els.positionSlider.value = String(state.currentBeat / state.totalBeats);
  updateReadout();
  updatePlayhead();
  if (wasPlaying) play();
}

function pointerEventToBeat(event) {
  const rect = els.scroll.getBoundingClientRect();
  const x = event.clientX - rect.left + els.scroll.scrollLeft;
  return clamp(x / state.pxPerBeat, 0, state.totalBeats);
}

function restartPlaybackIfNeeded() {
  if (!state.isPlaying) return;
  const beat = audiblePlaybackBeat();
  pause();
  state.currentBeat = clamp(beat, 0, state.totalBeats);
  play();
}

function scheduleAhead() {
  if (!state.isPlaying || !state.audio) return;
  const rawBeat = rawPlaybackBeat();
  const lookaheadBeats = secondsToBeats(0.75);
  const fromBeat = state.scheduledUntilBeat;
  const toBeat = Math.min(rawBeat + lookaheadBeats, state.totalBeats);
  if (isAudible("lead")) {
    for (const note of state.song.tracks.lead_vocal) {
      if (note.start >= fromBeat && note.start < toBeat) scheduleLead(note);
    }
  }
  if (isAudible("chords")) {
    for (const chord of state.song.tracks.chords) {
      if (chord.start >= fromBeat && chord.start < toBeat) scheduleChord(chord);
    }
  }
  for (const track of state.tracks) {
    if (track.type !== "backing" || !isAudible(track.id)) continue;
    for (const note of track.source.part.notes) {
      if (note.start >= fromBeat && note.start < toBeat) scheduleBacking(note);
    }
  }
  state.scheduledUntilBeat = toBeat;
  if (rawBeat >= state.totalBeats) stop();
}

function scheduleLead(note) {
  const time = beatToAudioTime(note.start);
  const duration = beatsToSeconds(note.duration);
  playPianoNote(note.midi_note, time, duration, (note.velocity / 127) * 0.22);
}

function scheduleChord(chord) {
  const time = beatToAudioTime(chord.start);
  const duration = beatsToSeconds(Math.min(chord.duration, 4));
  const notes = chord.notes.map(noteNameToMidi).filter(Boolean);
  for (const midi of notes) {
    playPianoNote(midi, time, duration, 0.055);
  }
}

function scheduleBacking(note) {
  const time = beatToAudioTime(note.start);
  const duration = beatsToSeconds(note.duration);
  playPianoNote(note.midi_note, time, duration, (note.velocity / 127) * 0.12);
}

function playPianoNote(midi, time, duration, gainValue) {
  const audio = state.audio;
  const output = audio.createGain();
  const filter = audio.createBiquadFilter();
  const gain = audio.createGain();

  const frequency = 440 * 2 ** ((midi - 69) / 12);
  const stopTime = time + Math.max(0.18, Math.min(duration + 0.55, 2.4));
  const peak = Math.max(gainValue, 0.0002);
  const sustain = peak * 0.28;
  const releaseStart = Math.min(time + Math.max(duration, 0.08), stopTime - 0.08);

  filter.type = "lowpass";
  filter.frequency.setValueAtTime(Math.min(8500, frequency * 10 + 1800), time);
  filter.frequency.exponentialRampToValueAtTime(Math.max(900, frequency * 4), stopTime);
  filter.Q.value = 0.6;

  gain.gain.setValueAtTime(0.0001, time);
  gain.gain.exponentialRampToValueAtTime(peak, time + 0.006);
  gain.gain.exponentialRampToValueAtTime(Math.max(sustain, 0.0002), time + 0.12);
  gain.gain.setValueAtTime(Math.max(sustain, 0.0002), releaseStart);
  gain.gain.exponentialRampToValueAtTime(0.0001, stopTime);

  output.gain.value = 0.9;
  filter.connect(gain).connect(output).connect(audio.destination);

  const partials = [
    { ratio: 1, gain: 1, type: "triangle", detune: 0 },
    { ratio: 2, gain: 0.38, type: "sine", detune: 3 },
    { ratio: 3, gain: 0.16, type: "sine", detune: -4 },
    { ratio: 4.01, gain: 0.08, type: "sine", detune: 5 },
  ];
  const oscillators = partials.map((partial) => {
    const osc = audio.createOscillator();
    const partialGain = audio.createGain();
    osc.type = partial.type;
    osc.frequency.value = frequency * partial.ratio;
    osc.detune.value = partial.detune;
    partialGain.gain.value = partial.gain;
    osc.connect(partialGain).connect(filter);
    osc.start(time);
    osc.stop(stopTime + 0.03);
    state.activeNodes.push(osc);
    osc.addEventListener("ended", () => {
      state.activeNodes = state.activeNodes.filter((node) => node !== osc);
    });
    return osc;
  });

  const transient = audio.createOscillator();
  const transientGain = audio.createGain();
  transient.type = "square";
  transient.frequency.value = frequency * 8;
  transientGain.gain.setValueAtTime(peak * 0.08, time);
  transientGain.gain.exponentialRampToValueAtTime(0.0001, time + 0.018);
  transient.connect(transientGain).connect(output);
  transient.start(time);
  transient.stop(time + 0.025);
  state.activeNodes.push(transient);
  transient.addEventListener("ended", () => {
    state.activeNodes = state.activeNodes.filter((node) => node !== transient);
  });

  return oscillators;
}

function stopActiveNodes() {
  for (const node of state.activeNodes) {
    try {
      node.stop();
    } catch {
      // Already stopped by the WebAudio scheduler.
    }
  }
  state.activeNodes = [];
}

function animate() {
  if (!state.isPlaying) return;
  state.currentBeat = audiblePlaybackBeat();
  els.positionSlider.value = String(state.currentBeat / state.totalBeats);
  updateReadout();
  updatePlayhead();
  if (els.followToggle.checked) followPlayhead();
  state.animationId = requestAnimationFrame(animate);
}

function updateReadout() {
  els.timeReadout.textContent = formatTime(beatsToSeconds(state.currentBeat));
  els.beatReadout.textContent = `beat ${state.currentBeat.toFixed(2)}`;
}

function updatePlayhead() {
  els.playhead.style.transform = `translateX(${beatToX(state.currentBeat)}px)`;
}

function followPlayhead() {
  const x = beatToX(state.currentBeat);
  const left = els.scroll.scrollLeft;
  const right = left + els.scroll.clientWidth;
  if (x > right - 120) els.scroll.scrollLeft = x - els.scroll.clientWidth + 120;
  if (x < left + 80) els.scroll.scrollLeft = Math.max(0, x - 80);
}

function rawPlaybackBeat() {
  if (!state.audio) return state.currentBeat;
  return state.startBeat + secondsToBeats(state.audio.currentTime - state.startAudioTime);
}

function audiblePlaybackBeat() {
  if (!state.audio) return state.currentBeat;
  return clamp(rawPlaybackBeat() - secondsToBeats(audioOutputLatencySeconds()), 0, state.totalBeats);
}

function audioOutputLatencySeconds() {
  if (!state.audio) return 0;
  const reportedLatency = state.audio.outputLatency || state.audio.baseLatency || 0;
  return Math.max(0, reportedLatency + state.syncOffsetMs / 1000);
}

function beatToAudioTime(beat) {
  return state.startAudioTime + beatsToSeconds(beat - state.startBeat);
}

function beatsToSeconds(beats) {
  return (beats * 60) / state.tempo;
}

function secondsToBeats(seconds) {
  return (seconds * state.tempo) / 60;
}

function beatToX(beat) {
  return beat * state.pxPerBeat;
}

function meterBeats() {
  const meter = String(state.song?.meta.meter || "4/4");
  const numerator = Number(meter.split("/")[0]);
  return Number.isFinite(numerator) && numerator > 0 ? numerator : 4;
}

function isAudible(trackId) {
  const track = state.tracks.find((item) => item.id === trackId);
  const anySolo = state.tracks.some((item) => item.solo);
  if (!track) return false;
  if (anySolo) return track.solo;
  return !track.muted;
}

function noteNameToMidi(name) {
  if (!name) return null;
  const match = String(name).trim().match(/^([A-Ga-g])([#b]?)(-?\d+)$/);
  if (!match) return null;
  const base = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 }[match[1].toUpperCase()];
  const accidental = match[2] === "#" ? 1 : match[2] === "b" ? -1 : 0;
  const octave = Number(match[3]);
  return (octave + 1) * 12 + base + accidental;
}

function formatTime(seconds) {
  const safeSeconds = Math.max(0, seconds || 0);
  const minutes = Math.floor(safeSeconds / 60);
  const wholeSeconds = Math.floor(safeSeconds % 60);
  const millis = Math.floor((safeSeconds % 1) * 1000);
  return `${minutes}:${String(wholeSeconds).padStart(2, "0")}.${String(millis).padStart(3, "0")}`;
}

function clamp(value, min, max) {
  return Math.min(Math.max(value, min), max);
}

function escapeHtml(value) {
  return String(value ?? "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

window.dslPlayer = {
  loadSong,
  parseDslYaml,
  state,
};
