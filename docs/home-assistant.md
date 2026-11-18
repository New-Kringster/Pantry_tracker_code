# Pantry tracker in Home Assistant

The server (`server/pantry_server.py`) talks to the mosquitto broker on the VM
and announces everything with MQTT discovery, so nothing is configured by hand
in Home Assistant. Entities appear under **Settings → Devices → MQTT**.

## Devices and entities

**Pantry Tracker** (the CoreS3)

| Entity | What it is |
|---|---|
| `sensor.pantry_tracker_pantry_items` | items in every pantry; attributes `items[]`, `pantries[]` |
| `sensor.pantry_tracker_pantry_expired` | count; attribute `items` (names) |
| `sensor.pantry_tracker_pantry_expiring_soon` | count within 3 days; attributes `items`, `threshold_days` |
| `sensor.pantry_tracker_pantry_next_expiry` | name of the next item to expire; attributes `expires`, `days_left`, `barcode`, `pantry` |
| `sensor.pantry_tracker_pantry_next_expiry_date` | the same as a date (device class `date`, so HA renders "in 3 days") |
| `sensor.pantry_tracker_pantry_expiring_list` | text: `Nutella (3 d), 农夫山泉 (today)` for notifications |
| `sensor.pantry_tracker_pantry_tracker_battery` | CoreS3 battery % |
| `binary_sensor.pantry_tracker_pantry_tracker_charging` | on USB power |
| `sensor.pantry_tracker_pantry_tracker_wifi_signal` | RSSI dBm (diagnostic) |
| `sensor.pantry_tracker_pantry_tracker_ip` | device IP (diagnostic) |
| `sensor.pantry_tracker_pantry_tracker_last_scan` | last barcode scanned, name; attributes `barcode`, `at` |
| `sensor.pantry_tracker_pantry_tracker_last_seen` | timestamp of the last telemetry (device posts every 60 s on HOME) |
| `sensor.pantry_tracker_pantry_tracker_firmware` | git revision the device runs |
| `sensor.pantry_tracker_pantry_server_version` / `..._gs1_china` / `..._speech_recognition` | server diagnostics |
| `event.pantry_tracker_pantry_tracker_event` | fires with `event_type` `scanned`, `added`, `removed`, `renamed`, `pantry_created`, `pantry_deleted` |

**Pantry: &lt;name&gt;** (one device per pantry, linked to the tracker via `via_device`)

`sensor.pantry_<name>_items`, `_expired`, `_expiring_soon`, `_next_expiry`,
`_next_expiry_date`. Created when a pantry is created, removed when it is
deleted (the retained discovery message is cleared).

Entity ids follow HA's `device name + entity name` rule; check the exact ids in
the MQTT device page if you renamed anything.

## Topics (for other consumers)

| Topic | Retained | Payload |
|---|---|---|
| `pantry/availability` | yes | `online` / `offline` (last will) |
| `pantry/state` | yes | totals, `next_*`, `expiring_text`, `items[]`, `pantries[]` |
| `pantry/<pantry id>/state` | yes | the same per pantry |
| `pantry/device` | yes | `battery`, `charging`, `rssi`, `ip`, `firmware`, `uptime_s`, `last_scan*`, `last_seen` |
| `pantry/diag` | yes | `version`, `gs1`, `stt` |
| `pantry/event` | no | `{"event_type": "...", ...}` |

## Dashboard card

A Markdown card that lists what is tracked, worst first:

```yaml
type: markdown
title: Pantry
content: >-
  {% set items = state_attr('sensor.pantry_tracker_pantry_items', 'items') or [] %}
  {% if not items %}Nothing tracked.{% endif %}
  {% for i in items %}
  {% set d = i.days_left %}
  {{ '🔴' if d < 0 else '🟠' if d <= 3 else '🟡' if d <= 7 else '🟢' }}
  **{{ i.name }}** · {{ i.pantry }} ·
  {{ 'expired' if d < 0 else 'today' if d == 0 else d ~ ' d' }}
  {% endfor %}
```

Tile cards work well for the counts, and the `date` sensor shows a relative
time by itself.

## Automations

Morning digest when something is expiring:

```yaml
alias: Pantry digest
trigger:
  - platform: time
    at: "08:00:00"
condition:
  - condition: numeric_state
    entity_id: sensor.pantry_tracker_pantry_expiring_soon
    above: 0
action:
  - service: notify.mobile_app_your_phone
    data:
      title: Pantry
      message: "Expiring: {{ states('sensor.pantry_tracker_pantry_expiring_list') }}"
```

React to a scan the moment it happens (the event entity's state is the timestamp of
the last event; the trigger below filters on the type):

```yaml
alias: Pantry scan announce
trigger:
  - platform: state
    entity_id: event.pantry_tracker_pantry_tracker_event
condition:
  - condition: template
    value_template: "{{ trigger.to_state.attributes.event_type == 'added' }}"
action:
  - service: tts.speak
    target:
      entity_id: tts.home_assistant_cloud
    data:
      media_player_entity_id: media_player.kitchen
      message: "Added {{ trigger.to_state.attributes.name }}, expires in {{ trigger.to_state.attributes.days_left }} days"
```

Low battery on the fridge:

```yaml
alias: Pantry tracker battery
trigger:
  - platform: numeric_state
    entity_id: sensor.pantry_tracker_pantry_tracker_battery
    below: 15
    for: "00:10:00"
action:
  - service: notify.mobile_app_your_phone
    data:
      message: "The pantry tracker is at {{ states('sensor.pantry_tracker_pantry_tracker_battery') }} %"
```

## Device telemetry

The CoreS3 posts `POST /api/device` every 60 s while on HOME and once per scan
(`event: scanned`). The server merges it into `pantry/device` and stamps
`last_seen`. If the device is off, `last_seen` goes stale; the summary sensors
keep working because the server, not the device, owns the item list.
