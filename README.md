# Pantry Expiry Tracker

An expiry tracker for the fridge door, built on an **M5Stack CoreS3 SE** and a
**Module13.2 QRCode** scanner. You scan a product's barcode and pick when it
expires, and the screen always shows what to eat first. A small server on your
home network names the products (Open Food Facts worldwide, GS1 China for
products sold in mainland China), keeps the list, serves a web page for managing
it and sends reminders to Home Assistant over MQTT.

I built it during my internship at M5Stack. The write-up, with a demo video and
a working copy of the screen, is at
[chiambucket.com/pantry](https://www.chiambucket.com/pantry), and M5Stack wrote
about it on their blog in
[Turn Your Fridge Door into a Pantry Assistant with M5Stack CoreS3 SE](https://shop.m5stack.com/blogs/news/turn-your-fridge-door-into-a-pantry-assistant-with-m5stack-cores3-se).

## Downloads

| Download | For |
|---|---|
| [Firmware on M5Burner](https://burner.m5stack.com/share/firmware/R4VK5Z) | the CoreS3 SE, flashed with M5Burner |
| [Server for Intel and AMD computers](https://github.com/New-Kringster/Pantry_tracker_code/releases/latest/download/pantry-server-docker-amd64.tar.gz) | a mini PC, a NAS, a Windows or Linux PC |
| [Server for ARM computers](https://github.com/New-Kringster/Pantry_tracker_code/releases/latest/download/pantry-server-docker-arm64.tar.gz) | an Apple Silicon Mac, a Raspberry Pi 4 or 5 on a 64-bit OS |
| [Firmware image](https://github.com/New-Kringster/Pantry_tracker_code/releases/latest/download/pantry-tracker-cores3.bin) | the CoreS3 SE, if you flash it with esptool instead of Flashdeck |

Each server download holds the Docker images themselves, so nothing is pulled
from Docker Hub while it installs. Every version is on the
[Releases](https://github.com/New-Kringster/Pantry_tracker_code/releases) page.

## What you need

- [CoreS3 SE](https://shop.m5stack.com/products/m5stack-cores3-se-iot-controller-w-o-battery-bottom), which runs the tracker with its touch screen and WiFi
- [Module13.2 QRCode](https://shop.m5stack.com/products/qr-code-scanner-module-13-2), which reads 1D and 2D barcodes and stacks under the CoreS3 SE
- a battery base underneath, so it runs on the fridge without a cable
- a computer that stays on for the server, with Docker installed
- Home Assistant, if you want reminders

## Set it up

1. **Set the scanner's switches and stack the parts.** On the QRCode module,
   slide the interface toggle to UART and switch on number 2 in both the TX and
   RX DIP blocks, leaving the others off. Then stack the CoreS3 SE on top, the
   module in the middle and the battery base underneath.

2. **Flash the firmware.** The firmware is on M5Burner as
   [CoreS3 SE Pantry Assistant](https://burner.m5stack.com/share/firmware/R4VK5Z): open that page, plug the CoreS3 SE in over
   USB-C and burn it from M5Burner. You can also open
   [Flashdeck](https://flashdeck.shebang.pro/) in Chrome or Edge, choose
   **Pantry** in the list and press **Install**, which writes the image straight
   from the browser. To use esptool instead, download the firmware image above and run:

   ```sh
   esptool --chip esp32s3 --port <your port> --baud 921600 write_flash 0x0 pantry-tracker-cores3.bin
   ```

3. **Start the server.** Download the server for your computer, unpack it and
   set `TZ` in `docker-compose.yml` to your time zone, because the tracker counts
   "days left" from local midnight. Then run the installer in that folder:

   ```sh
   tar -xzf pantry-server-docker-amd64.tar.gz
   cd pantry-server-docker
   ./install.sh
   ```

   On Windows, load the two images in `images/` with `docker load -i <file>` and
   run `docker compose up -d`. Opening `http://<the computer's IP>:8080/` shows an
   empty pantry page once the server is running.

4. **Connect the tracker.** When it starts with no WiFi saved, the tracker opens
   a hotspot called `Pantry-XXXX` and shows a QR code. Scan it with your phone,
   and the setup page asks for your WiFi and the server address, for example
   `http://192.168.1.20:8080`. The same page stays available afterwards at
   `http://pantry-tracker.local/`.

5. **Switch the scan engine to serial.** Tap **SCAN NOW** and scan
   [this setting code](docs/scanner-codes/serial-communication-21424000.png) from
   a screen once. The engine ships set to USB, and this code switches it to the
   serial line the CoreS3 SE listens on.

6. **Add Home Assistant** (optional). Add the **MQTT** integration with the
   server's IP address and port 1883, with no username or password, and each
   pantry appears as its own device. [docs/home-assistant.md](docs/home-assistant.md)
   lists every entity and event with a dashboard card and a reminder automation.

7. **Add GS1 China** (optional). For products sold in mainland China, create a
   free account at gds.org.cn, enter it under **Settings** on the server's web
   page and sign in once there. The server keeps the session alive after that.

The server's web page holds the rest of the settings: the product-name language
(Chinese, English or the product's own), the "expiring soon" days and a
different MQTT broker if you already run one.

## Use it

- **Scan a product.** Tap **SCAN NOW** and hold the barcode about a hand's width
  from the scan window, then set the expiry with the Days, Weeks and Months
  arrows and tap **SAVE**.
- **See what to eat first.** The home screen lists the pantry by expiry, and the
  count turns red within a day of the date and amber within a week.
- **Scan again to open it.** Scanning something already on the list opens it,
  so you can check the date or delete it.
- **Swipe.** Swipe a card from right to left for the delete button, and swipe
  from left to right anywhere to go back.
- **Keep several pantries.** The **Pantry** button switches between pantries,
  such as the fridge, the freezer and a cupboard, and creates new ones.
- **Control centre.** The status button at the top right has the beep switch,
  the brightness and the battery level, and its house icon turns green when the
  tracker is connected to Home Assistant.

## Build it yourself

The firmware is one PlatformIO project for the CoreS3:

```sh
pio run -t upload
```

The server is `server/pantry_server.py`, standard-library Python plus Pillow,
with paho-mqtt for Home Assistant. Build its image with `server/Dockerfile`, or
run it straight on a Linux host with `server/pantry.service`.

| Path | What it is |
|---|---|
| `src/main.cpp` | the firmware, in one file |
| `lib/M5Module-QRCode/` | M5Stack's scanner driver (MIT), vendored with one fix described in `VENDORED.md` |
| `server/` | the server, the GS1 China lookup, the web page, the Dockerfile and the release bundle's templates in `server/docker/` |
| `docs/` | the scanner's setting codes, a demo barcode sheet and the Home Assistant guide |

Product names and photos come from [Open Food Facts](https://world.openfoodfacts.org/)
(data under the Open Database License, photos under CC BY-SA) and from GS1 China.
