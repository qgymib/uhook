#include "inline_hook.h"
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <inttypes.h>
#include <string.h>

#if defined(_WIN32)
#   include <windows.h>
#else
#   include <stdint.h>
#   include <unistd.h>
#   include <sys/mman.h>
#endif

//#define INLINE_HOOK_DEBUG
#if defined(INLINE_HOOK_DEBUG)
#   define LOG(fmt, ...)   printf("[%s:%d] " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)
#else
#   define LOG(fmt, ...)   do {} while (0)
#endif

/**
 * @brief Cast a member of a structure out to the containing structure
 * @param[in] ptr       The pointer to the member.
 * @param[in] TYPE      The type of the container struct this is embedded in.
 * @param[in] member    The name of the member within the struct.
 * @return              The address of the containing structure
 */
#if !defined(container_of)
#   define container_of(ptr, TYPE, member)    \
        ((TYPE*)((uint8_t*)(ptr) - (size_t)&((TYPE*)0)->member))
#endif

#define INLINE_HOOK_MAKE_INTERFACE(fn_inject, fn_uninject) \
    int inline_hook_inject(void** origin, void* target, void* detour) {\
        int ret;\
        if ((ret = fn_inject(origin, target, detour)) != 0) {\
            *origin = NULL;\
        }\
        return ret;\
    }\
    void inline_hook_uninject(void** origin) {\
        fn_uninject(*origin);\
        *origin = NULL;\
    }

static size_t _get_page_size(void)
{
#if defined(_WIN32)
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    unsigned long page_size = sys_info.dwPageSize;
#elif defined(__linux__)
    long page_size = sysconf(_SC_PAGE_SIZE);
#else
    long page_size = 0;
#endif

    return page_size <= 0 ? 4096 : page_size;
}

/**
 * @brief Get start address of page from given address.
 * @parm[in] addr           Address to calculate
 * @param[in] page_size     Page size
 * @return                  The start address of page
 */
static void* _page_of(void* addr, size_t page_size)
{
    return (char*)((uintptr_t)addr & ~(page_size - 1));
}

/**
 * @brief Set memory protect mode as READ/WRITE/EXEC
 */
static int _system_protect_as_RWE(void* addr, size_t size)
{
    int flag_failure = 0;
#if defined(_WIN32)
    DWORD lpflOldProtect;
    flag_failure = 0 == VirtualProtect(addr, size, PAGE_EXECUTE_READWRITE, &lpflOldProtect);
#elif defined(__linux__)
    flag_failure = -1 == mprotect(addr, size, PROT_READ | PROT_WRITE | PROT_EXEC);
#else
    flag_failure = 1;
#endif
    return flag_failure ? -1 : 0;
}

/**
 * @brief Set memory protect mode as READ/EXEC
 */
static int _system_protect_as_RE(void* addr, size_t size)
{
    int flag_failure = 0;
#if defined(_WIN32)
    DWORD lpflOldProtect;
    flag_failure = 0 == VirtualProtect(addr, size, PAGE_EXECUTE_READ, &lpflOldProtect);
#elif defined(__linux__)
    flag_failure = -1 == mprotect(addr, size, PROT_READ | PROT_EXEC);
#else
    flag_failure = 1;
#endif
    return flag_failure ? -1 : 0;
}

static int _system_modify_opcode(void* addr, size_t size, void (*callback)(void*), void* data)
{
    const size_t page_size = _get_page_size();

    uint8_t* start_addr = (uint8_t*)_page_of(addr, page_size);
    uint8_t* end_addr = (uint8_t*)addr + size;

    const size_t n_page = ((end_addr - start_addr - 1) / page_size) + 1;
    const size_t protect_size = page_size * n_page;

    /* Remove write protect */
    if (_system_protect_as_RWE(start_addr, protect_size) < 0)
    {
        return -1;
    }

    /* call callback */
    callback(data);

    /* Add write protect */
    int ret = _system_protect_as_RE(start_addr, protect_size);
    assert(ret == 0);

    return 0;
}

/**
 * @brief Flush the processor's instruction cache for the region of memory.
 *
 * Some targets require that the instruction cache be flushed, after modifying
 * memory containing code, in order to obtain deterministic behavior.
 * 
 * @param[in] addr      Start address
 * @param[in] size      Address length
 */
static void _flush_instruction_cache(void* addr, size_t size)
{
#if defined(_WIN32)
    HANDLE process_handle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, GetCurrentProcessId());
    if (process_handle == NULL)
    {
        return;
    }

    FlushInstructionCache(process_handle, addr, size);
    CloseHandle(process_handle);
#elif defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache(addr, (uint8_t*)addr + size);
#else
#   error "unsupport flush_instruction_cache"
#endif
}

/**
 * @brief Alloc a block of memory that has EXEC attribute
 * @param[in] size  Memory size
 * @return          Address
 */
static void* _alloc_execute_memory(size_t size)
{
#if defined(_WIN32)
    return VirtualAlloc(NULL, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void* ptr = NULL;
    if (posix_memalign(&ptr, _get_page_size(), size) != 0)
    {
        return NULL;
    }
    if (_system_protect_as_RWE(ptr, size) < 0)
    {
        free(ptr);
        return NULL;
    }

    return ptr;
#endif
}

/**
 * @brief Release memory alloc by #_alloc_execute_memory()
 */
static void _free_execute_memory(void* ptr)
{
#if defined(_WIN32)
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    free(ptr);
#endif
}

#if defined(__i386__) || defined(__amd64__) || defined(_M_IX86) || defined(_M_AMD64)

#include "Zydis/Zydis.h"

#define X86_64_MAX_INSTRUCTION_SIZE         15
#define X86_64_COND_JUMP_SHORT_SIZE         2
#define X86_64_COND_JUMP_NEAR_SIZE          6
#define X86_64_OPCODE_INT3                  (0xcc)

typedef struct x86_64_convert_ctx
{
    size_t      o_offset;               /**< offset for #trampline_x86_64_t::wrap_opcode */
    size_t      t_offset;               /**< offset for #trampline_x86_64_t::addr_target */
    size_t      ext_pos;                /**< position of ext space */
}x86_64_convert_ctx_t;
#define X86_64_CONVERT_CTX_INIT { 0, 0, 31 }

typedef struct x86_64_trampoline
{
    uint8_t*    addr_target;            /**< Target function address */
    uint8_t*    addr_detour;            /**< Detour function address */
    uint8_t     redirect_opcode[14];    /**< Opcode to redirect to detour function */
    uint8_t     backup_opcode[14];      /**< Original function code for recover inject */

    /**
     * @brief Opcode to call original function, 64 bytes.
     * 
     * Instruction layout:
     * ```
     *  [LOW ADDR] | ------------------------ |
     *           0 |                          |
     *             | wrap original code       | -> 5 or more bytes (align to whole instruction, max 4+15=19 bytes)
     *           n |                          |
     *             | ------------------------ |
     *         n+1 |                          |
     *             | force redirect code      | -> 10 bytes
     *        n+10 |                          |
     *             | ------------------------ |
     *             |                          |
     *       [gap] | 0xcc                     | -> any space left must set to INT3
     *             |                          |
     *             | ------------------------ |
     *          31 |                          |
     *             | ext space (0xcc)         | -> EXT space for wrapping opcode. Any space left must set to INT3
     *          63 |                          |
     * [HIGH ADDR] | ------------------------ |
     * ```
     * 
     * Field explain as:
     * [0, n]:      Store translated original function opcode. Redirect code inject into target function always take 5
     *              bytes, that means the worst case of original function opcode is 4 bytes whole instructions and 1 broken
     *              instruction. Since max length of x86_64 instructions is 15 bytes, this field will max take 19 bytes.
     * [n+1, n+10]: Redirect opcode to jump back to original function.
     * [gap]:       There might be some gap due to unknown size of [0, n]. For safety it is set to `INT3` (0xcc). This
     *              aera has minimum 1 byte if n==18.
     * [31, 63]:    EXT space . Need that for `JCXZ`-like opcode which only has short jump (128 range). We only need
     *              three because inject only cost 5 bytes, so the worst case is 3 short jump instructions.
     */
    uint8_t     wrap_opcode[64];
}x86_64_trampoline_t;

static int _x86_64_is_near_size(ptrdiff_t addr_diff)
{
    return -(ptrdiff_t)2147483648 <= addr_diff && addr_diff <= (ptrdiff_t)2147483647;
}

/**
 * ```
 * e9 4_byte_rel_addr
 * ```
 */
static int _x86_64_fill_jump_code_near(uint8_t jump_code[5], ptrdiff_t addr_diff)
{
    assert(_x86_64_is_near_size(addr_diff));

    jump_code[0] = 0xE9;
    uint32_t code = (uint32_t)(addr_diff - 5);
    memcpy(&jump_code[1], &code, sizeof(code));

    return 5;
}

/**
 * ```
 * ff 25 00 00 00 00           jmp qword ptr [rip]      jmp *(%rip)
 * yo ur ad dr re ss he re     some random assembly
 * ```
 */
static int _x86_64_fill_jump_code_far(uint8_t jump_code[14], void* dst_addr)
{
    jump_code[0] = 0xff;
    jump_code[1] = 0x25;
    jump_code[2] = 0x00;
    jump_code[3] = 0x00;
    jump_code[4] = 0x00;
    jump_code[5] = 0x00;
    uint64_t code = (uint64_t)(dst_addr);
    memcpy(&jump_code[6], &code, sizeof(code));
    return 14;
}

static int _x86_64_fill_jump_code(uint8_t jump_code[14], void* src, void* dst)
{
    ptrdiff_t addr_diff = (uint8_t*)dst - (uint8_t*)src;

    return _x86_64_is_near_size(addr_diff) ?
        _x86_64_fill_jump_code_near(jump_code, addr_diff) : _x86_64_fill_jump_code_far(jump_code, dst);
}

static size_t _x86_64_get_opcode_copy_size(const x86_64_trampoline_t* handle)
{
    size_t copy_size = handle->redirect_opcode[sizeof(handle->redirect_opcode) - 1] == X86_64_OPCODE_INT3 ? 5 : 14;
    return copy_size;
}

static void _x86_64_do_inject(void* arg)
{
    x86_64_trampoline_t* handle = arg;
    memcpy(handle->addr_target, handle->redirect_opcode, _x86_64_get_opcode_copy_size(handle));
}

static void _x86_64_undo_inject(void* arg)
{
    x86_64_trampoline_t* handle = arg;
    memcpy(handle->addr_target, handle->backup_opcode, _x86_64_get_opcode_copy_size(handle));
}

static ZydisAddressWidth _x86_64_get_address_width(void)
{
    switch (sizeof(void*))
    {
    case 2:
        return ZYDIS_ADDRESS_WIDTH_16;
    case 4:
        return ZYDIS_ADDRESS_WIDTH_32;
    case 8:
        return ZYDIS_ADDRESS_WIDTH_64;
    default:
        break;
    }
    return ZYDIS_ADDRESS_WIDTH_MAX_VALUE;
}

static uint8_t* _x86_64_get_dist_addr(uint8_t* baseaddr, const ZydisDecodedOperand* oper)
{
    if (oper->imm.is_signed)
    {
        return baseaddr + oper->imm.value.s;
    }
    return baseaddr + oper->imm.value.u;
}

/**
 * @return  0:not covert; 1: success; -1: failure
 */
static int _x86_64_try_convert_jmp(x86_64_trampoline_t* handle, const ZydisDecodedInstruction* insn, x86_64_convert_ctx_t* ctx)
{
#define FILL_JUMP_NEAR_CODE_AND_RETURN(opcode2)    \
    do {\
        handle->wrap_opcode[ctx->o_offset++] = 0x0f;\
        handle->wrap_opcode[ctx->o_offset++] = opcode2;\
        assert(insn->operand_count == 1);\
        uint8_t* dist_addr = _x86_64_get_dist_addr(handle->addr_target + ctx->t_offset, &insn->operands[0]);\
        uint32_t relative_addr = (uint32_t)(dist_addr - &handle->wrap_opcode[ctx->o_offset] - 6);\
        memcpy(&handle->wrap_opcode[ctx->o_offset], &relative_addr, sizeof(relative_addr));\
        ctx->o_offset += 4;\
    } while (0);\
    return 1

    switch (insn->mnemonic)
    {
    case ZYDIS_MNEMONIC_JB:     FILL_JUMP_NEAR_CODE_AND_RETURN(0x82);
    case ZYDIS_MNEMONIC_JBE:    FILL_JUMP_NEAR_CODE_AND_RETURN(0x86);

    case ZYDIS_MNEMONIC_JCXZ:   //-fallthrough
    case ZYDIS_MNEMONIC_JECXZ: {
        /* Calculate redirect opcode position */
        uint8_t* opcode_pos = &handle->wrap_opcode[ctx->ext_pos];
        /* JCXZ [redirect opcode] */
        uint8_t relative_addr = (uint8_t)(opcode_pos - &handle->wrap_opcode[ctx->o_offset] - 2);
        handle->wrap_opcode[ctx->o_offset++] = 0xe3;
        handle->wrap_opcode[ctx->o_offset++] = relative_addr;
        /* JMP [original jcxz position] */
        uint8_t* dst_addr = _x86_64_get_dist_addr(handle->addr_target + ctx->t_offset, &insn->operands[0]);

        int ret;
        if ((ret = _x86_64_fill_jump_code(opcode_pos, opcode_pos, dst_addr)) < 0)
        {
            LOG("generate opcode failed");
            return -1;
        }
        ctx->ext_pos += ret;
        return 1;
    }

    case ZYDIS_MNEMONIC_JL:     FILL_JUMP_NEAR_CODE_AND_RETURN(0x8c);
    case ZYDIS_MNEMONIC_JLE:    FILL_JUMP_NEAR_CODE_AND_RETURN(0x8e);
    case ZYDIS_MNEMONIC_JMP: {
        uint8_t* dist_addr = _x86_64_get_dist_addr(handle->addr_target + ctx->t_offset, &insn->operands[0]);
        int ret;
        if ((ret = _x86_64_fill_jump_code(&handle->wrap_opcode[ctx->o_offset], &handle->wrap_opcode[ctx->o_offset], dist_addr)) < 0)
        {
            LOG("generate opcode failed");
            return -1;
        }
        ctx->o_offset += ret;
        return 1;
    }
    case ZYDIS_MNEMONIC_JNB:    FILL_JUMP_NEAR_CODE_AND_RETURN(0x83);
    case ZYDIS_MNEMONIC_JNBE:   FILL_JUMP_NEAR_CODE_AND_RETURN(0x87);
    case ZYDIS_MNEMONIC_JNL:    FILL_JUMP_NEAR_CODE_AND_RETURN(0x8d);
    case ZYDIS_MNEMONIC_JNLE:   FILL_JUMP_NEAR_CODE_AND_RETURN(0x8f);
    case ZYDIS_MNEMONIC_JNO:    FILL_JUMP_NEAR_CODE_AND_RETURN(0x81);
    case ZYDIS_MNEMONIC_JNP:    FILL_JUMP_NEAR_CODE_AND_RETURN(0x8b);
    case ZYDIS_MNEMONIC_JNS:    FILL_JUMP_NEAR_CODE_AND_RETURN(0x89);
    case ZYDIS_MNEMONIC_JNZ:    FILL_JUMP_NEAR_CODE_AND_RETURN(0x85);
    case ZYDIS_MNEMONIC_JO:     FILL_JUMP_NEAR_CODE_AND_RETURN(0x80);
    case ZYDIS_MNEMONIC_JP:     FILL_JUMP_NEAR_CODE_AND_RETURN(0x8a);
    case ZYDIS_MNEMONIC_JS:     FILL_JUMP_NEAR_CODE_AND_RETURN(0x88);
    case ZYDIS_MNEMONIC_JZ:     FILL_JUMP_NEAR_CODE_AND_RETURN(0x84);
    default:
        break;
    }
    return 0;

#undef FILL_JUMP_NEAR_CODE_AND_RETURN
}

/**
 * @brief Generate swap code and jump to original function
 * @return  If return size if smaller than `num_opcode`, it is failure
 */
static int _x86_64_generate_trampoline_opcode(x86_64_trampoline_t* handle)
{
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, _x86_64_get_address_width());
    ZydisDecodedInstruction instruction;

    x86_64_convert_ctx_t convert_ctx = X86_64_CONVERT_CTX_INIT;
    for (; convert_ctx.t_offset <= sizeof(handle->redirect_opcode) && ZYAN_SUCCESS(ZydisDecoderDecodeBuffer(&decoder,
        (uint8_t*)handle->addr_target + convert_ctx.t_offset, sizeof(handle->wrap_opcode) - convert_ctx.t_offset, &instruction));
        convert_ctx.t_offset += instruction.length)
    {
        /* If RET occur before opcode inject position, it is not save to inject because it may broken other things */
        if (instruction.mnemonic == ZYDIS_MNEMONIC_RET)
        {
            return -1;
        }

        int ret = _x86_64_try_convert_jmp(handle, &instruction, &convert_ctx);
        if (ret == -1)
        {/* failure */
            return -1;
        }
        else if (ret == 1)
        {/* success */
            continue;
        }

        memcpy(&handle->wrap_opcode[convert_ctx.o_offset], &handle->addr_target[convert_ctx.t_offset], instruction.length);
        convert_ctx.o_offset += instruction.length;
    }

    if (_x86_64_fill_jump_code(&handle->wrap_opcode[convert_ctx.o_offset],
        &handle->wrap_opcode[convert_ctx.o_offset], &handle->addr_target[convert_ctx.t_offset]) < 0)
    {
        LOG("generate opcode failed");
        return -1;
    }

    return 0;
}

static int _x86_64_inline_hook_inject(void** origin, void* target, void* detour)
{
    size_t page_size = _get_page_size();
    x86_64_trampoline_t* handle = _alloc_execute_memory(page_size);
    if (handle == NULL)
    {
        return -1;
    }

    memset(handle, X86_64_OPCODE_INT3, sizeof(*handle));
    handle->addr_target = target;
    handle->addr_detour = detour;

    int ret;
    if ((ret = _x86_64_fill_jump_code(handle->redirect_opcode, target, detour)) < 0)
    {
        LOG("generate redirect opcode failed");
        _free_execute_memory(handle);
        return -1;
    }
    memcpy(handle->backup_opcode, target, ret);

    if (_x86_64_generate_trampoline_opcode(handle) < 0)
    {
        _free_execute_memory(handle);
        return -1;
    }

    if (_system_modify_opcode(target, sizeof(handle->redirect_opcode), _x86_64_do_inject, handle) < 0)
    {
        _free_execute_memory(handle);
        return -1;
    }

    *origin = handle->wrap_opcode;
    _flush_instruction_cache(target, _x86_64_get_opcode_copy_size(handle));

    return 0;
}

static void _x86_64_inline_hook_uninject(void* origin)
{
    x86_64_trampoline_t* handle = container_of(origin, x86_64_trampoline_t, wrap_opcode);
    if (_system_modify_opcode(handle->addr_target, sizeof(handle->redirect_opcode), _x86_64_undo_inject, handle) > 0)
    {
        assert(!"modify opcode failed");
    }
    _flush_instruction_cache(handle->addr_target, _x86_64_get_opcode_copy_size(handle));
    _free_execute_memory(handle);
}

int inline_hook_dump(char* buffer, unsigned size, const void* origin)
{
    const x86_64_trampoline_t* handle = container_of(origin, x86_64_trampoline_t, wrap_opcode);

    /* 5 bytes near jump */
    if (handle->redirect_opcode[sizeof(handle->redirect_opcode) - 1] == X86_64_OPCODE_INT3)
    {
        return snprintf(buffer, size,
            "[INJECT]\n"
            "%p | %02x %02x %02x %02x %02x\n"
            "[BACKUP]\n"
            "%p | %02x %02x %02x %02x %02x\n"
            "[OPCODE]\n"
            "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n"
            "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n"
            "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n"
            "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n"
            "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n"
            "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n"
            "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n"
            "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n",
            handle->addr_target, handle->addr_target[0], handle->addr_target[1], handle->addr_target[2], handle->addr_target[3], handle->addr_target[4],
            handle->backup_opcode, handle->backup_opcode[0], handle->backup_opcode[1], handle->backup_opcode[2], handle->backup_opcode[3], handle->backup_opcode[4],
            &handle->wrap_opcode[0], handle->wrap_opcode[0], handle->wrap_opcode[1], handle->wrap_opcode[2], handle->wrap_opcode[3], handle->wrap_opcode[4], handle->wrap_opcode[5], handle->wrap_opcode[6], handle->wrap_opcode[7],
            &handle->wrap_opcode[8], handle->wrap_opcode[8], handle->wrap_opcode[9], handle->wrap_opcode[10], handle->wrap_opcode[11], handle->wrap_opcode[12], handle->wrap_opcode[13], handle->wrap_opcode[14], handle->wrap_opcode[15],
            &handle->wrap_opcode[16], handle->wrap_opcode[16], handle->wrap_opcode[17], handle->wrap_opcode[18], handle->wrap_opcode[19], handle->wrap_opcode[20], handle->wrap_opcode[21], handle->wrap_opcode[22], handle->wrap_opcode[23],
            &handle->wrap_opcode[24], handle->wrap_opcode[24], handle->wrap_opcode[25], handle->wrap_opcode[26], handle->wrap_opcode[27], handle->wrap_opcode[28], handle->wrap_opcode[29], handle->wrap_opcode[30], handle->wrap_opcode[31],
            &handle->wrap_opcode[32], handle->wrap_opcode[32], handle->wrap_opcode[33], handle->wrap_opcode[34], handle->wrap_opcode[35], handle->wrap_opcode[36], handle->wrap_opcode[37], handle->wrap_opcode[38], handle->wrap_opcode[39],
            &handle->wrap_opcode[40], handle->wrap_opcode[40], handle->wrap_opcode[41], handle->wrap_opcode[42], handle->wrap_opcode[43], handle->wrap_opcode[44], handle->wrap_opcode[45], handle->wrap_opcode[46], handle->wrap_opcode[47],
            &handle->wrap_opcode[48], handle->wrap_opcode[48], handle->wrap_opcode[49], handle->wrap_opcode[50], handle->wrap_opcode[51], handle->wrap_opcode[52], handle->wrap_opcode[53], handle->wrap_opcode[54], handle->wrap_opcode[55],
            &handle->wrap_opcode[56], handle->wrap_opcode[56], handle->wrap_opcode[57], handle->wrap_opcode[58], handle->wrap_opcode[59], handle->wrap_opcode[60], handle->wrap_opcode[61], handle->wrap_opcode[62], handle->wrap_opcode[63]);
    }

    /* 14 bytes long jump */
    return snprintf(buffer, size,
        "[INJECT]\n"
        "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n"
        "%p | %02x %02x %02x %02x %02x %02x\n"
        "[BACKUP]\n"
        "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n"
        "%p | %02x %02x %02x %02x %02x %02x\n"
        "[OPCODE]\n"
        "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n"
        "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n"
        "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n"
        "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n"
        "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n"
        "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n"
        "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n"
        "%p | %02x %02x %02x %02x %02x %02x %02x %02x\n",
        &handle->addr_target[0], handle->addr_target[0], handle->addr_target[1], handle->addr_target[2], handle->addr_target[3], handle->addr_target[4], handle->addr_target[5], handle->addr_target[6], handle->addr_target[7],
        &handle->addr_target[8], handle->addr_target[8], handle->addr_target[9], handle->addr_target[10], handle->addr_target[11], handle->addr_target[12], handle->addr_target[13],
        &handle->backup_opcode[0], handle->backup_opcode[0], handle->backup_opcode[1], handle->backup_opcode[2], handle->backup_opcode[3], handle->backup_opcode[4], handle->backup_opcode[5], handle->backup_opcode[6], handle->backup_opcode[7],
        &handle->backup_opcode[8], handle->backup_opcode[8], handle->backup_opcode[9], handle->backup_opcode[10], handle->backup_opcode[11], handle->backup_opcode[12], handle->backup_opcode[13],
        &handle->wrap_opcode[0], handle->wrap_opcode[0], handle->wrap_opcode[1], handle->wrap_opcode[2], handle->wrap_opcode[3], handle->wrap_opcode[4], handle->wrap_opcode[5], handle->wrap_opcode[6], handle->wrap_opcode[7],
        &handle->wrap_opcode[8], handle->wrap_opcode[8], handle->wrap_opcode[9], handle->wrap_opcode[10], handle->wrap_opcode[11], handle->wrap_opcode[12], handle->wrap_opcode[13], handle->wrap_opcode[14], handle->wrap_opcode[15],
        &handle->wrap_opcode[16], handle->wrap_opcode[16], handle->wrap_opcode[17], handle->wrap_opcode[18], handle->wrap_opcode[19], handle->wrap_opcode[20], handle->wrap_opcode[21], handle->wrap_opcode[22], handle->wrap_opcode[23],
        &handle->wrap_opcode[24], handle->wrap_opcode[24], handle->wrap_opcode[25], handle->wrap_opcode[26], handle->wrap_opcode[27], handle->wrap_opcode[28], handle->wrap_opcode[29], handle->wrap_opcode[30], handle->wrap_opcode[31],
        &handle->wrap_opcode[32], handle->wrap_opcode[32], handle->wrap_opcode[33], handle->wrap_opcode[34], handle->wrap_opcode[35], handle->wrap_opcode[36], handle->wrap_opcode[37], handle->wrap_opcode[38], handle->wrap_opcode[39],
        &handle->wrap_opcode[40], handle->wrap_opcode[40], handle->wrap_opcode[41], handle->wrap_opcode[42], handle->wrap_opcode[43], handle->wrap_opcode[44], handle->wrap_opcode[45], handle->wrap_opcode[46], handle->wrap_opcode[47],
        &handle->wrap_opcode[48], handle->wrap_opcode[48], handle->wrap_opcode[49], handle->wrap_opcode[50], handle->wrap_opcode[51], handle->wrap_opcode[52], handle->wrap_opcode[53], handle->wrap_opcode[54], handle->wrap_opcode[55],
        &handle->wrap_opcode[56], handle->wrap_opcode[56], handle->wrap_opcode[57], handle->wrap_opcode[58], handle->wrap_opcode[59], handle->wrap_opcode[60], handle->wrap_opcode[61], handle->wrap_opcode[62], handle->wrap_opcode[63]);
}

INLINE_HOOK_MAKE_INTERFACE(_x86_64_inline_hook_inject, _x86_64_inline_hook_uninject)

#elif defined(__ARM_ARCH_6T2__) ||\
    defined(__ARM_ARCH_7__) ||\
    defined(__ARM_ARCH_7A__) ||\
    defined(__ARM_ARCH_7R__) ||\
    defined(__ARM_ARCH_7M__) ||\
    defined(__ARM_ARCH_7S__) ||\
    defined(__aarch64__)
/**
 * @brief Hook context for arm64
 *
 * The jump code can be either 4 bytes (with address space < 32MB):
 * ```
 * b #(address)
 * ```
 * or 12 bytes (with any address space)
 * ```
 * mov  r0, #(lower 16 bit address)
 * movt r0, #(higher 16 bit address)
 * bx   r0
 * ```
 *
 * To know whether 12 bytes code is used, check if #trampoline_arm64_t::redirect_opcode[2]
 * is non-zero.
 */
typedef struct arm_trampoline
{
    uint8_t*    addr_target;            /**< Target function address */
    uint8_t*    addr_detour;            /**< Detour function address */
    uint32_t    redirect_opcode[3];     /**< Opcode to redirect to detour function */
    uint32_t    backup_opcode[3];       /**< Original function code */
    uint32_t    wrap_opcode[64];        /**< Opcode to call original function */
}arm_trampoline_t;

static void _arm_fill_jump_code_near(uint32_t jump_code[1], intptr_t addr_diff)
{
    jump_code[0] = (((addr_diff - 8) >> 2) & 0x00FFFFFF) | 0xea000000;
}

static void _arm_fill_jump_code_far(uint32_t jump_code[3], void* dest)
{
    uint32_t _fn_stub_l = (uintptr_t)dest & 0x0000FFFF;
    uint32_t _fn_stub_h = ((uintptr_t)dest >> 16) & 0x0000FFFF;

    /* mov  r0, #(lower 16 bit address) */
    jump_code[0] = (_fn_stub_l & 0x00000FFF) | ((_fn_stub_l & 0x0000F000) << 4) | 0xe3000000;
    /* movt r0, #(higher 16 bit address) */
    jump_code[1] = (_fn_stub_h & 0x00000FFF) | ((_fn_stub_h & 0x0000F000) << 4) | 0xe3400000;
    /* bx   r0 */
    jump_code[2] = 0xe12fff10;
}

/**
 * @brief Generate jump code
 * @param[out] jump_code    The generated jump code
 * @param[in] target        The position where code will be executed
 * @param[in] detour        The destination address
 */
static void _arm_fill_jump_code(uint32_t jump_code[3], void* target, void* detour)
{
    intptr_t addr_diff = (intptr_t)detour - (intptr_t)target;
    if (-0x2000000 <= addr_diff && addr_diff < 0x2000000)
    {
        _arm_fill_jump_code_near(jump_code, addr_diff);
    }
    else
    {
        _arm_fill_jump_code_far(jump_code, detour);
    }
}

static void _arm_do_inject(void* arg)
{
    arm_trampoline_t* handle = arg;

    size_t copy_size = (handle->redirect_opcode[2] != 0 ? 3 : 1) * sizeof(uint32_t);
    memcpy(handle->addr_target, handle->redirect_opcode, copy_size);
}

static void _arm_init_trampoline(arm_trampoline_t* handle, uint32_t jump_code[3], void* target, void* detour)
{
    handle->addr_target = target;
    handle->addr_detour = detour;
    memcpy(handle->redirect_opcode, jump_code, sizeof(handle->redirect_opcode));

    uint32_t* p_target = target;
    if (jump_code[2] != 0)
    {
        memcpy(&handle->wrap_opcode[0], target, sizeof(uint32_t) * 3);
        _arm_fill_jump_code_far(&handle->wrap_opcode[3], &p_target[3]);
    }
    else
    {
        memcpy(&handle->wrap_opcode[0], target, sizeof(uint32_t) * 1);
        _arm_fill_jump_code_far(&handle->wrap_opcode[1], &p_target[1]);
    }
}

static void _arm_undo_inject(void* arg)
{
    arm_trampoline_t* handle = arg;

    size_t copy_size = (handle->redirect_opcode[2] != 0 ? 3 : 1) * sizeof(uint32_t);
    memcpy(handle->addr_target, handle->backup_opcode, copy_size);
}

static int _arm64_inline_hook_inject(void** origin, void* target, void* detour)
{
    uint32_t jump_code[3] = { 0, 0, 0 };
    _arm_fill_jump_code(jump_code, target, detour);

    arm_trampoline_t* handle = _alloc_execute_memory(sizeof(arm_trampoline_t) + sizeof(uint32_t) * (3 * 2));
    if (handle == NULL)
    {
        return -1;
    }
    _arm_init_trampoline(handle, jump_code, target, detour);

    if (_system_modify_opcode(target, jump_code[2] != 0 ? 3 : 1, _arm_do_inject, handle) < 0)
    {
        _free_execute_memory(handle);
        return -1;
    }

    *origin = handle->wrap_opcode;
    _flush_instruction_cache(target, sizeof(handle->redirect_opcode));

    return 0;
}

static void _arm64_inline_hook_uninject(void* origin)
{
    arm_trampoline_t* handle = container_of(origin, arm_trampoline_t, wrap_opcode);
    if (_system_modify_opcode(handle->addr_target, handle->redirect_opcode[2] != 0 ? 3 : 1, _arm_undo_inject, handle) > 0)
    {
        assert(!"modify opcode failed");
    }
    _flush_instruction_cache(handle->addr_target, sizeof(handle->redirect_opcode));
    _free_execute_memory(handle);
}

INLINE_HOOK_MAKE_INTERFACE(_arm64_inline_hook_inject, _arm64_inline_hook_uninject)

#else
#   error "unsupport hardware platform"
INLINE_HOOK_MAKE_INTERFACE(NULL, NULL)
#endif
