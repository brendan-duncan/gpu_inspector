// Ported from WebGPU Inspector (MIT): src/utils/flags.js

/** A map of flag name -> bit value, e.g. `{ COPY_SRC: 4, COPY_DST: 8 }`. */
export type FlagMap = Record<string, number>;

export function getFlagString(value: number, flags: FlagMap): string {
  function _addFlagString(flags: string, flag: string): string {
    return flags === "" ? flag : `${flags} | ${flag}`;
  }
  let flagStr = "";
  for (const flagName in flags) {
    const flag = flags[flagName];
    if (value & flag) {
      flagStr = _addFlagString(flagStr, flagName);
    }
  }
  return flagStr;
}
