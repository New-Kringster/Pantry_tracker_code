# Pantry tracker server (Docker)

This folder runs the server for the M5Stack CoreS3 pantry expiry tracker on any
computer that stays on: a mini PC, a NAS, a Raspberry Pi 4 or 5 with a 64-bit
OS, or a Mac. The images are in `images/`, so nothing has to be downloaded from
Docker Hub.

| File | What it is |
|---|---|
| `images/pantry-server-*-amd64.tar.gz` | server image for Intel and AMD computers |
| `images/pantry-server-*-arm64.tar.gz` | server image for Apple Silicon Macs and Raspberry Pi |
| `images/mosquitto-2-*.tar.gz` | MQTT broker for Home Assistant |
| `docker-compose.yml` | starts both containers |
| `mosquitto.conf` | broker settings: no password, home network only |
| `install.sh` | loads the right images and starts everything |

## Install

You need Docker with the compose plugin. Docker Desktop on Mac and Windows has both.

On Linux or a Mac, open a terminal in this folder and run:

```sh
./install.sh
```

On Windows, or to do it by hand, load the two images that match your CPU and
start the containers:

```sh
docker load -i images/pantry-server-0.2.0-amd64.tar.gz
docker load -i images/mosquitto-2-amd64.tar.gz
docker compose up -d
```

Open `http://<this computer's IP>:8080/`. An empty pantry page means the server
is running. Enter the same address as the server URL on the tracker's setup page.

## Settings

Edit `docker-compose.yml` before the first start if you need to:

- `TZ` is your time zone. The tracker counts "days left" from local midnight.
- `HF_ENDPOINT` is commented out. Uncomment it where huggingface.co is blocked,
  such as mainland China, so voice naming can download its speech model.

Everything else is on the web page under **Settings**: the GS1 China account, a
different MQTT broker, the product-name language and the "expiring soon" days.

## Home Assistant

In Home Assistant, add the **MQTT** integration with this computer's IP address
as the broker and port 1883, no username or password. The pantry devices appear
by themselves.

## Update or remove

Load the newer images and run `docker compose up -d` again. Your items stay in
the `pantry-data` volume. `docker compose down` stops the server; add `-v` to
delete the stored items as well.
