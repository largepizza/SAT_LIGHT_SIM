#pragma once
// SatModelTool --pass <spec.json> [--pass-out <file>]: a satellite pass from given states (e.g. a TLE through SGP4)
// evaluated by the CPU photometric evaluator, sample by sample. See pass_eval.cpp for the formats.
#include <filesystem>
#include <string>

bool runPassCommand(const std::string &specPath, const std::string &outPath);
