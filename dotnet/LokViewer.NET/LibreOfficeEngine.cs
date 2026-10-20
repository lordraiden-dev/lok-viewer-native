using System;
using System.IO;
using System.Runtime.InteropServices;

namespace LokViewer;

/// <summary>
/// Low-level P/Invoke bindings for lok_viewer.dll
/// </summary>
internal static class LokNative
{
    private const string LibName = "lok_viewer";

    static LokNative()
    {
        NativeLibrary.SetDllImportResolver(typeof(LokNative).Assembly, ResolveDll);
    }

    private static IntPtr ResolveDll(string libraryName, System.Reflection.Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (libraryName == LibName)
        {
            string baseDir = AppContext.BaseDirectory;
            string dllName = "lok_viewer.dll";

            // 1. AppContext.BaseDirectory
            string directPath = Path.Combine(baseDir, dllName);
            if (File.Exists(directPath) && NativeLibrary.TryLoad(directPath, out IntPtr h1))
                return h1;

            // 2. runtimes/win-x64/native/
            string ridPath = Path.Combine(baseDir, "runtimes", "win-x64", "native", dllName);
            if (File.Exists(ridPath) && NativeLibrary.TryLoad(ridPath, out IntPtr h2))
                return h2;

            // 3. Solution target/release
            string devPath = Path.GetFullPath(Path.Combine(baseDir, @"..\..\..\..\target\release\lok_viewer.dll"));
            if (File.Exists(devPath) && NativeLibrary.TryLoad(devPath, out IntPtr h3))
                return h3;

            // 4. Default fallback
            if (NativeLibrary.TryLoad(dllName, assembly, searchPath, out IntPtr h4))
                return h4;
        }

        return IntPtr.Zero;
    }

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int lok_viewer_init([MarshalAs(UnmanagedType.LPUTF8Str)] string? installPath);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void lok_viewer_destroy();

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr lok_doc_open([MarshalAs(UnmanagedType.LPUTF8Str)] string filePath);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void lok_doc_close(IntPtr doc);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int lok_doc_get_parts(IntPtr doc);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void lok_doc_set_part(IntPtr doc, int part);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int lok_doc_get_size_twips(IntPtr doc, out int width, out int height);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe int lok_doc_paint_tile(
        IntPtr doc,
        byte* buffer,
        int canvasWidth,
        int canvasHeight,
        int tilePosX,
        int tilePosY,
        int tileWidth,
        int tileHeight);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int lok_doc_save_as(
        IntPtr doc,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string outPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string format,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? filterOptions);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr lok_get_last_error();

    internal static string? GetLastError()
    {
        IntPtr ptr = lok_get_last_error();
        return ptr == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(ptr);
    }
}

/// <summary>
/// LibreOfficeKit motorunu yöneten ana sınıf.
/// </summary>
public static class LibreOfficeEngine
{
    private const string LibreOfficeDownloadUrl = "https://www.libreoffice.org/download/download-libreoffice/";
    private const int InitFailedCode = -3; // LOK_E_INIT_FAILED in lok_viewer.h

    private static readonly object _lock = new();
    private static bool _isInitialized = false;

    /// <summary>
    /// LibreOfficeKit çekirdeğini in-process olarak başlatır.
    /// Uygulama ömrü boyunca 1 kez çağrılması yeterlidir.
    /// </summary>
    /// <param name="programPath">LibreOffice program dizini (örn: @"C:\Program Files\LibreOffice\program"). Null verilirse varsayılan konumu arar.</param>
    public static void Initialize(string? programPath = null)
    {
        lock (_lock)
        {
            if (_isInitialized) return;

            string targetPath = programPath ?? @"C:\Program Files\LibreOffice\program";
            int res;
            try
            {
                res = LokNative.lok_viewer_init(targetPath);
            }
            catch (Exception ex) when (ex is DllNotFoundException or BadImageFormatException)
            {
                // BadImageFormatException: x64 native DLL loaded into a non-x64 process.
                throw new InvalidOperationException(
                    $"lok_viewer.dll yüklenemedi ({ex.GetType().Name}). " +
                    $"lok_viewer.dll uygulama klasöründe olmalı ({AppContext.BaseDirectory}) " +
                    "ve uygulama x64 çalışmalı (<PlatformTarget>x64</PlatformTarget>).", ex);
            }

            if (res != 0)
            {
                string err = LokNative.GetLastError() ?? $"Hata kodu: {res}";
                string hint = res == InitFailedCode
                    ? $" LibreOffice kurulumunu ve program klasörünü kontrol et. İndirme: {LibreOfficeDownloadUrl}"
                    : string.Empty;
                throw new InvalidOperationException($"LibreOfficeKit başlatılamadı: {err}{hint}");
            }

            _isInitialized = true;
        }
    }

    /// <summary>
    /// Belgeyi açar (DOCX, XLSX, PPTX, ODT, ODS, vb.).
    /// </summary>
    public static LokDocument OpenDocument(string filePath)
    {
        if (!_isInitialized)
        {
            Initialize();
        }

        ArgumentException.ThrowIfNullOrWhiteSpace(filePath);
        if (!File.Exists(filePath))
            throw new FileNotFoundException("Belirtilen dosya bulunamadı.", filePath);

        string absPath = Path.GetFullPath(filePath);
        IntPtr handle = LokNative.lok_doc_open(absPath);
        if (handle == IntPtr.Zero)
        {
            string err = LokNative.GetLastError() ?? "Bilinmeyen hata.";
            throw new InvalidOperationException($"Belge açılamadı: {err}");
        }

        return new LokDocument(handle, absPath);
    }

    /// <summary>
    /// Motoru kapatır ve kaynakları serbest bırakır.
    /// </summary>
    public static void Shutdown()
    {
        lock (_lock)
        {
            if (!_isInitialized) return;
            LokNative.lok_viewer_destroy();
            _isInitialized = false;
        }
    }
}

/// <summary>
/// Açık bir LibreOffice belgesini temsil eder. (IDisposable)
/// </summary>
public sealed class LokDocument : IDisposable
{
    private IntPtr _handle;
    public string FilePath { get; }

    internal LokDocument(IntPtr handle, string filePath)
    {
        _handle = handle;
        FilePath = filePath;
    }

    /// <summary>
    /// Toplam parça / sayfa sayısı (Writer/PDF için sayfa, Impress için slayt, Calc için sayfa).
    /// </summary>
    public int PartCount
    {
        get
        {
            EnsureNotDisposed();
            return LokNative.lok_doc_get_parts(_handle);
        }
    }

    /// <summary>
    /// Aktif parçayı / sayfayı değiştirir.
    /// </summary>
    public void SetPart(int partIndex)
    {
        EnsureNotDisposed();
        LokNative.lok_doc_set_part(_handle, partIndex);
    }

    /// <summary>
    /// Belgenin mantıksal boyutlarını (TWIP cinsinden: 1/1440 inç) döndürür.
    /// </summary>
    public (int WidthTwips, int HeightTwips) GetDocumentSizeTwips()
    {
        EnsureNotDisposed();
        int res = LokNative.lok_doc_get_size_twips(_handle, out int width, out int height);
        if (res != 0)
            throw new InvalidOperationException("Belge boyutları alınamadı.");
        return (width, height);
    }

    /// <summary>
    /// Belirtilen alanı veya tüm sayfayı doğrudan 32-bit BGRA piksel tamponuna render eder (Tiled rendering).
    /// </summary>
    public unsafe void PaintTile(
        byte[] buffer,
        int canvasWidth,
        int canvasHeight,
        int tilePosXTwips,
        int tilePosYTwips,
        int tileWidthTwips,
        int tileHeightTwips)
    {
        EnsureNotDisposed();
        ArgumentNullException.ThrowIfNull(buffer);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(canvasWidth);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(canvasHeight);

        // long: an int product can overflow to a negative value and pass the size check.
        long minSize = (long)canvasWidth * canvasHeight * 4;
        if (buffer.Length < minSize)
            throw new ArgumentException($"Buffer boyutu en az {minSize} byte olmalıdır.");

        fixed (byte* pBuf = buffer)
        {
            int res = LokNative.lok_doc_paint_tile(
                _handle,
                pBuf,
                canvasWidth,
                canvasHeight,
                tilePosXTwips,
                tilePosYTwips,
                tileWidthTwips,
                tileHeightTwips);

            if (res != 0)
            {
                string err = LokNative.GetLastError() ?? $"Hata: {res}";
                throw new InvalidOperationException($"PaintTile başarısız oldu: {err}");
            }
        }
    }

    /// <summary>
    /// Belgeyi doğrudan başka bir formata kaydeder (Örn: "pdf").
    /// </summary>
    public void SaveAs(string outputPath, string format = "pdf", string? filterOptions = null)
    {
        EnsureNotDisposed();
        ArgumentException.ThrowIfNullOrWhiteSpace(outputPath);

        string absOut = Path.GetFullPath(outputPath);
        int res = LokNative.lok_doc_save_as(_handle, absOut, format, filterOptions);
        if (res != 0)
        {
            string err = LokNative.GetLastError() ?? $"Hata kodu: {res}";
            throw new InvalidOperationException($"Belge kaydedilemedi ({format}): {err}");
        }
    }

    /// <summary>
    /// Belgeyi doğrudan PDF byte dizisi olarak alır.
    /// </summary>
    public byte[] ExportToPdfBytes()
    {
        EnsureNotDisposed();
        string tempPdf = Path.Combine(Path.GetTempPath(), $"lok_{Guid.NewGuid():N}.pdf");
        try
        {
            SaveAs(tempPdf, "pdf");
            return File.ReadAllBytes(tempPdf);
        }
        finally
        {
            if (File.Exists(tempPdf))
            {
                try { File.Delete(tempPdf); } catch { }
            }
        }
    }

    private void EnsureNotDisposed()
    {
        if (_handle == IntPtr.Zero)
            throw new ObjectDisposedException(nameof(LokDocument));
    }

    public void Dispose()
    {
        if (_handle != IntPtr.Zero)
        {
            LokNative.lok_doc_close(_handle);
            _handle = IntPtr.Zero;
        }
    }
}
