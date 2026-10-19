# Online server

The server for the game's **Xbox LIVE** mode (Create Match, Quick Match, the
lobby and online races), packaged to run on a machine that does not play: a
home server, a VPS, or a free cloud VM. Friends anywhere on the Internet can
then race together without anyone opening ports at home.

It is the same server the game runs itself with **Host the server**
(`src/live_service.cpp`); this one is `live_server.py`, a single Python file
that needs nothing but Python 3.10 or later. How the online mode works:
[docs/xbox-live.md](../docs/xbox-live.md).

## What players do

On every device: **Launcher → General → Online play → Xbox LIVE**, then:

- leave **Host the server** off;
- **Server address**: the server's public address or name, for example
  `203.0.113.7` or `riders.example.com` (`:47800` may be left out);
- **Password**: the room password, when the server has one;
- **Online name**: a different name for each player.

**Test connection** says whether the server answers, and whether it needs a
password or the one entered is wrong. In the game, one player chooses
**Main Menu → Xbox LIVE → Create Match**; the others choose **Quick Match**.

**Latency:** every packet of a race goes through this server, not straight
between the players. Put the server in a region close to all the players: a
server far from everyone adds that distance to every packet.

## Run it with Docker

```bash
docker run -d --name free-riders-server --restart unless-stopped \
  -p 47800:47800/tcp -p 47800:47800/udp \
  -e SFR_LIVE_PASSWORD=choose-a-room-password \
  ghcr.io/yuutatsubasa/free-riders-server:latest
```

Publish **both TCP and UDP** port 47800. TCP carries the lobbies; UDP carries
the race. With TCP alone, players find each other's lobbies but the race
never starts. Use the image of the same release as the players' games
(`:0.10.11` and so on); `:latest` is the newest release.

With Docker Compose, [docker-compose.yml](docker-compose.yml) in this folder
does the same:

```bash
docker compose up -d
docker compose logs -f
```

To build the image from this folder instead: `docker build -t free-riders-server .`

Without Docker:

```bash
python3 server/live_server.py            # all addresses, port 47800
python3 server/live_server.py --port 47900
```

(`scripts/live_server.py` still works and runs this file.)

## Settings

All are environment variables, read when the server starts.

| Variable | Default | Meaning |
|---|---|---|
| `SFR_LIVE_PASSWORD` | empty | Room password. Empty: anyone who knows the address can join. |
| `SFR_LIVE_PASSWORD_FILE` | empty | Read the password from this file instead (Docker secrets). |
| `SFR_LIVE_PORT` | 47800 | TCP and UDP port. The games use 47800 unless the address says `:port`. |
| `SFR_LIVE_BIND` | 0.0.0.0 | Address to listen on. |
| `SFR_LIVE_UDP_BIND` | `SFR_LIVE_BIND` | Address of the UDP socket, when it must differ (Fly.io, below). |
| `SFR_LIVE_MAX_CONSOLES` | 64 | Games connected at once. One more gets `server full`. |
| `SFR_LIVE_MAX_SESSIONS` | 32 | Lobbies open at once. |
| `SFR_LIVE_SESSIONS_PER_CONSOLE` | 4 | Lobbies one game may hold open. |
| `SFR_LIVE_IDLE_TIMEOUT` | 900 | Seconds without a request or a datagram before a game is dropped (0: never). Games send a keep-alive every 10 s, so only games that vanished are dropped. |
| `SFR_LIVE_HELLO_TIMEOUT` | 30 | Seconds a connection may stay without joining. |
| `SFR_LIVE_MAX_LINE` | 16384 | Longest request line, in bytes; a longer one closes the connection. |
| `SFR_LIVE_MAX_REQUESTS` | 100 | Requests per second on one connection (bursts of twice that); more closes it. |
| `SFR_LIVE_LOG_LEVEL` | info | `debug`, `info`, `warning` or `error`. |

The log shows games joining and leaving (name, virtual address, where from),
lobbies opening and closing, and refusals. It never shows the password.

### About the password

The password keeps strangers out of the room; it is not account security.
Games send it as plain hex over TCP, without encryption, so anyone who can
watch the traffic between a player and the server can read it. Do not reuse
a password used anywhere else. A wrong password closes the connection.

Games of releases before the password cannot send one: they get
`ERR password required` from a server with a password (their log says
`LIVE_SERVER unreachable=... reason=unexpected`). Against a server without a
password, games old and new play together as before.

The image's health check asks `PING`, which the server answers without the
password (the launcher's **Test connection** does the same first).

## Deploying

The server needs very little: one CPU core and 64 MB of memory are plenty for
a few lobbies. What matters is where it is (see Latency above) and that
**TCP and UDP port 47800** reach it.

### A VPS or any Linux machine

1. Install Docker (for example `curl -fsSL https://get.docker.com | sh` on
   Debian or Ubuntu, or your distribution's package).
2. Run the `docker run` command above, or copy `docker-compose.yml` there and
   run `docker compose up -d`.
3. Open TCP and UDP port 47800 in the provider's firewall (the control panel's
   "firewall", "security group" or similar) and in the machine's own firewall
   if it has one, for example `sudo ufw allow 47800/tcp` and
   `sudo ufw allow 47800/udp`.
4. Check from a player's PC: Launcher → Online play → Server address →
   **Test connection** (TCP only), and the race's UDP path with this
   repository's script:

   ```bash
   SFR_LIVE_PASSWORD=the-room-password python3 server/live_server.py --check-relay <address>
   ```

   It joins as two test consoles for a moment and sends a datagram from one
   to the other through the server. "OK" means TCP and UDP both work; "no
   datagram came back" means UDP 47800 is blocked on the way.

### Oracle Cloud free VM

Oracle Cloud's Always Free tier includes Ampere (ARM64) and small x86 VMs;
the image runs on both.

1. Create an instance with Ubuntu, and note its public IP address.
2. **Open the port in the virtual network:** Networking → Virtual cloud
   networks → the instance's VCN → Security Lists → Default Security List →
   **Add Ingress Rules**, twice: source `0.0.0.0/0`, IP protocol **TCP**,
   destination port `47800`; and the same with IP protocol **UDP**.
3. **Open it on the instance too.** Oracle's Ubuntu images come with
   iptables rules that reject everything but SSH. Over SSH:

   ```bash
   sudo iptables -I INPUT 6 -m state --state NEW -p tcp --dport 47800 -j ACCEPT
   sudo iptables -I INPUT 6 -p udp --dport 47800 -j ACCEPT
   sudo netfilter-persistent save
   ```

   (Position 6 keeps the rules above the image's final REJECT; check with
   `sudo iptables -L INPUT --line-numbers`.)
4. Install Docker and start the server as for a VPS above. If players still
   get no answer while `--check 127.0.0.1` on the VM works, run the container
   with the host's network instead (`--network host` in place of the two
   `-p` options, or `network_mode: host` in the compose file).

### Fly.io

Fly.io can run the image, with two requirements of its UDP support (from
[Fly's UDP and TCP guide](https://fly.io/docs/networking/udp-and-tcp/)):

- UDP needs a **dedicated IPv4 address** (`fly ips allocate-v4`, billed
  monthly); a shared IPv4 or IPv6 address carries TCP only.
- The UDP socket must listen on the address `fly-global-services`, and the
  port must be the same inside and outside. TCP must not listen there, so set
  `SFR_LIVE_UDP_BIND=fly-global-services` and leave `SFR_LIVE_BIND` alone.

A `fly.toml` for it:

```toml
app = "my-free-riders-server"
primary_region = "nrt"   # a region near the players

[build]
  image = "ghcr.io/yuutatsubasa/free-riders-server:latest"

[env]
  SFR_LIVE_UDP_BIND = "fly-global-services"

[[services]]
  internal_port = 47800
  protocol = "udp"
  [[services.ports]]
    port = 47800

[[services]]
  internal_port = 47800
  protocol = "tcp"
  [[services.ports]]
    port = 47800
```

```bash
fly launch --no-deploy --copy-config
fly ips allocate-v4
fly secrets set SFR_LIVE_PASSWORD=choose-a-room-password
fly deploy
fly ips list    # the dedicated IPv4 is the Server address
```

Keep the app at one machine (`fly scale count 1`): every game must reach the
same server. This setup follows Fly's documentation but has not been tested
on Fly.io itself.

## What is checked

`tests/test_live_server.py` runs the server on the loopback: PING, the
password (accepted, refused, never logged), the limits, idle connections, and
datagrams relayed only between registered games. The game's own server and
client have the same checks in `tests/live_connect_test.cpp`. The workflow
`.github/workflows/server-image.yml` builds the image for linux/amd64 and
linux/arm64, and checks that a container answers PING and relays a datagram
over UDP (`--check-relay`) before anything is pushed.
