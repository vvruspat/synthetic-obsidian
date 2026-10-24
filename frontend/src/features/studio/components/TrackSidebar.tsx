import { BackingGroupHeader } from "@/features/studio/components/BackingGroupHeader";
import { TrackHeader } from "@/features/studio/components/TrackHeader";
import { VoiceProfileSelect } from "@/features/studio/components/VoiceProfileSelect";
import type { StudioController } from "@/features/studio/hooks/useStudioController";
import type { ArrangementRow } from "@/features/studio/lib/arrangement";

export function TrackSidebar({
  controller,
  row,
  backingRenderBusy,
}: {
  controller: StudioController;
  row: ArrangementRow;
  backingRenderBusy: boolean;
}) {
  const { track, depth, number } = row;
  const { actions, trackState, trackLayerState, trackLayerAvailability } = controller;
  const instrumental = track.id === "Instrumental";
  return (
    <div className="track-row-header" data-track-header={track.id}>
      {row.kind === "backing-group" ? (
        <BackingGroupHeader
          row={row}
          onToggle={() => actions.toggleTrackCollapsed(track.id, !track.collapsed)}
        />
      ) : (
        <TrackHeader
          id={track.id}
          name={track.name}
          depth={depth}
          rowNumber={number}
          state={trackState[track.id] ?? { mute: false, solo: false }}
          onToggle={(mode) => actions.toggleTrack(track.id, mode)}
          volume={instrumental ? controller.instrumentalVolume : undefined}
          onVolumeChange={instrumental ? actions.setInstrumentalVolumeValue : undefined}
          layerState={
            instrumental
              ? undefined
              : (trackLayerState[track.id] ?? { audioMuted: false, notesMuted: false })
          }
          layerAvailability={instrumental ? undefined : trackLayerAvailability[track.id]}
          onToggleLayer={
            instrumental ? undefined : (layer) => actions.toggleTrackLayer(track.id, layer)
          }
          selected={controller.selectedTracks.includes(track.id)}
          onSelect={(additive) => actions.selectTrack(track.id, additive)}
          onOpenFile={depth ? undefined : () => actions.importTrackAudio(track.id)}
          voiceControl={
            depth ? (
              <VoiceProfileSelect
                compact
                track={track.name}
                profiles={controller.voiceProfiles}
                value={
                  controller.backingTrackContents.find((item) => item.id === track.id)
                    ?.voiceProfileId ?? ""
                }
                disabled={backingRenderBusy || !trackLayerAvailability[track.id]?.notesAvailable}
                onChange={(profileId) => actions.setBackingVoiceProfile(track.id, profileId)}
              />
            ) : undefined
          }
          onDelete={instrumental ? undefined : () => actions.removeTrack(track.id)}
        />
      )}
    </div>
  );
}
