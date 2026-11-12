# WebServer (copy from the ESP32 Arduino core 2.0.17)

Unmodified copy of `libraries/WebServer` from arduino-esp32 2.0.17 (ESP-IDF v4.4.7), the
core this project builds with, apart from one change in `src/WebServer.h`:

- `HTTP_MAX_DATA_WAIT` is 500 ms instead of 5000 ms.

PlatformIO uses a library in the project's `lib` folder in place of the core's own copy of
the same name. Why the change: the server handles one connection at a time, and an idle
browser connection (Chrome opens spare ones) held it for 5 s each time, so firmware
uploads over WiFi timed out while the logger's page was open.

Licence: GNU LGPL 2.1 or later, as stated in the source files (Ivan Grokhotkov / Espressif).
If the project moves to a newer core, re-copy the library from that core and reapply the change.
