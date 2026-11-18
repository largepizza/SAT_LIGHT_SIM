// The wildfire smoke layer's map on the CPU (SatelliteSimSmokeLayer.cpp, 2026-11-17, issue_20261117_195951): the
// Sun's transmittance through the layer's column above a point, so what the CPU lights (the smoke streaks, the smoke at
// the flames) is reddened by the haze it sits in as the layer's own light is (smoke_layer.comp). Reads the last map
// SatelliteSim::smokeLayerBuild made (a frame old where it is called from the dust fill, before this frame's build).
#pragma once
#include <glm/glm.hpp>

// P: a point (ECEF, m); sunE: the Sun's direction (ECEF, unit); specS: the Angstrom scale (smoke.glsl smokeSpecK's s).
// Returns the transmittance per channel (650 / 550 / 450 nm); 1 where the map holds no smoke. colAbove (optional): the
// layer's vertical optical depth above P (550 nm).
glm::dvec3 smokeLayerSunTransmitCpu(const glm::dvec3 &P, const glm::dvec3 &sunE, double specS, double *colAbove = nullptr);
