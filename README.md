# Cutefish Compositor

The Cutefish Wayland compositor, with window management, the lock screen and
Xwayland in one process. Much of it is ported from Chromium's Exo and Ash.

## Dependencies

Qt 6.8 and libwayland 1.23 or newer. At runtime it needs `adwaita-icon-theme`
and `xwayland`. Tests also need `libgtest-dev`.

## Build

```bash
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr ..
make
sudo make install
```

Pass `-DBUILD_TESTING=ON` to build the tests.

## License

BSD-3-Clause; see [LICENSE](LICENSE). Code ported from Chromium keeps its
original notice in [LICENSE.chromium](LICENSE.chromium), and code from
sommelier in [LICENSE.sommelier](LICENSE.sommelier).
