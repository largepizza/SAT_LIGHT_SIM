
## Now (addendum, round 2)
- The sky TAA's new-sample weight also ramps with the PARALLAX (the reprojection's offset from where a pure rotation
  would put the pixel) over 0.1 -> 1 px; the total motion keeps its 0.5 -> 4 px ramp. Climbing or descending over a
  city at night no longer smears the lights (Glasgow, 60 m/s: tstab 2.08 -> 1.67 at 75% render scale).
- With "Automatic render scale" on, the scale drops while moving (the moving cloud march costs more), and temporal
  upscaling only recovers its detail when still: a lower scale reads softer in motion.
