# ietf-hardware-state netconfd module — Matter over Thread

Reports Matter sensor devices, e.g. two **IKEA ALPSTUGA** air quality monitors, as
`/hardware` components (RFC 8348 `ietf-hardware-state`) over NETCONF.

Each Matter node is one `ianahw:container` component, its measurements are
`ianahw:sensor` child components:

| Component                       | Class            | Contents                                                         |
|---------------------------------|------------------|------------------------------------------------------------------|
| `<product>-<node-id>`           | ianahw:container | mfg-name, model-name, serial-num, hardware-rev, software-rev     |
| `<product>-<node-id>-temperature` | ianahw:sensor  | celsius, value-scale milli                                       |
| `<product>-<node-id>-humidity`  | ianahw:sensor    | percent-RH, value-scale milli                                    |
| `<product>-<node-id>-co2`       | ianahw:sensor    | other, ppm × 1000, value-scale milli                             |
| `<product>-<node-id>-pm25`      | ianahw:sensor    | other, µg/m³ × 1000, value-scale milli                           |
| `<product>-<node-id>-air-quality` | ianahw:sensor  | other, Matter AirQualityEnum (1 = good … 6 = extremely poor)     |

Any other Matter measurement cluster a device has (PM1, PM10, TVOC, CO, NO2, ozone,
formaldehyde, radon, battery) is reported the same way. If a node is offline its
sensors are reported with `<oper-status>unavailable</oper-status>`.

## Architecture

```
 IKEA ALPSTUGA  x2
    |
    | Matter over Thread
    v
 Home Assistant Connect ZBT-2 USB dongle (Thread RCP firmware)
    |
    v
 Raspberry Pi (or any Linux host)
  +- OpenThread Border Router (OTBR)        REST API  :8081
  +- Matter Server (python-matter-server)   WebSocket :5580
  +- ietf-hardware-state-matter  <--popen--  netconfd (ietf-hardware-state module)
```

The netconfd module itself (`ietf-hardware-state.c`) only runs the helper
`ietf-hardware-state-get` (a symlink to the Python script `ietf-hardware-state-matter`)
when `/hardware` is retrieved. The helper reads the nodes from the
[Open Home Foundation Matter Server](https://github.com/home-assistant-libs/python-matter-server)
(the Matter controller used by Home Assistant / Nabu Casa) over its WebSocket API.
The Matter Server talks to the Thread devices through an OpenThread Border Router.

The helper uses only the Python standard library (own minimal WebSocket client),
so it runs unchanged on any host with `python3`.

## Dependencies

netconfd, yangcli and the yuma headers:

```sh
apt-get install netconfd yangcli libyuma-dev
```

`python3` (standard library only).

OTBR and Matter Server, see [`example/docker-compose.yml`](example/docker-compose.yml):

```sh
cd example
ZBT2_DEVICE=/dev/ttyACM0 INFRA_IF=eth0 docker compose up -d
```

### ZBT-2 firmware

The Home Assistant Connect ZBT-2 ships with **Zigbee** (EmberZNet/EZSP) firmware.
For Thread it must run the **OpenThread RCP** firmware from the Nabu Casa
[silabs-firmware-builder releases](https://github.com/NabuCasa/silabs-firmware-builder/releases)
(otherwise `otbr-agent` fails with `Init() at spinel_driver.cpp: Failure`).
Stop the `otbr` container while flashing:

```sh
python3 -m venv /tmp/usf && /tmp/usf/bin/pip install universal-silabs-flasher
/tmp/usf/bin/universal-silabs-flasher --device /dev/ttyACM0 probe
#   ... Detected ApplicationType.EZSP ...            -> Zigbee firmware, flash:
curl -LO https://github.com/NabuCasa/silabs-firmware-builder/releases/download/v2026.02.23/zbt2_openthread_rcp_2.7.2.0_GitHub-fb0446f53_gsdk_2025.6.2.gbl
/tmp/usf/bin/universal-silabs-flasher --device /dev/ttyACM0 flash --firmware zbt2_openthread_rcp_2.7.2.0_GitHub-fb0446f53_gsdk_2025.6.2.gbl
/tmp/usf/bin/universal-silabs-flasher --device /dev/ttyACM0 probe
#   ... Detected ApplicationType.SPINEL, version 'SL-OPENTHREAD/2.7.2.0...' at 460800 baudrate
```

The same release contains `zbt2_zigbee_ncp_*.gbl` to flash back.

## Installation

```sh
autoreconf -i -f
./configure CFLAGS="-g -O0"  CXXFLAGS="-g -O0" --prefix=/usr
make
make install
```

## Forming the Thread network (once)

```sh
docker compose exec otbr ot-ctl dataset init new
docker compose exec otbr ot-ctl dataset commit active
docker compose exec otbr ot-ctl ifconfig up
docker compose exec otbr ot-ctl thread start
docker compose exec otbr ot-ctl state        # -> leader after a few seconds
```

## Commissioning the sensors (once)

Use the Matter pairing code of each ALPSTUGA: the `MT:...` QR payload or the
11 digit manual code printed on the underside (`0488-961-3535` → `04889613535`).
Power the sensor up; a new device advertises for commissioning over Bluetooth LE.
If it was paired before (DIRIGERA, Apple Home, ...) either remove it there or
factory reset it (see the ALPSTUGA manual). The helper hands the Thread network
credentials (fetched from the OTBR REST API) to the Matter Server and then
commissions over BLE — keep the sensor within a few meters of the host during pairing:

```
$ ietf-hardware-state-matter commission 04889613535 MT:Y.K9042C00KA0648G01
Commissioning 04889613535 ...
  commissioned as node 1 (IKEA of Sweden ALPSTUGA air quality monitor), component alpstuga-1
...

$ ietf-hardware-state-matter nodes
Matter Server ws://localhost:5580/ws: fabric 1, sdk 2025.7.0, thread credentials set: True, bluetooth: True
node 1    online     IKEA of Sweden ALPSTUGA air quality monitor  serial=? component=alpstuga-1 clusters=0x005B,0x0402,0x0405,0x040D,0x042A
node 2    online     IKEA of Sweden ALPSTUGA air quality monitor  serial=? component=alpstuga-2 clusters=0x005B,0x0402,0x0405,0x040D,0x042A
```

The devices stay commissioned in the Matter Server storage, after that the
module picks them up automatically (every node with a measurement cluster).

ALPSTUGA (hw P2.0, sw 1.0.15) exposes TemperatureMeasurement, RelativeHumidity,
CO2 (ppm), PM2.5 (µg/m³) and AirQuality; it has no SerialNumber so the Matter
UniqueID is reported as `serial-num`.

## Configuration

Environment of netconfd / the helper:

| Variable                  | Default                   | Meaning                                                |
|---------------------------|---------------------------|--------------------------------------------------------|
| `MATTER_SERVER_URL`       | `ws://localhost:5580/ws`  | Matter Server WebSocket URL                            |
| `MATTER_NODE_ID`          | *(unset)*                 | report only this node, see [one instance per device](#one-netconfd-instance-per-device) |
| `MATTER_TIMEOUT`          | `5`                       | seconds, WebSocket request timeout                     |
| `OTBR_REST_URL`           | `http://localhost:8081`   | used by `commission` to fetch the Thread dataset       |
| `THREAD_DATASET`          | *(unset)*                 | hex dataset, overrides `OTBR_REST_URL`                 |
| `IETF_HARDWARE_STATE_GET` | `ietf-hardware-state-get` | helper command run by the module                       |

Helper subcommands: `get` (default, used by netconfd), `nodes`, `commission <code>...`,
`thread-dataset [<hex>]`, `--help`.

## Testing installation

```sh
netconfd --module=ietf-hardware-state --no-startup --superuser=$USER
```

Other terminal:

```
$ yangcli --server=localhost --user=$USER

yangcli pi@localhost> xget /hardware
...
  data {
    hardware {
      last-change 2026-08-19T10:50:17Z
      component alpstuga-1 {
        name alpstuga-1
        class ianahw:container
        description 'Matter node 1'
        hardware-rev P2.0
        software-rev 1.0.15
        serial-num 08a3987c8e77da4fc0e521a70a6a19d0
        mfg-name 'IKEA of Sweden'
        model-name 'ALPSTUGA air quality monitor'
      }
      component alpstuga-1-temperature {
        name alpstuga-1-temperature
        class ianahw:sensor
        parent alpstuga-1
        sensor-data {
          value 23070
          value-type celsius
          value-scale milli
          value-precision 2
          oper-status ok
          units-display 'milli degrees'
          value-timestamp 2026-08-19T10:50:26Z
          value-update-rate 0
        }
      }
      component alpstuga-1-humidity {
        ...
      }
      component alpstuga-1-co2 {
        ...
      }
      component alpstuga-1-pm25 {
        ...
      }
      component alpstuga-1-air-quality {
        ...
      }
      component alpstuga-2 {
        ...
      }
      ...
    }
  }
```

## One netconfd instance per device

By default one netconfd reports all sensors with `<product>-<node-id>-` prefixed
component names. To expose each ALPSTUGA as its own NETCONF server set
`MATTER_NODE_ID` and start one instance per device on different ports (as with
the other lsi modules). In this mode the components get the bare names used by
the other ietf-hardware-state implementations — `alpstuga` (container),
`temperature`, `humidity`, `co2`, `pm25`, `air-quality` (and `pm1`, `pm10`,
`voc`, ... if the device has them) — so dashboards keyed on
`<node-id>.<component-name>` understand them:

```sh
MATTER_NODE_ID=2 netconfd --module=ietf-hardware-state --no-startup --superuser=$USER
MATTER_NODE_ID=3 netconfd --module=ietf-hardware-state --no-startup \
   --ncxserver-sockname=/tmp/ncxserver-10830.sock --port=10830 --superuser=$USER
```

with the corresponding ports enabled in `/etc/ssh/sshd_config`:

```
Port 830
Port 10830
Subsystem netconf "/usr/sbin/netconf-subsystem --ncxserver-sockname=830@/tmp/ncxserver.sock --ncxserver-sockname=10830@/tmp/ncxserver-10830.sock"
```

## Testing without hardware

[`test/mock-matter-server`](test/mock-matter-server) emulates the Matter Server with
two ALPSTUGA nodes (node 1 online, node 2 offline) and accepts commissioning:

```sh
test/mock-matter-server 5580 &
./ietf-hardware-state-matter nodes
./ietf-hardware-state-get
./ietf-hardware-state-matter commission MT:TEST
```

With netconfd (module from the build tree, helper from the source tree):

```sh
PATH=$PWD:$PATH netconfd --module=ietf-hardware-state --runpath=$PWD/.libs --no-startup --superuser=$USER
```
