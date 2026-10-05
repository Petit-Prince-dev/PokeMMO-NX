# Third-party notices

The MIT license of this project (`LICENSE`) covers the code in this repository only. The application (`PokeMMO.nro`) is linked with the libraries below, each under its own license; their copyright notices and license texts are in the upstream projects linked here.

| Component | Used for | License | Source |
| --- | --- | --- | --- |
| libnx | Switch system services, the console font of the file chooser | ISC | https://github.com/switchbrew/libnx |
| newlib (C library and math library of devkitA64) | libc, libm | BSD-style licenses | https://sourceware.org/newlib/ |
| Mesa 20.1 (EGL, OpenGL, nouveau driver) | the game's graphics | MIT | https://www.mesa3d.org/license.html |
| libdrm (nouveau) | the graphics driver | MIT | https://gitlab.freedesktop.org/mesa/drm |
| glad | OpenGL function loader | public domain / MIT | https://github.com/Dav1dde/glad |
| zlib | compression for the game | zlib license | https://zlib.net/zlib_license.html |
| libstdc++ (GCC 12) | C++ runtime of Mesa; also shipped as `libstdc++.so.6` and `libgcc_s.so.1` next to the game, unmodified from Debian 12 | GPL version 3 with the GCC Runtime Library Exception | https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html, https://www.gnu.org/licenses/gcc-exception-3.1.html |

The release zip also contains `PokeMMO-forwarder.nsp`, a home menu launcher made with an online NSP forwarder generator. It only starts `switch/PokeMMO/PokeMMO.nro` and holds no game data; it is not covered by the MIT license of this project.

The release zip also contains the PokeMMO client, unmodified (revision 32920, Linux ARM64 part). It is proprietary software of the PokeMMO team and is not covered by the MIT license.
