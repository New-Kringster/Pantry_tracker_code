#!/bin/sh
# Load the bundled images for this computer's CPU and start the pantry server.
#   ./install.sh
# Needs Docker with the compose plugin (Docker Desktop on Mac and Windows has both).
set -e
cd "$(dirname "$0")"
case "$(uname -m)" in
  x86_64|amd64) ARCH=amd64 ;;
  aarch64|arm64) ARCH=arm64 ;;
  *) echo "Unsupported CPU $(uname -m); the bundle has amd64 and arm64 images." >&2; exit 1 ;;
esac
for f in images/*-"$ARCH".tar.gz; do
  echo "Loading $f"
  docker load -i "$f"
done
PANTRY_VERSION=$(cat VERSION) docker compose up -d
echo
echo "Pantry server is starting. Open http://<this computer's IP>:8080/ in a browser,"
echo "then enter that address as the server URL on the tracker's setup page."
