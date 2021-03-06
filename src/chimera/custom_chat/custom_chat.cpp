#include <windows.h>
#include <chrono>
#include <cstring>
#include <locale>
#include <codecvt>
#include <cwchar>
#include <regex>
#include "../event/frame.hpp"
#include "../event/tick.hpp"
#include "../signature/hook.hpp"
#include "../signature/signature.hpp"
#include "../output/draw_text.hpp"
#include "../output/output.hpp"
#include "../chimera.hpp"
#include "../halo_data/player.hpp"
#include "../halo_data/server.hpp"
#include "../halo_data/tag.hpp"
#include "../halo_data/multiplayer.hpp"
#include "../config/ini.hpp"
#include "../localization/localization.hpp"
#include "../console/console.hpp"

extern "C" void on_multiplayer_message(const wchar_t *message);
extern "C" void on_chat_message(const wchar_t *message);
extern "C" void on_chat_button(int channel);
extern "C" void chat_out_asm(int channel, const wchar_t *message);
extern "C" void on_kill_feed(wchar_t *a, std::uint32_t b, wchar_t *c);
const void *kill_feed_message;

namespace Chimera {
    static void on_custom_chat_frame() noexcept;
    static void on_chat_input() noexcept;
    static void enable_input(bool enabled) noexcept;
    static bool custom_chat_initialized = false;
    static const char *color_id_for_player(std::uint8_t player, ColorARGB *color_to_use);
    static void check_for_quit_players();
    static void load_chat_settings();
    static const wchar_t *special_name_color(const wchar_t *name);

    // Initialize the stuff
    void initialize_custom_chat() noexcept {
        // First, make this function do nothing
        const SigByte goodbye_code[] = { 0xC3, 0x90, 0x90, 0x90, 0x90 };
        write_code_s(get_chimera().get_signature("multiplayer_message_sig").data(), goodbye_code);

        // Make the multiplayer hook
        static Hook chat_hook;
        write_jmp_call(get_chimera().get_signature("on_multiplayer_chat_sig").data(), chat_hook, nullptr, reinterpret_cast<const void *>(on_chat_message));

        // Make the hook for pressing the chat key
        static Hook chat_key_hook;
        const void *old_fn;
        write_function_override(get_chimera().get_signature("chat_open_sig").data(), chat_key_hook, reinterpret_cast<const void *>(on_chat_button), &old_fn);

        // Make the hook for pressing the chat key
        static Hook key_press_hook;
        write_jmp_call(get_chimera().get_signature("on_key_press_sig").data(), key_press_hook, nullptr, reinterpret_cast<const void *>(on_chat_input));

        add_preframe_event(on_custom_chat_frame);
        add_pretick_event(check_for_quit_players);

        // Make the hook for handling kill feed things
        static Hook kill_feed_hook;
        write_function_override(get_chimera().get_signature("kill_feed_sig").data(), kill_feed_hook, reinterpret_cast<const void *>(on_kill_feed), &kill_feed_message);

        load_chat_settings();

        custom_chat_initialized = true;
    }

    bool custom_chat_enabled() noexcept {
        return custom_chat_initialized;
    }

    typedef std::chrono::high_resolution_clock clock;

    static float seconds_since_time(const clock::time_point &time) noexcept {
        auto duration = clock::now() - time;
        return static_cast<float>(duration.count()) / static_cast<float>(decltype(duration)::period::den);
    }

    #define MAX_MESSAGE_LENGTH 256
    struct ChatMessage {
        /** Message text */
        char message[MAX_MESSAGE_LENGTH];

        /** Just for padding */
        std::uint32_t zero = 0;

        /** Time message was created */
        clock::time_point created;

        /** Color to use */
        ColorARGB color = {1.0, 1.0, 1.0, 1.0};

        /**
         * Return true if valid
         * @return true if valid
         */
        bool valid() {
            return *this->message;
        }

        /**
         * Return time in seconds since message creation
         * @return time in seconds since message creation
         */
        float time_since_creation() {
            return seconds_since_time(created);
        }
    };

    static void add_message_to_array(ChatMessage *array, const ChatMessage &message);

    #define MESSAGE_BUFFER_SIZE 256
    static ChatMessage server_messages[MESSAGE_BUFFER_SIZE] = {};
    static ChatMessage chat_messages[MESSAGE_BUFFER_SIZE] = {};

    ////////////////////////////////////////////////////////////////////////////
    // Server messages
    ////////////////////////////////////////////////////////////////////////////
    static ColorARGB server_message_color = {0.65, 0.25, 0.9, 0.25};
    static std::uint16_t server_message_x = 310;
    static std::uint16_t server_message_y = 30;
    static std::uint16_t server_message_w = 300;
    static std::uint16_t server_message_h = 80;
    static std::uint16_t server_message_h_chat_open = 160;
    static TextAnchor server_message_anchor = TextAnchor::ANCHOR_TOP_RIGHT;
    static bool server_message_hide_on_console = false;
    static GenericFont server_message_font = GenericFont::FONT_CONSOLE;

    ////////////////////////////////////////////////////////////////////////////
    // Chat messages
    ////////////////////////////////////////////////////////////////////////////
    static ColorARGB chat_message_color_ffa = {0.85, 0.85, 1.0, 0.85};
    static ColorARGB chat_message_color_red = {0.85, 1.0, 0.85, 0.85};
    static ColorARGB chat_message_color_blue = {0.85, 0.85, 0.85, 1.0};
    static std::uint16_t chat_message_x = 8;
    static std::uint16_t chat_message_y = 38;
    static std::uint16_t chat_message_w = 612;
    static std::uint16_t chat_message_h = 300;
    static std::uint16_t chat_message_h_chat_open = 300;
    static TextAnchor chat_message_anchor = TextAnchor::ANCHOR_TOP_LEFT;
    static bool chat_message_hide_on_console = true;
    static GenericFont chat_message_font = GenericFont::FONT_CONSOLE;

    ////////////////////////////////////////////////////////////////////////////
    // Chat input
    ////////////////////////////////////////////////////////////////////////////
    static ColorARGB chat_input_color = {1.0, 1.0, 1.0, 1.0};
    static std::uint16_t chat_input_x = 8;
    static std::uint16_t chat_input_y = 338;
    static std::uint16_t chat_input_w = 612;
    static TextAnchor chat_input_anchor = TextAnchor::ANCHOR_TOP_LEFT;
    static GenericFont chat_input_font = GenericFont::FONT_CONSOLE;

    static float slide_time_length = 0.25;
    static float time_up = 5.0;
    static float fade_out_time = 1.0;

    static std::size_t chat_message_scroll = 0;
    static bool block_ips = false;

    #define INPUT_BUFFER_SIZE 64
    static char chat_input_buffer[INPUT_BUFFER_SIZE];
    static std::size_t chat_input_cursor = 0;
    static int chat_input_channel = 0;
    static bool chat_input_open = false;
    static clock::time_point chat_open_state_changed;

    static bool player_in_server[16] = {};
    static char player_name[16][14];

    static bool show_chat_color_help = false;
    void set_show_color_help(bool show_help) noexcept {
        show_chat_color_help = show_help;
    }
    bool show_color_help() noexcept {
        return show_chat_color_help;
    }

    static void on_custom_chat_frame() noexcept {
        auto handle_messages = [](auto array, auto x, auto y, auto w, auto h, auto anchor, bool ignore_age, std::size_t scroll, GenericFont font_type) {
            // Define the font
            auto font = get_generic_font(font_type);
            std::uint16_t line_height = font_pixel_height(font);
            if(line_height == 0) {
                line_height = 1;
            }

            // Calculate the offset for the first (bottom) line
            std::uint16_t y_offset = y + h;

            // Find out how many lines we can have. Either it'll be do to pixel space requirements (most likely) or due to the maximum buffer size
            std::size_t max_lines = MESSAGE_BUFFER_SIZE - 1;
            std::size_t max_lines_height = h / line_height;
            std::size_t max_lines_actual = max_lines;
            if(max_lines_actual > max_lines_height) {
                max_lines_actual = max_lines_height;
            }

            // Find out if we have a brand new one
            float first_time_alive = array[0].time_since_creation();
            std::size_t new_count = 0;
            if(first_time_alive < slide_time_length) {
                // Find how many are also new
                new_count = 1;
                for(std::size_t i = 1; i < max_lines; i++) {
                    if(array[i].time_since_creation() < slide_time_length) {
                        new_count++;
                    }
                    else {
                        break;
                    }
                }
            }

            // Figure out how many we're going through
            std::size_t max_lines_iteration = max_lines_actual + new_count;
            if(max_lines_iteration > max_lines) {
                max_lines_iteration = max_lines;
            }

            if(scroll == 0) {
                // Increase the y value by how many new lines are coming in multiplied by the slide timing, but only if we're not scrolling
                y_offset += (new_count * line_height) - (first_time_alive / slide_time_length) * (new_count * line_height);
            }

            for(std::size_t i = scroll; i < max_lines_iteration + scroll; i++) {
                if(i >= max_lines) {
                    break;
                }

                if(!array[i].valid()) {
                    continue;
                }

                // Check if this is too old
                float time_alive = array[i].time_since_creation();
                if(!ignore_age && time_alive >= time_up) {
                    continue;
                }

                // Anyway, make the opacity based on whether it's fading out or in
                ColorARGB color = array[i].color;
                if(!ignore_age && time_alive > time_up - fade_out_time) {
                    color.alpha *= 1.0 - (time_alive - (time_up - fade_out_time)) / fade_out_time;
                }
                if(time_alive < slide_time_length) {
                    color.alpha *= time_alive / slide_time_length;
                }

                // Print the damn thing
                if(color.alpha > 0.0) {
                    apply_text_quake_colors(std::string(array[i].message), x, y_offset, w, line_height, color, font, anchor);
                }

                y_offset -= line_height;
            }
        };

        bool console_is_open = get_console_open();

        if((!console_is_open || !server_message_hide_on_console) && !server_messages_blocked()) {
            handle_messages(server_messages, server_message_x, server_message_y, server_message_w, chat_input_open ? server_message_h_chat_open : server_message_h, server_message_anchor, chat_input_open, 0, server_message_font);
        }
        if(!console_is_open || !chat_message_hide_on_console) {
            handle_messages(chat_messages, chat_message_x, chat_message_y, chat_message_w, chat_input_open ? chat_message_h_chat_open : chat_message_h, chat_message_anchor, chat_input_open, chat_message_scroll, chat_message_font);
        }

        // Handle chat input
        if(chat_input_open) {
            char buffer_to_show[INPUT_BUFFER_SIZE * 2] = {};
            const char *channel_name;
            if(chat_input_channel == 0) {
                channel_name = "chimera_custom_chat_to_all";
            }
            else if(chat_input_channel == 1) {
                channel_name = "chimera_custom_chat_to_team";
            }
            else {
                channel_name = "chimera_custom_chat_to_vehicle";
            }

            // Define the font for chat input
            auto font = get_generic_font(chat_input_font);
            std::uint16_t line_height = font_pixel_height(font);
            if(line_height == 0) {
                line_height = 1;
            }

            std::size_t adjusted_y = chat_input_y + line_height * 2;

            std::snprintf(buffer_to_show, sizeof(buffer_to_show), "%s - %s", localize(channel_name), chat_input_buffer);
            apply_text_quake_colors(buffer_to_show, chat_input_x, adjusted_y, chat_input_w, line_height, chat_input_color, font, chat_input_anchor);

            std::size_t length_of_shown_buffer = std::strlen(buffer_to_show);
            std::size_t length_of_input_buffer = std::strlen(chat_input_buffer);
            std::size_t input_buffer_start = length_of_shown_buffer - length_of_input_buffer;
            std::size_t cursor_location = input_buffer_start + chat_input_cursor;
            buffer_to_show[cursor_location] = 0;
            std::size_t cursor_x = text_pixel_length(buffer_to_show, font);

            // Subtract color codes
            char cursor_code[4];
            cursor_code[0] = '^';
            cursor_code[1] = ';';
            cursor_code[2] = 0;
            for(std::size_t i = 0; i < cursor_location; i++) {
                if(buffer_to_show[i] == '^' && buffer_to_show[i + 1] != 0) {
                    if(buffer_to_show[i + 1] == '^') {
                        cursor_x -= text_pixel_length("^", font);
                        i++;
                        continue;
                    }

                    cursor_code[0] = buffer_to_show[i];
                    cursor_code[1] = buffer_to_show[i + 1];
                    cursor_x -= text_pixel_length(cursor_code, font);
                    i ++;
                }
            }
            cursor_code[2] = '_';
            cursor_code[3] = 0;

            apply_text_quake_colors(cursor_code, cursor_x + chat_input_x, adjusted_y, chat_input_w, line_height, chat_input_color, font, chat_input_anchor);

            if(show_chat_color_help) {
                const char *color_codes = "1234567890\nqwertyuiop QWERTYUIOP\nasdfghjkl ASDFGHJKL\nzxcvbnm ZXCVBNM";
                std::size_t help_y = adjusted_y + line_height;
                std::size_t help_x = chat_input_x;
                for(const char *code = color_codes; *code; code++) {
                    if(*code == '\n') {
                        help_y += line_height;
                        help_x = chat_input_x;
                    }
                    else if(*code != ' ') {
                        char code_text[] = {'^', *code, '^', '^', *code, 0};
                        apply_text_quake_colors(code_text, help_x, help_y, chat_input_w, line_height, chat_input_color, font, chat_input_anchor);
                    }
                    char code_space[] = {'^', *code, 0};
                    help_x += text_pixel_length(code_space, font);
                }
            }
        }
    }

    static void initialize_chat_message(ChatMessage &message, const char *message_text, const ColorARGB &color);

    static void u16_to_u8(char *u8, std::size_t u8_len, const wchar_t *u16) {
        std::size_t i;
        for(i = 0; i < u8_len - 1; i++) {
            u8[i] = static_cast<char>(u16[i]);
            if(!*u8) {
                break;
            }
        }
        u8[i + 1] = 0;
    }

    // Determine what color ID to use for a player
    static const char *color_id_for_player(std::uint8_t player, ColorARGB *color_to_use) {
        auto &player_info = ServerInfoPlayerList::get_server_info_player_list()->players[player];
        std::uint8_t armor_color = player_info.armor_color;
        std::uint8_t team = player_info.team;

        // Correct these values (SAPP's st command may have memed them)
        auto *player_table_player = player_info.get_player_table_player();
        if(player_table_player) {
            team = player_table_player->team;
        }

        const char *name_color_to_use = ";";
        if(is_team()) {
            if(team == 0) {
                name_color_to_use = "<";
                if(color_to_use) {
                    *color_to_use = chat_message_color_red;
                }
            }
            else {
                name_color_to_use = ">";
                if(color_to_use) {
                    *color_to_use = chat_message_color_blue;
                }
            }
        }
        else {
            static const char *armor_colors[] = {
                "m", // white
                "0", // black
                "1", // red
                "4", // blue
                "9", // gray
                "3", // yellow
                "V", // green
                "J", // pink
                "H", // purple
                "5", // cyan
                "F", // cobalt
                "u", // orange
                "B", // teal
                "S", // sage
                "O", // brown
                "o", // tan
                "I", // maroon
                "K"  // salmon
            };
            if(armor_color >= sizeof(armor_colors) / sizeof(*armor_colors)) {
                armor_color = 0;
            }
            name_color_to_use = armor_colors[armor_color];
            if(color_to_use) {
                *color_to_use = chat_message_color_ffa;
            }
        }

        return name_color_to_use;
    }

    static void draw_chat_message(const char *message, std::uint32_t p_int, std::uint32_t c_int) {
        ChatMessage chat_message;

        std::string message_filtered;
        if(block_ips) {
            std::regex r("(\\d(\\^\\d)*){1,3}(\\.((\\^\\d)*\\d(\\&\\d)*){1,3}){3}");
            message_filtered = std::regex_replace(message, r, "#.#.#.#");
            message = message_filtered.data();
        }

        std::uint8_t player_index = static_cast<std::uint8_t>(p_int);
        std::uint8_t channel_index = static_cast<std::uint8_t>(c_int);

        // Ignore invalid channels
        if(channel_index > 3) {
            return;
        }

        // This is a server message. No need to format.
        if(channel_index == 3 || player_index > 15) {
            // First, get the length
            std::size_t length = std::strlen(message);
            char s[256] = {};
            if(length >= sizeof(s)) {
                length = 255;
            }

            // Next, let's see if we can break it up into lines
            auto font = get_generic_font(server_message_font);
            std::size_t pos = 0;
            std::size_t s_len = 0;
            std::size_t last_space = 0;
            for(; pos < length; pos++) {
                s[s_len] = message[pos];
                s[s_len + 1] = 0;
                if(s[s_len] == ' ') {
                    last_space = s_len;
                }
                if(text_pixel_length(s, font) > server_message_w) {
                    if(last_space) {
                        s[last_space] = 0;
                        pos -= (s_len - last_space);
                        s_len = 0;
                        last_space = 0;
                    }
                    else {
                        s[s_len] = 0;
                        s_len = 0;
                        pos--;
                    }

                    initialize_chat_message(chat_message, s, server_message_color);
                    add_message_to_array(server_messages, chat_message);
                    continue;
                }
                s_len++;
            }
            if(s_len) {
                initialize_chat_message(chat_message, s, server_message_color);
                add_message_to_array(server_messages, chat_message);
            }
            return;
        }

        // Find the correct format to use
        const char *format_to_use;
        switch(channel_index) {
            case 1:
                format_to_use = localize("chimera_custom_chat_from_team");
                break;
            case 2:
                format_to_use = localize("chimera_custom_chat_from_vehicle");
                break;
            default:
                format_to_use = localize("chimera_custom_chat_from_all");
                break;
        }

        // Get player info
        auto &player_info = ServerInfoPlayerList::get_server_info_player_list()->players[player_index];
        ColorARGB color_to_use;
        auto *name_color_to_use = color_id_for_player(player_index, &color_to_use);

        // Get player name
        char player_u8[256];
        u16_to_u8(player_u8, sizeof(player_u8), special_name_color(player_info.name));

        // Format a message
        char entire_message[MAX_MESSAGE_LENGTH];
        std::snprintf(entire_message, sizeof(entire_message) - 1, format_to_use, name_color_to_use, player_u8, message);

        // Initialize
        initialize_chat_message(chat_message, entire_message, color_to_use);
        add_message_to_array(chat_messages, chat_message);

        if(chat_message_scroll && chat_message_scroll + 1 != MESSAGE_BUFFER_SIZE) {
            chat_message_scroll++;
        }
    }

    void add_server_message(const char *message) {
        draw_chat_message(message, 255, 3);
    }

    extern "C" void draw_chat_message(const wchar_t *message, std::uint32_t p_int, std::uint32_t c_int) {
        char message_u8[256] = {};
        u16_to_u8(message_u8, sizeof(message_u8), message);

        draw_chat_message(message_u8, p_int, c_int);
    }

    static void initialize_chat_message(ChatMessage &message, const char *message_text, const ColorARGB &color) {
        message.created = clock::now();
        message.color = color;
        std::strncpy(message.message, message_text, sizeof(message.message));
    }

    static void add_message_to_array(ChatMessage *array, const ChatMessage &message) {
        for(std::size_t i = MESSAGE_BUFFER_SIZE - 1; i > 0; i--) {
            array[i] = array[i - 1];
        }
        *array = message;
    }

    extern "C" void bring_up_chat_prompt(int channel) {
        if(chat_input_open) {
            return;
        }
        chat_input_open = true;
        chat_input_buffer[0] = 0;
        chat_input_cursor = 0;
        chat_input_channel = channel;
        chat_open_state_changed = clock::now();
        enable_input(false);
    }

    static void on_chat_input() noexcept {
        static std::uint8_t *modifier = nullptr;
        static std::uint8_t *character = nullptr;
        static std::uint8_t *key_code = nullptr;
        if(!modifier) {
            auto *data = *reinterpret_cast<std::uint8_t **>(get_chimera().get_signature("on_key_press_sig").data() + 10);
            modifier = data + 2;
            character = data + 3;
            key_code = data + 4;
        }

        // Handle keyboard input if we have the chat input open
        if(chat_input_open) {
            // Special key pressed
            if(*character == 0xFF) {
                if(*key_code == 0) {
                    chat_input_open = false;
                    chat_open_state_changed = clock::now();
                    chat_message_scroll = 0;
                    enable_input(true);
                }
                // Left arrow
                else if(*key_code == 0x4F) {
                    if(chat_input_cursor > 0) {
                        chat_input_cursor--;
                    }
                }
                // Right arrow
                else if(*key_code == 0x50) {
                    if(chat_input_buffer[chat_input_cursor] != 0) {
                        chat_input_cursor++;
                    }
                }
                // Up arrow
                else if(*key_code == 0x4D) {
                    if(chat_message_scroll + 1 != MESSAGE_BUFFER_SIZE && chat_messages[chat_message_scroll + 1].valid()) {
                        chat_message_scroll++;
                    }
                }
                // Page down
                else if(*key_code == 0x4E) {
                    if(chat_message_scroll != 0) {
                        chat_message_scroll--;
                    }
                }
                // Backspace/Delete
                else if(*key_code == 0x1D) {
                    static bool ignore_next_key = false;
                    if(ignore_next_key) {
                        ignore_next_key = false;
                    }
                    else if(chat_input_cursor > 0) {
                        std::size_t length = std::strlen(chat_input_buffer);
                        // Move everything after the cursor down a character
                        for(std::size_t i = chat_input_cursor - 1; i < length; i++) {
                            chat_input_buffer[i] = chat_input_buffer[i + 1];
                        }
                        chat_input_buffer[length - 1] = 0;
                        chat_input_cursor--;
                        ignore_next_key = true;
                    }
                }
                // Del
                else if(*key_code == 0x54) {
                    std::size_t length = std::strlen(chat_input_buffer);

                    // Move everything after the cursor down a character
                    for(std::size_t i = chat_input_cursor; i < length; i++) {
                        chat_input_buffer[i] = chat_input_buffer[i + 1];
                    }
                }
                // Enter
                else if(*key_code == 0x38) {
                    if(std::strlen(chat_input_buffer) && server_type() != ServerType::SERVER_NONE) {
                        wchar_t chat_data[sizeof(chat_input_buffer)];
                        std::copy(chat_input_buffer, chat_input_buffer + sizeof(chat_input_buffer), chat_data);
                        chat_out_asm(chat_input_channel, chat_data);
                    }
                    chat_input_open = false;
                    chat_open_state_changed = clock::now();
                    chat_message_scroll = 0;
                    enable_input(true);
                }
            }
            // Insert a character
            else {
                std::size_t length = std::strlen(chat_input_buffer);
                if(length + 1 < INPUT_BUFFER_SIZE) {
                    // Null terminate so we don't get blown up
                    chat_input_buffer[length + 1] = 0;
                    // Move everything after the cursor up a character
                    for(std::size_t i = length; i > chat_input_cursor; i--) {
                        chat_input_buffer[i] = chat_input_buffer[i - 1];
                    }
                    chat_input_buffer[chat_input_cursor++] = *character;
                }
            }
        }
    }

    static void enable_input(bool enabled) noexcept {
        auto &sig = get_chimera().get_signature("key_press_mov_sig");
        overwrite(sig.data() + 6, static_cast<std::uint8_t>(enabled));
    }

    template<typename T> static void strip_color_codes(T *str_data) {
        for(;*str_data;str_data++) {
            if(*str_data == '^') {
                std::size_t deletion_count = 2;
                if(str_data[1] == '^') {
                    deletion_count = 1;
                }
                T *str;
                for(str = str_data; str[deletion_count]; str++) {
                    str[0] = str[deletion_count];
                }
                *str = 0;
            }
        }
    }

    std::string get_string_from_string_list(const char *tag, std::uint32_t index) {
        auto *ustr = get_tag(tag, TagClassInt::TAG_CLASS_UNICODE_STRING_LIST);
        if(!ustr) {
            return std::string();
        }

        auto *ustr_tag_data = ustr->data;
        std::uint32_t strings_count = *reinterpret_cast<std::uint32_t *>(ustr_tag_data);
        if(index >= strings_count) {
            return std::string();
        }
        auto *strings = *reinterpret_cast<std::byte **>(ustr_tag_data + 0x4);
        auto *str = *reinterpret_cast<wchar_t **>(strings + 0x14 * index + 0xC);

        // Convert
        char s[256];
        u16_to_u8(s, sizeof(s), str);
        return std::string(s);
    }

    extern "C" void welcome_message(PlayerID player_a) {
        // Get server information
        auto *server_info = ServerInfoPlayerList::get_server_info_player_list();

        // Get the player name
        auto get_player_name = [&server_info](PlayerID id, ColorARGB *color_to_use) -> std::string {
            char name[256];
            auto *player = server_info->get_player(id);
            u16_to_u8(name, sizeof(name), special_name_color(player->name));

            return std::string("^") + color_id_for_player(player - server_info->players, color_to_use) + name + "^;";
        };

        // Show a single message
        auto single_message = [&get_player_name](PlayerID id, int index) {
            auto str = get_string_from_string_list("ui\\multiplayer_game_text", index);
            ColorARGB color_to_use = {1.0, 1.0, 1.0, 1.0};

            std::string player_name = get_player_name(id, &color_to_use);

            char message[256];
            std::snprintf(message, sizeof(message), str.data(), player_name.data());

            ChatMessage chat_message;
            initialize_chat_message(chat_message, message, color_to_use);
            add_message_to_array(chat_messages, chat_message);
        };

        if(!player_in_server[player_a.index.index]) {
            auto *player_table_player = PlayerTable::get_player_table().get_player(player_a);
            single_message(player_a, 74);
            player_in_server[player_a.index.index] = true;

            std::copy(player_table_player->name, player_table_player->name + sizeof(player_table_player->name) / sizeof(*player_table_player->name) + 1, player_name[player_a.index.index]);
            char *t;
            for(t = player_name[player_a.index.index]; *t; t++);
            t[0] = '^';
            t[1] = ';';
            t[2] = 0;

            strip_color_codes(player_table_player->name);
        }
    }

    static void check_for_quit_players() {
        auto *server_info = ServerInfoPlayerList::get_server_info_player_list();
        if(!server_info) {
            std::fill(player_in_server, player_in_server + sizeof(player_in_server), false);
            return;
        }

        char message[256];

        for(bool &s : player_in_server) {
            if(!s) {
                continue;
            }
            std::size_t index = &s - player_in_server;
            if(server_info->players[index].player_id == 0xFF) {
                s = false;
                ChatMessage chat_message;
                auto s = get_string_from_string_list("ui\\multiplayer_game_text", 80);
                std::snprintf(message, sizeof(message), s.data(), player_name[index]);
                initialize_chat_message(chat_message, message, {1.0, 1.0, 1.0, 1.0});
                add_message_to_array(chat_messages, chat_message);
            }
        }
    }

    bool ips_blocked() noexcept {
        return block_ips;
    }
    void set_block_ips(bool block) noexcept {
        block_ips = block;
    }

    static void load_chat_settings() {
        auto *ini = get_chimera().get_ini();

        #define LOAD_IF_POSSIBLE(ini_value, setting, parser) { \
            const char *setting_value = ini->get_value("custom_chat." # ini_value); \
            if(setting_value) { \
                setting = parser(setting_value); \
            } \
        }

        #define LOAD_IF_POSSIBLE_SETTING(setting, parser) LOAD_IF_POSSIBLE(setting, setting, parser)

        LOAD_IF_POSSIBLE_SETTING(server_message_x, std::stoi)
        LOAD_IF_POSSIBLE_SETTING(server_message_y, std::stoi)
        LOAD_IF_POSSIBLE_SETTING(server_message_w, std::stoi)
        LOAD_IF_POSSIBLE_SETTING(server_message_h, std::stoi)
        LOAD_IF_POSSIBLE_SETTING(server_message_h_chat_open, std::stoi)
        LOAD_IF_POSSIBLE_SETTING(server_message_hide_on_console, std::stoi)

        LOAD_IF_POSSIBLE_SETTING(chat_message_x, std::stoi)
        LOAD_IF_POSSIBLE_SETTING(chat_message_y, std::stoi)
        LOAD_IF_POSSIBLE_SETTING(chat_message_w, std::stoi)
        LOAD_IF_POSSIBLE_SETTING(chat_message_h, std::stoi)
        LOAD_IF_POSSIBLE_SETTING(chat_message_h_chat_open, std::stoi)
        LOAD_IF_POSSIBLE_SETTING(chat_message_hide_on_console, std::stoi)

        LOAD_IF_POSSIBLE_SETTING(chat_input_x, std::stoi)
        LOAD_IF_POSSIBLE_SETTING(chat_input_y, std::stoi)
        LOAD_IF_POSSIBLE_SETTING(chat_input_w, std::stoi)

        LOAD_IF_POSSIBLE(server_message_color_a, server_message_color.alpha, std::stof)
        LOAD_IF_POSSIBLE(server_message_color_r, server_message_color.red, std::stof)
        LOAD_IF_POSSIBLE(server_message_color_g, server_message_color.green, std::stof)
        LOAD_IF_POSSIBLE(server_message_color_b, server_message_color.blue, std::stof)

        LOAD_IF_POSSIBLE(chat_message_color_ffa_a, chat_message_color_ffa.alpha, std::stof)
        LOAD_IF_POSSIBLE(chat_message_color_ffa_r, chat_message_color_ffa.red, std::stof)
        LOAD_IF_POSSIBLE(chat_message_color_ffa_g, chat_message_color_ffa.green, std::stof)
        LOAD_IF_POSSIBLE(chat_message_color_ffa_b, chat_message_color_ffa.blue, std::stof)

        LOAD_IF_POSSIBLE(chat_message_color_red_a, chat_message_color_red.alpha, std::stof)
        LOAD_IF_POSSIBLE(chat_message_color_red_r, chat_message_color_red.red, std::stof)
        LOAD_IF_POSSIBLE(chat_message_color_red_g, chat_message_color_red.green, std::stof)
        LOAD_IF_POSSIBLE(chat_message_color_red_b, chat_message_color_red.blue, std::stof)

        LOAD_IF_POSSIBLE(chat_message_color_blue_a, chat_message_color_blue.alpha, std::stof)
        LOAD_IF_POSSIBLE(chat_message_color_blue_r, chat_message_color_blue.red, std::stof)
        LOAD_IF_POSSIBLE(chat_message_color_blue_g, chat_message_color_blue.green, std::stof)
        LOAD_IF_POSSIBLE(chat_message_color_blue_b, chat_message_color_blue.blue, std::stof)

        LOAD_IF_POSSIBLE(chat_input_color_a, chat_input_color.alpha, std::stof)
        LOAD_IF_POSSIBLE(chat_input_color_r, chat_input_color.red, std::stof)
        LOAD_IF_POSSIBLE(chat_input_color_g, chat_input_color.green, std::stof)
        LOAD_IF_POSSIBLE(chat_input_color_b, chat_input_color.blue, std::stof)

        auto get_anchor = [](const char *anchor) {
            if(std::strcmp(anchor, "top_right") == 0) {
                return TextAnchor::ANCHOR_TOP_RIGHT;
            }
            else if(std::strcmp(anchor, "top_left") == 0) {
                return TextAnchor::ANCHOR_TOP_LEFT;
            }
            else if(std::strcmp(anchor, "center") == 0) {
                return TextAnchor::ANCHOR_CENTER;
            }
            else if(std::strcmp(anchor, "bottom_right") == 0) {
                return TextAnchor::ANCHOR_BOTTOM_RIGHT;
            }
            else if(std::strcmp(anchor, "bottom_left") == 0) {
                return TextAnchor::ANCHOR_BOTTOM_LEFT;
            }
            return TextAnchor::ANCHOR_TOP_RIGHT;
        };

        LOAD_IF_POSSIBLE_SETTING(server_message_anchor, get_anchor)
        LOAD_IF_POSSIBLE_SETTING(chat_message_anchor, get_anchor)
        LOAD_IF_POSSIBLE_SETTING(chat_input_anchor, get_anchor)

        auto get_font = [](const char *anchor) {
            if(std::strcmp(anchor, "console") == 0) {
                return GenericFont::FONT_CONSOLE;
            }
            else if(std::strcmp(anchor, "small") == 0) {
                return GenericFont::FONT_SMALL;
            }
            else if(std::strcmp(anchor, "large") == 0) {
                return GenericFont::FONT_LARGE;
            }
            return GenericFont::FONT_CONSOLE;
        };

        LOAD_IF_POSSIBLE_SETTING(server_message_font, get_font)
        LOAD_IF_POSSIBLE_SETTING(chat_message_font, get_font)
        LOAD_IF_POSSIBLE_SETTING(chat_input_font, get_font)

        LOAD_IF_POSSIBLE_SETTING(slide_time_length, std::stof)
        LOAD_IF_POSSIBLE_SETTING(time_up, std::stof)
        LOAD_IF_POSSIBLE_SETTING(fade_out_time, std::stof)
    }

    static const wchar_t *special_name_color(const wchar_t *name) {
        if(name == nullptr) {
            return nullptr;
        }
        auto name_matches = [&name](const wchar_t *name_to_match) {
            for(std::size_t i = 0;;i++) {
                if(name[i] != name_to_match[i]) {
                    return false;
                }
                else if(name[i] == 0) {
                    return true;
                }
            }
        };

        if(name_matches(L"Kavawuvi")) {
            return L"^KKavawuvi^;";
        }
        else if(name_matches(L"Vaporeon")) {
            return L"^5Vaporeon^;";
        }
        else if(name_matches(L"ST34MF0X")) {
            return L"^qST34M^8F0X^;";
        }
        else {
            return name;
        }
    }
}
