# Droneships and landing ships (2026-10-20, branch kessler)

Pages: launches / rockets (simulation), launch sites; harness reference.

- Each launch now chooses its booster's recovery from its payload mass (launches.json: program `sat_mass_kg` or
  `payload_mass_kg`; vehicle `rtls_max_kg`): Falcon 9 flies back to its pad's landing zone with a light payload
  (<= 7 t: OneWeb, Reflect Orbital), else lands on a droneship ~600 km downrange (Crew Dragon 550 km). New Glenn lands
  on its landing ship Jacklyn ~1000 km downrange. Chinese reusable boosters land on a recovery barge from Hainan and on
  the ground downrange from inland Jiuquan.
- The ships (A Shortfall of Gravitas / Just Read the Instructions off Florida, Of Course I Still Love You off
  Vandenberg, Jacklyn) are drawn as meshes on the sea from 8 h before the launch; the booster lands on the deck, and
  1.5 h later the ship steams home at ~8 knots with the booster aboard, drawn until 12 h after the landing. At night
  they carry floodlights, beacons and navigation lights and their deck is floodlit.
- The landing burn of every recovered booster (towers included) is now a constant deceleration that brings it to rest
  exactly on its target; boosters from far downrange still lean noticeably in the burn.
- Ship models: `tools/make_ship_models.py` -> `data/satellite_models/ship_droneship.json`, `ship_jacklyn.json`.
- Harness: `launch droneship`, `launch ships`, `launch state` boosters / ships, new `launch check` fields;
  `tools/harness/scripts/droneship.satcmd`.
- Falcon 9 / Crew Dragon / New Glenn stage faster than before (MECO ~2.2 / 2.2 / 3.3 km/s) so their boosters reach
  the ships: their ascents and upper-stage tracks changed slightly.
