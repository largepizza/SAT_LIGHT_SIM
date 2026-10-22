#pragma once
// SatModelTool --dump-json <file>: the model's triangles, its lobes at several budgets, and lobe-vs-brute-force
// samples, for visualisation (tools/video). See dump_json.cpp for the format.
#include <string>
#include <vector>

struct SatModel;
struct SatTri;

bool dumpModelJson(const SatModel &m, const std::string &id, const std::vector<SatTri> &tris,
                   const std::vector<int> &budgets, const std::string &outPath);
