# tools/re — firmware analysis helpers

Reproduces the analysis behind `documentation/HARDWARE_MAP.md`. **No firmware images or decompiled
output are stored in this repository** (they are third-party code); keep them locally.

1. Extract `ota_0` (flash offset `0x10000`, length `0x180000`) from your own flash dump.
2. Ghidra 12 headless, language `Xtensa:LE:32:default`:
   `analyzeHeadless <proj_dir> iflo -import ota_0.bin -processor "Xtensa:LE:32:default" -preScript Esp32S2OtaRemap.java ota_0.bin -scriptPath tools/re -postScript DumpAll.java <out_dir>`
   (`Esp32S2OtaRemap.java` rebuilds the ESP32-S2 segment map; `DumpAll.java` decompiles every function.)
3. `RE_DIR=<dir with ota_0.bin + all_decomp.c> python3 -I resolve.py` inlines literal-pool strings and pointers
   (Ghidra leaves them as `PTR_DAT_xxxx`), writing `resolved.c`.
4. `RE_DIR=<same dir> python3 -I fn.py <hex addr> ...` prints chosen functions from `resolved.c`.
