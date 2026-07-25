import type { FormEvent, InputHTMLAttributes } from "react";

type RangeInputProps = Omit<
  InputHTMLAttributes<HTMLInputElement>,
  "type" | "onChange" | "onInput"
> & {
  onValueChange(value: number): void;
};

export function RangeInput({ onValueChange, ...inputProps }: RangeInputProps) {
  const handleInput = (event: FormEvent<HTMLInputElement>) => {
    onValueChange(Number(event.currentTarget.value));
  };

  return <input {...inputProps} type="range" onInput={handleInput} />;
}
