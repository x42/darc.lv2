darc.lv2 - Dynamic Audio Range Compressor
=========================================

darc.lv2 is a general purpose audio signal compressor.

The compression gain is controlled by threshold and ratio only:
Makeup gain is automatically set to retain equal loudness at -10 dBFS/RMS with a soft knee.
This maintains perceived loudness over a wide range of thresholds and ratio settings.

It is available as [LV2 plugin](http://lv2plug.in/) and standalone [JACK](http://jackaudio.org/)-application.

Usage
-----

The Plugin has five controls which can be operated by mouse-drag and the scroll-wheel.
Holding the _Ctrl_ key allows for fine-grained control when dragging or using the mouse-wheel on a knob.

Furthermore the UI offers
*   Shift + click: reset to default
*   Right-click on knob: toggle current value with default, 2nd click restore.

For an elaborate descripton and manual, please see https://x42-plugins.com/x42/x42-compressor

Install
-------

Compiling darc.lv2 requires the LV2 SDK, jack-headers, gnu-make, a c++-compiler,
liblo, libpango, libcairo and openGL (sometimes called: glu, glx, mesa).

```bash
  git clone https://github.com/x42/darc.lv2.git
  cd darc.lv2
  make submodules
  make
  sudo make install PREFIX=/usr
```

Note to packagers: the Makefile honors `PREFIX` and `DESTDIR` variables as well
as `CXXFLAGS`, `LDFLAGS` and `OPTIMIZATIONS` (additions to `CXXFLAGS`), also
see the first 10 lines of the Makefile.
You really want to package the superset of [x42-plugins](https://github.com/x42/x42-plugins).

Screenshots
-----------

![screenshot](https://raw.github.com/x42/darc.lv2/master/img/darc.png "DARC LV2 GUI")

Credits
-------

darc.lv2 was inspired by Fons Adriaensen's zita-dc1-0.3.3.
