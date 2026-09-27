# Cutefish Compositor

Cutefish Wayland compositor: DRM/KMS output, rendering, input, window
management, server-side decorations, the lock screen and rootless Xwayland in
one process. Much of it is ported from Chromium's Exo and Ash.

It is written in C++ with Qt 6, and the lock screen and the application
switcher are drawn with Qt Quick.

## Dependencies

Qt 6.8 or newer is required. At runtime it needs `adwaita-icon-theme` for
cursors and `xwayland` for X11 applications. Tests additionally need
`libgtest-dev`.

## Build

```bash
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr ..
make
sudo make install
```

Pass `-DBUILD_TESTING=ON` to build the tests, or
`-DCUTEFISH_ENABLE_XWAYLAND=OFF` to leave Xwayland out.

## Run

The compositor normally runs as the session's display server, taking the seat
through libseat. `--no-drm` starts a headless Wayland server instead, and
`--no-xwayland` turns Xwayland off at runtime.

## License

BSD-3-Clause; see [LICENSE](LICENSE). Code ported from Chromium keeps its
original notice in [LICENSE.chromium](LICENSE.chromium), and code from
sommelier in [LICENSE.sommelier](LICENSE.sommelier).
