# ietf-hardware-state netconfd module — Zigbee

Reports Zigbee sensor devices — e.g. **Aqara** climate/door/motion sensors and
**IKEA** (PARASOLL, VALLHORN, TIMMERFLOTTE, ...) sensors — as `/hardware`
components (RFC 8348 `ietf-hardware-state`) over NETCONF.

Each Zigbee device is one `ianahw:container` component, its readings are
`ianahw:sensor` child components:

| Component                | Class            | Contents                                                     |
|--------------------------|------------------|--------------------------------------------------------------|
| `<model>-<ieee>`         | ianahw:container | mfg-name, model-name, serial-num (IEEE address)              |
| `<model>-<ieee>-temperature` | ianahw:sensor | celsius, value-scale milli                                   |
| `<model>-<ieee>-humidity`    | ianahw:sensor | percent-RH, value-scale milli                                |
| `<model>-<ieee>-pressure`    | ianahw:sensor | other, hPa × 1000, value-scale milli                         |
| `<model>-<ieee>-battery`     | ianahw:sensor | other, % × 1000, value-scale milli                           |
| `<model>-<ieee>-occupancy` / `-contact` / `-water_leak` / `-smoke` | ianahw:sensor | other, 0/1 |

`<model>` is the first word of the Zigbee model identifier, `<ieee>` the last 4
hex digits of the IEEE address (e.g. `vallhorn-2b3c`, `lumi-weather-2c3d`).
Illuminance, CO2 and PM2.5 are reported the same way when a device has them.
A reading older than `ZIGBEE_MAX_AGE` (default 2 h) is reported with
`<oper-status>unavailable</oper-status>`.

## Architecture

```
 Aqara / IKEA Zigbee sensors
    |
    | Zigbee
    v
 Home Assistant Connect ZBT-2 USB dongle (factory Zigbee/EZSP firmware)
    |
    v
 Raspberry Pi (or any Linux host)
  +- ietf-hardware-state-zigbeed          the ONE long-running process:
  |     (zigpy + bellows, the ZHA stack)  owns the radio, receives reports,
  |                                       writes <state-dir>/state.json
  +- ietf-hardware-state-zigbee  <--popen--  netconfd (ietf-hardware-state module)
        reads state.json, stdlib only
```

No MQTT broker, no containers. Something must run continuously because Zigbee
battery sensors wake up, push their report and go back to sleep — that
something is the single `ietf-hardware-state-zigbeed` daemon. The netconfd
module (`ietf-hardware-state.c`) is unchanged from the other branches: it runs
`ietf-hardware-state-get` (symlink to the stateless helper) on every read of
`/hardware`.

## Dependencies

netconfd, yangcli and the yuma headers:

```sh
apt-get install netconfd yangcli libyuma-dev
```

For the daemon only (the helper is python3 stdlib):

```sh
python3 -m venv /opt/ietf-hardware-state-zigbee/venv
/opt/ietf-hardware-state-zigbee/venv/bin/pip install bellows zha-quirks
```

`zha-quirks` is optional but strongly recommended — without it many Aqara
devices report little or nothing.

### ZBT-2 firmware

The Zigbee (EmberZNet/EZSP) firmware is the ZBT-2 factory firmware, so a new
dongle works as is. If a dongle was previously flashed with the OpenThread RCP
firmware (see the hs-nabucasa branch), flash the Zigbee firmware back with
`universal-silabs-flasher` and the `zbt2_zigbee_ncp_*.gbl` from the
[silabs-firmware-builder releases](https://github.com/NabuCasa/silabs-firmware-builder/releases).

With a Thread ZBT-2 **and** a Zigbee ZBT-2 on the same host, `/dev/ttyACM0` is
ambiguous — always use the stable path
`/dev/serial/by-id/usb-Nabu_Casa_ZBT-2_<serial>-if00`.

## Installation

```sh
autoreconf -i -f
./configure CFLAGS="-g -O0"  CXXFLAGS="-g -O0" --prefix=/usr
make
make install
```

Start the daemon, either with the systemd unit
([`example/ietf-hardware-state-zigbeed.service`](example/ietf-hardware-state-zigbeed.service)):

```sh
sudo cp example/ietf-hardware-state-zigbeed.service /etc/systemd/system/
sudo systemctl enable --now ietf-hardware-state-zigbeed
```

or from `/etc/rc.local`:

```sh
ZIGBEE_DEVICE_PATH=/dev/serial/by-id/usb-Nabu_Casa_ZBT-2_XXXXXXXX-if00 \
  /opt/ietf-hardware-state-zigbee/venv/bin/python3 /usr/bin/ietf-hardware-state-zigbeed \
  1>/var/log/ietf-hardware-state-zigbeed.log 2>&1 &
```

The daemon forms a Zigbee network on first start and persists it (plus the
paired devices) in `<state-dir>/zigbee.db`.

## Pairing the sensors (once)

```sh
ietf-hardware-state-zigbee permit-join          # 254 s join window
# put each sensor in pairing mode (usually: hold its button ~5-10 s)
ietf-hardware-state-zigbee devices
```

```
$ ietf-hardware-state-zigbee devices
state file /var/lib/ietf-hardware-state-zigbee/state.json: updated 1s ago
00:15:8d:00:0a:1b:2c:3d    Aqara lumi.weather component=lumi-weather-2c3d last-seen=... properties=battery,humidity,pressure,temperature
84:27:12:ff:fe:1a:2b:3c    IKEA VALLHORN Wireless Motion Sensor component=vallhorn-2b3c last-seen=... properties=battery,illuminance,occupancy
```

Devices stay paired in `zigbee.db`; after that the module picks them up
automatically.

## Configuration

Daemon environment:

| Variable              | Default                              | Meaning                              |
|-----------------------|--------------------------------------|--------------------------------------|
| `ZIGBEE_DEVICE_PATH`  | `/dev/ttyACM0`                       | coordinator serial port (use `/dev/serial/by-id/...`) |
| `ZIGBEE_BAUDRATE`     | `460800`                             | ZBT-2 Zigbee firmware baud rate      |
| `ZIGBEE_FLOW_CONTROL` | `hardware`                           | `hardware`, `software` or empty      |
| `ZIGBEE_STATE_DIR`    | `/var/lib/ietf-hardware-state-zigbee`| state.json, zigbee.db, permit-join   |

Helper environment (netconfd):

| Variable                | Default   | Meaning                                                     |
|-------------------------|-----------|-------------------------------------------------------------|
| `ZIGBEE_STATE_DIR`      | as above  | must match the daemon                                       |
| `ZIGBEE_DEVICE`         | *(unset)* | report only this IEEE address, with bare component names (`temperature`, `co2`, ...) — one netconfd instance per device, as on the other branches |
| `ZIGBEE_MAX_AGE`        | `7200`    | seconds before a reading is reported unavailable            |
| `ZIGBEE_DAEMON_MAX_AGE` | `300`     | seconds without a state.json update before everything is reported unavailable |
| `IETF_HARDWARE_STATE_GET` | `ietf-hardware-state-get` | helper command run by the module            |

## Testing installation

```sh
netconfd --module=ietf-hardware-state --no-startup --superuser=$USER
```

Other terminal:

```
$ yangcli --server=localhost --user=$USER
yangcli> xget /hardware
```

## Testing without hardware

[`test/mock-zigbeed`](test/mock-zigbeed) writes the state.json the daemon
would write (an Aqara climate sensor, an IKEA motion sensor and an IKEA door
sensor with stale readings):

```sh
export ZIGBEE_STATE_DIR=/tmp/zigbee-test
test/mock-zigbeed
./ietf-hardware-state-zigbee devices
./ietf-hardware-state-get
```

With netconfd (module from the build tree, helper from the source tree):

```sh
PATH=$PWD:$PATH netconfd --module=ietf-hardware-state --runpath=$PWD/.libs --no-startup --superuser=$USER
```
