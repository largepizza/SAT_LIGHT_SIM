---
pages: [rendering/aurora-airglow.md]
code: [shaders/cloud_march.comp]
---
## Now
The red airglow lies behind the clouds. Seen from under its 100-km shell, the red band is added to the cloud
composite's in-scattered light (`B_total`) times the composite's transmittance (`A_total`: cirrus, cloud and plumes),
and is not marched at all where that transmittance is under 1e-3. From above the shell (orbit) its march runs from the
eye to the top of the air, in front of every cloud, and is added unchanged. Green and sodium are added to the sky before
the cloud composite, so the clouds already covered them; the aurora takes the transmittance cubed.

## History
Until 2026-11-15 the red band had no cloud term: at dusk from 1.6 km a red gradient with a hard bottom edge (the
horizon, where its march stops on the ground) lay over an opaque rain cloud (issue_20261115_015559). Over the pixels
with cloud transmittance < 0.05 at that view, red airglow on vs off: 2.0 -> 0.13 levels; the clear sky's red share
is unchanged (0.72 vs 0.69 levels).
