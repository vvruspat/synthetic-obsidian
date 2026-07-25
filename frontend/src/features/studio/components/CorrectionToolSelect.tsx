import {
  AdjustmentsHorizontalIcon,
  ArrowsRightLeftIcon,
  BackspaceIcon,
  CursorArrowRaysIcon,
  LinkIcon,
  MagnifyingGlassIcon,
  PencilIcon,
  ScissorsIcon,
  SignalIcon,
} from "@heroicons/react/24/outline";
import type { CorrectionToolId } from "@/domain/studio";
import { SelectMenu, type SelectMenuOption } from "@/ui";

const correctionTools: SelectMenuOption<CorrectionToolId>[] = [
  { value: "pointer", label: "Pointer Tool", icon: CursorArrowRaysIcon },
  { value: "pencil", label: "Pencil Tool", icon: PencilIcon },
  { value: "eraser", label: "Eraser Tool", icon: BackspaceIcon },
  { value: "scissors", label: "Scissors Tool", icon: ScissorsIcon },
  { value: "join", label: "Join Tool", icon: LinkIcon },
  { value: "flex", label: "Flex Tool", icon: ArrowsRightLeftIcon },
  { value: "vibrato", label: "Flex Pitch Vibrato", icon: SignalIcon },
  { value: "gain", label: "Gain Tool", icon: AdjustmentsHorizontalIcon },
  { value: "zoom", label: "Zoom Tool", icon: MagnifyingGlassIcon },
];

type CorrectionToolSelectProps = {
  label: string;
  value: CorrectionToolId;
  onChange(value: CorrectionToolId): void;
};

export function CorrectionToolSelect(props: CorrectionToolSelectProps) {
  return <SelectMenu {...props} options={correctionTools} className="tool-selector" />;
}
