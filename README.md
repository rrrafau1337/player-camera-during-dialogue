# Player Camera During Dialogue

<p align="center">

<img src="docs/demo.gif" alt="Player Camera During Dialogue Demo" width="900">

</p>

<p align="center">

A native <strong>SFSE</strong> plugin for <strong>Starfield</strong> that replaces the vanilla dialogue camera with dynamic player-focused cinematic shots.

</p>

<p align="center">

<a href="https://www.nexusmods.com/starfield/mods/17856"><strong>📥 Nexus Mods</strong></a> •
<a href="#installation"><strong>Installation</strong></a> •
<a href="#building"><strong>Building</strong></a> •
<a href="#known-issues"><strong>Known Issues</strong></a>

</p>

---

## Features

- 🎥 Automatic player camera during dialogue
- 🎮 Manual camera switching mode
- 🔄 Automatic camera switching based on the active speaker
- 🚧 Camera obstruction avoidance
- 😀 Expression cycling hotkey
- ⚙️ Fully configurable via INI
- ⚡ Native SFSE plugin (no Papyrus)
- 📦 No ESP/ESM required

---

## Installation

Download the latest release from Nexus Mods:

**https://www.nexusmods.com/starfield/mods/17856**

Copy

```
PointCameraAtPlayer.dll
PointCameraAtPlayer.ini
```

into

```
Starfield/Data/SFSE/Plugins/
```

### Required game setting

Disable the vanilla dialogue camera:

```
Settings
→ Accessibility
→ Dialogue Camera
→ OFF
```

Otherwise the vanilla camera will override this mod.

---

## Configuration

All settings are located in

```
Data/SFSE/Plugins/PointCameraAtPlayer.ini
```

You can configure:

- Automatic / Manual mode
- Camera delays
- Camera positioning
- Obstruction handling
- Manual camera key
- Expression key
- Notifications

---

## Building

Clone together with submodules

```bash
git clone --recurse-submodules https://github.com/rrrafau1337/player-camera-during-dialogue.git
```

or

```bash
git submodule update --init --recursive
```

Build using XMake

```bash
xmake build
```

Output

```
build/windows/x64/release/PointCameraAtPlayer.dll
```

---

# Known Issues

The plugin is still actively being developed.

Current known issues include:

- FIXED in 1.18.12: Ship hailing dialogue can leave the gameplay camera in an incorrect state in some situations (currently under investigation).
- Rare dialogue voice-line overlap can still occur during automatic camera switching.
- Reports of character T posing after dialogue ends in certain situations (zero G, awkward angles), so far could not reproduce
- Hard crash when speaking to phantoms inside Ma’leen Dam (investigating)

If you encounter a reproducible issue, please include:

- Plugin version
- Steps to reproduce
- `PointCameraAtPlayer.log`
- Set Starfield Engine Fixes bDetailedCrashLogger setting to 1 (in Data\SFSE\Plugins\StarfieldEngineFixes.ini): copy paste the whole crashlog
- Pastebin or create issue or leave comment on Nexusmods

---

# Roadmap

Current development focuses on:

- Better cinematic camera framing
- Improved shot variety
- More natural camera transitions
- Additional dialogue camera presets
- Ship communication support

---

# Contributing

Source code is provided for transparency and preservation.

Bug reports are always welcome.

Please note that this repository mirrors released versions of the mod and may lag behind active development.

---

# Credits

- libxse / CommonLibSF
- SFSE Team
- Bethesda Game Studios
- Everyone who has provided bug reports and testing

---

# License

See LICENSE.
