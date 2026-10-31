---
pages: [simulation/kessler.md]
code: [src/simulations/SatelliteSimKessler.cpp, src/simulations/SatelliteSim.h]
---
## Now
A pool record's fragments are drawn only from the moment of their own event, not from their birth *second*. The GPU
gates a pool record by whole seconds (`sat_orbit.comp`: `scenNowSec < bornSec || scenNowSec >= deathSec`), so a piece
born at te = 2702.142 s used to be drawn from 2702.0 s and for that fraction of a second every fragment was evaluated
at t < te: all of them converging on the impact point ("they turn into debris and implode before the collision",
issue_20261030_180204).

The mirror now keeps a sub-second birth gate. `kFirstBounds` indexes a slot's first record (which `kGenBounds`, made
for reused slots, cannot) by its birth second; at that second `kesslerShowGen` compares the record's own event time —
`kEvents.tOf(rec.event)`, the worker's event log, exact to the step — with the continuous sim time. A reused slot
shows its previous generation until the new one's event time. A slot with no generation yet is hidden in the GPU
record (`bornSec = kNever`, so the shader's born gate never opens) and listed in `kPoolUnborn`; `kesslerShowUnborn`
shows it on the first frame at or after its event time. Records that arrive ahead of the player (the worker publishes
minutes to days ahead) are gated when their second is entered, not when they arrive.

## History
The previous fix (2026-10-30, d62306d) drew the pair collision up close: a projectile stand-in for the last 45 s, a
flash and fireball at te, and the resolved-dust surface brightness. It left the whole-second born gate: at te-0.126 s
and te-0.017 s the structural pieces (the parents' array sections) were already visible, spread around the impact
point. With the gate they appear at te, with the flash and the first dust.

Still whole-second: a roster satellite's END word (and a pool target's death second) hides it up to 1 s before the
collision, so the target itself is not drawn in the last frames before the impact (the flash covers the contact).
