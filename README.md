# TB M50 Single Channel Strip (2026 Mac)

One input channel of the **TASCAM M-50** mixing console, **mono in and mono out**, as **CLAP, VST3 and
AU (AUv2)** plugins for current Macs: universal (Apple Silicon + Intel), macOS 11 and later. For an
insert on every track, so a whole session sounds recorded through an M-50, with
TB M50 Dual Channel Strip on the mix buss. Non-commercial, for personal use.

The sound, panel and presets are the approved 10.8.5 build's
([TB-M50-Single-Channel-Strip-MOUNTAIN-LION](https://github.com/tonybuzzcock/TB-M50-Single-Channel-Strip-MOUNTAIN-LION)),
and the output matches it sample for sample. The signal goes straight through the strip to the channel
fader: INPUT (MIC / D.I. / LINE), TRIM with the large OVERLOAD LED (the drive starts rounding just under
it: turn TRIM up until it flashes on the loudest moments), EQ BYPASS, three sweep EQ sections, BYPASS
(this plugin only, click-free), fader, VU. Right-click (or Control-click) a control to see its exact
value. Named presets are shared by all three formats.

The panel is the 10.8.5 strip in its mahogany frame, drawn at **75 % (276 x 771)** so it fits a MacBook
screen.

## Build

GitHub Actions builds universal binaries, checks both architectures and macOS 11, ad-hoc signs them,
runs the CLAP host test, the DSP test and the panel test, validates with auval, clap-validator and
pluginval, publishes panel screenshots to `gui-preview` and uploads `TB-M50-Single-Channel-Strip-mac.zip`.

## Install

Actions -> the newest green run -> Artifacts -> `TB-M50-Single-Channel-Strip-mac`. Unzip, then follow
`INSTALL.txt`: copy the plugins into `/Library/Audio/Plug-Ins/` (VST3, Components, CLAP), then in
Terminal, one line at a time:

    cd /Library/Audio/Plug-Ins
    ls */ | grep -i m50
    sudo xattr -cr */"TB M50 Single Channel Strip".*
    sudo codesign --force --deep --sign - */"TB M50 Single Channel Strip".*
    killall -9 AudioComponentRegistrar

Then rescan in your DAW (Reaper: Settings > Plug-ins > VST and CLAP > Clear cache/re-scan).
