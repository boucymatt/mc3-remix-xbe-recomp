#pragma once

#include <cstdint>

struct Mc3DebugVehiclePair {
    uint32_t player_matrix = 0;
    uint32_t opponent_matrix = 0;
    float player[3] = {};
    float opponent[3] = {};
};

void Mc3DebugInit();
void Mc3DebugTransform(uint32_t type, float* matrix);
bool Mc3DebugHasArg(const wchar_t* key);
const wchar_t* Mc3DebugArg(const wchar_t* key);
// Returns the player and closest other car from the title's live Matrix34
// objects.  The scan is throttled and never writes guest memory.
bool Mc3DebugGetClosestVehiclePair(Mc3DebugVehiclePair& pair, uint32_t preferred_opponent = 0);

// Resolve the Matrix34 owned by a guest object through the short object graph
// used by mcPlayer and aiBrain-derived classes. The optional offsets identify
// the validated pointer path and are intended for trace/provenance only.
uint32_t Mc3DebugFindVehicleMatrix(uint32_t root, uint32_t* first_offset = nullptr,
                                   uint32_t* second_offset = nullptr);
uint32_t Mc3DebugFindVehicleOwnerByVtable(uint32_t vtable, uint32_t* vehicle_matrix = nullptr,
                                         uint32_t* first_offset = nullptr,
                                         uint32_t* second_offset = nullptr,
                                         uint32_t* object_count = nullptr);

