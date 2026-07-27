import { UserCircleIcon } from "@heroicons/react/24/outline";
import type { BackingTrackName, VoiceProfileOption } from "@/domain/studio";

export function VoiceProfileSelect({
  track,
  profiles,
  value,
  disabled,
  onChange,
}: {
  track: BackingTrackName;
  profiles: VoiceProfileOption[];
  value: string;
  disabled: boolean;
  onChange(profileId: string): void;
}) {
  const selectedValue = profiles.some((profile) => profile.id === value)
    ? value
    : (profiles[0]?.id ?? "");

  return (
    <label
      className="voice-profile-select"
      title={profiles.length > 0 ? `Voice model for ${track}` : "Train a LoRA voice first"}
    >
      <UserCircleIcon aria-hidden="true" />
      <span className="sr-only">Voice model for {track}</span>
      <select
        aria-label={`Voice model for ${track}`}
        value={selectedValue}
        disabled={disabled || profiles.length === 0}
        onChange={(event) => onChange(event.target.value)}
      >
        {profiles.length === 0 ? <option value="">No trained voices</option> : null}
        {profiles.map((profile) => (
          <option key={profile.id} value={profile.id}>
            {profile.name} · {profile.quality === "high" ? "High" : "Balanced"}
            {profile.active ? " · Active" : ""}
          </option>
        ))}
      </select>
    </label>
  );
}
