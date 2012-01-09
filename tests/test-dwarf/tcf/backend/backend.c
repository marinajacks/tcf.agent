/*******************************************************************************
 * Copyright (c) 2010, 2011 Wind River Systems, Inc. and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 * The Eclipse Public License is available at
 * http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 * You may elect to redistribute this code under either of these licenses.
 *
 * Contributors:
 *     Wind River Systems - initial API and implementation
 *******************************************************************************/

/* Fake debug context API implementation. It used for testing symbol services. */

#include <tcf/config.h>

#include <sys/stat.h>
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#if !defined(WIN32) || defined(__CYGWIN__)
#  include <dirent.h>
#endif

#include <tcf/framework/context.h>
#include <tcf/framework/events.h>
#include <tcf/framework/myalloc.h>
#include <tcf/framework/exceptions.h>

#include <tcf/services/tcf_elf.h>
#include <tcf/services/symbols.h>
#include <tcf/services/linenumbers.h>
#include <tcf/services/memorymap.h>
#include <tcf/services/dwarfframe.h>
#include <tcf/services/dwarfcache.h>
#include <tcf/services/stacktrace.h>
#include <tcf/services/dwarf.h>

#include <tcf/backend/backend.h>

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

#define MAX_REGS 2000

struct RegisterData {
    uint8_t data[MAX_REGS * 8];
    uint8_t mask[MAX_REGS * 8];
};

static Context * elf_ctx = NULL;
static MemoryMap mem_map;
static RegisterDefinition reg_defs[MAX_REGS];
static char reg_names[MAX_REGS][32];
static uint8_t reg_vals[MAX_REGS * 8];
static unsigned reg_size = 0;

static uint8_t frame_data[0x1000];
static ContextAddress frame_addr = 0x40000000u;

static const char * elf_file_name = NULL;
static int mem_region_pos = 0;
static ContextAddress pc = 0;
static unsigned pass_cnt = 0;
static int test_posted = 0;
static struct timespec time_start;

static char ** files = NULL;
static unsigned files_max = 0;
static unsigned files_cnt = 0;

static int line_area_ok = 0;

extern ObjectInfo * get_symbol_object(Symbol * sym);

static RegisterDefinition * get_reg_by_dwarf_id(unsigned id) {
    static RegisterDefinition ** map = NULL;
    static unsigned map_length = 0;

    if (map == NULL) {
        RegisterDefinition * r;
        RegisterDefinition * regs_index = get_reg_definitions(NULL);
        for (r = regs_index; r->name != NULL; r++) {
            if (r->dwarf_id >= (int)map_length) map_length = r->dwarf_id + 1;
        }
        map = (RegisterDefinition **)loc_alloc_zero(sizeof(RegisterDefinition *) * map_length);
        for (r = regs_index; r->name != NULL; r++) {
            if (r->dwarf_id >= 0) map[r->dwarf_id] = r;
        }
    }
    return id < map_length ? map[id] : NULL;
}

static RegisterDefinition * get_reg_by_eh_frame_id(unsigned id) {
    static RegisterDefinition ** map = NULL;
    static unsigned map_length = 0;

    if (map == NULL) {
        RegisterDefinition * r;
        RegisterDefinition * regs_index = get_reg_definitions(NULL);
        for (r = regs_index; r->name != NULL; r++) {
            if (r->eh_frame_id >= (int)map_length) map_length = r->eh_frame_id + 1;
        }
        map = (RegisterDefinition **)loc_alloc_zero(sizeof(RegisterDefinition *) * map_length);
        for (r = regs_index; r->name != NULL; r++) {
            if (r->eh_frame_id >= 0) map[r->eh_frame_id] = r;
        }
    }
    return id < map_length ? map[id] : NULL;
}

RegisterDefinition * get_reg_by_id(Context * ctx, unsigned id, RegisterIdScope * scope) {
    RegisterDefinition * def = NULL;
    switch (scope->id_type) {
    case REGNUM_DWARF: def = get_reg_by_dwarf_id(id); break;
    case REGNUM_EH_FRAME: def = get_reg_by_eh_frame_id(id); break;
    }
    if (def == NULL) set_errno(ERR_OTHER, "Invalid register ID");
    return def;
}

int read_reg_bytes(StackFrame * frame, RegisterDefinition * reg_def, unsigned offs, unsigned size, uint8_t * buf) {
    if (reg_def != NULL && frame != NULL) {
        if (frame->is_top_frame) {
            return context_read_reg(frame->ctx, reg_def, offs, size, buf);
        }
        if (frame->regs != NULL) {
            size_t i;
            uint8_t * r_addr = (uint8_t *)&frame->regs->data + reg_def->offset;
            uint8_t * m_addr = (uint8_t *)&frame->regs->mask + reg_def->offset;
            for (i = 0; i < size; i++) {
                if (m_addr[offs + i] != 0xff) {
                    errno = ERR_INV_CONTEXT;
                    return -1;
                }
            }
            if (offs + size > reg_def->size) {
                errno = ERR_INV_DATA_SIZE;
                return -1;
            }
            memcpy(buf, r_addr + offs, size);
            return 0;
        }
    }
    errno = ERR_INV_CONTEXT;
    return -1;
}

int write_reg_bytes(StackFrame * frame, RegisterDefinition * reg_def, unsigned offs, unsigned size, uint8_t * buf) {
    if (reg_def != NULL && frame != NULL) {
        if (frame->is_top_frame) {
            return context_write_reg(frame->ctx, reg_def, offs, size, buf);
        }
        if (frame->regs == NULL && context_has_state(frame->ctx)) {
            frame->regs = (RegisterData *)loc_alloc_zero(sizeof(RegisterData));
        }
        if (frame->regs != NULL) {
            uint8_t * r_addr = (uint8_t *)&frame->regs->data + reg_def->offset;
            uint8_t * m_addr = (uint8_t *)&frame->regs->mask + reg_def->offset;

            if (offs + size > reg_def->size) {
                errno = ERR_INV_DATA_SIZE;
                return -1;
            }
            memcpy(r_addr + offs, buf, size);
            memset(m_addr + offs, 0xff, size);
            return 0;
        }
    }
    errno = ERR_INV_CONTEXT;
    return -1;
}

RegisterDefinition * get_reg_definitions(Context * ctx) {
    return reg_defs;
}

RegisterDefinition * get_PC_definition(Context * ctx) {
    return reg_defs;
}

Context * id2ctx(const char * id) {
    if (id != NULL && strcmp(id, elf_ctx->id) == 0) return elf_ctx;
    return NULL;
}

unsigned context_word_size(Context * ctx) {
    return get_PC_definition(ctx)->size;
}

int context_has_state(Context * ctx) {
    return 1;
}

Context * context_get_group(Context * ctx, int group) {
    return ctx;
}

int context_read_reg(Context * ctx, RegisterDefinition * def, unsigned offs, unsigned size, void * buf) {
    if (ctx != elf_ctx) {
        errno = ERR_INV_CONTEXT;
        return -1;
    }
    memcpy(buf, reg_vals + def->offset + offs, size);
    return 0;
}

int context_write_reg(Context * ctx, RegisterDefinition * def, unsigned offs, unsigned size, void * buf) {
    if (ctx != elf_ctx) {
        errno = ERR_INV_CONTEXT;
        return -1;
    }
    memcpy(reg_vals + def->offset + offs, buf, size);
    return 0;
}

int context_read_mem(Context * ctx, ContextAddress address, void * buf, size_t size) {
    if (address >= frame_addr && address + size >= address && address + size <= frame_addr + sizeof(frame_data)) {
        memcpy(buf, frame_data + (address - frame_addr), size);
        return 0;
    }
    memset(buf, 0, size);
    return 0;
}

int context_write_mem(Context * ctx, ContextAddress address, void * buf, size_t size) {
    /* TODO: context_write_mem */
    errno = ERR_UNSUPPORTED;
    return -1;
}

int context_get_memory_map(Context * ctx, MemoryMap * map) {
    unsigned i;
    for (i = 0; i < mem_map.region_cnt; i++) {
        MemoryRegion * r = NULL;
        if (map->region_cnt >= map->region_max) {
            map->region_max += 8;
            map->regions = (MemoryRegion *)loc_realloc(map->regions, sizeof(MemoryRegion) * map->region_max);
        }
        r = map->regions + map->region_cnt++;
        *r = mem_map.regions[i];
        if (r->file_name) r->file_name = loc_strdup(r->file_name);
        if (r->sect_name) r->sect_name = loc_strdup(r->sect_name);
    }
    return 0;
}

int crawl_stack_frame(StackFrame * frame, StackFrame * down) {
    if (frame->is_top_frame) {
        frame->fp = frame_addr;
        return 0;
    }
    errno = ERR_INV_ADDRESS;
    return -1;
}

static void error(const char * func) {
    int err = errno;
    printf("File    : %s\n", elf_file_name);
    if (elf_open(elf_file_name)->debug_info_file_name) {
        printf("Symbols : %s\n", elf_open(elf_file_name)->debug_info_file_name);
    }
    printf("Address : 0x%" PRIX64 "\n", (uint64_t)pc);
    printf("Function: %s\n", func);
    printf("Error   : %s\n", errno_to_str(err));
    fflush(stdout);
    exit(1);
}

static void addr_to_line_callback(CodeArea * area, void * args) {
    CodeArea * dst = (CodeArea *)args;
    if (area->start_address > pc || area->end_address <= pc) {
        errno = set_errno(ERR_OTHER, "Invalid line area address");
        error("address_to_line");
    }
    *dst = *area;
}

static void line_to_addr_callback(CodeArea * area, void * args) {
    CodeArea * org = (CodeArea *)args;
    if (area->start_line > org->start_line || (area->start_line == org->start_line && area->start_column > org->start_column) ||
        area->end_line < org->start_line || (area->end_line == org->start_line && area->end_column <= org->start_column)) {
        errno = set_errno(ERR_OTHER, "Invalid line area line numbers");
        error("line_to_address");
    }
    if (area->start_address > pc || area->end_address <= pc) return;
    if (org->start_address == area->start_address || org->end_address == area->end_address) {
        line_area_ok = 1;
    }
}

static void print_time(struct timespec time_start, int cnt) {
    struct timespec time_now;
    struct timespec time_diff;
    if (cnt == 0) return;
    clock_gettime(CLOCK_REALTIME, &time_now);
    time_diff.tv_sec = time_now.tv_sec - time_start.tv_sec;
    if (time_now.tv_nsec < time_start.tv_nsec) {
        time_diff.tv_sec--;
        time_diff.tv_nsec = time_now.tv_nsec + 1000000000 - time_start.tv_nsec;
    }
    else {
        time_diff.tv_nsec = time_now.tv_nsec - time_start.tv_nsec;
    }
    time_diff.tv_nsec /= cnt;
    time_diff.tv_nsec += (long)(((uint64_t)(time_diff.tv_sec % cnt) * 1000000000) / cnt);
    time_diff.tv_sec /= cnt;
    printf("search time: %ld.%06ld\n", (long)time_diff.tv_sec, time_diff.tv_nsec / 1000);
    fflush(stdout);
}

static void test(void * args);

static void loc_var_func(void * args, Symbol * sym) {
    int frame = 0;
    Context * ctx = NULL;
    RegisterDefinition * reg = NULL;
    ContextAddress addr = 0;
    ContextAddress size = 0;
    SYM_FLAGS flags = 0;
    int symbol_class = 0;
    int type_class = 0;
    Symbol * type = NULL;
    Symbol * index_type = NULL;
    Symbol * base_type = NULL;
    ContextAddress length = 0;
    int64_t lower_bound = 0;
    void * value = NULL;
    size_t value_size = 0;
    int value_big_endian = 0;
    char * name = NULL;
    StackFrame * frame_info = NULL;
    LocationInfo * loc_info = NULL;

    if (get_symbol_flags(sym, &flags) < 0) {
        error("get_symbol_flags");
    }
    if (get_symbol_name(sym, &name) < 0) {
        error("get_symbol_name");
    }
    if (get_symbol_address(sym, &addr) < 0) {
        if ((get_symbol_register(sym, &ctx, &frame, &reg) < 0 || reg == NULL) &&
            (get_symbol_value(sym, &value, &value_size, &value_big_endian) < 0 || value == NULL)) {
            int err = errno;
            if (strncmp(errno_to_str(err), "Object location or value info not available", 43) == 0) return;
            if (strncmp(errno_to_str(err), "No object location info found", 29) == 0) return;
            if (strncmp(errno_to_str(err), "Object is not available", 23) == 0) return;
            if (strncmp(errno_to_str(err), "Division by zero in location", 28) == 0) return;
            if (strncmp(errno_to_str(err), "Cannot find loader debug", 24) == 0) return;
            errno = err;
            error("get_symbol_value");
        }
    }
    else if (get_location_info(sym, &loc_info) < 0) {
        error("get_location_info");
    }
    else if (get_frame_info(elf_ctx, STACK_TOP_FRAME, &frame_info) < 0) {
        error("get_frame_info");
    }
    else {
        Trap trap;
        assert(loc_info->cmds_cnt > 0);
        assert(loc_info->size == 0 || (loc_info->addr <= pc && loc_info->addr + loc_info->size > pc));
        if (set_trap(&trap)) {
            LocationExpressionState * state = evaluate_location_expression(elf_ctx, frame_info, loc_info->cmds, loc_info->cmds_cnt, NULL, 0);
            if (state->stk_pos != 1) str_exception(ERR_OTHER, "invalid location expression stack");
            if (state->stk[0] != addr) str_fmt_exception(ERR_OTHER,
                "ID 0x%" PRIX64 ": invalid location expression result 0x%" PRIX64 " != 0x%" PRIX64,
                get_symbol_object(sym)->mID, state->stk[0], addr);
            clear_trap(&trap);
        }
        else {
            error("evaluate_location_expression");
        }
    }
    if (get_symbol_class(sym, &symbol_class) < 0) {
        error("get_symbol_class");
    }
    if (get_symbol_type(sym, &type) < 0) {
        error("get_symbol_type");
    }
    if (get_symbol_size(sym, &size) < 0) {
        int ok = 0;
        int err = errno;
        if (type != NULL) {
            char * type_name;
            unsigned type_flags;
            if (get_symbol_name(type, &type_name) < 0) {
                error("get_symbol_name");
            }
            if (get_symbol_flags(type, &type_flags) < 0) {
                error("get_symbol_flags");
            }
            if (name == NULL && type_name != NULL && strcmp(type_name, "exception") == 0 && (type_flags & SYM_FLAG_CLASS_TYPE)) {
                /* GCC does not tell size of std::exception class */
                ok = 1;
            }
        }
        if (!ok) {
            errno = err;
            error("get_symbol_size");
        }
    }
    if (type != NULL) {
        Symbol * container = NULL;
        if (get_symbol_type_class(sym, &type_class) < 0) {
            error("get_symbol_type_class");
        }
        if (get_symbol_flags(type, &flags) < 0) {
            error("get_symbol_flags");
        }
        if (get_symbol_index_type(type, &index_type) < 0) {
            if (type_class == TYPE_CLASS_ARRAY) {
                error("get_symbol_index_type");
            }
        }
        if (get_symbol_base_type(type, &base_type) < 0) {
            if (type_class == TYPE_CLASS_ARRAY || type_class == TYPE_CLASS_FUNCTION ||
                type_class == TYPE_CLASS_POINTER || type_class == TYPE_CLASS_MEMBER_PTR) {
                error("get_symbol_base_type");
            }
        }
        if (get_symbol_container(type, &container) < 0) {
            if (type_class == TYPE_CLASS_MEMBER_PTR) {
                error("get_symbol_container");
            }
        }
        if (get_symbol_length(type, &length) < 0) {
            if (type_class == TYPE_CLASS_ARRAY) {
                error("get_symbol_length");
            }
        }
        if (type_class == TYPE_CLASS_ARRAY) {
            if (get_symbol_lower_bound(type, &lower_bound) < 0) {
                error("get_symbol_lower_bound");
            }
        }
        else if (type_class == TYPE_CLASS_ENUMERATION) {
            int i;
            int count = 0;
            Symbol ** children = NULL;
            if (get_symbol_children(type, &children, &count) < 0) {
                error("get_symbol_children");
            }
            for (i = 0; i < count; i++) {
                void * value = NULL;
                size_t value_size = 0;
                int big_endian = 0;
                if (get_symbol_value(children[i], &value, &value_size, &big_endian) < 0) {
                    error("get_symbol_value");
                }
            }
        }
        else if (type_class == TYPE_CLASS_COMPOSITE) {
            int i;
            int count = 0;
            Symbol ** children = NULL;
            if (get_symbol_children(type, &children, &count) < 0) {
                error("get_symbol_children");
            }
            for (i = 0; i < count; i++) {
                int member_class = 0;
                ContextAddress offs = 0;
                if (get_symbol_class(children[i], &member_class) < 0) {
                    error("get_symbol_class");
                }
                if (member_class == SYM_CLASS_REFERENCE) {
                    if (get_symbol_address(children[i], &offs) < 0) {
                        if (get_symbol_offset(children[i], &offs) < 0) {
#if 0
                            int ok = 0;
                            int err = errno;
                            unsigned type_flags;
                            if (get_symbol_flags(children[i], &type_flags) < 0) {
                                error("get_symbol_flags");
                            }
                            if (type_flags & SYM_FLAG_EXTERNAL) ok = 1;
                            if (!ok) {
                                errno = err;
                                error("get_symbol_offset");
                            }
#endif
                        }
                    }
                }
                else if (member_class == SYM_CLASS_VALUE) {
                    void * value = NULL;
                    size_t value_size = 0;
                    int big_endian = 0;
                    if (get_symbol_value(children[i], &value, &value_size, &big_endian) < 0) {
                        error("get_symbol_value");
                    }
                }
            }
        }
    }
}

static void next_pc(void) {
    Symbol * sym = NULL;
    CodeArea area;
    ContextAddress lt_addr;
    ELF_File * lt_file;
    ELF_Section * lt_sec;
    ObjectInfo * func_object = NULL;
    struct timespec time_now;
    Trap trap;
    int test_cnt = 0;
    int loaded = mem_region_pos < 0;

    for (;;) {
        if (mem_region_pos < 0) {
            mem_region_pos = 0;
            pc = mem_map.regions[mem_region_pos].addr;
        }
        else if (pc + 5 < mem_map.regions[mem_region_pos].addr + mem_map.regions[mem_region_pos].size) {
            pc += 5;
        }
        else if (mem_region_pos + 1 < (int)mem_map.region_cnt) {
            mem_region_pos++;
            pc = mem_map.regions[mem_region_pos].addr;
        }
        else {
            mem_region_pos++;
            pc = 0;
            print_time(time_start, test_cnt);
            post_event_with_delay(test, NULL, 1000000);
            test_posted = 1;
            return;
        }

        while ((mem_map.regions[mem_region_pos].flags & MM_FLAG_X) == 0) {
            if (mem_region_pos + 1 < (int)mem_map.region_cnt) {
                mem_region_pos++;
                pc = mem_map.regions[mem_region_pos].addr;
            }
            else {
                mem_region_pos++;
                pc = 0;
                print_time(time_start, test_cnt);
                post_event_with_delay(test, NULL, 1000000);
                test_posted = 1;
                return;
            }
        }

        set_regs_PC(elf_ctx, pc);
        send_context_changed_event(elf_ctx);

        func_object = NULL;
        if (find_symbol_by_addr(elf_ctx, STACK_NO_FRAME, pc, &sym) < 0) {
            if (get_error_code(errno) != ERR_SYM_NOT_FOUND) {
                error("find_symbol_by_addr");
            }
        }
        else {
            char * name = NULL;
            ContextAddress addr = 0;
            ContextAddress size = 0;
            func_object = get_symbol_object(sym);
            if (get_symbol_name(sym, &name) < 0) {
                error("get_symbol_name");
            }
            if (get_symbol_address(sym, &addr) < 0) {
                error("get_symbol_address");
            }
            if (get_symbol_size(sym, &size) < 0) {
                error("get_symbol_size");
            }
            if (pc < addr || pc >= addr + size) {
                errno = ERR_OTHER;
                error("invalid symbol address");
            }
            if (name != NULL) {
                char * name_buf = tmp_strdup(name);
                if (find_symbol_by_name(elf_ctx, STACK_TOP_FRAME, 0, name_buf, &sym) < 0) {
                    if (get_error_code(errno) != ERR_SYM_NOT_FOUND) {
                        error("find_symbol_by_name");
                    }
                }
                else {
                    if (get_symbol_name(sym, &name) < 0) {
                        error("get_symbol_name");
                    }
                    if (strcmp(name_buf, name) != 0) {
                        errno = ERR_OTHER;
                        error("strcmp(name_buf, name)");
                    }
                }
            }
        }

        if (find_symbol_by_name(elf_ctx, STACK_TOP_FRAME, 0, "@ non existing name @", &sym) < 0) {
            if (get_error_code(errno) != ERR_SYM_NOT_FOUND) {
                error("find_symbol_by_name");
            }
        }

        line_area_ok = 0;
        memset(&area, 0, sizeof(area));
        if (address_to_line(elf_ctx, pc, pc + 1, addr_to_line_callback, &area) < 0) {
            error("address_to_line");
        }
        else if (area.start_line > 0) {
            char * elf_file_name = tmp_strdup(area.file);
            if (area.start_address > pc || area.end_address <= pc) {
                errno = set_errno(ERR_OTHER, "Invalid line area address");
                error("address_to_line");
            }
            if (line_to_address(elf_ctx, elf_file_name, area.start_line, area.start_column, line_to_addr_callback, &area) < 0) {
                error("line_to_address");
            }
            if (!line_area_ok) {
                errno = set_errno(ERR_OTHER, "Invalid line area address");
                error("line_to_address");
            }
        }

        lt_file = NULL;
        lt_sec = NULL;
        lt_addr = elf_map_to_link_time_address(elf_ctx, pc, &lt_file, &lt_sec);
        assert(lt_file != NULL);
        assert(pc == elf_map_to_run_time_address(elf_ctx, lt_file, lt_sec, lt_addr));
        if (set_trap(&trap)) {
            get_dwarf_stack_frame_info(elf_ctx, lt_file, lt_sec, lt_addr);
            clear_trap(&trap);
        }
        else {
            error("get_dwarf_stack_frame_info");
        }

        if (enumerate_symbols(elf_ctx, STACK_TOP_FRAME, loc_var_func, NULL) < 0) {
            error("enumerate_symbols");
        }

        if (func_object != NULL) {
            if (set_trap(&trap)) {
                StackFrame * frame = NULL;
                if (get_frame_info(elf_ctx, STACK_TOP_FRAME, &frame) < 0) exception(errno);
                if (frame->fp != frame_addr) {
                    PropertyValue v;
                    uint64_t addr = 0;
                    memset(&v, 0, sizeof(v));
                    read_and_evaluate_dwarf_object_property(elf_ctx, STACK_TOP_FRAME, func_object, AT_frame_base, &v);
                    if (v.mPieceCnt == 1 && v.mPieces[0].reg != NULL && v.mPieces[0].bit_size == 0) {
                        if (read_reg_value(frame, v.mPieces[0].reg, &addr) < 0) exception(errno);
                    }
                    else {
                        addr = get_numeric_property_value(&v);
                    }
                    if (addr != frame->fp) {
                        /* AT_frame_base is not valid in prologue and epilogue.
                        str_exception(ERR_OTHER, "Invalid FP");
                        */
                    }
                }
                clear_trap(&trap);
            }
            else if (trap.error != ERR_SYM_NOT_FOUND) {
                error("AT_frame_base");
            }
        }

        test_cnt++;
        if (test_cnt % 10 == 0) tmp_gc();

        if (loaded) {
            struct timespec time_diff;
            clock_gettime(CLOCK_REALTIME, &time_now);
            time_diff.tv_sec = time_now.tv_sec - time_start.tv_sec;
            if (time_now.tv_nsec < time_start.tv_nsec) {
                time_diff.tv_sec--;
                time_diff.tv_nsec = time_now.tv_nsec + 1000000000 - time_start.tv_nsec;
            }
            else {
                time_diff.tv_nsec = time_now.tv_nsec - time_start.tv_nsec;
            }
            printf("load time: %ld.%06ld\n", (long)time_diff.tv_sec, time_diff.tv_nsec / 1000);
            fflush(stdout);
            time_start = time_now;
            loaded = 0;
        }
        else if (test_cnt >= 10000) {
            print_time(time_start, test_cnt);
            clock_gettime(CLOCK_REALTIME, &time_start);
            test_posted = 1;
            post_event(test, NULL);
            return;
        }
    }
}

static void next_file(void) {
    unsigned j;
    ELF_File * f = NULL;
    struct stat st;

    if (pass_cnt == files_cnt) exit(0);
    elf_file_name = files[pass_cnt % files_cnt];

    printf("File: %s\n", elf_file_name);
    fflush(stdout);
    if (stat(elf_file_name, &st) < 0) {
        printf("Cannot stat ELF: %s\n", errno_to_str(errno));
        exit(1);
    }

    clock_gettime(CLOCK_REALTIME, &time_start);

    f = elf_open(elf_file_name);
    if (f == NULL) {
        printf("Cannot open ELF: %s\n", errno_to_str(errno));
        exit(1);
    }

    if (elf_ctx == NULL) {
        elf_ctx = create_context("test");
        elf_ctx->stopped = 1;
        elf_ctx->pending_intercept = 1;
        elf_ctx->mem = elf_ctx;
        elf_ctx->big_endian = f->big_endian;
        list_add_first(&elf_ctx->ctxl, &context_root);
        elf_ctx->ref_count++;
    }

    context_clear_memory_map(&mem_map);
    for (j = 0; j < f->pheader_cnt; j++) {
        MemoryRegion * r = NULL;
        ELF_PHeader * p = f->pheaders + j;
        if (p->type != PT_LOAD) continue;
        if (mem_map.region_cnt >= mem_map.region_max) {
            mem_map.region_max += 8;
            mem_map.regions = (MemoryRegion *)loc_realloc(mem_map.regions, sizeof(MemoryRegion) * mem_map.region_max);
        }
        r = mem_map.regions + mem_map.region_cnt++;
        memset(r, 0, sizeof(MemoryRegion));
        r->addr = (ContextAddress)p->address;
        r->file_name = loc_strdup(elf_file_name);
        r->file_offs = p->offset;
        r->size = (ContextAddress)p->mem_size;
        r->flags = MM_FLAG_R | MM_FLAG_W;
        if (p->flags & PF_X) r->flags |= MM_FLAG_X;
        r->dev = st.st_dev;
        r->ino = st.st_ino;
    }
    if (mem_map.region_cnt == 0) {
        for (j = 0; j < f->section_cnt; j++) {
            ELF_Section * sec = f->sections + j;
            if (sec->size == 0) continue;
            if (sec->name == NULL) continue;
            if (strcmp(sec->name, ".text") == 0 ||
                strcmp(sec->name, ".data") == 0 ||
                strcmp(sec->name, ".bss") == 0) {
                MemoryRegion * r = NULL;
                if (mem_map.region_cnt >= mem_map.region_max) {
                    mem_map.region_max += 8;
                    mem_map.regions = (MemoryRegion *)loc_realloc(mem_map.regions, sizeof(MemoryRegion) * mem_map.region_max);
                }
                r = mem_map.regions + mem_map.region_cnt++;
                memset(r, 0, sizeof(MemoryRegion));
                r->addr = (ContextAddress)(sec->addr + 0x10000);
                r->size = (ContextAddress)sec->size;
                r->file_offs = sec->offset;
                r->bss = strcmp(sec->name, ".bss") == 0;
                r->dev = st.st_dev;
                r->ino = st.st_ino;
                r->file_name = loc_strdup(elf_file_name);
                r->sect_name = loc_strdup(sec->name);
                r->flags = MM_FLAG_R | MM_FLAG_W;
                if (strcmp(sec->name, ".text") == 0) r->flags |= MM_FLAG_X;
            }
        }
    }
    if (mem_map.region_cnt == 0) {
        printf("File has no program headers.\n");
        exit(1);
    }
    memory_map_event_module_loaded(elf_ctx);
    mem_region_pos = -1;

    reg_size = 0;
    memset(reg_defs, 0, sizeof(reg_defs));
    memset(reg_vals, 0, sizeof(reg_vals));
    for (j = 0; j < MAX_REGS - 1; j++) {
        RegisterDefinition * r = reg_defs + j;
        r->big_endian = f->big_endian;
        r->dwarf_id = (int16_t)(j == 0 ? -1 : j - 1);
        r->eh_frame_id = r->dwarf_id;
        r->name = reg_names[j];
        if (j == 0) {
            snprintf(reg_names[j], sizeof(reg_names[j]), "PC");
        }
        else {
            snprintf(reg_names[j], sizeof(reg_names[j]), "R%d", j - 1);
        }
        r->offset = reg_size;
        r->size = f->elf64 ? 8 : 4;
        if (j == 0) r->role = "PC";
        reg_size += r->size;
    }

    pc = 0;
    pass_cnt++;

    test_posted = 1;
    post_event(test, NULL);
}

static void test(void * args) {
    assert(test_posted);
    test_posted = 0;
    if (elf_file_name == NULL || mem_region_pos >= (int)mem_map.region_cnt) {
        next_file();
    }
    else {
        next_pc();
    }
}

static void add_dir(const char * dir_name) {
    DIR * dir = opendir(dir_name);
    if (dir == NULL) {
        printf("Cannot open '%s' directory\n", dir_name);
        fflush(stdout);
        exit(1);
    }
    for (;;) {
        struct dirent * e = readdir(dir);
        char path[FILE_PATH_SIZE];
        struct stat st;
        if (e == NULL) break;
        if (strcmp(e->d_name, ".") == 0) continue;
        if (strcmp(e->d_name, "..") == 0) continue;
        if (strcmp(e->d_name + strlen(e->d_name) - 6, ".debug") == 0) continue;
        if (strcmp(e->d_name + strlen(e->d_name) - 4, ".txt") == 0) continue;
        snprintf(path, sizeof(path), "%s/%s", dir_name, e->d_name);
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                add_dir(path);
            }
            else {
                int fd = open(path, O_RDONLY | O_BINARY, 0);
                if (fd < 0) {
                    printf("File %s: %s\n", path, errno_to_str(errno));
                }
                else {
                    close(fd);
                    if (files_cnt >= files_max) {
                        files_max += 8;
                        files = (char **)loc_realloc(files, files_max * sizeof(char *));
                    }
                    files[files_cnt++] = loc_strdup(path);
                }
            }
        }
    }
    closedir(dir);
}

void init_contexts_sys_dep(void) {
    const char * dir_name = "files";
    add_dir(dir_name);
    test_posted = 1;
    post_event(test, NULL);
}