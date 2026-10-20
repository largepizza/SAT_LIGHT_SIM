---
pages: [using/controls.md, using/satellites.md, development/harness.md]
code: [src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSimMissions.cpp, src/simulations/SatelliteSimLaunches.cpp, src/simulations/SatelliteSimHarness.cpp]
---
## Now
**Go to keeps above the ground.** The follow camera never goes below the terrain or the sea: its floor is the larger
of the CPU elevation map at the camera and the GPU's detailed ground under the eye (eased over ~0.2 s), plus 3 m.
At the floor the camera slides along the ground and the offset follows it, so moving away needs no climb back out.

**Return** (follow HUD button "Return", key H = "Go To: Return", harness `follow return`): while following, the camera
flies back to the default view of the object (behind and above it, ~4 model radii), turning round it and closing the
distance geometrically in the object's own frame. After leaving Go to (Stay here, Exit, an object that ended) the same
key, or a "Go to <name> again" chip shown for 30 s at the top of the screen, follows the last object again, flying
from where the camera is. A rocket or ship is carried along its mission to the present first; a launch record whose
slot changed hands, or a debris slot reused, is reported gone.

**Go to from Go to** flies straight from where the camera is to the new object, in the new object's frame: the
direction is kept and the distance closes to the follow distance. (The arc / observer's line of sight is still used
from the ground, and when the Earth lies between the two.)

**Selecting is not brightness-gated where an object is named.** Clicking in the sky picks from what is drawn (the
GPU's visible list: above the horizon, past the magnitude limit), and that cannot change. Everything named elsewhere
is selected by index or key: Missions rows (rockets, crew transfers: Select and Go to whether drawn or not; trains and
retirees: Select and Go to the nearest), stations, the harness. A rocket the list does not hold is placed by the
launch model, and a selected rocket stays selected while out of view. Harness: `missions ongoing <row> [go=1]`.

## Was
Following a low rocket could take the camera kilometres underground (the report: 3.9 km under Louisiana). There was
no way back to the followed object short of re-selecting it. Go to from one followed object to the next flew along
the ground observer's line of sight from a start distance clamped at 1 m: the camera passed through the new object.
Missions' rocket Select was disabled when the rocket was not drawn ("Not drawn from here"), and a selected rocket was
dropped 2 s after it stopped being listed.
