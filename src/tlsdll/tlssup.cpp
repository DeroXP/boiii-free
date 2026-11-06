// Thread-local storage support for tlsdll.dll (docs/DESIGN.md 16.5).
//
// Clean-room implementation, written for Xerces on 2026-11-05 without reading
// the code of the file it replaces, from the documented PE/COFF TLS ABI (the
// TLS directory, IMAGE_TLS_DIRECTORY64, _tls_index, _tls_used and the .tls and
// .CRT$XL* sections). The replaced file came from the Microsoft C runtime
// sources, which this program cannot distribute; of it only the comments,
// pragmas and section names were seen. Provenance: docs/DESIGN.md 16.5. Part of
// Xerces, licensed under the GNU General Public License version 3 (see
// LICENSE).
//
// The compiler and linker expect the symbols below from a C runtime. Defining
// them here means the DLL does not take them from Microsoft's runtime library:
//
// - _tls_index: the 32-bit variable in which the Windows loader stores the TLS
//   index it gives this DLL. Code that reads a thread_local variable reads it.
// - _tls_start and _tls_end: one-byte markers in .tls and .tls$ZZZ. The linker
//   joins the .tls groups in name order, so the thread_local variables (the
//   compiler puts them in .tls$) lie between the two. The bytes from _tls_start
//   up to, not including, _tls_end are the template the loader copies into
//   each thread's block.
// - __xl_a and __xl_z: null entries in .CRT$XLA and .CRT$XLZ around the TLS
//   callback array. Callbacks that other objects put in .CRT$XLB to .CRT$XLY
//   land between them. The directory points at the entry after __xl_a, and the
//   loader calls entries until it reaches a null one. This DLL adds none.
// - _tls_used: the TLS directory itself, in .rdata$T. The linker points the
//   image's TLS data directory at the symbol with this name, and lld-link
//   writes the alignment of the .tls section into its Characteristics.
//
// src/client/loader/loader.cpp (load_tls) gives the index and the thread
// blocks this produces to the game executable.

#define WIN32_LEAN_AND_MEAN
#include <windows.h> // IWYU pragma: keep

#include <cstddef>

// IMAGE_TLS_DIRECTORY with pointer fields, so that the directory can be built
// at compile time (constinit) from the addresses below. The asserts pin it to
// the layout the loader reads.
struct tls_directory {
  const void *start_of_raw_data;
  const void *end_of_raw_data;
  ULONG *address_of_index;
  const PIMAGE_TLS_CALLBACK *address_of_callbacks;
  ULONG size_of_zero_fill;
  ULONG characteristics;
};

static_assert(sizeof(tls_directory) == sizeof(IMAGE_TLS_DIRECTORY));
static_assert(offsetof(tls_directory, start_of_raw_data) ==
              offsetof(IMAGE_TLS_DIRECTORY, StartAddressOfRawData));
static_assert(offsetof(tls_directory, end_of_raw_data) ==
              offsetof(IMAGE_TLS_DIRECTORY, EndAddressOfRawData));
static_assert(offsetof(tls_directory, address_of_index) ==
              offsetof(IMAGE_TLS_DIRECTORY, AddressOfIndex));
static_assert(offsetof(tls_directory, address_of_callbacks) ==
              offsetof(IMAGE_TLS_DIRECTORY, AddressOfCallBacks));
static_assert(offsetof(tls_directory, size_of_zero_fill) ==
              offsetof(IMAGE_TLS_DIRECTORY, SizeOfZeroFill));
static_assert(offsetof(tls_directory, characteristics) ==
              offsetof(IMAGE_TLS_DIRECTORY, Characteristics));

extern "C" {
extern ULONG _tls_index;
extern char _tls_start;
extern char _tls_end;
extern const PIMAGE_TLS_CALLBACK __xl_a;
extern const PIMAGE_TLS_CALLBACK __xl_z;
extern const tls_directory _tls_used;
}

#pragma section(".tls", read, write)
#pragma section(".tls$ZZZ", read, write)
#pragma section(".CRT$XLA", read)
#pragma section(".CRT$XLZ", read)
#pragma section(".rdata$T", read)

ULONG _tls_index = 0;

__declspec(allocate(".tls")) char _tls_start = 0;
__declspec(allocate(".tls$ZZZ")) char _tls_end = 0;

__declspec(allocate(".CRT$XLA")) const PIMAGE_TLS_CALLBACK __xl_a = nullptr;
__declspec(allocate(".CRT$XLZ")) const PIMAGE_TLS_CALLBACK __xl_z = nullptr;

__declspec(allocate(".rdata$T")) constinit const tls_directory _tls_used = {
    &_tls_start,
    &_tls_end,
    &_tls_index,
    // The first callback, or __xl_z when there is none.
    &__xl_a + 1,
    // No zero fill: the template covers the whole block.
    0,
    // Alignment bits; the linker sets them.
    0,
};
