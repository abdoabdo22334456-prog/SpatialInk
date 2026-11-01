# UNI 0.8.0 — Windows VST3

UNI is a native Windows x64 spatial audio effect. **Two Pages** duplicates the input into two independent processing pages, A and B, and blends their audible outputs equally. Each page has its own path, orbit, XY position, room, ambience, height, mix, output, bypass, transport mode, tempo and speed. Switching page tabs changes which controls are shown; both pages continue playing. The original **Single** mode and the earlier **Double Orbit** mode remain available separately.

- [Windows installer](UNI-VST3-Setup.exe): close your DAW, run as administrator, then rescan VST3 plugins.
- [Complete Arabic guide](README-AR.md)
- [Build instructions](source/BUILD.md) and [installer instructions](installer/BUILD.md)
- [Test report](TEST-REPORT.md)

The VST3 bundle is in `UNI.vst3/`; copy the entire folder if installing manually. The installer places it under `C:\Program Files\Common Files\VST3`. The plugin has 39 automatable parameters. Existing VST3 parameter IDs and identity are preserved, and state formats v3–v6 load into this version. New v7 states require UNI 0.8.0 or newer.

The Steinberg VST3 Validator passed 47 tests with no failures. A native VST3 host verified both pages, automation, state migration, GUI gestures and the preservation of Single and Double Orbit output against previous binaries. Live FL Studio operation and elevated installation in Program Files were not exercised in this test session.

![Two Pages interface](Interface-VST3-TwoPages.png)
