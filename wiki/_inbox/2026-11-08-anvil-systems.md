# Anvils carried by the organised storm systems; one envelope per storm; a continuous plate

Date: 2026-11-08 (branch weather-salvage). Pages: storms / weather objects, anvils, settings.

- What changed: single cells spread an anvil shield only as supercells ("Single-storm anvils", default 1; 0 none, 2 all).
  Squall lines and MCSs carry the anvils: at most one per 6-degree cell and 3-h bin, none for "Storm complex spacing" bins
  (default 2) after one; their rate no longer takes "Storm frequency"; over tropical seas they are more often MCS clusters.
- A storm's shield forms ONCE (first tower at the lid, over min(formation time, 0.7 x its fed span)), holds while fed,
  lingers after. Before, each tower restarted its own ramp, so long formation times left anvils faint and fading.
- The anvil plate drawn through the cumulus field no longer has holes: its lobes only bulge and the lid / underside lobe
  settings apply to the whole plate (they were scaled by "Anvil fibres"). Lid lobes default 0.25 -> 0.1.
- Numbers: shields worldwide at the user's settings (US Plains, July 21Z) 927 -> 115; a supercell anvil forms over ~75 min
  and is still at 0.7 strength 3 h later.
