use std::cell::RefCell;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::path::Path;
use std::sync::Mutex;
use windows::core::PCSTR;
use windows::Win32::Foundation::HMODULE;
use windows::Win32::System::LibraryLoader::{GetProcAddress, LoadLibraryA, SetDllDirectoryA};

#[repr(C)]
pub struct LibreOfficeKitStruct {
    pub pClass: *mut LibreOfficeKitClassStruct,
}

#[repr(C)]
pub struct LibreOfficeKitClassStruct {
    pub nSize: usize,
    pub destroy: unsafe extern "C" fn(pThis: *mut LibreOfficeKitStruct),
    pub documentLoad: unsafe extern "C" fn(
        pThis: *mut LibreOfficeKitStruct,
        pURL: *const c_char,
    ) -> *mut LibreOfficeKitDocumentStruct,
    pub getError: unsafe extern "C" fn(pThis: *mut LibreOfficeKitStruct) -> *mut c_char,
    pub documentLoadWithOptions: unsafe extern "C" fn(
        pThis: *mut LibreOfficeKitStruct,
        pURL: *const c_char,
        pOptions: *const c_char,
    ) -> *mut LibreOfficeKitDocumentStruct,
    pub freeError: unsafe extern "C" fn(pFree: *mut c_char),
}

#[repr(C)]
pub struct LibreOfficeKitDocumentStruct {
    pub pClass: *mut LibreOfficeKitDocumentClassStruct,
}

#[repr(C)]
pub struct LibreOfficeKitDocumentClassStruct {
    pub nSize: usize,
    pub destroy: unsafe extern "C" fn(pThis: *mut LibreOfficeKitDocumentStruct),
    pub saveAs: unsafe extern "C" fn(
        pThis: *mut LibreOfficeKitDocumentStruct,
        pUrl: *const c_char,
        pFormat: *const c_char,
        pFilterOptions: *const c_char,
    ) -> i32,
    pub getDocumentType: unsafe extern "C" fn(pThis: *mut LibreOfficeKitDocumentStruct) -> i32,
    pub getParts: unsafe extern "C" fn(pThis: *mut LibreOfficeKitDocumentStruct) -> i32,
    pub getPartPageRectangles:
        unsafe extern "C" fn(pThis: *mut LibreOfficeKitDocumentStruct) -> *mut c_char,
    pub getPart: unsafe extern "C" fn(pThis: *mut LibreOfficeKitDocumentStruct) -> i32,
    pub setPart: unsafe extern "C" fn(pThis: *mut LibreOfficeKitDocumentStruct, nPart: i32),
    pub getPartName: unsafe extern "C" fn(
        pThis: *mut LibreOfficeKitDocumentStruct,
        nPart: i32,
    ) -> *mut c_char,
    pub setPartMode: unsafe extern "C" fn(pThis: *mut LibreOfficeKitDocumentStruct, nMode: i32),
    pub paintTile: unsafe extern "C" fn(
        pThis: *mut LibreOfficeKitDocumentStruct,
        pBuffer: *mut u8,
        nCanvasWidth: i32,
        nCanvasHeight: i32,
        nTilePosX: i32,
        nTilePosY: i32,
        nTileWidth: i32,
        nTileHeight: i32,
    ),
    pub getTileMode: unsafe extern "C" fn(pThis: *mut LibreOfficeKitDocumentStruct) -> i32,
    pub getDocumentSize: unsafe extern "C" fn(
        pThis: *mut LibreOfficeKitDocumentStruct,
        pWidth: *mut i32,
        pHeight: *mut i32,
    ),
    pub initializeForRendering: unsafe extern "C" fn(
        pThis: *mut LibreOfficeKitDocumentStruct,
        pArguments: *const c_char,
    ),
}

type LokHookFn2 = unsafe extern "C" fn(
    pInstallPath: *const c_char,
    pUserProfileUrl: *const c_char,
) -> *mut LibreOfficeKitStruct;

type LokHookFn = unsafe extern "C" fn(pInstallPath: *const c_char) -> *mut LibreOfficeKitStruct;

struct GlobalLok {
    office: *mut LibreOfficeKitStruct,
    _module: HMODULE,
}

unsafe impl Send for GlobalLok {}
unsafe impl Sync for GlobalLok {}

static G_LOK: Mutex<Option<GlobalLok>> = Mutex::new(None);

thread_local! {
    static LAST_ERROR: RefCell<Option<CString>> = const { RefCell::new(None) };
}

fn set_last_error(msg: String) {
    LAST_ERROR.with(|cell| {
        *cell.borrow_mut() = CString::new(msg).ok();
    });
}

fn to_file_url(path: &str) -> String {
    if path.starts_with("file://") {
        path.to_string()
    } else {
        let abs = Path::new(path)
            .canonicalize()
            .unwrap_or_else(|_| Path::new(path).to_path_buf());
        let s = abs.to_string_lossy().replace('\\', "/");
        let s = s.trim_start_matches("//?/");
        format!("file:///{}", s)
    }
}

/// Initialize LibreOfficeKit engine.
#[no_mangle]
pub unsafe extern "C" fn lok_viewer_init(install_path: *const c_char) -> i32 {
    let mut guard = G_LOK.lock().unwrap();
    if guard.is_some() {
        return 0; // Already initialized
    }

    let path_str = if install_path.is_null() {
        "C:\\Program Files\\LibreOffice\\program".to_string()
    } else {
        CStr::from_ptr(install_path)
            .to_str()
            .unwrap_or("C:\\Program Files\\LibreOffice\\program")
            .to_string()
    };

    let path_c = match CString::new(path_str.clone()) {
        Ok(c) => c,
        Err(e) => {
            set_last_error(format!("Invalid install path: {e}"));
            return -1;
        }
    };

    // Set DLL directory so mergedlo.dll dependencies resolve
    let _ = SetDllDirectoryA(PCSTR(path_c.as_ptr() as *const u8));

    let mergedlo_path = format!("{}\\mergedlo.dll", path_str);
    let mergedlo_c = match CString::new(mergedlo_path.clone()) {
        Ok(c) => c,
        Err(e) => {
            set_last_error(format!("Invalid DLL path: {e}"));
            return -2;
        }
    };

    let hmodule = match LoadLibraryA(PCSTR(mergedlo_c.as_ptr() as *const u8)) {
        Ok(h) => h,
        Err(e) => {
            set_last_error(format!("Failed to load mergedlo.dll at {mergedlo_path}: {e}"));
            return -3;
        }
    };

    let hook_symbol = CString::new("libreofficekit_hook_2").unwrap();
    let proc_addr = GetProcAddress(hmodule, PCSTR(hook_symbol.as_ptr() as *const u8));

    let office_ptr = if let Some(proc) = proc_addr {
        let hook: LokHookFn2 = std::mem::transmute(proc);
        hook(path_c.as_ptr(), std::ptr::null())
    } else {
        let hook_symbol1 = CString::new("libreofficekit_hook").unwrap();
        let proc_addr1 = GetProcAddress(hmodule, PCSTR(hook_symbol1.as_ptr() as *const u8));
        if let Some(proc1) = proc_addr1 {
            let hook1: LokHookFn = std::mem::transmute(proc1);
            hook1(path_c.as_ptr())
        } else {
            set_last_error("Neither libreofficekit_hook_2 nor libreofficekit_hook found in mergedlo.dll".to_string());
            return -4;
        }
    };

    if office_ptr.is_null() {
        set_last_error("lok_hook returned NULL LibreOfficeKit instance".to_string());
        return -5;
    }

    *guard = Some(GlobalLok {
        office: office_ptr,
        _module: hmodule,
    });

    0
}

/// Shut down LibreOfficeKit engine.
#[no_mangle]
pub unsafe extern "C" fn lok_viewer_destroy() {
    let mut guard = G_LOK.lock().unwrap();
    if let Some(lok) = guard.take() {
        if !lok.office.is_null() {
            let class_ptr = (*lok.office).pClass;
            if !class_ptr.is_null() {
                ((*class_ptr).destroy)(lok.office);
            }
        }
    }
}

/// Open a document (DOCX, XLSX, PPTX, ODT, etc.).
#[no_mangle]
pub unsafe extern "C" fn lok_doc_open(file_path: *const c_char) -> *mut LibreOfficeKitDocumentStruct {
    if file_path.is_null() {
        set_last_error("Null file path provided".to_string());
        return std::ptr::null_mut();
    }

    let path_str = match CStr::from_ptr(file_path).to_str() {
        Ok(s) => s,
        Err(e) => {
            set_last_error(format!("Invalid UTF-8 in file path: {e}"));
            return std::ptr::null_mut();
        }
    };

    let file_url = to_file_url(path_str);
    let url_c = match CString::new(file_url) {
        Ok(c) => c,
        Err(e) => {
            set_last_error(format!("URL conversion error: {e}"));
            return std::ptr::null_mut();
        }
    };

    let guard = G_LOK.lock().unwrap();
    let lok = match guard.as_ref() {
        Some(l) => l,
        None => {
            set_last_error("LibreOfficeKit not initialized".to_string());
            return std::ptr::null_mut();
        }
    };

    let class_ptr = (*lok.office).pClass;
    let doc_ptr = ((*class_ptr).documentLoad)(lok.office, url_c.as_ptr());
    if doc_ptr.is_null() {
        let err_ptr = ((*class_ptr).getError)(lok.office);
        if !err_ptr.is_null() {
            let err = CStr::from_ptr(err_ptr).to_string_lossy().into_owned();
            ((*class_ptr).freeError)(err_ptr);
            set_last_error(format!("documentLoad failed: {err}"));
        } else {
            set_last_error("documentLoad failed with unknown error".to_string());
        }
        return std::ptr::null_mut();
    }

    // Initialize for rendering with empty args
    let doc_class = (*doc_ptr).pClass;
    let empty_args = CString::new("").unwrap();
    ((*doc_class).initializeForRendering)(doc_ptr, empty_args.as_ptr());

    doc_ptr
}

/// Close document.
#[no_mangle]
pub unsafe extern "C" fn lok_doc_close(doc: *mut LibreOfficeKitDocumentStruct) {
    if !doc.is_null() {
        let class_ptr = (*doc).pClass;
        if !class_ptr.is_null() {
            ((*class_ptr).destroy)(doc);
        }
    }
}

/// Get document parts count (pages, slides, or sheets).
#[no_mangle]
pub unsafe extern "C" fn lok_doc_get_parts(doc: *mut LibreOfficeKitDocumentStruct) -> i32 {
    if doc.is_null() {
        return 0;
    }
    let class_ptr = (*doc).pClass;
    ((*class_ptr).getParts)(doc)
}

/// Set active part.
#[no_mangle]
pub unsafe extern "C" fn lok_doc_set_part(doc: *mut LibreOfficeKitDocumentStruct, part: i32) {
    if !doc.is_null() {
        let class_ptr = (*doc).pClass;
        ((*class_ptr).setPart)(doc, part);
    }
}

/// Get document size in TWIPs.
#[no_mangle]
pub unsafe extern "C" fn lok_doc_get_size_twips(
    doc: *mut LibreOfficeKitDocumentStruct,
    out_width: *mut i32,
    out_height: *mut i32,
) -> i32 {
    if doc.is_null() || out_width.is_null() || out_height.is_null() {
        return -1;
    }
    let class_ptr = (*doc).pClass;
    ((*class_ptr).getDocumentSize)(doc, out_width, out_height);
    0
}

/// Render a tile/page into pre-allocated 32-bit BGRA buffer.
#[no_mangle]
pub unsafe extern "C" fn lok_doc_paint_tile(
    doc: *mut LibreOfficeKitDocumentStruct,
    buffer: *mut u8,
    canvas_width: i32,
    canvas_height: i32,
    tile_pos_x: i32,
    tile_pos_y: i32,
    tile_width: i32,
    tile_height: i32,
) -> i32 {
    if doc.is_null() || buffer.is_null() {
        set_last_error("Invalid argument to paintTile".to_string());
        return -1;
    }

    let class_ptr = (*doc).pClass;
    ((*class_ptr).paintTile)(
        doc,
        buffer,
        canvas_width,
        canvas_height,
        tile_pos_x,
        tile_pos_y,
        tile_width,
        tile_height,
    );
    0
}

/// Save document directly to a file (e.g. PDF, HTML, etc.).
#[no_mangle]
pub unsafe extern "C" fn lok_doc_save_as(
    doc: *mut LibreOfficeKitDocumentStruct,
    out_path: *const c_char,
    format: *const c_char,
    filter_options: *const c_char,
) -> i32 {
    if doc.is_null() || out_path.is_null() {
        set_last_error("Invalid arguments to saveAs".to_string());
        return -1;
    }

    let out_str = match CStr::from_ptr(out_path).to_str() {
        Ok(s) => s,
        Err(e) => {
            set_last_error(format!("Invalid UTF-8 in out_path: {e}"));
            return -2;
        }
    };

    let file_url = to_file_url(out_str);
    let url_c = match CString::new(file_url) {
        Ok(c) => c,
        Err(e) => {
            set_last_error(format!("URL error: {e}"));
            return -3;
        }
    };

    let class_ptr = (*doc).pClass;
    let res = ((*class_ptr).saveAs)(doc, url_c.as_ptr(), format, filter_options);
    if res == 0 {
        set_last_error("saveAs failed (returned 0 / false)".to_string());
        return -4;
    }

    0
}

/// Get the last error string on this thread.
#[no_mangle]
pub extern "C" fn lok_get_last_error() -> *const c_char {
    LAST_ERROR.with(|cell| match cell.borrow().as_ref() {
        Some(c) => c.as_ptr(),
        None => std::ptr::null(),
    })
}
