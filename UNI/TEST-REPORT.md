# UNI 0.8.0 test report

- Built native Windows x64 VST3 with the pinned CLAP wrapper and Steinberg VST3 SDK. Plugin SHA-256: `c92dc50176eb397052095af6583b3373d179b43b86601f44d1cca2452cd35085`.
- Steinberg VST3 Validator: **47 passed, 0 failed**. Full output: `tests/VALIDATOR-LOG.txt`.
- Native VST3 integration host: passed against both actual previous UNI 0.7.0 (v6 state) and UNI 0.6.0 (v5 state) binaries. Logs: `tests/TEST-VST3-HOST-LOG-v6.txt` and `tests/TEST-VST3-HOST-LOG-v5.txt`.
- Host tests covered all 39 automatable parameters, normalized range and sample timing, stereo input/output, two independent page engines, simultaneous audible outputs, per-page bypass, paths, spatial automation and tempo clocks, two-page state save/restore, GUI control gestures, and preservation of Single and Double Orbit audio after migration.
- Installer scratch tests passed for bundle creation, payload checksum, repeated installation and backup handling, locked/corrupt path rejection and preservation of unrelated files. The shipping EXE resource check confirmed payload SHA-256, administrator manifest, native dialog dependency and version.
- Actual elevated installation into Program Files and a live FL Studio project were not exercised in this session. The native VST3 host exercised the plugin and its GUI.
