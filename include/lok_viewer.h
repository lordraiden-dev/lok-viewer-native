#ifndef LOK_VIEWER_API_H
#define LOK_VIEWER_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
  #ifdef LOK_VIEWER_EXPORTS
    #define LOK_API __declspec(dllexport)
  #else
    #define LOK_API __declspec(dllimport)
  #endif
#else
  #define LOK_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// Document handle opaque type
typedef void* LokDocHandle;

/// Result codes of the int functions below. Callers may treat any non-zero value as failure.
enum LokResult {
    LOK_OK                 = 0,
    LOK_E_INVALID_ARG      = -1, /* null/empty argument, invalid UTF-8, unknown or closed handle */
    LOK_E_NOT_INITIALIZED  = -2, /* lok_viewer_init() was not called, or lok_viewer_destroy() was */
    LOK_E_INIT_FAILED      = -3, /* LibreOffice could not be loaded or started */
    LOK_E_OPERATION_FAILED = -4, /* LibreOffice reported a failure (load, save, ...) */
    LOK_E_INTERNAL         = -5  /* out of memory or unexpected internal error */
};

/**
 * Initialize LibreOfficeKit engine.
 * @param install_path Path to LibreOffice program folder (e.g. "C:\\Program Files\\LibreOffice\\program"), UTF-8.
 *                     If NULL or empty, "C:\\Program Files\\LibreOffice\\program" is used.
 * @return 0 on success (also when already initialized), negative LokResult on failure.
 *
 * LibreOfficeKit can be started only once per process: after lok_viewer_destroy(), or after
 * LibreOffice failed to start, this function fails until the process is restarted.
 * The program folder becomes the process DLL directory (SetDllDirectoryW), because
 * LibreOffice keeps loading DLLs from it at run time.
 * LibreOffice runs with its own user profile, not the desktop one (whose saved UI state can
 * deadlock document loading): %LOK_VIEWER_PROFILE% (folder or file: URL) if set,
 * otherwise %LOCALAPPDATA%\LokViewer\lo-profile. The folder must be writable.
 */
LOK_API int lok_viewer_init(const char* install_path);

/**
 * Shut down LibreOfficeKit engine and free resources.
 * Documents that are still open are closed; their handles become invalid.
 */
LOK_API void lok_viewer_destroy(void);

/**
 * Open a document (DOCX, XLSX, PPTX, ODT, ODS, etc.).
 * @param file_path Path (UTF-8, absolute or relative to the current directory) or "file:" URL.
 * @return Document handle on success, NULL on failure.
 */
LOK_API LokDocHandle lok_doc_open(const char* file_path);

/**
 * Close a previously opened document. A NULL handle is ignored; an unknown or already
 * closed handle is ignored and records an error message.
 */
LOK_API void lok_doc_close(LokDocHandle doc);

/**
 * Get document total parts (e.g., page count for Writer/PDF, slide count for Impress, sheet count for Calc).
 * @return Part count, or 0 on error.
 */
LOK_API int lok_doc_get_parts(LokDocHandle doc);

/**
 * Set active part (page / slide / sheet).
 */
LOK_API void lok_doc_set_part(LokDocHandle doc, int part);

/**
 * Get document size in TWIPs (1/1440th of an inch).
 * @return 0 on success, negative LokResult on failure.
 */
LOK_API int lok_doc_get_size_twips(LokDocHandle doc, int32_t* out_width, int32_t* out_height);

/**
 * Render a tile / page directly into a 32-bit BGRA pixel buffer.
 *
 * @param doc Document handle
 * @param buffer Allocated buffer with size at least (canvas_width * canvas_height * 4) bytes
 * @param canvas_width Output pixel width (> 0)
 * @param canvas_height Output pixel height (> 0)
 * @param tile_pos_x X offset in document twips
 * @param tile_pos_y Y offset in document twips
 * @param tile_width Width in document twips (> 0)
 * @param tile_height Height in document twips (> 0)
 * @return 0 on success, negative LokResult on error
 */
LOK_API int lok_doc_paint_tile(
    LokDocHandle doc,
    unsigned char* buffer,
    int canvas_width,
    int canvas_height,
    int tile_pos_x,
    int tile_pos_y,
    int tile_width,
    int tile_height
);

/**
 * Export document directly to a file format (e.g. "pdf", "html", etc.).
 * @param out_path Output path (UTF-8) or "file:" URL.
 * @param format Target format; NULL lets LibreOffice pick it from the file extension.
 * @param filter_options Optional filter options; may be NULL.
 * @return 0 on success, negative LokResult on failure.
 */
LOK_API int lok_doc_save_as(LokDocHandle doc, const char* out_path, const char* format, const char* filter_options);

/**
 * Get the last error message (UTF-8) recorded on the calling thread, or NULL if there is none.
 * The pointer stays valid until the next failing call on the same thread.
 */
LOK_API const char* lok_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif // LOK_VIEWER_API_H
