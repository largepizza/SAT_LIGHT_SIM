---
pages: [rendering/atmosphere-and-sky.md, "new: rendering/rocket-exhaust.md"]
code: [src/simulations/SatelliteSimLaunches.cpp, shaders/cloud_march.comp]
---
## Now
A burning stage's young exhaust (its first 10 s of age) is drawn as Gaussian pieces between knots at FIXED ages behind
the nozzle (0, 0.02, 0.05, 0.1, 0.2, 0.35, 0.5, 0.75, 1, 1.5, 2, 3, 4, 5.5, 7.5, 10 s), each knot interpolated between
the two exhaust parcels that bracket it in emission time, with the parcels' state read at the knot's age: the young
cone looks the same at every moment of the 1-1.5-s parcel emission, so it no longer pulses. Each piece carries the
exhaust's mass rate over its age span. The shader cuts a burning stage's exhaust Gaussians at the nozzle's plane
(closed form) and lights them at the centroid of what is left: no exhaust glow over the hull. A plume whose build
finishes while its launch is already flying eases in over ~0.75 s, and a rebuild for a changed "Plume growth" or
dispersal keeps the old plume drawn until the new one is ready.

## History
2026-11-15, issue_20261115_164337 "Plume particles". Before: a new bead grew out of the nozzle every parcel interval
(a 0.67-Hz pulse, 3.8x the noise floor of the frame's light at a 64-km view; now 0.7x), the engine light lit the
Gaussians' forward tails into an orange bell round the hull at night (plume light above the nozzle 3.21 -> 0.25
levels), and plumes appeared whole a few frames after a jump or report load (35-163% one-frame steps; now <= 3.8%).
Found on the way: hard switches in the shell rim (height, radius, tube vs ring) and in the ozone tint (a grazing Sun
ray), each stepping a whole piece in one frame; both are eased now. At night the engine-lit exhaust just under a
stage is ~50% brighter than before (it now sits where the engine light is).
