/*
 * Copyright (c) 2019-2021 Kim Jørgensen
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

static void sd_format_size(char *buffer, u32 size)
{
    char units[] = "BKmg";
    char *unit = units;

    while (size >= 1000)
    {
        size += 24;
        size /= 1024;
        unit++;
    }

    if (size < 100)
    {
        *buffer++ = ' ';
    }
    if (size < 10)
    {
        *buffer++ = ' ';
    }

    sprint(buffer, "%u%c ", size, *unit);
}

static void sd_format_element(char *buffer, FILINFO *info)
{
    *buffer++ = ' ';

    // Element name
    const u8 filename_len = (ELEMENT_LENGTH-1)-6;
    to_petscii_pad(buffer, info->fname, filename_len);
    buffer += filename_len;
    *buffer++ = ' ';

    // Element type or size
    if (info->fattrib & AM_DIR)
    {
        sprint(buffer, " dir ");
    }
    else
    {
        sd_format_size(buffer, info->fsize);
    }
}

static void sd_send_not_found(SD_STATE *state)
{
    to_petscii_pad(scratch_buf, " no files found", ELEMENT_LENGTH);
    if (state->search[0])
    {
        scratch_buf[0] = SELECTED_ELEMENT;
    }

    c64_send_data(scratch_buf, ELEMENT_LENGTH);
}

static u8 sd_send_page(SD_STATE *state, u8 selected_element)
{
    bool send_dot_dot = !state->in_root && state->page_no == 0;
    bool send_not_found = state->page_no == 0;

    FILINFO file_info;
    u8 element;
    for (element=0; element<MAX_ELEMENTS_PAGE; element++)
    {
        if (send_dot_dot)
        {
            file_info.fname[0] = '.';
            file_info.fname[1] = '.';
            file_info.fname[2] = 0;
            file_info.fattrib = AM_DIR;
            send_dot_dot = false;
        }
        else
        {
            if (!dir_read(&state->end_page, &file_info))
            {
                file_info.fname[0] = 0;
            }

            if (file_info.fname[0])
            {
                send_not_found = false;
            }
            else
            {
                // End of dir
                dir_close(&state->end_page);
                state->dir_end = true;

                if (send_not_found)
                {
                    sd_send_not_found(state);
                }

                send_page_end();
                break;
            }
        }

        sd_format_element(scratch_buf, &file_info);
        if (element == selected_element)
        {
            scratch_buf[0] = SELECTED_ELEMENT;
        }

        c64_send_data(scratch_buf, ELEMENT_LENGTH);
    }

    return element;
}

static void handle_dir_open(SD_STATE *state)
{
    // Append star to end of search string
    size_t search_len = strlen(state->search);
    if (search_len && state->search[search_len-1] != '*')
    {
        state->search[search_len] = '*';
        state->search[search_len + 1] = 0;
    }

    if (!dir_open(&state->start_page, state->search))
    {
        handle_failed_to_read_sd();
    }

    state->end_page = state->start_page;
    state->page_no = 0;
    state->dir_end = false;
}

static void sd_send_warning_restart(const char *message, const char *filename)
{
    c64_interface(true);
    sprint(scratch_buf, "%s\r\n\r\n%s", message, filename);
    c64_send_warning(scratch_buf);
    restart_to_menu();
}

static u8 sd_parse_file_number(char *filename, u8 *extension)
{
    u8 pos = *extension-1;
    if (pos <= 5 || filename[pos--] != ')')
    {
        return 0;
    }

    u8 number;
    if (filename[pos] >= '0' && filename[pos] <= '9')
    {
        number = filename[pos--] - '0';
    }
    else
    {
        return 0;
    }

    if (filename[pos] >= '0' && filename[pos] <= '9')
    {
        number += (filename[pos--] - '0') * 10;
    }

    if(filename[pos--] != '(' || filename[pos] != ' ')
    {
        return 0;
    }

    *extension = pos;
    return number;
}

static bool sd_generate_new_filename(void)
{
    char *filename = dat_file.file;

    u8 extension;
    u8 length = get_filename_length(filename, &extension);
    if (length <= extension)
    {
        return false;
    }

    u8 file_number = sd_parse_file_number(filename, &extension);
    if (++file_number >= 100)
    {
        return false;
    }

    if (length < (sizeof(dat_file.file)-5))
    {
        sprint(filename + extension, " (%u).crt", file_number);
        return true;
    }

    return false;
}

static void handle_save_updated_crt(u8 flags)
{
    c64_send_exit_menu();
    c64_send_prg_message("Saving CRT file.");
    c64_interface(false);

    if (!(flags & SELECT_FLAG_OVERWRITE))
    {
        bool file_exists = true;
        while (file_exists)
        {
            if (!sd_generate_new_filename())
            {
                if (!load_dat())
                {
                    restart_to_menu();
                }
                sd_send_warning_restart("Failed to generate new filename for",
                                        dat_file.file);
            }

            FILINFO file_info;
            file_exists = file_stat(dat_file.file, &file_info);
        }
    }

    FIL file;
    if (!file_open(&file, dat_file.file, FA_WRITE|FA_CREATE_ALWAYS) ||
        !crt_write_header(&file, CRT_EASYFLASH, 1, 0, "EASYFLASH") ||
        !crt_write_file(&file, dat_file.crt.banks))
    {
        sd_send_warning_restart("Failed to write CRT file", dat_file.file);
    }
    file_close(&file);

    // Updated flag is cleared so we need to calculate a new hash
    u32 flash_hash = crt_calc_flash_crc(dat_file.crt.banks);
    dat_file.crt.flash_hash = flash_hash;
    save_dat();

    restart_to_menu();
}

static bool handle_updated_crt(SD_STATE *state)
{
    if (dat_file.boot_type != DAT_CRT ||
        !(dat_file.crt.flags & CRT_FLAG_UPDATED) ||
        dat_file.crt.type != CRT_EASYFLASH ||
        !dat_file.file[0])
    {
        return false;
    }

    dat_file.crt.flags &= ~CRT_FLAG_UPDATED; // Don't ask to save file again
    handle_unsaved_crt(dat_file.file, handle_save_updated_crt);
    return true;
}

static void handle_dir_command(SD_STATE *state)
{
    if (handle_updated_crt(state))
    {
        return;
    }

    handle_dir_open(state);

    dir_current(dat_file.path, sizeof(dat_file.path));
    state->in_root = format_path(scratch_buf, false);
    scratch_buf[0] = state->search[0] ? SEARCH_SUPPORTED : CLEAR_SEARCH;
    dbg("Reading path %s\n", dat_file.path);

    // Search for last selected element
    FILINFO file_info;
    bool found = false;
    u8 selected_element = MAX_ELEMENTS_PAGE;

    if (dat_file.file[0])
    {
        DIR first_page = state->start_page;
        while (true)
        {
            u8 element = 0;
            if (!state->in_root && state->page_no == 0)
            {
                element++;
            }

            for (; element<MAX_ELEMENTS_PAGE; element++)
            {
                if (!dir_read(&state->end_page, &file_info))
                {
                    file_info.fname[0] = 0;
                }

                if (!file_info.fname[0])
                {
                    break;
                }

                if (strncmp(dat_file.file, file_info.fname, sizeof(dat_file.file)) == 0)
                {
                    found = true;
                    break;
                }
            }

            if (found)
            {
                state->end_page = state->start_page;
                selected_element = element;
                break;
            }

            if (!file_info.fname[0])
            {
                state->start_page = first_page;
                state->end_page = first_page;
                state->page_no = 0;
                break;
            }

            state->start_page = state->end_page;
            state->page_no++;
        }
    }

    if (!found)
    {
        dat_file.file[0] = 0;
    }
    else if (dat_file.prg.element != ELEMENT_NOT_SELECTED)
    {
        u8 file_type = get_file_type(&file_info);
        if (file_type == FILE_D64)
        {
            menu = d64_menu_init(file_info.fname);
            menu->dir(menu->state);
            return;
        }
        else if (file_type == FILE_T64)
        {
            menu = t64_menu_init(file_info.fname);
            menu->dir(menu->state);
            return;
        }
    }

    c64_send_reply(REPLY_READ_DIR);
    c64_send_data(scratch_buf, DIR_NAME_LENGTH);
    sd_send_page(state, selected_element);
}

static void handle_change_dir(SD_STATE *state, char *path, bool select_old)
{
    if (select_old && !state->in_root)
    {
        u16 dir_index = 0;
        u16 len;
        for (len = 0; len < sizeof(dat_file.path); len++)
        {
            char c = dat_file.path[len];
            if (!c)
            {
                break;
            }
            if (c == '/')
            {
                dir_index = len+1;
            }
        }

        memcpy(dat_file.file, dat_file.path + dir_index, len - dir_index);
        dat_file.file[len - dir_index] = 0;
    }
    else
    {
        dat_file.file[0] = 0;
    }

    state->search[0] = 0;
    if (!dir_change(path))
    {
        handle_failed_to_read_sd();
    }

    handle_dir_command(state);
}

static void handle_dir_next_page_command(SD_STATE *state)
{
    c64_send_reply(REPLY_READ_DIR_PAGE);

    if (!state->dir_end)
    {
        DIR start = state->end_page;
        state->page_no++;

        if (sd_send_page(state, MAX_ELEMENTS_PAGE) > 0)
        {
            state->start_page = start;
        }
        else
        {
            state->page_no--;
        }
    }
    else
    {
        send_page_end();
    }
}

static void handle_dir_prev_page_command(SD_STATE *state)
{
    if (state->page_no)
    {
        u16 target_page = state->page_no-1;
        bool not_found = false;

        handle_dir_open(state);
        c64_send_reply(REPLY_READ_DIR_PAGE);

        u16 elements_to_skip = MAX_ELEMENTS_PAGE * target_page;
        if (elements_to_skip && !state->in_root) elements_to_skip--;

        while (elements_to_skip--)
        {
            FILINFO file_info;
            if (!dir_read(&state->end_page, &file_info))
            {
                file_info.fname[0] = 0;
            }

            if (!file_info.fname[0])
            {
                not_found = true;
                break;
            }
        }

        if (not_found)
        {
            state->end_page = state->start_page;
        }
        else
        {
            state->page_no = target_page;
            state->start_page = state->end_page;
        }

        sd_send_page(state, MAX_ELEMENTS_PAGE);
    }
    else
    {
        reply_page_end();
    }
}

static void handle_delete_file(const char *file_name)
{
    c64_send_exit_menu();
    c64_send_prg_message("Deleting file.");
    c64_interface(false);

    if (!file_delete(file_name))
    {
        sd_send_warning_restart("Failed to delete file", file_name);
    }

    c64_interface(true);
    c64_send_reset_to_menu();
}

static void handle_file_open(FIL *file, const char *file_name)
{
    if (!file_open(file, file_name, FA_READ))
    {
        handle_failed_to_read_sd();
    }
}

static bool handle_crt_supported(u32 cartridge_type)
{
    if (crt_is_supported(cartridge_type))
    {
        return true;
    }

    sprint(scratch_buf, "Unsupported CRT type (%u)", cartridge_type);
    handle_unsupported_ex("Unsupported", scratch_buf, dat_file.file);
    return false;
}

static void handle_fw_not_in_root(const char *file_name)
{
    handle_unsupported_ex("Unsupported",
        "Please put the firmware file in the root directory of the SD card",
        file_name);
}

static bool handle_load_file(SD_STATE *state, const char *file_name,
                             u8 file_type, u8 flags, u8 element)
{
    bool exit_menu = false;

    switch (file_type)
    {
        case FILE_PRG:
        case FILE_P00:
        {
            FIL file;
            handle_file_open(&file, file_name);
            dat_file.prg.name[0] = 0;

            bool prg_loaded;
            if (file_type == FILE_PRG)
            {
                prg_loaded = prg_load_file(&file);
            }
            else
            {
                prg_loaded = p00_load_file(&file);
            }

            if (prg_loaded)
            {
                c64_send_exit_menu();
                dat_file.prg.element = 0;
                dat_file.boot_type = DAT_PRG;
                exit_menu = true;
            }
            else
            {
                handle_unsupported(file_name);
            }

            file_close(&file);
        }
        break;

        case FILE_ROM:
        {
            if (!(flags & SELECT_FLAG_ACCEPT))
            {
                FIL file;
                handle_file_open(&file, file_name);

                if (rom_load_file(&file) &&
                    // Look for C128 CRT identifier
                    (memcmp("CBM", &dat_buffer[0x0007], 3) == 0 ||
                     memcmp("CBM", &dat_buffer[0x4007], 3) == 0))
                {
                    if (!handle_crt_supported(CRT_C128_NORMAL_CARTRIDGE))
                    {
                        break;
                    }

                    // Show warning on a C64
                    if (!(flags & SELECT_FLAG_C128))
                    {
                        handle_unsupported_warning(
                            "Cartridge will only run on a C128", file_name,
                             element);
                    }
                    else
                    {
                        exit_menu = true;
                    }
                }
                else
                {
                    handle_unsupported(file_name);
                }

                file_close(&file);
            }
            else
            {
                exit_menu = true;
            }

            if (exit_menu)
            {
                c64_send_exit_menu();

                u8 banks = 4;
                u32 flash_hash = crt_calc_flash_crc(banks);
                dat_file.crt.type = CRT_C128_NORMAL_CARTRIDGE;
                dat_file.crt.hw_rev = 0;
                dat_file.crt.exrom = 1;
                dat_file.crt.game = 1;
                dat_file.crt.banks = banks;
                dat_file.crt.flags = CRT_FLAG_VIC;
                dat_file.crt.flash_hash = flash_hash;
                dat_file.boot_type = DAT_CRT;
            }
        }
        break;

        case FILE_CRT:
        {
            FIL file;
            handle_file_open(&file, file_name);

            CRT_HEADER header;
            if (!crt_load_header(&file, &header))
            {
                handle_unsupported(file_name);
                break;
            }

            if (!handle_crt_supported(header.cartridge_type))
            {
                break;
            }

            c64_send_exit_menu();
            c64_send_prg_message("Programming flash memory.");
            c64_interface(false);

            u8 banks = crt_program_file(&file, header.cartridge_type);
            if (!banks)
            {
                sd_send_warning_restart("Failed to read CRT file", file_name);
            }

            u8 crt_flags = CRT_FLAG_NONE;
            if (flags & SELECT_FLAG_VIC)
            {
                crt_flags |= CRT_FLAG_VIC;
            }
            else if (header.cartridge_type == CRT_EASYFLASH &&
                     memcmp("eapi", dat_buffer + EAPI_OFFSET, 4) == 0)
            {
                convert_to_ascii(scratch_buf, dat_buffer + EAPI_OFFSET + 4, 16);
                dbg("EAPI detected: %s\n", scratch_buf);

                // Replace EAPI with the one for Kung Fu Flash
                memcpy(dat_buffer + EAPI_OFFSET,
                       CRT_LAUNCHER_BANK + EAPI_OFFSET, EAPI_SIZE);
            }

            u32 flash_hash = crt_calc_flash_crc(banks);
            dat_file.crt.type = header.cartridge_type;
            dat_file.crt.hw_rev = header.hardware_revision;
            dat_file.crt.exrom = header.exrom;
            dat_file.crt.game = header.game;
            dat_file.crt.banks = banks;
            dat_file.crt.flags = crt_flags;
            dat_file.crt.flash_hash = flash_hash;
            dat_file.boot_type = DAT_CRT;
            exit_menu = true;
        }
        break;

        case FILE_D64:
        {
            if (flags & SELECT_FLAG_MOUNT)
            {
                basic_no_commands();
                dat_file.boot_type = DAT_DISK;
                exit_menu = true;
            }
            else if (!(flags & SELECT_FLAG_ACCEPT) && autostart_d64())
            {
                basic_load("*");
                dat_file.boot_type = DAT_DISK;
                exit_menu = true;
            }
            else
            {
                state->search[0] = 0;
                dat_file.prg.element = 0;
                menu = d64_menu_init(file_name);
                menu->dir(menu->state);
            }
        }
        break;

        case FILE_T64:
        {
            if (!(flags & SELECT_FLAG_ACCEPT) && autostart_d64())
            {
                exit_menu = t64_load_first(file_name);
                dat_file.prg.element = ELEMENT_NOT_SELECTED;
            }
            else
            {
                state->search[0] = 0;
                dat_file.prg.element = 0;
                menu = t64_menu_init(file_name);
                menu->dir(menu->state);
            }
        }
        break;

        case FILE_UPD:
        {
            if (!(flags & SELECT_FLAG_ACCEPT))
            {
                FIL file;
                handle_file_open(&file, file_name);

                if (f_size(&file) > sizeof(dat_buffer) && !state->in_root)
                {
                    handle_fw_not_in_root(file_name);
                }
                else
                {
                    char firmware[FW_NAME_SIZE];
                    if (upd_load(&file, firmware))
                    {
                        handle_upgrade_menu(firmware, element);
                    }
                    else
                    {
                        handle_unsupported(file_name);
                    }
                }

                file_close(&file);
            }
            else
            {
                c64_send_exit_menu();
                c64_send_prg_message("Upgrading firmware.");
                c64_interface(false);

                upd_program();
                save_dat();
                restart_to_menu();
            }
        }
        break;

        case FILE_DAT:
        {
            handle_unsupported_ex("System File", "This file is used by Kung Fu Flash", file_name);
        }
        break;

        default:
        {
            handle_unsupported(file_name);
        }
        break;
    }

    return exit_menu;
}

static bool handle_select_command(SD_STATE *state, u8 flags, u8 element)
{
    bool exit_menu = false;
    u8 element_no = element;

    dat_file.boot_type = DAT_NONE;
    dat_file.prg.element = ELEMENT_NOT_SELECTED;    // don't auto open T64/D64
    dat_file.file[0] = 0;

    if (!state->in_root && state->page_no == 0)
    {
        if (element_no == 0)
        {
            if (flags & SELECT_FLAG_OPTIONS)
            {
                handle_file_options("..", FILE_NONE, element);
            }
            else
            {
                handle_change_dir(state, "..", true);
            }

            return exit_menu;
        }

        element_no--;
    }

    FILINFO file_info;
    DIR dir = state->start_page;
    for (u8 i=0; i<=element_no; i++)
    {
        if (!dir_read(&dir, &file_info))
        {
            handle_failed_to_read_sd();
        }

        if (!file_info.fname[0]) // End of dir
        {
            break;
        }
    }

    dir_close(&dir);

    if (file_info.fname[0] == 0)
    {
        // File not not found
        state->search[0] = 0;
        handle_dir_command(state);
        return exit_menu;
    }

    u8 file_type = get_file_type(&file_info);
    strcpy(dat_file.file, file_info.fname);

    if (flags & SELECT_FLAG_OPTIONS)
    {
        handle_file_options(file_info.fname, file_type, element);
    }
    else if (file_type == FILE_NONE)
    {
        handle_change_dir(state, file_info.fname, false);
    }
    else if (flags & SELECT_FLAG_DELETE)
    {
        handle_delete_file(file_info.fname);
    }
    else
    {
        exit_menu = handle_load_file(state, file_info.fname, file_type, flags, element);
    }

    return exit_menu;
}

static void handle_dir_up_command(SD_STATE *state, bool root)
{
    if (root)
    {
        handle_change_dir(state, "/", false);
    }
    else
    {
        handle_change_dir(state, "..", true);
    }
}

static const MENU sd_menu = {
    .state = &sd_state,
    .dir = (void (*)(void *))handle_dir_command,
    .dir_up = (void (*)(void *, bool))handle_dir_up_command,
    .prev_page = (void (*)(void *))handle_dir_prev_page_command,
    .next_page = (void (*)(void *))handle_dir_next_page_command,
    .select = (bool (*)(void *, u8, u8))handle_select_command
};

static const MENU *sd_menu_init(void)
{
    chdir_last();
    sd_state.page_no = 0;
    sd_state.dir_end = true;

    return &sd_menu;
}
