/*===- InstrProfilingPlatformLinux.c - Profile data Linux platform ------===*\
|*
|* Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
|* See https://llvm.org/LICENSE.txt for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
\*===----------------------------------------------------------------------===*/

#if defined(__linux__) || defined(__FreeBSD__) || defined(__Fuchsia__) || \
    (defined(__sun__) && defined(__svr4__)) || defined(__NetBSD__) || \
    defined(_AIX)

#if !defined(_AIX)
#include <elf.h>
#include <link.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "InstrProfiling.h"
#include "InstrProfilingInternal.h"

#if defined(__FreeBSD__) && !defined(ElfW)
/*
 * FreeBSD's elf.h and link.h headers do not define the ElfW(type) macro yet.
 * If this is added to all supported FreeBSD versions in the future, this
 * compatibility macro can be removed.
 */
#define ElfW(type) __ElfN(type)
#endif

#define PROF_DATA_START INSTR_PROF_SECT_START(INSTR_PROF_DATA_COMMON)
#define PROF_DATA_STOP INSTR_PROF_SECT_STOP(INSTR_PROF_DATA_COMMON)
#define PROF_NAME_START INSTR_PROF_SECT_START(INSTR_PROF_NAME_COMMON)
#define PROF_NAME_STOP INSTR_PROF_SECT_STOP(INSTR_PROF_NAME_COMMON)
#define PROF_CNTS_START INSTR_PROF_SECT_START(INSTR_PROF_CNTS_COMMON)
#define PROF_CNTS_STOP INSTR_PROF_SECT_STOP(INSTR_PROF_CNTS_COMMON)
#define PROF_ORDERFILE_START INSTR_PROF_SECT_START(INSTR_PROF_ORDERFILE_COMMON)
#define PROF_VNODES_START INSTR_PROF_SECT_START(INSTR_PROF_VNODES_COMMON)
#define PROF_VNODES_STOP INSTR_PROF_SECT_STOP(INSTR_PROF_VNODES_COMMON)

/* Declare section start and stop symbols for various sections
 * generated by compiler instrumentation.
 */
extern __llvm_profile_data PROF_DATA_START COMPILER_RT_VISIBILITY
    COMPILER_RT_WEAK;
extern __llvm_profile_data PROF_DATA_STOP COMPILER_RT_VISIBILITY
    COMPILER_RT_WEAK;
extern char PROF_CNTS_START COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern char PROF_CNTS_STOP COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern uint32_t PROF_ORDERFILE_START COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern char PROF_NAME_START COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern char PROF_NAME_STOP COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern ValueProfNode PROF_VNODES_START COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;
extern ValueProfNode PROF_VNODES_STOP COMPILER_RT_VISIBILITY COMPILER_RT_WEAK;

COMPILER_RT_VISIBILITY const __llvm_profile_data *
__llvm_profile_begin_data(void) {
  return &PROF_DATA_START;
}
COMPILER_RT_VISIBILITY const __llvm_profile_data *
__llvm_profile_end_data(void) {
  return &PROF_DATA_STOP;
}
COMPILER_RT_VISIBILITY const char *__llvm_profile_begin_names(void) {
  return &PROF_NAME_START;
}
COMPILER_RT_VISIBILITY const char *__llvm_profile_end_names(void) {
  return &PROF_NAME_STOP;
}
COMPILER_RT_VISIBILITY char *__llvm_profile_begin_counters(void) {
  return &PROF_CNTS_START;
}
COMPILER_RT_VISIBILITY char *__llvm_profile_end_counters(void) {
  return &PROF_CNTS_STOP;
}
COMPILER_RT_VISIBILITY uint32_t *__llvm_profile_begin_orderfile(void) {
  return &PROF_ORDERFILE_START;
}

COMPILER_RT_VISIBILITY ValueProfNode *
__llvm_profile_begin_vnodes(void) {
  return &PROF_VNODES_START;
}
COMPILER_RT_VISIBILITY ValueProfNode *__llvm_profile_end_vnodes(void) {
  return &PROF_VNODES_STOP;
}
COMPILER_RT_VISIBILITY ValueProfNode *CurrentVNode = &PROF_VNODES_START;
COMPILER_RT_VISIBILITY ValueProfNode *EndVNode = &PROF_VNODES_STOP;

#ifdef NT_GNU_BUILD_ID
static size_t RoundUp(size_t size, size_t align) {
  return (size + align - 1) & ~(align - 1);
}

/*
 * Write binary id length and then its data, because binary id does not
 * have a fixed length.
 */
static int WriteOneBinaryId(ProfDataWriter *Writer, uint64_t BinaryIdLen,
                            const uint8_t *BinaryIdData,
                            uint64_t BinaryIdPadding) {
  ProfDataIOVec BinaryIdIOVec[] = {
      {&BinaryIdLen, sizeof(uint64_t), 1, 0},
      {BinaryIdData, sizeof(uint8_t), BinaryIdLen, 0},
      {NULL, sizeof(uint8_t), BinaryIdPadding, 1},
  };
  if (Writer->Write(Writer, BinaryIdIOVec,
                    sizeof(BinaryIdIOVec) / sizeof(*BinaryIdIOVec)))
    return -1;

  /* Successfully wrote binary id, report success. */
  return 0;
}

/*
 * Look for the note that has the name "GNU\0" and type NT_GNU_BUILD_ID
 * that contains build id. If build id exists, write binary id.
 *
 * Each note in notes section starts with a struct which includes
 * n_namesz, n_descsz, and n_type members. It is followed by the name
 * (whose length is defined in n_namesz) and then by the descriptor
 * (whose length is defined in n_descsz).
 *
 * Note sections like .note.ABI-tag and .note.gnu.build-id are aligned
 * to 4 bytes, so round n_namesz and n_descsz to the nearest 4 bytes.
 */
static int WriteBinaryIdForNote(ProfDataWriter *Writer,
                                const ElfW(Nhdr) * Note) {
  int BinaryIdSize = 0;
  const char *NoteName = (const char *)Note + sizeof(ElfW(Nhdr));
  if (Note->n_type == NT_GNU_BUILD_ID && Note->n_namesz == 4 &&
      memcmp(NoteName, "GNU\0", 4) == 0) {
    uint64_t BinaryIdLen = Note->n_descsz;
    const uint8_t *BinaryIdData =
        (const uint8_t *)(NoteName + RoundUp(Note->n_namesz, 4));
    uint8_t BinaryIdPadding = __llvm_profile_get_num_padding_bytes(BinaryIdLen);
    if (Writer != NULL && WriteOneBinaryId(Writer, BinaryIdLen, BinaryIdData,
                                           BinaryIdPadding) == -1)
      return -1;

    BinaryIdSize = sizeof(BinaryIdLen) + BinaryIdLen + BinaryIdPadding;
  }

  return BinaryIdSize;
}

/*
 * Helper function that iterates through notes section and find build ids.
 * If writer is given, write binary ids into profiles.
 * If an error happens while writing, return -1.
 */
static int WriteBinaryIds(ProfDataWriter *Writer, const ElfW(Nhdr) * Note,
                          const ElfW(Nhdr) * NotesEnd) {
  int BinaryIdsSize = 0;
  while (Note < NotesEnd) {
    int OneBinaryIdSize = WriteBinaryIdForNote(Writer, Note);
    if (OneBinaryIdSize == -1)
      return -1;
    BinaryIdsSize += OneBinaryIdSize;

    /* Calculate the offset of the next note in notes section. */
    size_t NoteOffset = sizeof(ElfW(Nhdr)) + RoundUp(Note->n_namesz, 4) +
                        RoundUp(Note->n_descsz, 4);
    Note = (const ElfW(Nhdr) *)((const char *)(Note) + NoteOffset);
  }

  return BinaryIdsSize;
}

/*
 * Write binary ids into profiles if writer is given.
 * Return the total size of binary ids.
 * If an error happens while writing, return -1.
 */
COMPILER_RT_VISIBILITY int __llvm_write_binary_ids(ProfDataWriter *Writer) {
  extern const ElfW(Ehdr) __ehdr_start __attribute__((visibility("hidden")));
  const ElfW(Ehdr) *ElfHeader = &__ehdr_start;
  const ElfW(Phdr) *ProgramHeader =
      (const ElfW(Phdr) *)((uintptr_t)ElfHeader + ElfHeader->e_phoff);

  int TotalBinaryIdsSize = 0;
  uint32_t I;
  /* Iterate through entries in the program header. */
  for (I = 0; I < ElfHeader->e_phnum; I++) {
    /* Look for the notes segment in program header entries. */
    if (ProgramHeader[I].p_type != PT_NOTE)
      continue;

    /* There can be multiple notes segment, and examine each of them. */
    const ElfW(Nhdr) * Note;
    const ElfW(Nhdr) * NotesEnd;
    /*
     * When examining notes in file, use p_offset, which is the offset within
     * the elf file, to find the start of notes.
     */
    if (ProgramHeader[I].p_memsz == 0 ||
        ProgramHeader[I].p_memsz == ProgramHeader[I].p_filesz) {
      Note = (const ElfW(Nhdr) *)((uintptr_t)ElfHeader +
                                  ProgramHeader[I].p_offset);
      NotesEnd = (const ElfW(Nhdr) *)((const char *)(Note) +
                                      ProgramHeader[I].p_filesz);
    } else {
      /*
       * When examining notes in memory, use p_vaddr, which is the address of
       * section after loaded to memory, to find the start of notes.
       */
      Note =
          (const ElfW(Nhdr) *)((uintptr_t)ElfHeader + ProgramHeader[I].p_vaddr);
      NotesEnd =
          (const ElfW(Nhdr) *)((const char *)(Note) + ProgramHeader[I].p_memsz);
    }

    int BinaryIdsSize = WriteBinaryIds(Writer, Note, NotesEnd);
    if (TotalBinaryIdsSize == -1)
      return -1;

    TotalBinaryIdsSize += BinaryIdsSize;
  }

  return TotalBinaryIdsSize;
}
#else /* !NT_GNU_BUILD_ID */
/*
 * Fallback implementation for targets that don't support the GNU
 * extensions NT_GNU_BUILD_ID and __ehdr_start.
 */
COMPILER_RT_VISIBILITY int __llvm_write_binary_ids(ProfDataWriter *Writer) {
  return 0;
}
#endif

#if defined(_AIX)
// Empty stubs to allow linking object files using the registration-based scheme
COMPILER_RT_VISIBILITY
void __llvm_profile_register_function(void *Data_) {}

COMPILER_RT_VISIBILITY
void __llvm_profile_register_names_function(void *NamesStart,
                                            uint64_t NamesSize) {}

// The __start_SECNAME and __stop_SECNAME symbols (for SECNAME \in
// {"__llvm_prf_cnts", "__llvm_prf_data", "__llvm_prf_name", "__llvm_prf_vnds"})
// are always live when linking on AIX, regardless if the .o's being linked
// reference symbols from the profile library (for example when no files were
// compiled with -fprofile-generate). That's because these symbols are kept
// alive through references in constructor functions that are always live in the
// default linking model on AIX (-bcdtors:all). The __start_SECNAME and
// __stop_SECNAME symbols are only resolved by the linker when the SECNAME
// section exists. So for the scenario where the user objects have no such
// section (i.e. when they are compiled with -fno-profile-generate), we always
// define these zero length variables in each of the above 4 sections.
COMPILER_RT_VISIBILITY int dummy_cnts[0] COMPILER_RT_SECTION(
    COMPILER_RT_SEG INSTR_PROF_CNTS_SECT_NAME);
COMPILER_RT_VISIBILITY int dummy_data[0] COMPILER_RT_SECTION(
    COMPILER_RT_SEG INSTR_PROF_DATA_SECT_NAME);
COMPILER_RT_VISIBILITY const int dummy_name[0] COMPILER_RT_SECTION(
    COMPILER_RT_SEG INSTR_PROF_NAME_SECT_NAME);
COMPILER_RT_VISIBILITY int dummy_vnds[0] COMPILER_RT_SECTION(
    COMPILER_RT_SEG INSTR_PROF_VNODES_SECT_NAME);

// Create a fake reference to avoid GC'ing of the dummy variables by the linker.
// Ideally, we create a ".ref" of each variable inside the function
// __llvm_profile_begin_counters(), but there's no source level construct
// that allows us to generate that.
__attribute__((destructor)) void keep() {
  int volatile use = &dummy_cnts < &dummy_data && &dummy_name < &dummy_vnds;
  (void)use;
}
#endif

#endif
