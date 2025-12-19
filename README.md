![Preview](https://github.com/GhostNaN/mpvpaper/blob/assests/preview.png)

# mpvpaper

**mpvpaper** is a lightweight wallpaper program for **wlroots-based Wayland compositors** (such as `sway`).  
It allows you to play **videos as your wallpaper** using `mpv`.

---

## ✨ Features

- Play videos as wallpapers on Wayland
- Supports **multiple outputs**
- Works with any wlroots-based compositor
- Forwards native **mpv options**
- Supports **IPC control** via `mpv` socket
- Lightweight and minimal

---

## 🔗 Dependencies

Make sure the following are installed on your system:

- [mpv](https://github.com/mpv-player/mpv)
- [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots)

---

## 🛠️ Build & Installation

### Build Requirements

- `ninja`
- `meson`
- `libmpv`

### Clone, Build & Install

# Clone repository
```
git clone --single-branch https://github.com/GhostNaN/mpvpaper
cd mpvpaper
```

# Configure build
```
meson setup build --prefix=/usr/local
```

# Build
```
ninja -C build
```

# Install
```
ninja -C build install
```

---

## ▶️ Usage
- **Basic Usage**
Play a video on a specific output:
```
mpvpaper DP-2 /path/to/video
```
- **Play on All Outputs**
```
mpvpaper ALL /path/to/video
```

# 🎛️ MPV Options
You can pass native `mpv` options using `--mpv-options` or `-o`:
```
mpvpaper -o "no-audio --loop-playlist shuffle" HDMI-A-1 https://www.url/to/playlist
```
This allows full control over playback behavior using standard mpv flags.

# 🔌 IPC Control (Advanced)
You can control `mpvpaper` just like `mpv` using keyboard bindings.
If you need to control it after it has forked, use an IPC socket:
```
mpvpaper -o "input-ipc-server=/tmp/mpv-socket" DP-1 /path/to/video
```
Example: Toggle Pause
```
echo 'cycle pause' | socat - /tmp/mpv-socket
```

📖 For more IPC commands, refer to the official mpv documentation:
https://mpv.io/manual/master/#command-interface

---

## 📘 Documentation
Full documentation is available in the [man page](/mpvpaper.man)

---

## 🙌 Acknowledgments
Initial boilerplate and inspiration from:
- https://hg.sr.ht/~scoopta/glpaper
- https://github.com/swaywm/swaybg

---

## 📄 License

This project is licensed under the **GPLv3 License**.  
See the [LICENSE](LICENSE) file for details.



