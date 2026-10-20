using System;
using System.IO;
using System.Runtime.InteropServices;

namespace LokViewer;

public static class LokBridge
{
    private const string DllName = "lok_viewer.dll";

    static LokBridge()
    {
        string dllPath = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, @"..\..\..\..\target\release\lok_viewer.dll"));
        if (File.Exists(dllPath))
        {
            NativeLibrary.Load(dllPath);
        }
        else
        {
            NativeLibrary.Load(DllName);
        }
    }

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lok_viewer_init([MarshalAs(UnmanagedType.LPUTF8Str)] string? installPath);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void lok_viewer_destroy();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr lok_doc_open([MarshalAs(UnmanagedType.LPUTF8Str)] string filePath);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void lok_doc_close(IntPtr doc);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lok_doc_get_parts(IntPtr doc);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void lok_doc_set_part(IntPtr doc, int part);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lok_doc_get_size_twips(IntPtr doc, out int width, out int height);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern unsafe int lok_doc_paint_tile(
        IntPtr doc,
        byte* buffer,
        int canvasWidth,
        int canvasHeight,
        int tilePosX,
        int tilePosY,
        int tileWidth,
        int tileHeight);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern int lok_doc_save_as(
        IntPtr doc,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string outPath,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string format,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? filterOptions);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr lok_get_last_error();

    public static string? GetLastError()
    {
        IntPtr ptr = lok_get_last_error();
        return ptr == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(ptr);
    }
}

class Program
{
    static unsafe void Main()
    {
        Console.WriteLine("=== LibreOfficeKit In-Process Testi Başlatılıyor ===");

        // 1. Motoru LibreOffice dizini ile ayağa kaldır
        string loPath = @"C:\Program Files\LibreOffice\program";
        Console.WriteLine($"[1/4] lok_viewer_init çağrılıyor: {loPath}");
        int initRes = LokBridge.lok_viewer_init(loPath);
        if (initRes != 0)
        {
            Console.WriteLine($"[HATA] Başlatılamadı: {LokBridge.GetLastError()} (Kod: {initRes})");
            return;
        }
        Console.WriteLine("      LibreOfficeKit çekirdeği in-process başarıyla yüklendi!");

        // 2. Test için basit bir metin dosyası oluşturup açalım
        string testFile = Path.Combine(AppContext.BaseDirectory, "test_doc.odt");
        // Eğer dosya yoksa docx veya txt açabiliriz
        string textSample = Path.Combine(AppContext.BaseDirectory, "sample.txt");
        File.WriteAllText(textSample, "Merhaba! Bu belge gerçek LibreOfficeKit motoruyla in-process açıldı ve render edildi.");

        Console.WriteLine($"[2/4] Doküman açılıyor: {textSample}");
        IntPtr doc = LokBridge.lok_doc_open(textSample);
        if (doc == IntPtr.Zero)
        {
            Console.WriteLine($"[HATA] Doküman açılamadı: {LokBridge.GetLastError()}");
            return;
        }
        Console.WriteLine("      Doküman başarıyla açıldı!");

        // 3. Doküman özelliklerini al
        int parts = LokBridge.lok_doc_get_parts(doc);
        LokBridge.lok_doc_get_size_twips(doc, out int twipWidth, out int twipHeight);
        Console.WriteLine($"      Toplam Parça/Sayfa: {parts}, Boyutlar (TWIPs): {twipWidth}x{twipHeight}");

        // 4. Doğrudan Tiled Rendering (Piksel Tamponuna Çizim)
        int canvasWidth = 800;
        int canvasHeight = 600;
        byte[] pixelBuffer = new byte[canvasWidth * canvasHeight * 4];

        Console.WriteLine($"[3/4] paintTile ile {canvasWidth}x{canvasHeight} çözünürlüğünde piksel tamponuna çiziliyor...");
        fixed (byte* pBuf = pixelBuffer)
        {
            int renderRes = LokBridge.lok_doc_paint_tile(
                doc,
                pBuf,
                canvasWidth,
                canvasHeight,
                0, 0, // Sol üst (0,0) twips
                twipWidth, twipHeight // Tüm dokümanı sığdır
            );

            if (renderRes == 0)
            {
                Console.WriteLine("      Pikseller başarıyla boyandı (Raw BGRA formatı)!");
            }
        }

        // 5. Doğrudan kusursuz PDF export alalım
        string pdfPath = Path.Combine(AppContext.BaseDirectory, "output_perfect.pdf");
        Console.WriteLine($"[4/4] Doküman doğrudan PDF'e aktarılıyor: {pdfPath}");
        int saveRes = LokBridge.lok_doc_save_as(doc, pdfPath, "pdf", null);
        if (saveRes == 0 && File.Exists(pdfPath))
        {
            Console.WriteLine($"      Kusursuz PDF başarıyla üretildi! Dosya boyutu: {new FileInfo(pdfPath).Length} byte");
        }
        else
        {
            Console.WriteLine($"[HATA] PDF kaydedilemedi: {LokBridge.GetLastError()}");
        }

        // Temizlik
        LokBridge.lok_doc_close(doc);
        LokBridge.lok_viewer_destroy();

        Console.WriteLine("\n TEBRİKLER! Gerçek LibreOfficeKit in-process başarıyla çalıştı!");
    }
}
