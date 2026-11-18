Vendored from https://github.com/m5stack/M5Module-QRCode commit 55a21101829819d26095cc027e82c987717dc987 (v1.0.1, MIT), 2026-11-06. Unmodified.

## Local changes

- `qrcode_m14.cpp` `waitResponse()`: upstream returned as soon as any byte had
  arrived, which on the CoreS3 gave a 1-byte "reply" and an empty firmware
  version ("invaild response size: 1"). It now reads until the UART has been
  quiet for 30 ms.
