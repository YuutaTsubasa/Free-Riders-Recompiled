# Free Riders Recompiled's online server (server/live_server.py) as a small
# image. Build from this folder:  docker build -t free-riders-server server
FROM python:3.12-alpine

LABEL org.opencontainers.image.title="Free Riders Recompiled online server" \
      org.opencontainers.image.description="Lobbies and the relay for Free Riders Recompiled's online play (Xbox LIVE mode)" \
      org.opencontainers.image.source="https://github.com/YuutaTsubasa/Free-Riders-Recompiled" \
      org.opencontainers.image.licenses="GPL-3.0-or-later"

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    SFR_LIVE_PORT=47800

# The server needs nothing but Python's standard library.
RUN adduser -D -H -u 10001 rider
WORKDIR /app
COPY live_server.py /app/live_server.py

USER rider
EXPOSE 47800/tcp 47800/udp

# PING over TCP, which a server answers without the room password.
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
    CMD python /app/live_server.py --check "127.0.0.1:${SFR_LIVE_PORT}" || exit 1

ENTRYPOINT ["python", "/app/live_server.py"]
