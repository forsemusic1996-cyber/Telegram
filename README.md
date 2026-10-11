# REAPER Telegram Uploader

Asynchronous Telegram document upload for REAPER. The upload runs in a native worker thread, so a large WAV does not block the REAPER UI.

## Windows 7 support

The Windows build targets Windows 7 (`_WIN32_WINNT=0x0601`) and uses a static libcurl build with Schannel. The resulting `Telegram.dll` does not require a separately installed `curl.exe` or MinGW runtime DLLs.

The GitHub Action builds a Windows x64 DLL. REAPER's official Windows extension SDK recommends MSVC-compatible builds; the workflow uses the Visual Studio toolchain.

## Installation

1. Download `Telegram.dll` from the GitHub Actions artifact.
2. Copy it to the REAPER `UserPlugins` directory.
3. Copy `scripts/Telegram.lua` and `scripts/Telegram.ini.example` to a folder in the REAPER Scripts directory.
4. Rename `Telegram.ini.example` to `Telegram.ini` and set `BOT_TOKEN` and `CHAT_ID`.
5. Load `Telegram.lua` from the REAPER Action List.

The token and chat ID are read at runtime. They are not compiled into the DLL and should not be committed to GitHub.

## ReaScript API

- `reaper.Telegram_StartUpload(token, chat_id, file_path, caption)` returns a job ID immediately.
- `reaper.Telegram_PollUpload(job)` returns `0` while uploading, `1` on success, or `-1` on failure.
- `reaper.Telegram_UploadError(job)` returns the error text.
- `reaper.Telegram_CancelUpload(job)` requests cancellation.
- `reaper.Telegram_ReleaseUpload(job)` releases a completed job.

Use `StartUpload` and `PollUpload` from a deferred Lua script. Do not call a blocking wait function from the REAPER UI thread.
