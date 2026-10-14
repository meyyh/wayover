# wayover

A lightweight, high-performance transparent video overlay client for Wayland compositors.

`wayover` decodes video files with alpha transparency (e.g. Apple ProRes 4444, VP9 with alpha) using FFmpeg and renders them directly onto your Wayland desktop as a borderless overlay using the `wlr-layer-shell` protocol, with seamless looping, click-through support, and interactive movement controls.

---

## Features

- **Alpha Channel Video Support**: Plays videos with true transparency (ProRes 4444, WebM VP8/VP9 with alpha, QuickTime Animation, etc.) via FFmpeg (`libavcodec` / `libswscale`).
- **Wayland Native Layer Shell**: Uses `wlr-layer-shell-unstable-v1` to display overlays above or across your desktop workspace on compatible compositors.
- **Click Passthrough**: Optional `--passthrough` mode sets an empty Wayland input region so clicks pass through directly to windows underneath.
- **Interactive Controls**: Move the overlay in real-time using `W`/`A`/`S`/`D` or the Arrow keys.
- **High Performance & Low Memory**:
  - Double-buffered shared memory (`wl_shm`).
  - Selective damage tracking (`wl_surface_damage_buffer`) so only moving/active video frames trigger redraws instead of repainting the entire desktop surface.
  - Direct frame decoding into shared memory buffers when completely on-screen.
  - Compositor frame callbacks (`wl_surface_frame`) synced to video framerate with jitter tolerance.
  - Minimal decoder footprint (~120MB) with bounded thread usage.
- **Automatic Looping**: Seamlessly loops video playback upon reaching end-of-stream.
- **Compositor Fallback**: Includes `--xdg` mode for running inside a standard XDG Shell window (useful on compositors without layer-shell support or for debugging).
- **Auto Resolution Detection**: Queries `wl_output` to automatically match your monitor's current display resolution.

---

## Requirements

To build and run `wayover`, you need:

- A C11 compiler (`gcc` or `clang`)
- `make`
- `wayland-client`
- `xkbcommon`
- FFmpeg libraries (`libavcodec`, `libavformat`, `libavutil`, `libswscale`)

### Installing Dependencies

#### Arch Linux
```bash
sudo pacman -S base-devel wayland ffmpeg libxkbcommon
```

#### Debian / Ubuntu
```bash
sudo apt update
sudo apt install build-essential libwayland-dev libxkbcommon-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev
```

#### Fedora
```bash
sudo dnf install gcc make wayland-devel libxkbcommon-devel \
    ffmpeg-free-devel
# Or if using RPM Fusion for full FFmpeg:
# sudo dnf install ffmpeg-devel
```

---

## Building

Clone the repository and build using `make`:

```bash
git clone https://github.com/username/wayover.git
cd wayover
make
```

This compiles the `client` executable.

To clean build artifacts:
```bash
make clean
```

---

## Usage

```bash
./client [options] <video_file> [x] [y]
```

### Positional Arguments

| Argument | Description |
| :--- | :--- |
| `<video_file>` | Path to a transparent video file (e.g. ProRes 4444 `.mov`, WebM `.webm`) |
| `[x]` | Initial horizontal coordinate on screen (`-1` or omitted to center) |
| `[y]` | Initial vertical coordinate on screen (`-1` or omitted to center) |

### Options

| Flag | Description |
| :--- | :--- |
| `-x <val>` | Set initial X coordinate (`-1` to center) |
| `-y <val>` | Set initial Y coordinate (`-1` to center) |
| `-w <width>` | Override canvas width (default: auto-detected monitor width) |
| `-h <height>` | Override canvas height (default: auto-detected monitor height) |
| `--passthrough` | Make the overlay transparent to mouse clicks (passes input through to background windows) |
| `--xdg` | Force a standard XDG-shell window instead of a layer-shell overlay |
| `--help`, `-h` | Display help and usage information |

### Controls

When focused (or in interactive mode):

| Key | Action |
| :--- | :--- |
| `W` / `Up Arrow` | Move overlay up |
| `S` / `Down Arrow` | Move overlay down |
| `A` / `Left Arrow` | Move overlay left |
| `D` / `Right Arrow` | Move overlay right |
| `Q` / `Escape` | Exit application |

---

## Examples

#### Play centered on screen
```bash
./client animation.mov
```

#### Play at specific screen coordinates
```bash
./client -x 100 -y 200 animation.mov
# or using positional arguments:
./client animation.mov 100 200
```

#### Play as a non-interactive desktop overlay with click passthrough
```bash
./client --passthrough animation.mov
```

#### Open inside a floating / managed window (XDG Shell)
```bash
./client --xdg animation.mov
```

---

## Compatible Compositors

- **Layer Shell (`wlr-layer-shell`)**:
  - Hyprland
  - Sway
  - River
  - Wayfire
  - Labwc
  - Any wlroots-based compositor
- **XDG Shell Mode (`--xdg`)**:
  - GNOME (Mutter)
  - KDE Plasma (KWin)
  - Any standard Wayland compositor

---

## Transparent Video Formats

Common codecs supported by FFmpeg with alpha channels:
- **Apple ProRes 4444 / 4444 XQ** (`ap4h` / `ap4x` in `.mov`)
- **VP8 / VP9 with Alpha** (`yuva420p` in `.webm`)
- **QuickTime Animation (RLE)** (`qtrle` in `.mov`)
- **PNG / APNG Sequence**

You can test video stream transparency with `ffprobe`:
```bash
ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt your_video.mov
```
*(Look for pixel formats containing `a`, such as `yuva444p12le`, `yuva420p`, or `rgba`.)*

---

## License

This project is licensed under the terms included in the repository.
