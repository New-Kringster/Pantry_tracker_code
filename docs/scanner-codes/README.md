# Scanner engine programming barcodes

Cropped from the ZBarcode Scanner User Guide 2.5 linked on the M5Stack
Module13.2 QRCode page. The engine reads them from a screen at about 15 cm.
Open the SCAN page on the CoreS3 first; while the UART link is down it holds
the engine's TRIG line low so the engine decodes without any serial command.

| File | Code | Effect |
|---|---|---|
| `serial-communication-21424000.png` | 21424000 | Interface = TTL serial. **Needed once per engine**: it ships on USB, and a factory reset puts it back. |
| `uart1-21414501.png` / `uart0-21414500.png` | 21414501 / 21414500 | Selects which internal UART the engine uses (not needed so far). |
| `test-ean13-6909548500734.png` | EAN-13 | Clean test barcode for checking the decode path from a screen. |

Factory reset code 327601 is on page 7 of the guide; it undoes the serial setting.
