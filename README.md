![Preview](https://github.com/GhostNaN/mpvpaper/blob/assests/preview.png)
# mpvpaper
### mpvpaper is a wallpaper program for wlroots based wayland compositors, such as sway. That allows you to play videos with mpv as your wallpaper.

## Dependencies
- [mpv](https://github.com/mpv-player/mpv)
- [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots)

## Building 
### Building Requirements:

- ninja
- meson
- libmpv
### Clone | Build | Install:
```
# Clone
git clone --single-branch https://github.com/GhostNaN/mpvpaper
# Build
cd mpvpaper
meson setup build --prefix=/usr/local
ninja -C build
# Install
ninja -C build install
```
## Usage
Simple example:
```
mpvpaper DP-2 /path/to/video
```
To play the same video on all outputs:
```
mpvpaper '*' /path/to/video
```
You can also forward mpv options by passing "--mpv-options" or "-o" like so:
```
mpvpaper -o "no-audio --loop-playlist shuffle" HDMI-A-1 www.url/to/playlist
```
You can also control mpvpaper just like mpv in the terminal with keyboard bindings. 

But if you would like to  control mpvpaper while it's forked, you could use a mpv input-ipc-server like this:
```
mpvpaper -o "input-ipc-server=/tmp/mpv-socket" DP-1 /path/to/video
```
Then input commands with socat. For example, toggle pause:
```
echo 'cycle pause' | socat - /tmp/mpv-socket
```
For more mpv commands read: https://mpv.io/manual/master/#command-interface

For more info on mpvpaper, please refer the the [man page](/mpvpaper.man).
## Acknowledgments
- glpaper and swaybg for the initial boilerplate code, check em out here:
  - https://hg.sr.ht/~scoopta/glpaper
  - https://github.com/swaywm/swaybg
## License
This project is licensed under the GPLv3 License - see the [LICENSE](/LICENSE) file for details
