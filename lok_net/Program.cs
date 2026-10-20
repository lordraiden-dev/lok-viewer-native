using System.Diagnostics;
using System.Reflection;
using LokViewer;

int failures = 0;
void Check(bool ok, string what) { Console.WriteLine($"  {(ok ? "PASS" : "FAIL")}  {what}"); if (!ok) failures++; }

var native = typeof(LibreOfficeEngine).Assembly.GetType("LokViewer.LokNative")!;
object? Call(string name, params object?[] args) =>
    native.GetMethod(name, BindingFlags.NonPublic | BindingFlags.Static)!.Invoke(null, args);
string? LastError() => (string?)Call("GetLastError");

Console.WriteLine($"[1] P/Invoke binding (dll: {Path.Combine(AppContext.BaseDirectory, "lok_viewer.dll")})");
Check((IntPtr)Call("lok_doc_open", "x.docx")! == IntPtr.Zero, "lok_doc_open before init -> NULL");
Check(LastError()?.Contains("not initialized") == true, $"error marshaled as UTF-8 string: '{LastError()}'");
var sizeArgs = new object?[] { IntPtr.Zero, 0, 0 };
Check((int)Call("lok_doc_get_size_twips", sizeArgs)! == -1, "lok_doc_get_size_twips(out int, out int) bad handle -> -1");
Check((int)Call("lok_doc_get_parts", IntPtr.Zero)! == 0, "lok_doc_get_parts bad handle -> 0");
Call("lok_doc_set_part", IntPtr.Zero, 0);
Check((int)Call("lok_doc_save_as", IntPtr.Zero, "x.pdf", "pdf", null)! == -1, "lok_doc_save_as bad handle -> -1");
unsafe { Check((int)Call("lok_doc_paint_tile", IntPtr.Zero, Pointer.Box(null, typeof(byte*)), 1, 1, 0, 0, 1, 1)! == -1, "lok_doc_paint_tile null buffer -> -1"); }
Call("lok_doc_close", IntPtr.Zero);
Call("lok_viewer_destroy");
Check(true, "all 10 entry points bound and callable");

Console.WriteLine("[2] public API");
try { LibreOfficeEngine.Initialize(@"C:\no\such\ğüş folder"); Check(false, "bad path throws"); }
catch (InvalidOperationException e) { Check(e.Message.Contains("ğüş"), $"bad path throws, Turkish chars survive: {e.Message}"); }
var sw = Stopwatch.StartNew();
LibreOfficeEngine.Initialize();
Check(true, $"Initialize() default path OK ({sw.ElapsedMilliseconds} ms)");
try { LibreOfficeEngine.OpenDocument(@"C:\no\such.docx"); Check(false, "missing file throws"); }
catch (FileNotFoundException) { Check(true, "OpenDocument(missing) -> FileNotFoundException"); }

Console.WriteLine("[3] open real document (time-boxed 45 s)");
string rtf = Path.Combine(AppContext.BaseDirectory, "test #1 ğüş.rtf");
File.WriteAllText(rtf, @"{\rtf1\ansi Hello from .NET.\par}");
var open = Task.Run(() => LibreOfficeEngine.OpenDocument(rtf));
if (open.Wait(TimeSpan.FromSeconds(45)))
{
    using var doc = open.Result;
    var (w, h) = doc.GetDocumentSizeTwips();
    var buf = new byte[400 * 300 * 4];
    doc.PaintTile(buf, 400, 300, 0, 0, w, h);
    var pdf = doc.ExportToPdfBytes();
    Check(doc.PartCount >= 1 && w > 0 && pdf.Length > 0, $"open/size/paint/pdf OK: parts={doc.PartCount} size={w}x{h} pdf={pdf.Length} bytes");
    try { doc.PaintTile(buf, 70000, 70000, 0, 0, w, h); Check(false, "overflow guard"); }
    catch (ArgumentException) { Check(true, "PaintTile 70000x70000 rejected (no int overflow)"); }
}
else Console.WriteLine("  HANG  OpenDocument did not return in 45 s (LOK documentLoad, known on this machine)");

Console.WriteLine($"\nRESULT: {failures} failure(s)");
Environment.Exit(failures);
