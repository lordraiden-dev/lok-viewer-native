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
- Fast, high-fidelity export to PDF and other formats.
- Complete thread safety: all LibreOfficeKit calls are synchronized with an internal mutex.
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

## .NET Usage

### 1. Initialize Engine

```csharp
using LokViewer;

// Automatically locates LibreOffice at default path or custom folder:
LibreOfficeEngine.Initialize();

// Or specify LibreOffice program path explicitly:
// LibreOfficeEngine.Initialize(@"C:\Program Files\LibreOffice\program");
```

### 2. Open and Render Document

```csharp
using var doc = LibreOfficeEngine.OpenDocument(@"C:\path\to\document.docx");

int partCount = doc.PartCount;
var (widthTwips, heightTwips) = doc.GetDocumentSizeTwips();

// Render tile to BGRA buffer
int widthPx = 800;
int heightPx = 600;
byte[] pixelBuffer = new byte[widthPx * heightPx * 4];

doc.PaintTile(pixelBuffer, widthPx, heightPx, tilePosXTwips: 0, tilePosYTwips: 0, widthTwips, heightTwips);
```

### 3. Convert Document to PDF

```csharp
// Export directly to byte array
byte[] pdfBytes = doc.ExportToPdfBytes();
File.WriteAllBytes("output.pdf", pdfBytes);

// Or save directly to disk
doc.SaveAs("output.pdf", "pdf");
```

### 4. Shutdown

```csharp
// Automatically called on process exit or can be explicitly called:
LibreOfficeEngine.Shutdown();
```

---

## License

This project is licensed under the MIT License.
