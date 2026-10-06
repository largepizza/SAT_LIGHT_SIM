---
pages: [modding/constellations.md, simulation/orbits.md]
code: [data/constellations.json, data/constellations.schema.json, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimAmbience.cpp]
---
## Now
SpaceX's Starmind orbital data centers fly the shells of SpaceX's 29 May 2026 letter to the FCC (Table 1,
SAT-LOA-20260108-00016), as two roster entries. "SpaceX Starmind - sun-synchronous X-ring": SSO groups at
565-585 km (10 shells), 707-744 km (22) and 967-1002 km (22), two planes per shell at LTAN 18:00 and 06:00, so
the two terminator rings cross at the equator. "SpaceX Starmind - 30 deg shells": Walker groups at 550-568 km
(10 shells, 26-32 deg), 686-718 km (25) and 946-978 km (25, both 30 deg), 30 planes per shell. The filed rows
are per-group maxima summing to 1,198,120 against a 1,000,000 cap, so satellites per plane are scaled by
1e6 / 1,198,120 and rounded down to whole clusters (988,672 flown). Satellites fly in formation rings of 8: a
regular octagon of 200 m radius in the orbit plane (along track x radial), each member at the centre's radius +
its radial offset but at the centre's mean motion, so the ring holds its shape (station-kept, not a free formation). The X-ring's planes spread their nodes
over +-10 deg of the terminator node (`raan_spread_deg`, `raan_spread` "even" golden-ratio | "random"; SpaceX filed
+-30 deg), so each shell flies its own plane: with tight nodes, shells 1.6-1.8 km apart carrying 2-km rings shared
one plane and slid through each other (13,000 cross-cluster pairs within 1 km at any instant; +-10 deg even: 26,
+-30 deg even: 12, +-30 random: 25; the 30 deg shells: 30).

New `constellations.json` fields: `"distribution": "Shells"` with a `groups` array (`alt_min_km`, `alt_max_km`,
`incl_deg` or `incl_min_deg`/`incl_max_deg`, `sun_synchronous`, `shells`, `planes_per_shell`, `per_plane`;
alt_km / num_planes / per_plane are derived), and `cluster_size` (Walker, Disk, Shells; default 1 = no clusters),
`cluster_shape` ("line": `cluster_spacing_km` apart along track, default 1; "ring": `cluster_radius_km`, default 0.2). A filed table goes in one entry because the GPU enable/highlight masks are 32 bits
(the roster is at 29 entries).

## History
Replaced a single 1M-satellite dawn-dusk Disk (575-1925 km, 2000 rings, one terminator plane), which predated
the FCC filings. the ring of 8 follows SpaceX's visualization (its text says
"10-ish satellites" per cluster). The first cut flew lines of 10, 1 km apart. Harness checks: tools/harness/scripts/starmind_orbits.satcmd, starmind_ring.satcmd.
