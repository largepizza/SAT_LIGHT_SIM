# Storm review checklist

For the reviewer of a `storm_review.py` run. You were not told what changed. Judge the sheets against these items,
each drawn from a QA report the user filed. For each finding, name the sheet, the cell (e.g. `supercell_ring.png`
m135), the item number, and what you see. Being too strict is better than too lenient: everything below has
passed a settled-stills check before and was still wrong.

## Shape
1. **No straight edges.** Look for straight sides, square or box columns, prisms, parallelograms, polygon
   outlines and texel squares. Nothing in a cloud is ruled straight. (Cloud shapes: "sharp edges and corners on
   columns". The map overlay showed parallelogram slabs.)
2. **No cliffs.** A cloud's edge tapers and feathers into the cloud beside it. Look for vertical walls,
   height steps, or a deck that ends in a face rather than thinning out. (Hurricane shape: "discontinuity on
   hurricane wall edge", "aggressive cloud cutting". Cloud base edge contour.)
3. **No rings or circles drawn by a radius.** Look for a brightness or shading ring, a phase boundary, a disc rim,
   or a circular cut-out. (Hurricane shape: "the phase boundary as a sharp ring". Droplet phase range.)
4. **Columns:** wide at the base and tapering upward, billowy and organic throughout. They join the anvil through
   billows, never through a pyramid, cone or funnel. (Cloud shapes: "connection to anvil is a pyramid shape".
   Anvil shoot-through mushroom.)
5. **Anvils:** a flat top under the tropopause, spreading downwind, with thin fibrous edges. No disc with a hard
   rim, no mushroom cap, no repeated pattern. (Anvil formation. Hurricanes need serious work: "pattern looks
   bad and repetitive".)
6. **Hurricanes:**
   - a solid central overcast;
   - spiral bands that merge into it;
   - a clear eye;
   - wall clouds that extend and blend into the cumulus below;
   - no stripes cut through cloud;
   - no gap between the low clouds and the top layer.
   The arms should not look drawn on or uniform. (Hurricane shape. Storm shape sliders: "blocky arms, drawn on".)
7. **Squall lines:**
   - a continuous line of towers;
   - a shelf cloud along the front;
   - stratiform cloud trailing behind;
   - no slab, no ruled band.
8. **Storms belong to their surroundings.** A storm grows out of the cumulus around it, with mid-level cloud,
   and is not a prop standing in clear air. ("These storms also need some mid layer help".)
9. **Rain falls only under cloud.** No rain under clear air or under a gap. (Precipitation does not map to clouds.
   "Gaps still have rain underneath".)

## Light
10. **Distance does not change the lighting.** Clouds farther away are lit like the near ones: no band or ring
    where they turn brighter or darker. ("Far away clouds in these outer formations go dark". "Farther clouds
    tend to be a lot brighter".)
11. **Backlit storms** read as a dark mass with a bright silver rim, not pale puffs.
12. **Bases** are darker than the sunlit sides. (Cloud bottoms too bright.)

## Time (the motion sheets, the mp4s, numbers.json)
13. **No jumps.** In `motion_*`, a `spike` over ~3 or any `over_3x` frames means something jumped. Look at the
    "jump" pair on the motion sheet. (Hurricane arms "teleport randomly". "Their advancement is super choppy".)
14. **Hurricanes rotate.** Over the motion run, the bands turn continuously, inner faster than outer.
15. **No flicker.** In `*_flicker_heat.png`, the cloud should be dark (unchanging) for a still camera at 1x. The
    sea and stars may twinkle. Grain, crawling texture or a checker pattern over cloud is a failure. (Cloud
    Render Noise / Flicker. "Tons of flicker and odd evolution".)
16. **Clouds are opaque where they are thick:** no see-through, ghostly or doubled clouds. ("Why are clouds
    transparent".)
