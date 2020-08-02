
#if !defined(DOWNLOAD_H) && defined(ADEPT_ENABLE_PACKAGE_MANAGER)
#define DOWNLOAD_H

#ifdef __cplusplus
extern "C" {
#endif

/*
    ================================ filename.h ===============================
    Module for manipulating filenames
    ---------------------------------------------------------------------------
*/

#include "UTIL/ground.h"

typedef struct {
    void *bytes;
    length_t length;

    #ifdef TRACK_MEMORY_USAGE
    length_t capacity;
    #endif
} download_buffer_t;

// ---------------- download ----------------
// Downloads a file, returns whether successful
successful_t download(weak_cstr_t url, weak_cstr_t destination);

// ---------------- filename_without_ext ----------------
// Downloads a file into memory, returns whether successful
// NOTE: If successful, out_memory->buffer must be freed by the caller
successful_t download_to_memory(weak_cstr_t url, download_buffer_t *out_memory);

#ifdef __cplusplus
}
#endif

#endif // DOWNLOAD_H
