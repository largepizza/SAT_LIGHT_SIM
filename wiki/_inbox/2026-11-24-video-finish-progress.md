---
pages: [using/features.md]
code: [src/VideoRecorder.cpp, src/simulations/SatelliteSimVideo.cpp, src/App.cpp]
---
## Now
While a video is being written after it stops, the badge in the top-left corner reads "Writing the video... N%
(a of b frames)", then "Writing the video... finishing the file". Closing the app (window, Esc) while a video is
recording or being written stops the recording and keeps the app open, drawing, until the file is written; the badge
adds "the app closes when it is written (close again to stop now)". Closing a second time cancels: the partial
file is deleted. Sound is written at most 0.5 s ahead of the picture, so a recording with sound finishes in a
fraction of a second even when HQ photos were taken during it.

## History
issue_20261124_190229 (2026-11-24): with sound on, an HQ photo taken during a recording left the sound seconds ahead
of the picture and Media Foundation's sink writer stalled: 54.5 s from stop to file for a 10-s clip at the
reporter's scene (0.1 s without the photo or without sound), 78 s for the user's 104-s clip, and one file never
finished. Closing during that froze the window inside cleanup until Windows killed it.
