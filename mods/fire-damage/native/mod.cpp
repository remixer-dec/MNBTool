// Standalone fire damage. Follow particle simulation, including props, bones,
// meshes and scripted bursts; never tie damage to rendering or camera distance.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "mod_api.h"

extern "C" void* realloc(void*, size_t);

namespace {
constexpr const char* kGame = "libMBExpMobile.so";
constexpr uintptr_t kMissionTick = 0xE4D28;
constexpr uintptr_t kParticleTick = 0x1E6FA8;
constexpr uintptr_t kHandleBlows = 0x97DEC;
// One hit every half second. The received-damage message rounds the hit after
// the difficulty's damage scale (down to 1/4 for the player): 3 still shows 1.
constexpr float kInterval = 0.5f;
constexpr float kDamagePerHit = 3.0f;
// A flame sprite's outer half is mostly transparent: only the core burns.
constexpr float kFlameCore = 0.2f;
constexpr size_t kAgentSize = 24240;
constexpr size_t kAgentId = 8;
constexpr size_t kRider = 320;  // a horse's rider agent, -1 when none
constexpr size_t kPendingBlow = 23352;
constexpr size_t kBlow = 23356;
constexpr size_t kHealth = 23704;

using MissionTick = int32_t (*)(void*, float, int32_t);
using ParticleTick = int32_t (*)(void*, const float*, const void*);
using HandleBlows = void (*)(void*, float);
MissionTick previous_mission_tick = nullptr;
ParticleTick previous_particle_tick = nullptr;
HandleBlows previous_handle_blows = nullptr;
const mf_api_v1* api = nullptr;
const mf_mod_context_v1* context = nullptr;
const void* mission_slot = nullptr;
const void* network_mode = nullptr;
const void* particle_manager = nullptr;
const void* last_mission = nullptr;
uint32_t last_scene = 0;
float elapsed = 0;
bool ticking = false;
bool collecting = false;
bool allocation_reported = false;

template<typename T> T& field(void* base, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}
template<typename T> const T& field(const void* base, size_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(base) + offset);
}
uint8_t* game_pointer(uint32_t address) {
    return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(address));
}
const char* game_string(const void* string) {
    const uint32_t address = field<uint32_t>(string, 4);
    return address ? reinterpret_cast<const char*>(game_pointer(address)) : static_cast<const char*>(string) + 4;
}
float absolute(float value) { return value < 0 ? -value : value; }
float maximum(float a, float b) { return a > b ? a : b; }
float minimum(float a, float b) { return a < b ? a : b; }
bool finite(float value) { return value > -1000000 && value < 1000000; }

// Read identifiers, not numeric psys IDs: module authors reorder those freely.
bool fire_name(const char* name) {
    return strstr(name, "fire") || strstr(name, "flame") || strstr(name, "burn") ||
        strstr(name, "torch") || strstr(name, "candle") || strstr(name, "brazier");
}
bool decorative_name(const char* name) {
    return strstr(name, "smoke") || strstr(name, "spark") || strstr(name, "glow") ||
        strstr(name, "firefly") || strstr(name, "fire_fly");
}
bool is_fire(const void* system) {
    const int32_t id = field<int32_t>(system, 420);
    const int32_t count = field<int32_t>(particle_manager, 4);
    const uint8_t* definitions = game_pointer(field<uint32_t>(particle_manager, 0));
    if (definitions == nullptr || id < 0 || id >= count || id >= 65536) return false;
    const void* definition = definitions + static_cast<size_t>(id) * 452;
    const char* name = game_string(definition);
    const char* mesh = game_string(static_cast<const uint8_t*>(definition) + 148);
    return !decorative_name(name) && !decorative_name(mesh) && (fire_name(name) || fire_name(mesh));
}

struct Bounds { float low[3], high[3]; };
Bounds* fires = nullptr;
size_t fire_count = 0;
size_t fire_capacity = 0;

bool include(Bounds& bounds, const void* position, const float extent[3]) {
    for (size_t axis = 0; axis < 3; ++axis) {
        const float coordinate = field<float>(position, axis * 4);
        if (!finite(coordinate) || !finite(extent[axis])) return false;
        bounds.low[axis] = minimum(bounds.low[axis], coordinate - extent[axis]);
        bounds.high[axis] = maximum(bounds.high[axis], coordinate + extent[axis]);
    }
    return true;
}

void collect_fire(const void* system) {
    if (!is_fire(system)) return;
    const uint32_t begin = field<uint32_t>(system, 12);
    const uint32_t end = field<uint32_t>(system, 16);
    if (end < begin || (end - begin) % 100 != 0) return;
    const size_t particles = (end - begin) / 100;
    if (particles > 65536 || (particles != 0 && begin == 0)) return;
    const bool emitting = field<int32_t>(system, 408) > 0 && field<float>(system, 404) > 0;
    if (!emitting && particles == 0) return;

    Bounds bounds{{1000000, 1000000, 1000000}, {-1000000, -1000000, -1000000}};
    const float size = maximum(field<float>(system, 104), field<float>(system, 112));
    if (!finite(size) || size <= 0) return;
    float extent[3]{};
    for (size_t axis = 0; axis < 3; ++axis) {
        for (size_t basis = 0; basis < 3; ++basis) {
            const float scale = absolute(field<float>(system, 240 + basis * 16 + axis * 4));
            extent[axis] += scale * (absolute(field<float>(system, 368 + basis * 4)) + size * kFlameCore);
        }
    }
    // Keep a live emitter hazardous even when the game's particle-detail setting
    // suppresses its visible particles. Finished bursts use only surviving flames.
    if (emitting && !include(bounds, static_cast<const uint8_t*>(system) + 288, extent)) return;
    const float particle_extent[3] = {size * kFlameCore, size * kFlameCore, size * kFlameCore};
    for (size_t i = 0; i < particles; ++i) {
        if (!include(bounds, game_pointer(begin) + i * 100 + 48, particle_extent)) return;
    }
    if (fire_count == fire_capacity) {
        const size_t capacity = fire_capacity ? fire_capacity * 2 : 32;
        void* allocation = capacity <= 65536 ? realloc(fires, capacity * sizeof(Bounds)) : nullptr;
        if (allocation == nullptr) {
            if (!allocation_reported) api->log(context, MF_LOG_WARNING, "fire source storage exhausted");
            allocation_reported = true;
            return;
        }
        fires = static_cast<Bounds*>(allocation);
        fire_capacity = capacity;
    }
    fires[fire_count++] = bounds;
}

bool touches_fire(const void* agent, const Bounds& fire) {
    const bool horse = field<int32_t>(agent, 40) == 1;
    const float radius = horse ? 0.45f : 0.15f;
    const float height = horse ? 2.0f : 1.7f;
    const float x = field<float>(agent, 64), y = field<float>(agent, 68), z = field<float>(agent, 72);
    if (!finite(x) || !finite(y) || !finite(z) || z + height < fire.low[2] || z > fire.high[2]) return false;
    const float dx = maximum(maximum(fire.low[0] - x, x - fire.high[0]), 0);
    const float dy = maximum(maximum(fire.low[1] - y, y - fire.high[1]), 0);
    return dx * dx + dy * dy <= radius * radius;
}

bool alive(const void* agent) {
    return field<int32_t>(agent, 4) != 0 && field<int32_t>(agent, 48) == 1 && field<float>(agent, kHealth) > 0;
}

// rglStable_vector<Agent>: chunks of 16, each chunk slot is four bytes.
void* agent_at(void* mission, int32_t index) {
    const int32_t count = field<int32_t>(mission, 4);
    if (index < 0 || index >= count || count > 65536) return nullptr;
    const void* chunks = game_pointer(field<uint32_t>(mission, 24));
    if (chunks == nullptr) return nullptr;
    uint8_t* chunk = game_pointer(field<uint32_t>(chunks, static_cast<size_t>(index / 16) * 4));
    return chunk != nullptr ? chunk + static_cast<size_t>(index % 16) * kAgentSize : nullptr;
}

// As the game credits a fall (Agent::do_dynamics_finish, 0x944b0): the agent
// itself, or a horse's living rider. Death scripts then get a real killer.
int32_t fire_attacker(void* mission, const void* agent) {
    const int32_t self = field<int32_t>(agent, kAgentId);
    if (field<int32_t>(agent, 40) != 1) return self;
    const int32_t rider = field<int32_t>(agent, kRider);
    const void* rider_agent = agent_at(mission, rider);
    return rider_agent != nullptr && field<int32_t>(rider_agent, 4) != 0 ? rider : self;
}

void burn(void* mission, void* agent, float damage, float delta) {
    // Finish any queued weapon hit first. The fire hit then uses the ordinary
    // damage/death pipeline, including difficulty, mission triggers and network events.
    if (field<int32_t>(agent, kPendingBlow)) previous_handle_blows(agent, delta);
    if (!alive(agent)) return;
    alignas(4) uint8_t blow[132]{};
    field<uint32_t>(blow, 0) = 0x20; // No weapon impact sound.
    field<int32_t>(blow, 4) = fire_attacker(mission, agent);
    memcpy(blow + 8, static_cast<uint8_t*>(agent) + 64, 12);
    field<float>(blow, 24) = 1;
    field<float>(blow, 40) = 1;
    field<int32_t>(blow, 60) = 1;
    field<float>(blow, 64) = damage;
    field<float>(blow, 68) = damage;
    field<int32_t>(blow, 72) = 0;    // Cut-type lethal damage; armour has already been bypassed.
    field<int32_t>(blow, 76) = 2;
    field<int32_t>(blow, 80) = -1;   // No weapon or missile.
    field<int32_t>(blow, 128) = 2;
    memcpy(static_cast<uint8_t*>(agent) + kBlow, blow, sizeof(blow));
    field<int32_t>(agent, kPendingBlow) = 1;
    previous_handle_blows(agent, delta);
}

void damage_agents(void* mission, float seconds) {
    const int32_t count = field<int32_t>(mission, 4);
    if (count <= 0 || count > 65536 || fire_count == 0) return;
    for (int32_t i = 0; i < count; ++i) {
        if (game_pointer(field<uint32_t>(mission_slot, 0)) != mission) break;
        void* agent = agent_at(mission, i);
        if (agent == nullptr || !alive(agent)) continue;
        for (size_t source = 0; source < fire_count; ++source) {
            if (touches_fire(agent, fires[source])) {
                burn(mission, agent, kDamagePerHit * seconds / kInterval, seconds);
                break; // Overlapping flame layers never multiply the damage rate.
            }
        }
    }
}

int32_t replacement_particle_tick(void* system, const float* delta, const void* frame) {
    const int32_t result = previous_particle_tick(system, delta, frame);
    if (collecting) collect_fire(system); // +240 now holds the complete world transform.
    return result;
}

void replacement_handle_blows(void* agent, float delta) { previous_handle_blows(agent, delta); }

int32_t replacement_mission_tick(void* mission, float delta, int32_t tick_entries) {
    if (ticking) return previous_mission_tick(mission, delta, tick_entries);
    const uint32_t scene = field<uint32_t>(mission, 0);
    if (last_mission != mission || last_scene != scene) {
        elapsed = 0;
        last_mission = mission;
        last_scene = scene;
    }
    // Clients follow the authoritative game's damage events. Zero-delta ticks
    // are paused; limiting long stalls prevents a catch-up burst on resume.
    const bool authoritative = field<int32_t>(network_mode, 0) != 1;
    const bool active = authoritative && game_pointer(field<uint32_t>(mission_slot, 0)) == mission && delta > 0 && finite(delta);
    if (active) elapsed += minimum(delta, 0.25f);
    const int pulses = static_cast<int>((elapsed + 0.000001f) / kInterval);
    const float seconds = active ? static_cast<float>(pulses) * kInterval : 0;
    if (seconds > 0) elapsed = maximum(0, elapsed - seconds);
    fire_count = 0;
    collecting = seconds > 0;
    ticking = true;
    const int32_t result = previous_mission_tick(mission, delta, tick_entries);
    collecting = false;
    if (seconds > 0 && game_pointer(field<uint32_t>(mission_slot, 0)) == mission) damage_agents(mission, seconds);
    ticking = false;
    return result;
}
} // namespace

MF_MOD_START(fire_damage)(const mf_api_v1* mod_api, const mf_mod_context_v1* mod_context) {
    if (mod_api == nullptr || mod_context == nullptr || mod_api->abi_version != MF_NATIVE_ABI_VERSION ||
        !MF_API_HAS(mod_api, resolve_game_address)) return MF_ERROR_INVALID_ARGUMENT;
    api = mod_api;
    context = mod_context;
    mission_slot = api->resolve_game_address(context, kGame, 0x7B9440, 4, MF_MEMORY_READ);
    network_mode = api->resolve_game_address(context, kGame, 0x7B8FDC, 4, MF_MEMORY_READ);
    particle_manager = api->resolve_game_address(context, kGame, 0x2732CC0, 8, MF_MEMORY_READ);
    if (!mission_slot || !network_mode || !particle_manager) return MF_ERROR_NOT_FOUND;

    const mf_function_hook_v1 blows{sizeof(blows), MF_HOOK_CHAIN, "fire-blows", kGame, nullptr, kHandleBlows,
        reinterpret_cast<void*>(&replacement_handle_blows), reinterpret_cast<void**>(&previous_handle_blows)};
    mf_result_v1 result = api->hook_function(context, &blows);
    if (result != MF_OK) return result;
    const mf_function_hook_v1 particles{sizeof(particles), MF_HOOK_CHAIN, "fire-particles", kGame, nullptr, kParticleTick,
        reinterpret_cast<void*>(&replacement_particle_tick), reinterpret_cast<void**>(&previous_particle_tick)};
    result = api->hook_function(context, &particles);
    if (result != MF_OK) return result;
    const mf_function_hook_v1 mission{sizeof(mission), MF_HOOK_CHAIN, "fire-mission", kGame, nullptr, kMissionTick,
        reinterpret_cast<void*>(&replacement_mission_tick), reinterpret_cast<void**>(&previous_mission_tick)};
    result = api->hook_function(context, &mission);
    if (result == MF_OK) api->log(context, MF_LOG_INFO, "active: fire damage follows mission simulation");
    return result;
}
