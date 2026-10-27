#pragma once
/* Thin, public-only ARM32 base-AAPCS adapters. Generated builds supply the
 * compiler-owned source prototype separately from the native callback type.
 * Runtime policy, checked memory and callback execution stay in ModFramework.
 * These helpers are also available to authors who explicitly manage ARM64. */
#include "mod_api.h"
#include <cstring>
#include <type_traits>

namespace mf::compat {
// Avoid hosted containers/utility headers: freestanding packages can declare
// their own permitted libc prototypes, which those headers may redeclare.
template<class T, size_t N> struct Buffer {
    T values[N ? N : 1]{};
    constexpr T& operator[](size_t index) { return values[index]; }
    constexpr const T& operator[](size_t index) const { return values[index]; }
};
template<size_t... I> struct Indices {};
template<size_t N, size_t... I> struct MakeIndices : MakeIndices<N-1, N-1, I...> {};
template<size_t... I> struct MakeIndices<0, I...> { using type = Indices<I...>; };
template<class... T> using IndicesFor = typename MakeIndices<sizeof...(T)>::type;
template<class T> constexpr bool core_type =
    ((std::is_integral_v<T> || std::is_enum_v<T>) && sizeof(T) <= 8) ||
    std::is_same_v<T, float> || std::is_same_v<T, double> ||
    (std::is_pointer_v<T> && !std::is_function_v<std::remove_pointer_t<T>>);
template<class T> constexpr size_t source_words = std::is_pointer_v<T> ? 1 : (sizeof(T)+3)/4;
template<class T, bool = std::is_enum_v<T>> struct Scalar { using type = T; };
template<class T> struct Scalar<T, true> { using type = std::underlying_type_t<T>; };
template<class Source, class Native> constexpr bool matching_type = [] {
    if constexpr (std::is_void_v<Source> || std::is_void_v<Native>)
        return std::is_void_v<Source> && std::is_void_v<Native>;
    else if constexpr (!core_type<Source> || !core_type<Native>) return false;
    else if constexpr (std::is_pointer_v<Source> || std::is_pointer_v<Native>)
        return std::is_same_v<Source, Native>;
    else if constexpr (std::is_floating_point_v<Source> || std::is_floating_point_v<Native>)
        return std::is_same_v<Source, Native>;
    else return std::is_signed_v<typename Scalar<Source>::type> ==
                std::is_signed_v<typename Scalar<Native>::type>;
}();

struct Services {
    const mf_api_v1* api = nullptr;
    const mf_mod_context_v1* context = nullptr;
    const mf_game_services_v1* game = nullptr;
    bool bind(const mf_api_v1* candidate, const mf_mod_context_v1* owner) {
        if (!candidate || !owner || candidate->abi_version != MF_NATIVE_ABI_VERSION ||
            !MF_API_HAS(candidate, get_game_services) || !candidate->get_game_services ||
            !candidate->invoke_callback || !candidate->hook_function_compatible ||
            (context && (context != owner || api != candidate))) return false;
        auto* contract = candidate->get_game_services(owner);
        if (!contract || contract->struct_size < sizeof(mf_game_services_v1) ||
            contract->source_abi != MF_GAME_ABI_ARM32_BASE) return false;
        api = candidate; context = owner; game = contract; return true;
    }
    // A broken contract stops the game; the reason goes to the mod's log first.
    void require(bool valid, const char* reason) const {
        if (valid) return;
        if (api && context) api->log(context, MF_LOG_ERROR, reason);
        __builtin_trap();
    }
    uint32_t read(size_t index) const {
        uint32_t value = 0;
        require(game && game->read_stack && index < 60 &&
                game->read_stack(context, static_cast<uint32_t>(index), &value) == MF_OK,
                "game hook: cannot read the game's stack arguments");
        return value;
    }
    void write(size_t index, uint32_t value) const {
        require(game && game->write_stack && index < 60 &&
                game->write_stack(context, static_cast<uint32_t>(index), value) == MF_OK,
                "game hook: cannot write the game's stack arguments");
    }
    template<class T> T decode(uint64_t bits) const {
        static_assert(core_type<T>, "Unsupported source core-register type");
        if constexpr (std::is_pointer_v<T>) {
            if (!bits) return nullptr;
            require(game && game->decode_pointer, "game hook: no game memory services");
            require(bits <= UINT32_MAX, "game hook: a game address wider than 32 bits");
            // As a C cast on ARMv7: game code passes literals as char*, end pointers and
            // sentinels, so the memory is checked only where the mod uses it.
            void* value = game->decode_pointer(context, static_cast<uint32_t>(bits), 0, 0);
            require(value != nullptr, "game hook: this launcher cannot convert game addresses");
            return static_cast<T>(value);
        } else if constexpr (std::is_floating_point_v<T>) {
            T value; std::memcpy(&value, &bits, sizeof(value)); return value;
        } else return static_cast<T>(bits);
    }
    template<class T> uint64_t encode(T value) const {
        static_assert(core_type<T>, "Unsupported source core-register type");
        if constexpr (std::is_pointer_v<T>) {
            if (!value) return 0;
            uint32_t bits = 0;
            require(game && game->encode_pointer &&
                    game->encode_pointer(context, value, &bits) == MF_OK,
                    "game hook: a pointer given to the game is outside game memory (a native stack variable "
                    "or launcher buffer?); use a static buffer, malloc or game memory");
            return bits;
        } else if constexpr (std::is_floating_point_v<T>) {
            uint64_t bits = 0; std::memcpy(&bits, &value, sizeof(value)); return bits;
        } else if constexpr (sizeof(T) <= 4) return static_cast<uint32_t>(value);
        else return static_cast<uint64_t>(value);
    }
};

template<class SourceSignature, class NativeSignature, auto Replacement, class Tag> struct Hook;
template<class SR, class... SA, class NR, class... NA, auto Replacement, class Tag>
struct Hook<SR (*)(SA...), NR (*)(NA...), Replacement, Tag> {
    static_assert(sizeof...(SA) == sizeof...(NA), "Source/native argument count mismatch");
    static_assert((matching_type<SA, NA> && ...), "Unsupported source/native argument contract");
    static_assert(matching_type<SR, NR>, "Unsupported source/native result contract");
    inline static Services services{};
    inline static void* previous_raw = nullptr;
    inline static constexpr auto offsets = [] {
        Buffer<size_t, sizeof...(SA)+1> result{};
        size_t position = 0, index = 0;
        ((position = (position + source_words<SA>-1) & ~(source_words<SA>-1),
          result[index++] = position, position += source_words<SA>), ...);
        result[index] = position; return result;
    }();
    static constexpr size_t word_count = offsets[sizeof...(SA)] < 4 ? 4 : offsets[sizeof...(SA)];
    static_assert(word_count <= 64, "Source prototype exceeds the bounded adapter contract");
    using Words = Buffer<uint32_t, word_count>;
    template<class T> static T decode(const Words& words, size_t index) {
        uint64_t bits = words[index];
        if constexpr (source_words<T> == 2) bits |= uint64_t(words[index+1]) << 32;
        return services.template decode<T>(bits);
    }
    template<class T> static void encode(Words& words, size_t index, T value) {
        const uint64_t bits = services.encode(value);
        words[index] = static_cast<uint32_t>(bits);
        if constexpr (source_words<T> == 2) words[index+1] = static_cast<uint32_t>(bits >> 32);
    }
    template<size_t... I> static uint64_t invoke(const Words& words, Indices<I...>) {
        if constexpr (std::is_void_v<SR>) {
            Replacement(static_cast<NA>(decode<SA>(words, offsets[I]))...); return 0;
        } else return services.encode(static_cast<SR>(
            Replacement(static_cast<NA>(decode<SA>(words, offsets[I]))...)));
    }
    struct Task { const Words* words; uint64_t result; };
    static void run(void* opaque) {
        auto& task = *static_cast<Task*>(opaque);
        task.result = invoke(*task.words, IndicesFor<SA...>{});
    }
    static uint64_t entry(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        Words words{}; words[0]=a; words[1]=b; words[2]=c; words[3]=d;
        for (size_t i=4; i<word_count; ++i) words[i] = services.read(i-4);
        Task task{&words, 0};
        services.require(services.api && services.api->invoke_callback(services.context, run, &task) == MF_OK,
                         "game hook: the framework did not run the replacement");
        return task.result;
    }
    struct Restore {
        Buffer<uint32_t, word_count-4> words{};
        size_t changed = 0;
        ~Restore() { for (size_t i=0; i<changed; ++i) services.write(i, words[i]); }
    };
    template<size_t... I> static Words pack(Indices<I...>, NA... args) {
        Words words{}; (encode<SA>(words, offsets[I], static_cast<SA>(args)), ...); return words;
    }
    static NR previous(NA... args) {
        auto original = reinterpret_cast<mf_game_core_function_v1>(
            __atomic_load_n(&previous_raw, __ATOMIC_ACQUIRE));
        services.require(original != nullptr, "game hook: the original game function is not bound yet");
        const auto words = pack(IndicesFor<SA...>{}, args...);
        Restore restore;
        for (size_t i=4; i<word_count; ++i) {
            restore.words[i-4] = services.read(i-4);
            services.write(i-4, words[i]); ++restore.changed;
        }
        uint64_t result = original(words[0], words[1], words[2], words[3]);
        // A one-word result leaves r1 undefined.
        if constexpr (!std::is_void_v<SR>) if (source_words<SR> == 1) result = static_cast<uint32_t>(result);
        if constexpr (!std::is_void_v<NR>) return static_cast<NR>(services.template decode<SR>(result));
    }
    inline static const mf_game_hook_adapter_v1 adapter{
        sizeof(mf_game_hook_adapter_v1), MF_GAME_ABI_ARM32_BASE,
        reinterpret_cast<void*>(Replacement), entry, &previous_raw, reinterpret_cast<void*>(previous)};
    static mf_result_v1 install(const mf_api_v1* api, const mf_mod_context_v1* context,
                                const mf_function_hook_v1* request) {
        if (!services.bind(api, context)) return MF_ERROR_DENIED;
        return api->hook_function_compatible(context, request, &adapter);
    }
};
template<class SourceSignature, auto Replacement,
         class Tag = std::integral_constant<decltype(Replacement), Replacement>>
using GameHook = Hook<SourceSignature, decltype(Replacement), Replacement, Tag>;
} // namespace mf::compat
