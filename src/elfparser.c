#include "elfparser.h"
#include <string.h>

#if __BYTE_ORDER == __LITTLE_ENDIAN
#   define HOST_EI_DATA 1
#elif __BYTE_ORDER == __BIG_ENDIAN
#   define HOST_EI_DATA 2
#else
#   error unknown endian
#endif

union ELF_U16
{
    uint16_t    val;
    uint8_t     u[2];
};

union ELF_U32
{
    uint32_t    val;
    uint8_t     u[4];
};

union ELF_U64
{
    uint64_t    val;
    uint8_t     u[8];
};

/**
 * @brief Parser 16bit data as uint16_t
 * @param[in] pdat      Buffer
 * @param[in] EI_DATA   Endian
 * @return              Result
 */
static uint16_t _elf_parser_16bit(const uint8_t* pdat, int EI_DATA)
{
    union ELF_U16 elf_u16;

    if (HOST_EI_DATA == EI_DATA)
    {
        elf_u16.u[0] = pdat[0];
        elf_u16.u[1] = pdat[1];
    }
    else
    {
        elf_u16.u[0] = pdat[1];
        elf_u16.u[1] = pdat[0];
    }
    return elf_u16.val;
}

/**
 * @brief Parser 32bit data as uint32_t
 * @param[in] pdat      Buffer
 * @param[in] EI_DATA   Endian
 * @return              Result
 */
static uint32_t _elf_parser_32bit(const uint8_t* pdat, int EI_DATA)
{
    union ELF_U32 elf_u32;

    if (HOST_EI_DATA == EI_DATA)
    {
        elf_u32.u[0] = pdat[0];
        elf_u32.u[1] = pdat[1];
        elf_u32.u[2] = pdat[2];
        elf_u32.u[3] = pdat[3];
    }
    else
    {
        elf_u32.u[0] = pdat[3];
        elf_u32.u[1] = pdat[2];
        elf_u32.u[2] = pdat[1];
        elf_u32.u[3] = pdat[0];
    }

    return elf_u32.val;
}

/**
 * @brief Parser 64bit data as uint64_t
 * @param[in] pdat      Buffer
 * @param[in] EI_DATA   Endian
 * @return              Result
 */
static uint64_t _elf_parser_64bit(const uint8_t* pdat, int EI_DATA)
{
    union ELF_U64 elf_u64;

    if (HOST_EI_DATA == EI_DATA)
    {
        elf_u64.u[0] = pdat[0];
        elf_u64.u[1] = pdat[1];
        elf_u64.u[2] = pdat[2];
        elf_u64.u[3] = pdat[3];
        elf_u64.u[4] = pdat[4];
        elf_u64.u[5] = pdat[5];
        elf_u64.u[6] = pdat[6];
        elf_u64.u[7] = pdat[7];
    }
    else
    {
        elf_u64.u[0] = pdat[7];
        elf_u64.u[1] = pdat[6];
        elf_u64.u[2] = pdat[5];
        elf_u64.u[3] = pdat[4];
        elf_u64.u[4] = pdat[3];
        elf_u64.u[5] = pdat[2];
        elf_u64.u[6] = pdat[1];
        elf_u64.u[7] = pdat[0];
    }
    return elf_u64.val;
}

static int _elf_parser_program32_header(elf_program_header_t* dst,
    const elf_file_header_t* header, const void* addr, size_t idx)
{
    const uint8_t* pdat = (uint8_t*)addr + header->e_phoff + idx * header->e_phentsize;
    if (idx >= header->e_phnum)
    {
        return -1;
    }

    size_t pos = 0;

    dst->p_type = _elf_parser_32bit(&pdat[pos], header->f_EI_DATA);
    pos += 4;

    dst->p_offset = _elf_parser_32bit(&pdat[pos], header->f_EI_DATA);
    pos += 4;

    dst->p_vaddr = _elf_parser_32bit(&pdat[pos], header->f_EI_DATA);
    pos += 4;

    dst->p_paddr = _elf_parser_32bit(&pdat[pos], header->f_EI_DATA);
    pos += 4;

    dst->p_filesz = _elf_parser_32bit(&pdat[pos], header->f_EI_DATA);
    pos += 4;

    dst->p_memsz = _elf_parser_32bit(&pdat[pos], header->f_EI_DATA);
    pos += 4;

    dst->p_flags = _elf_parser_32bit(&pdat[pos], header->f_EI_DATA);
    pos += 4;

    dst->p_align = _elf_parser_32bit(&pdat[pos], header->f_EI_DATA);
    pos += 4;

    return 0;
}

static int _elf_parser_program64_header(elf_program_header_t* dst,
    const elf_file_header_t* header, const void* addr, size_t idx)
{
    const uint8_t* pdat = (uint8_t*)addr + header->e_phoff + idx * header->e_phentsize;
    if (idx >= header->e_phnum)
    {
        return -1;
    }

    size_t pos = 0;

    dst->p_type = _elf_parser_32bit(&pdat[pos], header->f_EI_DATA);
    pos += 4;

    dst->p_flags = _elf_parser_32bit(&pdat[pos], header->f_EI_DATA);
    pos += 4;

    dst->p_offset = _elf_parser_64bit(&pdat[pos], header->f_EI_DATA);
    pos += 8;

    dst->p_vaddr = _elf_parser_64bit(&pdat[pos], header->f_EI_DATA);
    pos += 8;

    dst->p_paddr = _elf_parser_64bit(&pdat[pos], header->f_EI_DATA);
    pos += 8;

    dst->p_filesz = _elf_parser_64bit(&pdat[pos], header->f_EI_DATA);
    pos += 8;

    dst->p_memsz = _elf_parser_64bit(&pdat[pos], header->f_EI_DATA);
    pos += 8;

    dst->p_align = _elf_parser_64bit(&pdat[pos], header->f_EI_DATA);
    pos += 8;

    return 0;
}

static const char* _elf_dump_header_get_type(uint16_t type)
{
    switch (type)
    {
    case 0x00:      return "NONE";
    case 0x01:      return "REL";
    case 0x02:      return "EXEC";
    case 0x03:      return "DYN (Position-Independent Executable file)";
    case 0x04:      return "CORE";
    case 0xFE00:    return "LOOS";
    case 0xFEFF:    return "HIOS";
    case 0xFF00:    return "LOPROC";
    case 0xFFFF:    return "HIPROC";
    default:
        break;
    }
    return "[Unknown]";
}

static const char* _elf_dump_header_get_osabi(uint8_t osabi)
{
    switch (osabi)
    {
    case 0x00:  return "System V";
    case 0x01:  return "HP-UX";
    case 0x02:  return "NetBSD";
    case 0x03:  return "Linux";
    case 0x04:  return "GNU Hurd";
    case 0x06:  return "Solaris";
    case 0x07:  return "AIX";
    case 0x08:  return "IRIX";
    case 0x09:  return "FreeBSD";
    case 0x0A:  return "Tru64";
    case 0x0B:  return "Novell Modesto";
    case 0x0C:  return "OpenBSD";
    case 0x0D:  return "OpenVMS";
    case 0x0E:  return "NonStop Kernel";
    case 0x0F:  return "AROS";
    case 0x10:  return "Fenix OS";
    case 0x11:  return "CloudABI";
    case 0x12:  return "Stratus Technologies OpenVOS";
    default:    return "[Unknown]";
    }
}

static const char* _elf_dump_header_get_class(uint8_t ei_class)
{
    switch (ei_class)
    {
    case 1:     return "ELF32";
    case 2:     return "ELF64";
    default:    return "[Unknown]";
    }
}

static const char* _elf_dump_header_get_data(uint8_t ei_data)
{
    switch (ei_data)
    {
    case 1:     return "2's complement, little endian";
    case 2:     return "2's complement, big endian";
    default:    return "Unknown data format";
    }
}

static const char* _elf_dump_header_get_machine(uint16_t e_machine)
{
    switch (e_machine)
    {
    case 0x00:  return "No specific instruction set";
    case 0x01:  return "AT&T WE 32100";
    case 0x02:  return "SPARC";
    case 0x03:  return "x86";
    case 0x04:  return "Motorola 68000 (M68k)";
    case 0x05:  return "Motorola 88000 (M88k)";
    case 0x06:  return "Intel MCU";
    case 0x07:  return "Intel 80860";
    case 0x08:  return "MIPS";
    case 0x09:  return "IBM System/370";
    case 0x0A:  return "MIPS RS3000 Little-endian";
    case 0x0E:  return "Hewlett-Packard PA-RISC";
    case 0x0F:  return "Reserved for future use";
    case 0x13:  return "Intel 80960";
    case 0x14:  return "PowerPC";
    case 0x15:  return "PowerPC (64-bit)";
    case 0x16:  return "S390, including S390x";
    case 0x17:  return "IBM SPU/SPC";
    case 0x24:  return "NEC V800";
    case 0x25:  return "Fujitsu FR20";
    case 0x26:  return "TRW RH-32";
    case 0x27:  return "Motorola RCE";
    case 0x28:  return "ARM (up to ARMv7/Aarch32)";
    case 0x29:  return "Digital Alpha";
    case 0x2A:  return "SuperH";
    case 0x2B:  return "SPARC Version 9";
    case 0x2C:  return "Siemens TriCore embedded processor";
    case 0x2D:  return "Argonaut RISC Core";
    case 0x2E:  return "Hitachi H8/300";
    case 0x2F:  return "Hitachi H8/300H";
    case 0x30:  return "Hitachi H8S";
    case 0x31:  return "Hitachi H8/500";
    case 0x32:  return "IA-64";
    case 0x33:  return "Stanford MIPS-X";
    case 0x34:  return "Motorola ColdFire";
    case 0x35:  return "Motorola M68HC12";
    case 0x36:  return "Fujitsu MMA Multimedia Accelerator";
    case 0x37:  return "Siemens PCP";
    case 0x38:  return "Sony nCPU embedded RISC processor";
    case 0x39:  return "Denso NDR1 microprocessor";
    case 0x3A:  return "Motorola Star*Core processor";
    case 0x3B:  return "Toyota ME16 processor";
    case 0x3C:  return "STMicroelectronics ST100 processor";
    case 0x3D:  return "Advanced Logic Corp. TinyJ embedded processor family";
    case 0x3E:  return "Advanced Micro Devices X86-64";
    case 0x8C:  return "TMS320C6000 Family";
    case 0xAF:  return "MCST Elbrus e2k";
    case 0xB7:  return "ARM 64-bits (ARMv8/Aarch64)";
    case 0xF3:  return "RISC-V";
    case 0xF7:  return "Berkeley Packet Filter";
    case 0x101: return "WDC 65C816";
    default:    return "[Unknown]";
    }
}

static int _elf_dump_header(FILE* io, const elf_file_header_t* header)
{
    return fprintf(io,
        "Class:                             %s\n"
        "Data:                              %s\n"
        "Version:                           %d\n"
        "OS/ABI:                            %s\n"
        "ABI Version:                       %d\n"
        "Type:                              %s\n"
        "Machine:                           %s\n"
        "Version:                           0x%" PRIx32 "\n"
        "Entry point address:               0x%" PRIx64 "\n"
        "Start of program headers:          %" PRIu64 "\n"
        "Start of section headers:          %" PRIu64 "\n"
        "Flags:                             0x%" PRIx32 "\n"
        "Size of this header:               %u (bytes)\n"
        "Size of program headers:           %u (bytes)\n"
        "Number of program headers:         %u\n"
        "Size of section headers:           %u (bytes)\n"
        "Number of section headers:         %u\n"
        "Section header string table index: %u\n",
        _elf_dump_header_get_class(header->f_EI_CLASS),
        _elf_dump_header_get_data(header->f_EI_DATA),
        (int)header->f_EI_VERSION,
        _elf_dump_header_get_osabi(header->f_EI_OSABI),
        header->f_EI_ABIVERSION,
        _elf_dump_header_get_type(header->e_type),
        _elf_dump_header_get_machine(header->e_machine),
        header->e_version,
        header->e_entry,
        header->e_phoff,
        header->e_shoff,
        header->e_flags,
        (unsigned)header->e_ehsize,
        (unsigned)header->e_phentsize,
        (unsigned)header->e_phnum,
        (unsigned)header->e_shentsize,
        (unsigned)header->e_shnum,
        (unsigned)header->e_shstrndx);
}

static const char* _elf_dump_program_header_get_type(uint32_t p_type)
{
    switch (p_type)
    {
    case 0x00000000:    return "NULL";
    case 0x00000001:    return "LOAD";
    case 0x00000002:    return "DYNAMIC";
    case 0x00000003:    return "INTERP";
    case 0x00000004:    return "NOTE";
    case 0x00000005:    return "SHLIB";
    case 0x00000006:    return "PHDR";
    case 0x00000007:    return "TLS";
    case 0x60000000:    return "LOOS";
    case 0x6474e550:    return "GNU_EH_FRAME";
    case 0x6474e551:    return "GNU_STACK";
    case 0x6474e552:    return "GNU_RELRO";
    case 0x6FFFFFFF:    return "HIOS";
    case 0x70000000:    return "LOPROC";
    case 0x7FFFFFFF:    return "HIPROC";
    default:            return "[Unknown]";
    }
}

static int _elf_dump_program_header(FILE* io, const elf_program_header_t* program_hdr, int is_64bit)
{
    const int ptr_width = is_64bit ? 16 : 8;

    return fprintf(io,
        "%-*s 0x%0*" PRIx64 " 0x%0*" PRIx64 " 0x%0*" PRIx64 " 0x%0*" PRIx64 " 0x%0*" PRIx64 " 0x%0*" PRIx32 " %" PRIu64 "\n",
        12, _elf_dump_program_header_get_type(program_hdr->p_type),
        ptr_width, program_hdr->p_offset,
        ptr_width, program_hdr->p_vaddr,
        ptr_width, program_hdr->p_paddr,
        ptr_width, program_hdr->p_filesz,
        ptr_width, program_hdr->p_memsz,
        8, program_hdr->p_flags,
        program_hdr->p_align);
}

int elf_parser_file_header(elf_file_header_t* dst, const void* addr)
{
    const uint8_t* pdat = addr;
    size_t pos = 0;

    const uint8_t magic_header[4] = { 0x7f, 0x45, 0x4c, 0x46 };
    if (memcmp(addr, magic_header, sizeof(magic_header)) != 0)
    {
        return -1;
    }

    /* EI_MAG */
    memcpy(dst->f_EI_MAG, magic_header, sizeof(magic_header));
    pos += 4;

    dst->f_EI_CLASS = pdat[pos++];
    if (dst->f_EI_CLASS != 1 && dst->f_EI_CLASS != 2)
    {
        return -1;
    }

    dst->f_EI_DATA = pdat[pos++];
    if (dst->f_EI_DATA != 1 && dst->f_EI_DATA != 2)
    {
        return -1;
    }

    dst->f_EI_VERSION = pdat[pos++];
    dst->f_EI_OSABI = pdat[pos++];
    dst->f_EI_ABIVERSION = pdat[pos++];
    memcpy(dst->f_EI_PAD, &pdat[pos], sizeof(dst->f_EI_PAD));
    pos += 7;

    dst->e_type = _elf_parser_16bit(&pdat[pos], dst->f_EI_DATA);
    pos += 2;

    dst->e_machine = _elf_parser_16bit(&pdat[pos], dst->f_EI_DATA);
    pos += 2;

    dst->e_version = _elf_parser_32bit(&pdat[pos], dst->f_EI_DATA);
    pos += 4;

    if (dst->f_EI_CLASS == 1)
    {
        dst->e_entry = _elf_parser_32bit(&pdat[pos], dst->f_EI_DATA);
        pos += 4;
        dst->e_phoff = _elf_parser_32bit(&pdat[pos], dst->f_EI_DATA);
        pos += 4;
        dst->e_shoff = _elf_parser_32bit(&pdat[pos], dst->f_EI_DATA);
        pos += 4;
    }
    else
    {
        dst->e_entry = _elf_parser_64bit(&pdat[pos], dst->f_EI_DATA);
        pos += 8;
        dst->e_phoff = _elf_parser_64bit(&pdat[pos], dst->f_EI_DATA);
        pos += 8;
        dst->e_shoff = _elf_parser_64bit(&pdat[pos], dst->f_EI_DATA);
        pos += 8;
    }

    dst->e_flags = _elf_parser_32bit(&pdat[pos], dst->f_EI_DATA);
    pos += 4;

    dst->e_ehsize = _elf_parser_16bit(&pdat[pos], dst->f_EI_DATA);
    pos += 2;

    dst->e_phentsize = _elf_parser_16bit(&pdat[pos], dst->f_EI_DATA);
    pos += 2;

    dst->e_phnum = _elf_parser_16bit(&pdat[pos], dst->f_EI_DATA);
    pos += 2;

    dst->e_shentsize = _elf_parser_16bit(&pdat[pos], dst->f_EI_DATA);
    pos += 2;

    dst->e_shnum = _elf_parser_16bit(&pdat[pos], dst->f_EI_DATA);
    pos += 2;

    dst->e_shstrndx = _elf_parser_16bit(&pdat[pos], dst->f_EI_DATA);
    pos += 2;

    return 0;
}

int elf_parser_program_header(elf_program_header_t* dst,
    const elf_file_header_t* header, const void* addr, size_t idx)
{
    if (header->f_EI_CLASS == 1)
    {
        return _elf_parser_program32_header(dst, header, addr, idx);
    }
    else if (header->f_EI_CLASS == 2)
    {
        return _elf_parser_program64_header(dst, header, addr, idx);
    }

    return -1;
}

int elf_dump(FILE* io, const void* addr)
{
    int ret;
    int size_written = 0;

    elf_file_header_t file_hdr;
    if ((ret = elf_parser_file_header(&file_hdr, addr)) < 0)
    {
        return ret;
    }
    if ((ret = _elf_dump_header(io, &file_hdr)) < 0)
    {
        return ret;
    }
    size_written += ret;

    const int str_width = file_hdr.f_EI_CLASS == 2 ? 18 : 10;

    elf_program_header_t program_hdr;
    size_t idx;
    if ((ret = fprintf(io, "%-*s %-*s %-*s %-*s %-*s %-*s %-*s %s\n",
        12, "Type",
        str_width, "Offset",
        str_width, "VirtAddr",
        str_width, "PhysAddr",
        str_width, "FileSiz",
        str_width, "MemSiz",
        10, "Flags",
        "Align")) < 0)
    {
        return ret;
    }
    size_written += ret;

    for (idx = 0; idx < file_hdr.e_phnum; idx++)
    {
        if ((ret = elf_parser_program_header(&program_hdr, &file_hdr, addr, idx)) < 0)
        {
            return ret;
        }

        if ((ret = _elf_dump_program_header(io, &program_hdr, file_hdr.f_EI_CLASS == 2)) < 0)
        {
            return ret;
        }

        size_written += ret;
    }

    return size_written;
}
