#include "inline_hook.h"
#include <stdlib.h>
#include <assert.h>
#include <inttypes.h>
#include <string.h>

#ifdef _WIN32
#   include <windows.h>
#else
#   include <stdint.h>
#   include <unistd.h>
#   include <sys/mman.h>
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

static int _system_modify_opcode(void* addr, size_t size, void (*callback)(void*), void* data)
{
    const size_t page_size = _get_page_size();

    int flag_failure = 0;
    uint8_t* start_addr = (uint8_t*)_page_of(addr, page_size);
    uint8_t* end_addr = (uint8_t*)addr + size;

    const size_t n_page = ((end_addr - start_addr - 1) / page_size) + 1;
    const size_t protect_size = page_size * n_page;

    /* Remove memory protect */
#if defined(_WIN32)
    DWORD lpflOldProtect;
    flag_failure = 0 == VirtualProtect(start_addr, protect_size, PAGE_EXECUTE_READWRITE, &lpflOldProtect);
#elif defined(__linux__)
    flag_failure = -1 == mprotect(start_addr, protect_size, PROT_READ | PROT_WRITE | PROT_EXEC);
#endif
    if (flag_failure)
    {
        return -1;
    }

    /* call callback */
    callback(data);

    /* Add memory protect */
#if defined(_WIN32)
    flag_failure = 0 == VirtualProtect(start_addr, protect_size, PAGE_EXECUTE_READ, &lpflOldProtect);
#elif defined(__linux__)
    flag_failure = -1 == mprotect(start_addr, protect_size, PROT_READ | PROT_EXEC);
#endif
    assert(flag_failure == 0);

    return 0;
}

#if defined(__i386__) || defined(__amd64__) || defined(_M_IX86) || defined(_M_AMD64)

#include "Zydis/Zydis.h"

#define X86_64_MAX_INSTRUCTION_SIZE         15
#define X86_64_COND_JUMP_SHORT_SIZE         2
#define X86_64_COND_JUMP_NEAR_SIZE          6

typedef struct x86_64_convert_ctx
{
    size_t      o_offset;               /**< offset for #trampline_x86_64_t::wrap_opcode */
    size_t      t_offset;               /**< offset for #trampline_x86_64_t::addr_target */
    size_t      redirect_cnt;           /**< Counter for redirect field */
}x86_64_convert_ctx_t;
#define X86_64_CONVERT_CTX_INIT { 0, 0, 0 }

typedef struct trampline_x86_64
{
    uint8_t*    addr_target;            /**< Target function address */
    uint8_t*    addr_detour;            /**< Detour function address */
    uint8_t     redirect_opcode[5];     /**< Opcode to redirect to detour function */
    uint8_t     backup_opcode[5];       /**< Original function code for recover inject */

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
     *             | force redirect code      | -> 5 bytes
     *         n+5 |                          |
     *             | ------------------------ |
     *             |                          |
     *       [gap] | 0xcc                     | -> any space left must set to INT3
     *             |                          |
     *             | ------------------------ |
     *          24 |                          |
     *             | redirect space           | -> 15 bytes (3 jmp opcodes, each take 5 bytes)
     *          39 |                          |
     *             | ------------------------ |
     *          40 |                          |
     *             | 0xcc                     | -> any space left must set to INT3
     *          63 |                          |
     * [HIGH ADDR] | ------------------------ |
     * ```
     * 
     * Field explain as:
     * [0, n]:      Store translated original function opcode. Redirect code inject into target function always take 5
     *              bytes, that means the worst case of original function opcode is 4 bytes whole instructions and 1 broken
     *              instruction. Since max length of x86_64 instructions is 15 bytes, this field will max take 19 bytes.
     * [n+1, n+5]:  Redirect opcode to jump back to original function.
     * [gap]:       There might be some gap due to unknown size of [0, n]. For safety it is set to `INT3` (0xcc). This
     *              aera does not exists if n==18.
     * [24, 39]:    Redirect space contains 3 `JMP` instructions. Need that for `JCXZ`-like opcode which only has short
     *              jump (128 range). We only need three because inject only cost 5 bytes, so the worst case is 3 short
     *              jump instructions.
     * [40, 63]:    Reserved, not used.
     */
    uint8_t     wrap_opcode[64];
}trampline_x86_64_t;

static uint8_t* _x86_64_wrap_opcode_get_redirect_addr(trampline_x86_64_t* handle, size_t n)
{
    return &handle->wrap_opcode[24 + n * 5];
}

static void _x86_64_fill_jump_code_near(uint8_t jump_code[5], intptr_t addr_diff)
{
    /**
     * 5 byte(jmp rel32)
     */
    jump_code[0] = 0xE9;
    uint32_t code = (uint32_t)(addr_diff - 5);
    memcpy(&jump_code[1], &code, sizeof(code));
}

static void _x86_64_do_inject(void* arg)
{
    trampline_x86_64_t* handle = arg;
    memcpy(handle->addr_target, handle->redirect_opcode, sizeof(handle->redirect_opcode));
}

static void _x86_64_undo_inject(void* arg)
{
    trampline_x86_64_t* handle = arg;
    memcpy(handle->addr_target, handle->backup_opcode, sizeof(handle->backup_opcode));
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

static int _x86_64_try_convert_jmp(trampline_x86_64_t* handle, const ZydisDecodedInstruction* insn, x86_64_convert_ctx_t* ctx)
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
    return 0

    switch (insn->mnemonic)
    {
    case ZYDIS_MNEMONIC_JB:     FILL_JUMP_NEAR_CODE_AND_RETURN(0x82);
    case ZYDIS_MNEMONIC_JBE:    FILL_JUMP_NEAR_CODE_AND_RETURN(0x86);

    case ZYDIS_MNEMONIC_JCXZ:   //-fallthrough
    case ZYDIS_MNEMONIC_JECXZ: {
        /* Calculate redirect opcode position */
        uint8_t* opcode_pos = _x86_64_wrap_opcode_get_redirect_addr(handle, ctx->redirect_cnt++);
        /* JCXZ [redirect opcode] */
        uint8_t relative_addr = (uint8_t)(opcode_pos - &handle->wrap_opcode[ctx->o_offset] - 2);
        handle->wrap_opcode[ctx->o_offset++] = 0xe3;
        handle->wrap_opcode[ctx->o_offset++] = relative_addr;
        /* JMP [original jcxz position] */
        uint8_t* dst_addr = _x86_64_get_dist_addr(handle->addr_target + ctx->t_offset, &insn->operands[0]);
        _x86_64_fill_jump_code_near(opcode_pos, dst_addr - opcode_pos);
        return 0;
    }

    case ZYDIS_MNEMONIC_JL:     FILL_JUMP_NEAR_CODE_AND_RETURN(0x8c);
    case ZYDIS_MNEMONIC_JLE:    FILL_JUMP_NEAR_CODE_AND_RETURN(0x8e);
    case ZYDIS_MNEMONIC_JMP: {
        uint8_t* dist_addr = _x86_64_get_dist_addr(handle->addr_target + ctx->t_offset, &insn->operands[0]);
        _x86_64_fill_jump_code_near(&handle->wrap_opcode[ctx->o_offset], dist_addr - &handle->wrap_opcode[ctx->o_offset]);
        ctx->o_offset += 5;
        return 0;
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
    return -1;

#undef FILL_JUMP_NEAR_CODE_AND_RETURN
}

/**
 * @brief Generate swap code and jump to original function
 * @return  If return size if smaller than `num_opcode`, it is failure
 */
static int _x86_64_generate_trampoline_opcode(trampline_x86_64_t* handle)
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

        if (_x86_64_try_convert_jmp(handle, &instruction, &convert_ctx) == 0)
        {
            continue;
        }

        memcpy(&handle->wrap_opcode[convert_ctx.o_offset], &handle->addr_target[convert_ctx.t_offset], instruction.length);
        convert_ctx.o_offset += instruction.length;
    }

    int32_t addr_diff = &handle->addr_target[convert_ctx.t_offset] - &handle->wrap_opcode[convert_ctx.o_offset];
    _x86_64_fill_jump_code_near(&handle->wrap_opcode[convert_ctx.o_offset], addr_diff);

    return 0;
}

static int _x86_64_inline_hook_inject(void** origin, void* target, void* detour)
{
    trampline_x86_64_t* handle = malloc(sizeof(trampline_x86_64_t));
    if (handle == NULL)
    {
        return -1;
    }
    memset(handle->wrap_opcode, 0xcc, sizeof(handle->wrap_opcode));
    handle->addr_target = target;
    handle->addr_detour = detour;
    _x86_64_fill_jump_code_near(handle->redirect_opcode, (char*)detour - (char*)target);

    if (_x86_64_generate_trampoline_opcode(handle) < 0)
    {
        free(handle);
        return -1;
    }

    if (_system_modify_opcode(target, sizeof(handle->redirect_opcode), _x86_64_do_inject, handle) < 0)
    {
        free(handle);
        return -1;
    }

    *origin = handle->wrap_opcode;

    return 0;
}

static void _x86_64_inline_hook_uninject(void* origin)
{
    trampline_x86_64_t* handle = container_of(origin, trampline_x86_64_t, wrap_opcode);
    if (_system_modify_opcode(handle->addr_target, sizeof(handle->redirect_opcode), _x86_64_undo_inject, handle) > 0)
    {
        assert(!"modify opcode failed");
    }
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
typedef struct trampoline_arm64
{
    void*       addr_target;            /**< Target function address */
    void*       addr_detour;            /**< Detour function address */
    uint32_t    redirect_opcode[3];     /**< Opcode to redirect to detour function */
    uint32_t    backup_opcode[3];       /**< Original function code */
#if defined(_MSC_VER)
#   pragma warning(push)
#   pragma warning(disable: 4200)
#endif
    uint32_t    wrap_opcode[];          /**< Opcode to call original function */
#if defined(_MSC_VER)
#   pragma warning(pop)
#endif
}trampoline_arm64_t;

static void _reflash_insn_cache(void)
{
#if defined(__ARM_ARCH_6T2__) ||\
    defined(__ARM_ARCH_7__) ||\
    defined(__ARM_ARCH_7A__) ||\
    defined(__ARM_ARCH_7R__) ||\
    defined(__ARM_ARCH_7M__) ||\
    defined(__ARM_ARCH_7S__) ||\
    defined(__aarch64__)

    __asm__ __volatile__(
        "mov    r0, #0\n\t"
        "mcr    p15, 0, r0, c7, c1, 0\n\t"  /* invalidate I-cache inner shareable */
        "mcr    p15, 0, r0, c7, c5, 0"      /* I+BTB cache invalidate */
        :::"r0"
    );

#endif
}

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
    trampoline_arm64_t* handle = arg;

    size_t copy_size = (handle->redirect_opcode[2] != 0 ? 3 : 1) * sizeof(uint32_t);
    memcpy(handle->addr_target, handle->redirect_opcode, copy_size);
}

static void _arm_init_trampoline(trampoline_arm64_t* handle, uint32_t jump_code[3], void* target, void* detour)
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
    trampoline_arm64_t* handle = arg;

    size_t copy_size = (handle->redirect_opcode[2] != 0 ? 3 : 1) * sizeof(uint32_t);
    memcpy(handle->addr_target, handle->backup_opcode, copy_size);
}

static int _arm64_inline_hook_inject(void** origin, void* target, void* detour)
{
    uint32_t jump_code[3] = { 0, 0, 0 };
    _arm_fill_jump_code(jump_code, target, detour);

    trampoline_arm64_t* handle = calloc(1, sizeof(trampoline_arm64_t) + sizeof(uint32_t) * (3 * 2));
    if (handle == NULL)
    {
        return -1;
    }
    _arm_init_trampoline(handle, jump_code, target, detour);

    if (_system_modify_opcode(target, jump_code[2] != 0 ? 3 : 1, _arm_do_inject, handle) < 0)
    {
        free(handle);
        return -1;
    }

    _reflash_insn_cache();
    *origin = handle->wrap_opcode;

    return 0;
}

static void _arm64_inline_hook_uninject(void* origin)
{
    trampoline_arm64_t* handle = container_of(origin, trampoline_arm64_t, wrap_opcode);

    if (_system_modify_opcode(handle->addr_target, handle->redirect_opcode[2] != 0 ? 3 : 1, _arm_undo_inject, handle) > 0)
    {
        assert(!"modify opcode failed");
    }
}

INLINE_HOOK_MAKE_INTERFACE(_arm64_inline_hook_inject, _arm64_inline_hook_uninject)

#else
#   error "unsupport hardware platform"
INLINE_HOOK_MAKE_INTERFACE(NULL, NULL)
#endif
