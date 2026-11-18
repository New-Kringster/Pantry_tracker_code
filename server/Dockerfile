# Pantry tracker server: REST API and web page, product lookups (Open Food Facts,
# GS1 China), speech to text for voice naming (faster-whisper) and MQTT for Home
# Assistant. Everything it stores lives in /data.
#   docker build -t pantry-server .
# The release bundles carry this image built for amd64 and arm64.
FROM python:3.12-slim
ARG VERSION=dev
LABEL org.opencontainers.image.title="pantry-server" \
      org.opencontainers.image.description="Server for the M5Stack CoreS3 pantry expiry tracker" \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.source="https://github.com/New-Kringster/Pantry_tracker_code"
RUN pip install --no-cache-dir pillow==13.0.0 paho-mqtt==2.2.0 faster-whisper==1.2.1
WORKDIR /app
COPY pantry_server.py gs1_china.py index.html /app/
VOLUME ["/data"]
# The Whisper model downloads into /data/hf on first use, so it survives upgrades.
ENV HF_HOME=/data/hf PYTHONUNBUFFERED=1
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s \
  CMD python3 -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8080/api/health', timeout=4)"
CMD ["python3", "pantry_server.py", "--port", "8080", "--bind", "0.0.0.0", "--data", "/data"]
