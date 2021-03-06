#define _WIN32_WINNT _WIN32_WINNT_WIN7
#include <filesystem>
#include <sys/stat.h>
#include <windows.h>
#include "map_loading.hpp"
#include "laa.hpp"
#include "../chimera.hpp"
#include "../signature/signature.hpp"
#include "../signature/hook.hpp"
#include "../output/output.hpp"
#include "../halo_data/game_engine.hpp"
#include "../halo_data/map.hpp"
#include "../halo_data/tag.hpp"
#include "../config/ini.hpp"
#include <chrono>

namespace Invader::Compression {
    std::size_t decompress_map_file(const char *input, const char *output);
    std::size_t decompress_map_file(const char *input, std::byte *output, std::size_t output_size);
}

#define BYTES_TO_MiB(bytes) (bytes / 1024.0F / 1024.0F)

namespace Chimera {
    static bool do_maps_in_ram = false;
    static bool do_benchmark = false;

    std::byte *maps_in_ram_region = nullptr;
    static std::byte *ui_region = nullptr;

    static constexpr std::size_t UI_OFFSET = 1024 * 1024 * 1024;
    static constexpr std::size_t UI_SIZE = 256 * 1024 * 1024;
    static constexpr std::size_t CHIMERA_MEMORY_ALLOCATION_SIZE = (UI_OFFSET + UI_SIZE);

    enum CacheFileEngine : std::uint32_t {
        CACHE_FILE_XBOX = 0x5,
        CACHE_FILE_DEMO = 0x6,
        CACHE_FILE_RETAIL = 0x7,
        CACHE_FILE_CUSTOM_EDITION = 0x261,
        CACHE_FILE_DARK_CIRCLET = 0x1A86,

        CACHE_FILE_DEMO_COMPRESSED = 0x861A0006,
        CACHE_FILE_RETAIL_COMPRESSED = 0x861A0007,
        CACHE_FILE_CUSTOM_EDITION_COMPRESSED = 0x861A0261
    };

    struct CompressedMapIndex {
        char map_name[32];
        std::uint64_t date_modified;
    };

    static bool header_is_valid_for_this_game(const std::byte *header, bool *compressed = nullptr, const char *map_name = nullptr) {
        const auto &header_full_version = *reinterpret_cast<const MapHeader *>(header);
        const auto &header_demo_version = *reinterpret_cast<const MapHeaderDemo *>(header);

        // Copy everything lowercase
        char map_name_lowercase[sizeof(header_demo_version.name)] = {};
        std::size_t map_name_length = std::strlen(map_name);
        if(map_name_length >= sizeof(map_name_lowercase)) {
            return false; // the map is longer than 31 characters, thus it's a meme
        }
        for(std::size_t i = 0; i < map_name_length; i++) {
            map_name_lowercase[i] = std::tolower(map_name[i]);
        }

        // Set everything to lowercase
        char demo_name[sizeof(header_demo_version.name)] = {};
        char full_name[sizeof(header_full_version.name)] = {};
        for(std::size_t i = 0; i < sizeof(demo_name); i++) {
            demo_name[i] = std::tolower(header_demo_version.name[i]);
        }
        for(std::size_t i = 0; i < sizeof(full_name); i++) {
            full_name[i] = std::tolower(header_full_version.name[i]);
        }

        // Blorp
        demo_name[31] = 0;
        full_name[31] = 0;

        bool header_full_version_valid = header_full_version.head == MapHeader::HEAD_LITERAL && header_full_version.foot == MapHeader::FOOT_LITERAL && std::strcmp(full_name, map_name_lowercase) == 0;
        bool header_demo_version_valid = header_demo_version.head == MapHeaderDemo::HEAD_LITERAL && header_demo_version.foot == MapHeaderDemo::FOOT_LITERAL && std::strcmp(demo_name, map_name_lowercase) == 0;

        switch(game_engine()) {
            case GAME_ENGINE_DEMO:
                if(header_demo_version.engine_type == CACHE_FILE_DEMO) {
                    if(compressed) {
                        *compressed = false;
                    }
                    return header_demo_version_valid;
                }
                else if(header_full_version.engine_type == CACHE_FILE_DEMO_COMPRESSED) {
                    if(compressed) {
                        *compressed = true;
                    }
                    return header_full_version_valid;
                }
                else {
                    return false;
                }
            case GAME_ENGINE_CUSTOM_EDITION:
                if(header_full_version.engine_type == CACHE_FILE_CUSTOM_EDITION) {
                    if(compressed) {
                        *compressed = false;
                    }
                    return header_full_version_valid;
                }
                else if(header_full_version.engine_type == CACHE_FILE_CUSTOM_EDITION_COMPRESSED) {
                    if(compressed) {
                        *compressed = true;
                    }
                    return header_full_version_valid;
                }
                else {
                    return false;
                }
            case GAME_ENGINE_RETAIL:
                if(header_full_version.engine_type == CACHE_FILE_RETAIL) {
                    if(compressed) {
                        *compressed = false;
                    }
                    return header_full_version_valid;
                }
                else if(header_full_version.engine_type == CACHE_FILE_RETAIL_COMPRESSED) {
                    if(compressed) {
                        *compressed = true;
                    }
                    return header_full_version_valid;
                }
                else {
                    return false;
                }
        }
        return false;
    }

    static bool header_is_valid_for_this_game(const char *path, bool *compressed = nullptr, const char *map_name = nullptr) {
        // Open the map
        std::FILE *f = std::fopen(path, "rb");
        if(!f) {
            return false;
        }

        // Read the header
        std::byte header[0x800];
        if(std::fread(header, sizeof(header), 1, f) != 1) {
            std::fclose(f);
            return false;
        }

        // Close
        std::fclose(f);

        return header_is_valid_for_this_game(header, compressed, map_name);
    }

    extern "C" void do_free_map_handle_bugfix(HANDLE &handle) {
        if(handle) {
            CloseHandle(handle);
            handle = 0;
        }
    }

    // Hold my compressed map...
    static CompressedMapIndex compressed_maps[2] = {};
    static std::size_t last_loaded_map = 0;

    static void get_tmp_path(const CompressedMapIndex &compressed_map, char *buffer) {
        std::snprintf(buffer, MAX_PATH, "%s\\tmp_%zu.map", get_chimera().get_path(), &compressed_map - compressed_maps);
    }

    static void preload_assets_into_memory_buffer(std::byte *buffer, std::size_t buffer_used, std::size_t buffer_size, const char *map_name) noexcept;
    static char currently_loaded_map[32] = {};

    extern "C" void do_map_loading_handling(char *map_path, const char *map_name) {
        static bool ui_was_loaded = false;

        // If the map is already loaded, go away
        bool do_not_reload = (do_maps_in_ram && ((std::strcmp(map_name, "ui") == 0 && ui_was_loaded) || (std::strcmp(map_name, currently_loaded_map) == 0)));

        const char *new_path = path_for_map(map_name);
        if(new_path) {
            // Check if the map is valid. If not, don't worry about it
            bool compressed;
            bool valid = header_is_valid_for_this_game(new_path, &compressed, map_name);
            if(!valid) {
                std::exit(1);
            }

            std::size_t buffer_used = 0;
            std::size_t buffer_size = 0;
            std::byte *buffer = nullptr;

            if(!do_not_reload) {
                if(compressed) {
                    // Get filesystem data
                    struct stat64 s;
                    stat64(new_path, &s);
                    std::uint64_t mtime = s.st_mtime;

                    char tmp_path[MAX_PATH] = {};

                    // See if we can find it
                    if(!do_maps_in_ram) {
                        for(auto &map : compressed_maps) {
                            if(std::strcmp(map_name, map.map_name) == 0 && map.date_modified == mtime) {
                                get_tmp_path(map, tmp_path);
                                //console_output("Didn't need to decompress %s -> %s", new_path, tmp_path);
                                last_loaded_map = &map - compressed_maps;
                                std::strncpy(map_path, tmp_path, MAX_PATH);
                                return;
                            }
                        }
                    }

                    // Attempt to decompress
                    std::size_t new_index = !last_loaded_map;
                    auto &compressed_map_to_use = compressed_maps[new_index];
                    try {
                        get_tmp_path(compressed_maps[new_index], tmp_path);
                        //console_output("Trying to compress %s @ %s -> %s...", map_name, new_path, tmp_path);
                        auto start = std::chrono::steady_clock::now();

                        // If we're doing maps in RAM, output directly to the region allowed
                        if(do_maps_in_ram) {
                            std::size_t offset;
                            if(std::strcmp(map_name, "ui") == 0) {
                                buffer_size = UI_SIZE;
                                offset = UI_OFFSET;
                            }
                            else {
                                buffer_size = UI_OFFSET;
                                offset = 0;
                            }

                            buffer = maps_in_ram_region + offset;
                            buffer_used = Invader::Compression::decompress_map_file(new_path, buffer, buffer_size);
                        }

                        // Otherwise do a map file
                        else {
                            Invader::Compression::decompress_map_file(new_path, tmp_path);
                        }
                        auto end = std::chrono::steady_clock::now();

                        // Benchmark
                        if(do_benchmark) {
                            console_output("Decompressed %s in %zu ms", map_name, std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
                        }

                        // If we're not doing maps in RAM, change the path to the tmp file, increment loaded maps by 1
                        if(!do_maps_in_ram) {
                            std::strcpy(map_path, tmp_path);
                            last_loaded_map++;
                            if(last_loaded_map > sizeof(compressed_maps) / sizeof(*compressed_maps)) {
                                last_loaded_map = 0;
                            }
                            compressed_map_to_use.date_modified = mtime;
                            std::strncpy(compressed_map_to_use.map_name, map_name, sizeof(compressed_map_to_use.map_name));
                        }
                    }
                    catch (std::exception &e) {
                        compressed_map_to_use = {};
                        //console_output("Failed to decompress %s @ %s: %s", map_name, new_path, e.what());
                        return;
                    }
                }
                else if(do_maps_in_ram) {
                    std::size_t offset;
                    if(std::strcmp(map_name, "ui") == 0) {
                        buffer_size = UI_SIZE;
                        offset = UI_OFFSET;
                    }
                    else {
                        buffer_size = UI_OFFSET;
                        offset = 0;
                    }

                    std::FILE *f = std::fopen(new_path, "rb");
                    if(!f) {
                        return;
                    }
                    buffer = maps_in_ram_region + offset;
                    std::fseek(f, 0, SEEK_END);
                    buffer_used = std::ftell(f);
                    std::fseek(f, 0, SEEK_SET);
                    std::fread(buffer, buffer_size, 1, f);
                    std::fclose(f);
                }

                // Load everything from bitmaps.map and sounds.map that can fit
                if(do_maps_in_ram) {
                    preload_assets_into_memory_buffer(buffer, buffer_used, buffer_size, map_name);

                    if(std::strcmp(map_name, "ui") == 0) {
                        ui_was_loaded = true;
                    }
                    else {
                        std::fill(currently_loaded_map, currently_loaded_map + sizeof(currently_loaded_map), 0);
                        std::strncpy(currently_loaded_map, map_name, sizeof(currently_loaded_map) - 1);
                    }
                }
            }

            std::strcpy(map_path, new_path);
        }
    }

    const char *path_for_map(const char *map, bool tmp) noexcept {
        static char path[MAX_PATH];
        if(tmp) {
            for(auto &compressed_map : compressed_maps) {
                if(std::strcmp(map,compressed_map.map_name) == 0) {
                    get_tmp_path(compressed_map, path);
                    return path;
                }
            }
        }

        #define RETURN_IF_FOUND(...) std::snprintf(path, sizeof(path), __VA_ARGS__, map); if(std::filesystem::exists(path)) return path;
        RETURN_IF_FOUND("maps\\%s.map");
        RETURN_IF_FOUND("%s\\maps\\%s.map", get_chimera().get_path());
        return nullptr;
    }

    extern std::uint32_t calculate_crc32_of_map_file(std::FILE *f, const MapHeader &header) noexcept;
    std::uint32_t maps_in_ram_crc32;

    static void preload_assets_into_memory_buffer(std::byte *buffer, std::size_t buffer_used, std::size_t buffer_size, const char *map_name) noexcept {
        auto start = std::chrono::steady_clock::now();

        // Get tag data info
        std::uint32_t tag_data_address = reinterpret_cast<std::uint32_t>(get_tag_data_address());
        std::byte *tag_data;
        std::uint32_t tag_data_size;
        auto engine = game_engine();

        if(engine == GameEngine::GAME_ENGINE_DEMO) {
            auto &header = *reinterpret_cast<MapHeaderDemo *>(buffer);
            tag_data = buffer + header.tag_data_offset;
            tag_data_size = header.tag_data_size;
        }
        else {
            auto &header = *reinterpret_cast<MapHeader *>(buffer);
            tag_data = buffer + header.tag_data_offset;
            tag_data_size = header.tag_data_size;

            // Calculate the CRC32 if we aren't a UI file
            if(engine == GameEngine::GAME_ENGINE_CUSTOM_EDITION && buffer == maps_in_ram_region) {
                maps_in_ram_crc32 = ~calculate_crc32_of_map_file(nullptr, header);
            }
        }
        bool can_load_indexed_tags = (buffer + buffer_used) == (tag_data + tag_data_size) && game_engine() == GameEngine::GAME_ENGINE_CUSTOM_EDITION;

        // Get the header
        #define TRANSLATE_POINTER(pointer, to_type) reinterpret_cast<to_type>(tag_data + reinterpret_cast<std::uintptr_t>(pointer) - tag_data_address)

        // Open bitmaps.map and sounds.map
        std::size_t old_used = buffer_used;
        std::FILE *bitmaps_file = std::fopen("maps/bitmaps.map", "rb");
        std::FILE *sounds_file = std::fopen("maps/sounds.map", "rb");
        if(!bitmaps_file || !sounds_file) {
            return;
        }

        TagDataHeader &header = *reinterpret_cast<TagDataHeader *>(tag_data);
        auto *tag_array = TRANSLATE_POINTER(header.tag_array, Tag *);

        std::size_t missed_data = 0;

        struct ResourceHeader {
            std::uint32_t type;
            std::uint32_t path_offset;
            std::uint32_t resource_offset;
            std::uint32_t resource_count;
        };

        struct Resource {
            char path[MAX_PATH];
            std::uint32_t data_size;
            std::uint32_t data_offset;
        };

        auto load_resource_map = [](std::vector<Resource> &resources, std::FILE *file) {
            struct ResourceInMap {
                std::uint32_t path_offset;
                std::uint32_t data_size;
                std::uint32_t data_offset;
            };

            ResourceHeader header;
            std::fread(&header, sizeof(header), 1, file);
            resources.reserve(header.resource_count);

            // Load resources
            std::vector<ResourceInMap> resources_in_map(header.resource_count);
            std::fseek(file, header.resource_offset, SEEK_SET);
            std::fread(resources_in_map.data(), resources_in_map.size() * sizeof(*resources_in_map.data()), 1, file);

            // Load it all!
            for(std::size_t r = 0; r < header.resource_count; r++) {
                auto &resource = resources.emplace_back();
                resource.data_size = resources_in_map[r].data_size;
                resource.data_offset = resources_in_map[r].data_offset;
                std::fseek(file, header.path_offset, SEEK_SET);
                std::fseek(file, resources_in_map[r].path_offset, SEEK_CUR);
                std::fread(resource.path, sizeof(resource.path), 1, file);
            }
        };

        // Preload any indexed tags, if possible
        if(can_load_indexed_tags) {
            std::vector<Resource> bitmaps;
            std::vector<Resource> sounds;

            load_resource_map(bitmaps, bitmaps_file);
            load_resource_map(sounds, sounds_file);

            for(std::size_t t = 0; t < header.tag_count; t++) {
                auto &tag = tag_array[t];
                if(!tag.indexed) {
                    continue;
                }
                if(tag.primary_class == TagClassInt::TAG_CLASS_BITMAP) {
                    std::size_t resource_tag_index = reinterpret_cast<std::uint32_t>(tag.data);

                    // If this is screwed up, exit!
                    if(resource_tag_index > bitmaps.size()) {
                        std::exit(1);
                    }

                    auto &resource = bitmaps[resource_tag_index];

                    // All right!
                    std::size_t new_used = buffer_used + resource.data_size;
                    if(resource.data_size > buffer_size || new_used > buffer_size) {
                        missed_data += resource.data_size;
                        continue;
                    }

                    auto *baseline = buffer + buffer_used;
                    std::uint32_t baseline_address = baseline - tag_data + tag_data_address;
                    tag.data = reinterpret_cast<std::byte *>(baseline_address);
                    std::fseek(bitmaps_file, resource.data_offset, SEEK_SET);
                    std::fread(baseline, resource.data_size, 1, bitmaps_file);

                    // Now fix the pointers for sequence data
                    auto &sequences_count = *reinterpret_cast<std::uint32_t *>(baseline + 0x54);
                    if(sequences_count) {
                        auto &sequences_ptr = *reinterpret_cast<std::uint32_t *>(baseline + 0x58);
                        sequences_ptr += baseline_address;
                        auto *sequences = TRANSLATE_POINTER(sequences_ptr, std::byte *);
                        for(std::uint32_t s = 0; s < sequences_count; s++) {
                            auto *sequence = sequences + s * 0x40;
                            auto &sprites_count = *reinterpret_cast<std::uint32_t *>(sequence + 0x34);
                            if(sprites_count) {
                                auto &sprites = *reinterpret_cast<std::uint32_t *>(sequence + 0x38);
                                sprites += baseline_address;
                            }
                        }
                    }

                    // Fix bitmap data
                    auto &bitmap_data_count = *reinterpret_cast<std::uint32_t *>(baseline + 0x60);
                    if(bitmap_data_count) {
                        auto &bitmap_data_ptr = *reinterpret_cast<std::uint32_t *>(baseline + 0x64);
                        bitmap_data_ptr += baseline_address;
                        auto *bitmap_data = TRANSLATE_POINTER(bitmap_data_ptr, std::byte *);
                        for(std::size_t d = 0; d < bitmap_data_count; d++) {
                            auto *bitmap = bitmap_data + d * 0x30;
                            *reinterpret_cast<TagID *>(bitmap + 0x20) = tag.id;
                        }
                    }

                    buffer_used = new_used;
                    tag.indexed = 0;
                }
                else if(tag.primary_class == TagClassInt::TAG_CLASS_SOUND) {
                    std::optional<std::size_t> resource_tag_index;
                    const char *path = TRANSLATE_POINTER(tag.path, const char *);

                    for(auto &s : sounds) {
                        if(std::strcmp(path, s.path) == 0) {
                            resource_tag_index = &s - sounds.data();
                            break;
                        }
                    }

                    // If this is screwed up, exit!
                    if(!resource_tag_index.has_value()) {
                        std::exit(1);
                    }

                    // Load sounds
                    auto &resource = sounds[*resource_tag_index];
                    static constexpr std::size_t SOUND_HEADER_SIZE = 0xA4;
                    std::size_t bytes_to_read = resource.data_size - SOUND_HEADER_SIZE;
                    std::size_t new_used = buffer_used + bytes_to_read;
                    if(bytes_to_read > buffer_size || new_used > buffer_size) {
                        missed_data += bytes_to_read;
                        continue;
                    }

                    auto *baseline = buffer + buffer_used;
                    std::uint32_t baseline_address = baseline - tag_data + tag_data_address;
                    std::fseek(sounds_file, resource.data_offset, SEEK_SET);
                    std::byte base_sound_data[SOUND_HEADER_SIZE];
                    std::fread(base_sound_data, sizeof(base_sound_data), 1, sounds_file);
                    std::fread(baseline, bytes_to_read, 1, sounds_file);

                    auto *sound_data = TRANSLATE_POINTER(tag.data, std::byte *);
                    auto &pitch_range_count = *reinterpret_cast<std::uint32_t *>(sound_data + 0x98);

                    // Set encoding stuff
                    *reinterpret_cast<std::uint32_t *>(sound_data + 0x6C) = *reinterpret_cast<std::uint32_t *>(base_sound_data + 0x6C);

                    // Set sample rate
                    *reinterpret_cast<std::uint16_t *>(sound_data + 0x6) = *reinterpret_cast<std::uint16_t *>(base_sound_data + 0x6);

                    if(pitch_range_count) {
                        *reinterpret_cast<std::uint32_t *>(sound_data + 0x9C) = baseline_address;

                        // Fix the pointers
                        for(std::size_t p = 0; p < pitch_range_count; p++) {
                            auto *pitch_range = baseline + p * 0x48;
                            auto &permutation_count = *reinterpret_cast<std::uint32_t *>(pitch_range + 0x3C);
                            auto &permutation_ptr = *reinterpret_cast<std::uint32_t *>(pitch_range + 0x40);
                            *reinterpret_cast<std::uint32_t *>(pitch_range + 0x34) = 0xFFFFFFFF;
                            *reinterpret_cast<std::uint32_t *>(pitch_range + 0x38) = 0xFFFFFFFF;

                            if(permutation_count) {
                                permutation_ptr += baseline_address;
                                auto *permutations = TRANSLATE_POINTER(permutation_ptr, std::byte *);
                                for(std::size_t r = 0; r < permutation_count; r++) {
                                    auto *permutation = permutations + r * 0x7C;
                                    *reinterpret_cast<std::uint32_t *>(permutation + 0x2C) = 0xFFFFFFFF;
                                    *reinterpret_cast<std::uint32_t *>(permutation + 0x30) = 0;
                                    *reinterpret_cast<std::uint32_t *>(permutation + 0x34) = *reinterpret_cast<std::uint32_t *>(&tag.id);
                                    *reinterpret_cast<std::uint32_t *>(permutation + 0x3C) = *reinterpret_cast<std::uint32_t *>(&tag.id);

                                    auto &mouth_data = *reinterpret_cast<std::uint32_t *>(permutation + 0x54 + 0xC);
                                    auto &subtitle_data = *reinterpret_cast<std::uint32_t *>(permutation + 0x68 + 0xC);

                                    if(mouth_data) {
                                        mouth_data += baseline_address;
                                    }

                                    if(subtitle_data) {
                                        subtitle_data += baseline_address;
                                    }
                                }
                            }
                        }
                    }

                    buffer_used = new_used;
                    tag.indexed = 0;
                }
            }

            // Add up the difference
            std::size_t bytes_added = (buffer_used - old_used);
            reinterpret_cast<MapHeader *>(buffer)->tag_data_size += bytes_added;
        }

        // Preload all of the assets
        for(std::size_t t = 0; t < header.tag_count; t++) {
            auto &tag = tag_array[t];
            if(tag.indexed) {
                continue;
            }
            auto *data = TRANSLATE_POINTER(tag.data, std::byte *);
            if(tag.primary_class == TagClassInt::TAG_CLASS_BITMAP) {
                auto &bitmap_count = *reinterpret_cast<std::uint32_t *>(data + 0x60);
                auto *bitmap_data = TRANSLATE_POINTER(*reinterpret_cast<std::uint32_t *>(data + 0x64), std::byte *);
                for(std::uint32_t b = 0; b < bitmap_count; b++) {
                    auto *bitmap = bitmap_data + b * 48;
                    std::uint8_t &external = *reinterpret_cast<std::uint8_t *>(bitmap + 0xF);
                    if(!(external & 1)) {
                        continue;
                    }

                    // Get metadata
                    std::uint32_t bitmap_size = *reinterpret_cast<std::uint32_t *>(bitmap + 0x1C);
                    std::uint32_t &bitmap_offset = *reinterpret_cast<std::uint32_t *>(bitmap + 0x18);

                    // Don't add this bitmap if we can't fit it
                    std::size_t new_used = buffer_used + bitmap_size;
                    if(bitmap_size > buffer_size || new_used > buffer_size) {
                        missed_data += bitmap_size;
                        continue;
                    }

                    // Read the bitmap data and set the external flag to 0
                    std::fseek(bitmaps_file, bitmap_offset, SEEK_SET);
                    std::fread(buffer + buffer_used, bitmap_size, 1, bitmaps_file);
                    bitmap_offset = buffer_used;
                    buffer_used = new_used;
                    external ^= 1;
                }
            }
            else if(tag.primary_class == TagClassInt::TAG_CLASS_SOUND) {
                auto &pitch_range_count = *reinterpret_cast<std::uint32_t *>(data + 0x98);
                auto *pitch_range_data = TRANSLATE_POINTER(*reinterpret_cast<std::uint32_t *>(data + 0x9C), std::byte *);

                for(std::uint32_t r = 0; r < pitch_range_count; r++) {
                    auto *pitch_range = pitch_range_data + r * 0x48;
                    auto &permutation_count = *reinterpret_cast<std::uint32_t *>(pitch_range + 0x3C);
                    auto *permutation_data = TRANSLATE_POINTER(*reinterpret_cast<std::uint32_t *>(pitch_range + 0x40), std::byte *);
                    for(std::uint32_t p = 0; p < permutation_count; p++) {
                        auto *permutation = permutation_data + 0x7C * p;
                        std::uint32_t &external = *reinterpret_cast<std::uint32_t *>(permutation + 0x44);
                        if(!(external & 1)) {
                            continue;
                        }

                        // Get metadata
                        std::uint32_t sound_size = *reinterpret_cast<std::uint32_t *>(permutation + 0x40);
                        std::uint32_t &sound_offset = *reinterpret_cast<std::uint32_t *>(permutation + 0x48);

                        // Don't add this sound if we can't fit it
                        std::size_t new_used = buffer_used + sound_size;
                        if(sound_size > buffer_size || new_used > buffer_size) {
                            missed_data += sound_size;
                            continue;
                        }

                        // Read the sound data and set the external flag to 0
                        std::fseek(sounds_file, sound_offset, SEEK_SET);
                        std::fread(buffer + buffer_used, sound_size, 1, sounds_file);
                        sound_offset = buffer_used;
                        buffer_used = new_used;
                        external ^= 1;
                    }
                }
            }
        }

        std::fclose(bitmaps_file);
        std::fclose(sounds_file);

        auto end = std::chrono::steady_clock::now();

        if(do_benchmark) {
            if(missed_data) {
                console_error("Failed to preload %.02f MiB due to insufficient capacity", BYTES_TO_MiB(missed_data));
            }

            std::size_t total_preloaded = buffer_used - old_used;
            std::size_t count = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            console_output("Preloaded %.02f MiB from %s in %u ms (%.02f MiB / %.02f MiB - %.01f%%)", BYTES_TO_MiB(total_preloaded), map_name, count, BYTES_TO_MiB(buffer_used), BYTES_TO_MiB(buffer_size), 100.0F * buffer_used / buffer_size);
        }
    }

    extern "C" void map_loading_asm();
    extern "C" void free_map_handle_bugfix_asm();

    extern "C" void on_read_map_file_data_asm();
    extern "C" int on_read_map_file_data(HANDLE file_descriptor, std::byte *output, std::size_t size, LPOVERLAPPED overlapped) {
        std::size_t file_offset = overlapped->Offset;
        char file_name[MAX_PATH + 1] = {};
        GetFinalPathNameByHandle(file_descriptor, file_name, sizeof(file_name) - 1, 0);

        char *last_backslash = nullptr;
        char *last_dot = nullptr;
        for(char &c : file_name) {
            if(c == '.') {
                last_dot = &c;
            }
            if(c == '\\' || c == '/') {
                last_backslash = &c;
            }
        }

        // Is the path bullshit?
        if(!last_backslash || !last_dot || last_dot < last_backslash) {
            return 0;
        }

        *last_dot = 0;
        char *map_name = last_backslash + 1;

        if(std::strcmp(map_name, "ui") == 0) {
            std::copy(ui_region + file_offset, ui_region + file_offset + size, output);
            return 1;
        }
        else if(std::strcmp(map_name, currently_loaded_map) == 0) {
            std::copy(maps_in_ram_region + file_offset, maps_in_ram_region + file_offset + size, output);
            return 1;
        }

        return 0;
    }

    void set_up_map_loading() {
        static Hook hook;
        auto &map_load_path_sig = get_chimera().get_signature("map_load_path_sig");
        write_jmp_call(map_load_path_sig.data(), hook, nullptr, reinterpret_cast<const void *>(map_loading_asm));
        static Hook hook2;
        auto &create_file_mov_sig = get_chimera().get_signature("create_file_mov_sig");
        write_jmp_call(create_file_mov_sig.data(), hook2, reinterpret_cast<const void *>(free_map_handle_bugfix_asm), nullptr);

        // Make Halo not check the maps if they're bullshit
        static unsigned char return_1[6] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
        auto *map_check_sig = get_chimera().get_signature("map_check_sig").data();
        overwrite(map_check_sig, return_1, sizeof(return_1));

        // Get settings
        auto is_enabled = [](const char *what) -> bool {
            const char *value = get_chimera().get_ini()->get_value(what);
            return !(!value || std::strcmp(value, "1") != 0);
        };

        do_maps_in_ram = is_enabled("memory.enable_map_memory_buffer");
        do_benchmark = is_enabled("memory.benchmark");

        if(do_maps_in_ram) {
            if(!current_exe_is_laa_patched()) {
                MessageBox(0, "Map memory buffers requires an large address aware-patched executable.", "Error", 0);
                std::exit(1);
            }

            // Allocate memory, making sure to not do so after the 0x40000000 - 0x50000000 region used for tag data
            for(auto *m = reinterpret_cast<std::byte *>(0x80000000); m < reinterpret_cast<std::byte *>(0xF0000000) && !maps_in_ram_region; m += 0x10000000) {
                maps_in_ram_region = reinterpret_cast<std::byte *>(VirtualAlloc(m, CHIMERA_MEMORY_ALLOCATION_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            }

            if(!maps_in_ram_region) {
                char error_text[256] = {};
                std::snprintf(error_text, sizeof(error_text), "Failed to allocate %.02f GiB for map memory buffers.", BYTES_TO_MiB(CHIMERA_MEMORY_ALLOCATION_SIZE) / 1024.0F);
                MessageBox(0, error_text, "Error", 0);
                std::exit(1);
            }

            ui_region = maps_in_ram_region + UI_OFFSET;

            static Hook read_cache_file_data_hook;
            auto &read_map_file_data_sig = get_chimera().get_signature("read_map_file_data_sig");
            write_jmp_call(read_map_file_data_sig.data(), read_cache_file_data_hook, reinterpret_cast<const void *>(on_read_map_file_data_asm), nullptr);
        }
    }
}
