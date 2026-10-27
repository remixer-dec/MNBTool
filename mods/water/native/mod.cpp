// Mission water physics and breath. Game records keep their ARM32 layout on
// translated ARM64; only the public SDK owns the native/game call boundary.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "mod_api.h"

extern "C" void* realloc(void*, size_t);

namespace {
constexpr const char* kGame = "libMBExpMobile.so";
constexpr size_t kAgentSize = 0x5eb0;
constexpr size_t kPending = 0x5b38;
constexpr size_t kBlow = 0x5b3c;
constexpr size_t kHealth = 0x5c98;
constexpr int32_t kMaxAgents = 65536;
constexpr float kRecovery = 4.0f; // seconds of breath recovered per second

using MissionTick = int32_t (*)(void*, float, int32_t);
using ClearMission = void (*)(void*, bool);
// rglVec2 is returned via an explicit result pointer (r0), with this in r1.
// WATER.md's float-return sketch is not the game's ABI (see touch_combat.cpp).
using IdealSpeed = void (*)(float*, const void*);
using Step = void (*)(void*, const float*, const bool*);
using Finish = void (*)(void*, float, void*, const void*, float, float, bool, bool, bool, const void*);
using HandleBlows = void (*)(void*, float);
using AiEye = void (*)(const void*, void*, void*);
MissionTick previous_tick = nullptr;
ClearMission previous_clear = nullptr;
IdealSpeed previous_speed = nullptr;
Step previous_step = nullptr;
Finish previous_finish = nullptr;
HandleBlows previous_blows = nullptr;
AiEye get_ai_eye = nullptr;
const mf_api_v1* api = nullptr;
const mf_mod_context_v1* context = nullptr;
const void* mission_slot = nullptr;
const void* network_mode = nullptr;
// Fixed, not settings: every player in a multiplayer match must simulate the same water.
float water_speed = 0.5f;     // human wading speed multiplier
float sink_speed = 2.0f;      // maximum sinking speed, m/s
float oxygen_seconds = 10.0f; // breath before drowning hits start
float drown_damage = 6.0f;    // one hit per second without breath
bool ticking = false;
bool allocation_reported = false;
const void* last_mission = nullptr;
uint32_t last_scene = 0;
uint32_t mission_generation = 0;

template<class T> T& field(void* base, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}
template<class T> const T& field(const void* base, size_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(base) + offset);
}
uint8_t* game_pointer(uint32_t address) {
    return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(address));
}
float minimum(float a, float b) { return a < b ? a : b; }
float maximum(float a, float b) { return a > b ? a : b; }
float absolute(float a) { return a < 0 ? -a : a; }
bool finite(float a) { return a > -1000000.0f && a < 1000000.0f; }
float seconds(float delta) { return finite(delta) && delta > 0 ? minimum(delta, 0.25f) : 0; }
bool alive(const void* agent) {
    return agent && field<int32_t>(agent, 4) != 0 && field<int32_t>(agent, 0x30) == 1 &&
        field<float>(agent, kHealth) > 0;
}
void* current_mission() { return game_pointer(field<uint32_t>(mission_slot, 0)); }
bool active(const void* mission) {
    // basic_game+0x76d4: 0 single player, 1 client, 2 server (touch_common.h).
    // WATER.md's proposed "loading state" interpretation of this field is
    // incorrect. Simulation dt, the current mission and scene gate this mod.
    return mission && current_mission() == mission && field<uint32_t>(mission, 0) != 0 &&
        (field<int32_t>(network_mode, 0) == 0 || field<int32_t>(network_mode, 0) == 2);
}
bool wet(const void* agent, const void* mission) {
    if (!alive(agent) || !active(mission)) return false;
    const float water = field<float>(mission, 0xc1b8), feet = field<float>(agent, 0x48);
    return finite(water) && finite(feet) && feet < water;
}
void* agent_at(void* mission, int32_t index) {
    const int32_t count = field<int32_t>(mission, 4);
    if (index < 0 || index >= count || count > kMaxAgents) return nullptr;
    const void* chunks = game_pointer(field<uint32_t>(mission, 0x18));
    if (!chunks) return nullptr;
    uint8_t* chunk = game_pointer(field<uint32_t>(chunks, static_cast<size_t>(index >> 4) * 4));
    return chunk ? chunk + static_cast<size_t>(index & 15) * kAgentSize : nullptr;
}

struct Air {
    int32_t serial;
    bool valid;
    float oxygen;
    float damage_time;
};
Air* air = nullptr;
size_t air_capacity = 0;
void reset_air() {
    ++mission_generation;
    if (air) memset(air, 0, air_capacity * sizeof(Air));
    last_mission = nullptr;
    last_scene = 0;
}
bool reserve_air(int32_t count) {
    if (count <= 0 || count > kMaxAgents) return false;
    if (static_cast<size_t>(count) <= air_capacity) return true;
    size_t capacity = air_capacity ? air_capacity : 64;
    while (capacity < static_cast<size_t>(count)) capacity *= 2;
    void* memory = realloc(air, capacity * sizeof(Air));
    if (!memory) {
        if (!allocation_reported) api->log(context, MF_LOG_WARNING, "water: breath storage unavailable");
        allocation_reported = true;
        return false;
    }
    air = static_cast<Air*>(memory);
    memset(air + air_capacity, 0, (capacity - air_capacity) * sizeof(Air));
    air_capacity = capacity;
    return true;
}

// get_ai_eye (0x5aa70) refreshes the skeleton even for agents off camera and
// returns head-bone 9's origin + 0.2 * its forward axis in world coordinates.
// There is no engine mouth accessor. Model the mouth below that eye, as a small
// box in the animated head frame. Test its HIGHEST point, including head tilt
// and entity scale: a partly exposed mouth must never consume oxygen.
bool mouth_submerged(const void* agent, float water) {
    if (!finite(water) || field<int32_t>(agent, 0x28) != 0) return false;
    const float feet = field<float>(agent, 0x48);
    if (!finite(feet) || feet >= water) return false; // dry agents need no pose evaluation
    const void* entity = game_pointer(field<uint32_t>(agent, 0x4f8));
    if (!entity) return false;
    const void* skeleton = game_pointer(field<uint32_t>(entity, 0x2b8));
    if (!skeleton || field<int32_t>(skeleton, 0) < 10 || field<int32_t>(skeleton, 0) > 256 ||
        field<uint32_t>(skeleton, 4) == 0) return false;
    float eye[4]{}, direction[4]{};
    get_ai_eye(agent, eye, direction);
    const uint8_t* bones = game_pointer(field<uint32_t>(skeleton, 4));
    if (!bones || !finite(eye[2])) return false;
    const void* head = bones + 9 * 0x214 + 0x84;
    float z[3]{};
    for (size_t axis = 0; axis < 3; ++axis) {
        for (size_t k = 0; k < 3; ++k)
            z[axis] += field<float>(head, axis * 16 + k * 4) * field<float>(entity, 0x60 + k * 16);
        if (!finite(z[axis])) return false;
    }
    // Centre: 10 cm below the eye. Extents: 4 cm side, 2.5 cm forward,
    // 2 cm up (upper lip); units are before the entity's scale.
    const float top = eye[2] - 0.10f * z[2] +
        0.04f * absolute(z[0]) + 0.025f * absolute(z[1]) + 0.02f * absolute(z[2]);
    return finite(top) && top < water;
}

// Advance only the portion of this frame AFTER oxygen runs out. Surfacing
// drops fractional drowning time immediately, and recovers breath gradually.
float advance_air(Air& state, bool submerged, float delta) {
    if (delta <= 0) return 0;
    if (!submerged) {
        state.oxygen = minimum(oxygen_seconds, state.oxygen + delta * kRecovery);
        state.damage_time = 0;
        return 0;
    }
    const float used = minimum(state.oxygen, delta);
    state.oxygen = maximum(0, state.oxygen - used);
    state.damage_time += delta - used;
    const int32_t pulses = static_cast<int32_t>(state.damage_time + 0.000001f);
    if (pulses == 0) return 0;
    state.damage_time = maximum(0, state.damage_time - static_cast<float>(pulses));
    return drown_damage * static_cast<float>(pulses);
}

void drown(void* mission, void* agent, float damage, float delta) {
    const uint32_t generation = mission_generation;
    const int32_t serial = field<int32_t>(agent, 0xc);
    const uint32_t scene = field<uint32_t>(mission, 0);
    // Consume a real combat blow first. Its scripts may kill, respawn, teleport
    // or end the mission: revalidate before creating the environmental blow.
    if (field<int32_t>(agent, kPending)) previous_blows(agent, delta);
    if (generation != mission_generation || !active(mission) || field<uint32_t>(mission, 0) != scene || !alive(agent) ||
        field<int32_t>(agent, 0xc) != serial || !mouth_submerged(agent, field<float>(mission, 0xc1b8))) return;
    // Same verified environmental-hit layout as Fire Damage. Already computed
    // damage bypasses armour; handle_blows still applies difficulty, triggers,
    // death and network events. A self attacker matches the engine's fall hit.
    alignas(4) uint8_t blow[0x84]{};
    field<uint32_t>(blow, 0) = 0x20;
    field<int32_t>(blow, 4) = field<int32_t>(agent, 8);
    memcpy(blow + 8, static_cast<uint8_t*>(agent) + 0x40, 12);
    field<float>(blow, 24) = 1;
    field<float>(blow, 40) = 1;
    field<int32_t>(blow, 60) = 1;
    field<float>(blow, 64) = damage;
    field<float>(blow, 68) = damage;
    field<int32_t>(blow, 76) = 2;
    field<int32_t>(blow, 80) = -1;
    field<int32_t>(blow, 128) = 2;
    memcpy(static_cast<uint8_t*>(agent) + kBlow, blow, sizeof(blow));
    field<int32_t>(agent, kPending) = 1;
    previous_blows(agent, delta);
}

void update_air(void* mission, float delta) {
    const int32_t count = field<int32_t>(mission, 4);
    if (count == 0) {
        if (air) memset(air, 0, air_capacity * sizeof(Air));
        return;
    }
    if (!reserve_air(count)) return;
    const uint32_t generation = mission_generation;
    const uint32_t scene = field<uint32_t>(mission, 0);
    const float water = field<float>(mission, 0xc1b8);
    for (int32_t i = 0; i < count; ++i) {
        if (generation != mission_generation || !active(mission) || field<uint32_t>(mission, 0) != scene) break;
        void* agent = agent_at(mission, i);
        Air& state = air[i];
        if (!alive(agent) || field<int32_t>(agent, 0x28) != 0 || field<int32_t>(agent, 8) != i) {
            state.valid = false;
            continue;
        }
        const int32_t serial = field<int32_t>(agent, 0xc);
        if (!state.valid || state.serial != serial) state = {serial, true, oxygen_seconds, 0};
        const float damage = advance_air(state, mouth_submerged(agent, water), delta);
        if (damage > 0) drown(mission, agent, damage, delta);
    }
    // An unused tail may disappear without a scene clear; never retain its air.
    memset(air + count, 0, (air_capacity - static_cast<size_t>(count)) * sizeof(Air));
}

int32_t replacement_tick(void* mission, float delta, int32_t tick_entries) {
    if (ticking) return previous_tick(mission, delta, tick_entries);
    const uint32_t scene = field<uint32_t>(mission, 0);
    if (last_mission != mission || last_scene != scene) {
        reset_air();
        last_mission = mission;
        last_scene = scene;
    }
    const bool was_active = active(mission);
    ticking = true;
    const int32_t result = previous_tick(mission, delta, tick_entries);
    if (was_active && active(mission) && last_mission == mission && last_scene == scene &&
        field<uint32_t>(mission, 0) == scene && seconds(delta) > 0) update_air(mission, seconds(delta));
    ticking = false;
    return result;
}
void replacement_clear(void* mission, bool flag) {
    // Pointer/scene comparisons alone miss missions rebuilt in the same storage.
    reset_air();
    previous_clear(mission, flag);
}
void replacement_speed(float* velocity, const void* agent) {
    previous_speed(velocity, agent);
    if (wet(agent, current_mission())) {
        velocity[0] *= water_speed;
        velocity[1] *= water_speed;
    }
}
void cap_sink(void* agent) {
    float& vz = field<float>(agent, 0x5c08);
    if (finite(vz) && vz < -sink_speed) vz = -sink_speed;
}
void replacement_step(void* agent, const float* delta, const bool* flag) {
    void* mission = current_mission();
    const uint32_t generation = mission_generation;
    const float dt = delta ? seconds(*delta) : 0;
    if (dt > 0 && wet(agent, mission)) cap_sink(agent);
    previous_step(agent, delta, flag);
    if (generation == mission_generation && dt > 0 && wet(agent, mission)) {
        cap_sink(agent);
        // Time-based drag also catches AI-written velocities and horses. The
        // main human movement penalty is the ideal-speed hook above.
        const float drag = 1.0f / (1.0f + 2.0f * dt);
        for (size_t offset = 0x5c00; offset <= 0x5c04; offset += 4) {
            float& v = field<float>(agent, offset);
            if (finite(v)) v *= drag;
        }
    }
}
void replacement_finish(void* agent, float dt, void* new_pos, const void* old_pos,
                        float a, float b, bool c, bool d, bool e, const void* f) {
    void* mission = current_mission();
    const uint32_t generation = mission_generation;
    const bool eligible = seconds(dt) > 0 && active(mission) && alive(agent);
    const bool was_wet = eligible && wet(agent, mission);
    const int32_t pending = eligible ? field<int32_t>(agent, kPending) : 0;
    alignas(4) uint8_t saved[0x84]{};
    if (pending) memcpy(saved, static_cast<uint8_t*>(agent) + kBlow, sizeof(saved));
    previous_finish(agent, dt, new_pos, old_pos, a, b, c, d, e, f);
    // This function only creates fall blows. Restore the queued combat blow
    // even if a stronger fall overwrote it; clearing just the flag loses hits.
    if (eligible && generation == mission_generation && active(mission) && alive(agent) && (was_wet || wet(agent, mission))) {
        if (pending) memcpy(static_cast<uint8_t*>(agent) + kBlow, saved, sizeof(saved));
        field<int32_t>(agent, kPending) = pending;
    }
}
void replacement_blows(void* agent, float delta) { previous_blows(agent, delta); }
} // namespace

MF_MOD_START(water)(const mf_api_v1* mod_api, const mf_mod_context_v1* mod_context) {
    if (!mod_api || !mod_context || mod_api->abi_version != MF_NATIVE_ABI_VERSION ||
        !MF_API_HAS(mod_api, resolve_game_address) || !mod_api->resolve_game_address)
        return MF_ERROR_INVALID_ARGUMENT;
    api = mod_api;
    context = mod_context;
    mission_slot = api->resolve_game_address(context, kGame, 0x7b9440, 4, MF_MEMORY_READ);
    network_mode = api->resolve_game_address(context, kGame, 0x7b8fdc, 4, MF_MEMORY_READ);
    get_ai_eye = reinterpret_cast<AiEye>(api->resolve_game_address(context, kGame, 0x5aa70, 4, MF_MEMORY_EXECUTE));
    if (!mission_slot || !network_mode || !get_ai_eye) return MF_ERROR_NOT_FOUND;

    const mf_function_hook_v1 blows{sizeof(blows), MF_HOOK_CHAIN, "water-blows", kGame, nullptr, 0x97dec,
        reinterpret_cast<void*>(&replacement_blows), reinterpret_cast<void**>(&previous_blows)};
    mf_result_v1 result = api->hook_function(context, &blows);
    if (result != MF_OK) return result;
    const mf_function_hook_v1 clear{sizeof(clear), MF_HOOK_CHAIN, "water-clear", kGame, nullptr, 0xdbcb4,
        reinterpret_cast<void*>(&replacement_clear), reinterpret_cast<void**>(&previous_clear)};
    result = api->hook_function(context, &clear);
    if (result != MF_OK) return result;
    const mf_function_hook_v1 speed{sizeof(speed), MF_HOOK_CHAIN, "water-speed", kGame, nullptr, 0x77158,
        reinterpret_cast<void*>(&replacement_speed), reinterpret_cast<void**>(&previous_speed)};
    result = api->hook_function(context, &speed);
    if (result != MF_OK) return result;
    const mf_function_hook_v1 step{sizeof(step), MF_HOOK_CHAIN, "water-step", kGame, nullptr, 0x94a8c,
        reinterpret_cast<void*>(&replacement_step), reinterpret_cast<void**>(&previous_step)};
    result = api->hook_function(context, &step);
    if (result != MF_OK) return result;
    const mf_function_hook_v1 finish{sizeof(finish), MF_HOOK_CHAIN, "water-fall", kGame, nullptr, 0x93884,
        reinterpret_cast<void*>(&replacement_finish), reinterpret_cast<void**>(&previous_finish)};
    result = api->hook_function(context, &finish);
    if (result != MF_OK) return result;
    const mf_function_hook_v1 tick{sizeof(tick), MF_HOOK_CHAIN, "water-mission", kGame, nullptr, 0xe4d28,
        reinterpret_cast<void*>(&replacement_tick), reinterpret_cast<void**>(&previous_tick)};
    return api->hook_function(context, &tick);
}
