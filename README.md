# lok-viewer-native

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![.NET 8 | 9 | 10](https://img.shields.io/badge/.NET-8.0%20%7C%209.0%20%7C%2010.0-purple.svg)](https://dotnet.microsoft.com/)
[![Platform](https://img.shields.io/badge/Platform-Windows%20x64-lightgrey.svg)]()

High-performance, in-process **LibreOfficeKit** native wrapper and managed **.NET 8 / 9 / 10** bindings for document viewing, tile rendering, and document conversion.

---

## Overview

`lok-viewer-native` exposes a robust C ABI over LibreOfficeKit (Windows x64) to enable:
- In-process document loading and rendering for office documents (`DOCX`, `XLSX`, `PPTX`, `ODT`, `ODS`, `RTF`, etc.).
- Direct tile rendering (`paintTile`) to memory buffers (BGRA) for smooth UI viewing.
- Fast, high-fidelity export to PDF and other formats without external process spawning (`soffice.exe`).
- Complete thread safety: all LibreOfficeKit calls are synchronized internally with a native mutex.
- Opaque document handles: safe against double-close or stale pointers without crashing the host process.
- Isolated user profile support (`%LOCALAPPDATA%\LokViewer\lo-profile` or `%LOK_VIEWER_PROFILE%`) preventing desktop profile collisions and UI deadlocks.

The native library is implemented in both **C++17** and **Rust** (providing identical C ABI exports), coupled with the **`LokViewer.NET`** class library.

---

## Project Structure

```text
lok-viewer-native/
├── CMakeLists.txt              # CMake build configuration for C++ DLL
├── lok_viewer.vcxproj          # Visual Studio project for C++ DLL
├── Cargo.toml                  # Rust crate configuration (cdylib)
├── include/
│   ├── lok_viewer.h            # Public C ABI interface
│   └── LibreOfficeKit/         # LibreOfficeKit C/C++ header definitions
├── src/
│   ├── lok_viewer.cpp          # C++17 implementation
│   └── lib.rs                  # Rust implementation (alternative)
├── dotnet/
│   ├── LokViewer.NET/          # Managed .NET (8/9/10) library
│   │   ├── LibreOfficeEngine.cs
│   │   └── LokViewer.NET.csproj
│   └── LokViewer.Test/         # .NET integration tests
└── lok_net/                    # Quick validation console harness
```

---

## Prerequisites

- **Windows 64-bit** (x64)
- **LibreOffice** (x64) installed (default path: `C:\Program Files\LibreOffice\program`)
- **Visual Studio 2022+** (with C++ Desktop workload) and/or **CMake 3.20+**
- (Optional for Rust) **Rust toolchain** (`x86_64-pc-windows-msvc`)
- **.NET 8.0, 9.0 or 10.0 SDK**

---

## Building the Native Library

### Option A: Using CMake (C++)

```powershell
cmake -S . -B build-cpp -G "Visual Studio 17 2022" -A x64
cmake --build build-cpp --config Release
```
Output: `build-cpp/Release/lok_viewer.dll`

### Option B: Using Cargo (Rust)

```powershell
cargo build --release
```
Output: `target/release/lok_viewer.dll`

---

## Basic .NET Usage

### 1. Initialize Engine

```csharp
using LokViewer;

// Automatically locates LibreOffice at default path:
LibreOfficeEngine.Initialize();

// Or specify LibreOffice program path explicitly:
// LibreOfficeEngine.Initialize(@"C:\Program Files\LibreOffice\program");
```

### 2. Open and Render Document

```csharp
using var doc = LibreOfficeEngine.OpenDocument(@"C:\path\to\document.docx");

int partCount = doc.PartCount;
var (widthTwips, heightTwips) = doc.GetDocumentSizeTwips();

// Render tile to BGRA buffer (e.g. 800x600 px)
int widthPx = 800;
int heightPx = 600;
byte[] pixelBuffer = new byte[widthPx * heightPx * 4];

doc.PaintTile(pixelBuffer, widthPx, heightPx, tilePosXTwips: 0, tilePosYTwips: 0, widthTwips, heightTwips);
```

### 3. Convert Document to PDF

```csharp
// Export directly to byte array (in-memory)
byte[] pdfBytes = doc.ExportToPdfBytes();
File.WriteAllBytes("output.pdf", pdfBytes);

// Or save directly to disk
doc.SaveAs("output.pdf", "pdf");
```

### 4. Shutdown

```csharp
// Cleans up native engine resources on process termination:
LibreOfficeEngine.Shutdown();
```

---

## Dependency Injection (DI) & ASP.NET Core Integration

In web APIs, worker services, or enterprise .NET applications, you can wrap `LokViewer` inside a registered service to handle document conversions and rendering with proper lifecycle management.

### 1. Define the Service Interface

```csharp
public interface ILibreOfficeConverter
{
    byte[] ConvertToPdf(string sourceFilePath);
    Task<byte[]> ConvertToPdfAsync(Stream inputStream, string fileExtension, CancellationToken cancellationToken = default);
}
```

### 2. Implement the Service

```csharp
using LokViewer;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;

public class LibreOfficeConverter : ILibreOfficeConverter, IDisposable
{
    private readonly ILogger<LibreOfficeConverter> _logger;

    public LibreOfficeConverter(ILogger<LibreOfficeConverter> logger, IHostApplicationLifetime lifetime)
    {
        _logger = logger;

        // Initialize LibreOfficeKit engine once at startup
        _logger.LogInformation("Initializing LibreOfficeKit native engine...");
        LibreOfficeEngine.Initialize();

        // Register clean shutdown when the host application stops
        lifetime.ApplicationStopping.Register(() =>
        {
            _logger.LogInformation("Shutting down LibreOfficeKit native engine...");
            LibreOfficeEngine.Shutdown();
        });
    }

    public byte[] ConvertToPdf(string sourceFilePath)
    {
        using var doc = LibreOfficeEngine.OpenDocument(sourceFilePath);
        return doc.ExportToPdfBytes();
    }

    public async Task<byte[]> ConvertToPdfAsync(Stream inputStream, string fileExtension, CancellationToken cancellationToken = default)
    {
        // Write incoming stream to temporary file
        string tempInput = Path.Combine(Path.GetTempPath(), $"input_{Guid.NewGuid():N}{fileExtension}");
        try
        {
            using (var fileStream = File.Create(tempInput))
            {
                await inputStream.CopyToAsync(fileStream, cancellationToken);
            }

            // LibreOfficeKit is thread-safe internally via native mutex
            return await Task.Run(() => ConvertToPdf(tempInput), cancellationToken);
        }
        finally
        {
            if (File.Exists(tempInput))
            {
                try { File.Delete(tempInput); } catch { /* ignore */ }
            }
        }
    }

    public void Dispose()
    {
        // Engine is closed via ApplicationStopping
    }
}
```

### 3. Register in `Program.cs`

```csharp
var builder = WebApplication.CreateBuilder(args);

// Register LibreOffice converter as a Singleton
builder.Services.AddSingleton<ILibreOfficeConverter, LibreOfficeConverter>();

var app = builder.Build();
```

### 4. Minimal API Endpoint Example

Upload any supported document (`.docx`, `.xlsx`, `.pptx`, `.odt`) and stream back the converted PDF:

```csharp
app.MapPost("/api/documents/convert-to-pdf", async (IFormFile file, ILibreOfficeConverter converter) =>
{
    if (file == null || file.Length == 0)
        return Results.BadRequest("No file uploaded.");

    string extension = Path.GetExtension(file.FileName);
    using var stream = file.OpenReadStream();

    byte[] pdfBytes = await converter.ConvertToPdfAsync(stream, extension);

    return Results.File(pdfBytes, "application/pdf", $"{Path.GetFileNameWithoutExtension(file.FileName)}.pdf");
});

app.Run();
```

### 5. Controller Example

```csharp
[ApiController]
[Route("api/[controller]")]
public class DocumentController : ControllerBase
{
    private readonly ILibreOfficeConverter _converter;

    public DocumentController(ILibreOfficeConverter converter)
    {
        _converter = converter;
    }

    [HttpPost("to-pdf")]
    public async Task<IActionResult> ConvertToPdf(IFormFile file, CancellationToken ct)
    {
        if (file == null || file.Length == 0)
            return BadRequest("Dosya yüklenmedi.");

        using var stream = file.OpenReadStream();
        byte[] pdfData = await _converter.ConvertToPdfAsync(stream, Path.GetExtension(file.FileName), ct);

        return File(pdfData, "application/pdf", $"{Path.GetFileNameWithoutExtension(file.FileName)}.pdf");
    }
}
```

---

## Important Operational Notes

1. **Architecture (x64 Only):** The native `lok_viewer.dll` and LibreOffice are 64-bit binaries. Ensure your .NET project targets `x64`:
   ```xml
   <PlatformTarget>x64</PlatformTarget>
   ```
2. **Thread Safety:** LibreOfficeKit is not natively thread-safe; however, `lok-viewer-native` wraps every internal call in a global mutex (`std::mutex g_mutex`). Concurrent calls from multiple async tasks or threads are executed safely in queue without crashing or corrupting memory.
3. **User Profile Isolation:** Each instance automatically uses an isolated profile under `%LOCALAPPDATA%\LokViewer\lo-profile` (or `%LOK_VIEWER_PROFILE%`), preventing lock conflicts with interactive LibreOffice desktop sessions.

---

## Why lok-viewer-native?

| Feature | `soffice.exe --headless` (CLI) | `lok-viewer-native` (In-Process) |
|---|---|---|
| **Execution Model** | Spawns heavy external OS processes per file | Direct, in-process C ABI calls |
| **Performance & Latency** | High overhead (~1-3 seconds process startup) | Extremely fast (sub-millisecond once loaded) |
| **Process Crash & Memory Leaks** | Orphaned `soffice.bin` processes freeze servers | Managed lifecycle, structured exception handling |
| **Concurrency & Thread Safety** | Desktop profile lock collisions | Native mutex synchronization & profile isolation |
| **Document Tiling / Live View** | Not supported | Direct BGRA tile rendering into memory buffers |
| **.NET Integration** | Clunky `Process.Start` with string parsing | First-class C# async API with DI support |

---

## Supported Formats

- **Text Documents:** `.docx`, `.doc`, `.odt`, `.rtf`, `.txt`
- **Spreadsheets:** `.xlsx`, `.xls`, `.ods`, `.csv`
- **Presentations:** `.pptx`, `.ppt`, `.odp`
- **Export Targets:** `pdf`, `png`, `html`, etc.

---

## Author & Maintainer

- **Author:** Semih Yücel
- **GitHub:** [@lordraiden-dev](https://github.com/lordraiden-dev)
- **Repository:** [lordraiden-dev/lok-viewer-native](https://github.com/lordraiden-dev/lok-viewer-native)

---

## License

This project is licensed under the [MIT License](LICENSE).
Copyright (c) 2026 Semih Yücel.
