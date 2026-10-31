/*
 * TurboWarp Packager - single file executable stub
 *
 * This is a tiny native Windows program that is prepended to a zip archive to create a
 * "single file" executable. When the user runs the generated .exe, this stub:
 *
 *   1. Locates the zip archive that is appended to the end of its own executable file.
 *   2. Extracts the archive into a cache directory inside %LOCALAPPDATA% (only once per build).
 *   3. Launches the real application executable that lives inside the archive.
 *
 * Layout of the generated file:
 *
 *   +--------------------------------------+
 *   | stub (this program, stub-*.exe)      |
 *   +--------------------------------------+
 *   | zip archive (the entire application) |
 *   +--------------------------------------+
 *   | 32 byte trailer (see below)          |
 *   +--------------------------------------+
 *
 * Trailer layout (all integers little endian):
 *
 *   offset  size  field
 *   0       4     magic, "TSFX"
 *   4       4     format version (1)
 *   8       4     reserved (0)
 *   12      8     offset of the zip archive from the start of the file
 *   20      8     size of the zip archive in bytes
 *   28      4     CRC-32 (the one used by zlib) of the zip archive
 *
 * The file "src/packager/packager.js" writes this structure; the code that generates the
 * trailer must be kept in sync with this file.
 *
 * This program is compiled by src/sfx/build.ps1; the compiled binaries are checked into
 * the repository as static/sfx/stub-{x64,x86,arm64}.exe.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "miniz.h"

#define SFX_MAGIC "TSFX"
#define SFX_VERSION 1u
#define SFX_TRAILER_SIZE 32
#define SFX_CACHE_DIR_NAME L"TurboWarpPackager-SFX"
#define SFX_MARKER_FILE L".sfx-ok"
#define SFX_META_FILE "__sfx_meta.txt"

/* Largest path that we handle. Windows itself allows up to 32767 characters with the \\?\ prefix. */
#define SFX_PATH_MAX 32768

/* Windows shows a clear error if the cache path gets too long, as CreateProcessW is not reliable
   with extended length paths. */
#define SFX_LAUNCH_PATH_MAX 240

static void sfx_error_message(const wchar_t *message)
{
    MessageBoxW(NULL, message, L"TurboWarp Packager", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
}

static void sfx_errorf(const wchar_t *format, ...)
{
    wchar_t buffer[4096];
    va_list arguments;
    va_start(arguments, format);
    _vsnwprintf(buffer, 4095, format, arguments);
    va_end(arguments);
    buffer[4095] = 0;
    sfx_error_message(buffer);
}

static mz_uint32 read_u32(const unsigned char *p)
{
    return (mz_uint32)p[0] | ((mz_uint32)p[1] << 8) | ((mz_uint32)p[2] << 16) | ((mz_uint32)p[3] << 24);
}

static mz_uint64 read_u64(const unsigned char *p)
{
    return (mz_uint64)read_u32(p) | ((mz_uint64)read_u32(p + 4) << 32);
}

/* Decodes UTF-8 to UTF-16. Falls back to the system code page for archives created by
   tools that don't set the UTF-8 flag. */
static BOOL utf8_to_wide(const char *input, wchar_t *output, size_t output_count)
{
    int result = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, output, (int)output_count);
    if (result == 0)
    {
        result = MultiByteToWideChar(CP_ACP, 0, input, -1, output, (int)output_count);
    }
    return result != 0;
}

/* Returns a path suitable for the wide file APIs. Absolute paths get the \\?\ prefix so that
   the 260 character MAX_PATH limit doesn't apply. The returned pointer is either the input
   pointer or the provided buffer. */
static const wchar_t *to_api_path(const wchar_t *path, wchar_t *buffer, size_t buffer_count)
{
    size_t length = wcslen(path);
    if (wcsncmp(path, L"\\\\?\\", 4) == 0 || wcsncmp(path, L"\\\\.\\", 4) == 0)
    {
        return path;
    }
    if (length + 4 < buffer_count && path[0] != 0 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'))
    {
        wcscpy(buffer, L"\\\\?\\");
        wcscat(buffer, path);
        /* \\?\ paths must use backslashes and must not be normalized */
        for (wchar_t *p = buffer + 4; *p != 0; p++)
        {
            if (*p == L'/')
            {
                *p = L'\\';
            }
        }
        return buffer;
    }
    return path;
}

static BOOL file_exists(const wchar_t *path)
{
    wchar_t api_buffer[SFX_PATH_MAX + 8];
    DWORD attributes = GetFileAttributesW(to_api_path(path, api_buffer, SFX_PATH_MAX + 8));
    return attributes != INVALID_FILE_ATTRIBUTES;
}

/* Creates a directory and all of its parents. Returns TRUE if the directory exists when done. */
static BOOL ensure_directory(const wchar_t *path)
{
    wchar_t buffer[SFX_PATH_MAX];
    wchar_t api_buffer[SFX_PATH_MAX + 8];
    size_t length = wcslen(path);
    if (length == 0 || length >= SFX_PATH_MAX)
    {
        return FALSE;
    }
    wcscpy(buffer, path);
    for (size_t i = 1; i < length; i++)
    {
        if ((buffer[i] == L'\\' || buffer[i] == L'/') && buffer[i - 1] != L':')
        {
            buffer[i] = 0;
            if (!CreateDirectoryW(to_api_path(buffer, api_buffer, SFX_PATH_MAX + 8), NULL))
            {
                DWORD error = GetLastError();
                if (error != ERROR_ALREADY_EXISTS)
                {
                    return FALSE;
                }
            }
            buffer[i] = path[i];
        }
    }
    if (!CreateDirectoryW(to_api_path(buffer, api_buffer, SFX_PATH_MAX + 8), NULL))
    {
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS)
        {
            return FALSE;
        }
    }
    return TRUE;
}

/* Reads the entire executable file and returns the path of the running program. */
static BOOL get_self_path(wchar_t *buffer, DWORD buffer_count)
{
    DWORD size = buffer_count;
    if (QueryFullProcessImageNameW(GetCurrentProcess(), 0, buffer, &size) && size > 0)
    {
        return TRUE;
    }
    DWORD length = GetModuleFileNameW(NULL, buffer, buffer_count);
    return length > 0 && length < buffer_count;
}

/* State of the zip archive that is embedded inside the executable. */
typedef struct
{
    HANDLE file;
    mz_uint64 base;
    mz_uint64 size;
    mz_uint64 position;
} SfxReader;

static size_t sfx_read_callback(void *opaque, mz_uint64 offset, void *buffer, size_t size)
{
    SfxReader *reader = (SfxReader *)opaque;
    if (offset >= reader->size)
    {
        return 0;
    }
    if (offset + size > reader->size)
    {
        size = (size_t)(reader->size - offset);
    }
    if (reader->position != offset)
    {
        LARGE_INTEGER position;
        position.QuadPart = (LONGLONG)(reader->base + offset);
        if (!SetFilePointerEx(reader->file, position, NULL, FILE_BEGIN))
        {
            return 0;
        }
        reader->position = offset;
    }
    DWORD read = 0;
    if (!ReadFile(reader->file, buffer, (DWORD)size, &read, NULL))
    {
        return 0;
    }
    reader->position += read;
    return (size_t)read;
}

typedef struct
{
    HANDLE file;
} SfxWriter;

/* Returns the number of bytes written; miniz treats a short write as a failure. */
static size_t sfx_write_callback(void *opaque, mz_uint64 offset, const void *buffer, size_t size)
{
    (void)offset;
    SfxWriter *writer = (SfxWriter *)opaque;
    if (size > 0xFFFFFFFF)
    {
        return 0;
    }
    DWORD written = 0;
    if (!WriteFile(writer->file, buffer, (DWORD)size, &written, NULL))
    {
        return 0;
    }
    return (size_t)written;
}

/* Computes the CRC-32 of a region of the file. */
static BOOL compute_crc(HANDLE file, mz_uint64 offset, mz_uint64 size, mz_uint32 *result)
{
    static unsigned char buffer[1024 * 1024];
    LARGE_INTEGER position;
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(file, position, NULL, FILE_BEGIN))
    {
        return FALSE;
    }
    mz_ulong crc = 0;
    mz_uint64 remaining = size;
    while (remaining > 0)
    {
        DWORD to_read = (DWORD)(remaining > sizeof(buffer) ? sizeof(buffer) : remaining);
        DWORD read = 0;
        if (!ReadFile(file, buffer, to_read, &read, NULL) || read == 0)
        {
            return FALSE;
        }
        crc = mz_crc32(crc, buffer, read);
        remaining -= read;
    }
    *result = (mz_uint32)crc;
    return TRUE;
}

/* Rejects paths that could escape the destination directory. */
static BOOL is_safe_relative_name(const wchar_t *name)
{
    if (name[0] == L'/' || name[0] == L'\\')
    {
        return FALSE;
    }
    if (name[0] != 0 && name[1] == L':')
    {
        return FALSE;
    }
    size_t component_start = 0;
    size_t i = 0;
    for (;;)
    {
        wchar_t c = name[i];
        if (c == L'/' || c == L'\\' || c == 0)
        {
            size_t dots = 0;
            size_t spaces = 0;
            for (size_t j = component_start; j < i; j++)
            {
                if (name[j] == L'.')
                {
                    dots++;
                }
                else if (name[j] == L' ')
                {
                    spaces++;
                }
            }
            /* Windows strips trailing dots and spaces, so "." and ".." can appear in more forms
               than just the literal strings. */
            if (dots > 0 && dots + spaces == i - component_start)
            {
                return FALSE;
            }
            if (c == 0)
            {
                break;
            }
            component_start = i + 1;
        }
        i++;
    }
    return TRUE;
}

/* Finds "key=value" in the meta file. */
static BOOL meta_lookup(const char *meta, size_t meta_size, const char *key, char *output, size_t output_size)
{
    size_t key_length = strlen(key);
    size_t line_start = 0;
    while (line_start < meta_size)
    {
        size_t line_end = line_start;
        while (line_end < meta_size && meta[line_end] != '\n' && meta[line_end] != '\r')
        {
            line_end++;
        }
        if (line_end - line_start > key_length &&
            memcmp(meta + line_start, key, key_length) == 0 &&
            meta[line_start + key_length] == '=')
        {
            size_t value_start = line_start + key_length + 1;
            size_t value_length = line_end - value_start;
            if (value_length >= output_size)
            {
                value_length = output_size - 1;
            }
            memcpy(output, meta + value_start, value_length);
            output[value_length] = 0;
            return TRUE;
        }
        line_start = line_end;
        while (line_start < meta_size && (meta[line_start] == '\n' || meta[line_start] == '\r'))
        {
            line_start++;
        }
    }
    return FALSE;
}

/* Extracts every file in the archive into the destination directory. */
static BOOL extract_zip(mz_zip_archive *zip, const wchar_t *destination)
{
    static wchar_t path_buffer[SFX_PATH_MAX];
    static wchar_t api_buffer[SFX_PATH_MAX + 8];
    static wchar_t wide_name[4096];

    size_t destination_length = wcslen(destination);
    mz_uint file_count = mz_zip_reader_get_num_files(zip);
    for (mz_uint i = 0; i < file_count; i++)
    {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(zip, i, &stat))
        {
            sfx_errorf(L"Failed to read entry %u of the embedded archive.", i);
            return FALSE;
        }
        if (!utf8_to_wide(stat.m_filename, wide_name, 4096))
        {
            sfx_errorf(L"Entry %u of the embedded archive has a filename that could not be decoded.", i);
            return FALSE;
        }
        if (!is_safe_relative_name(wide_name))
        {
            sfx_errorf(L"The embedded archive contains an unsafe path: %ls", wide_name);
            return FALSE;
        }
        size_t name_length = wcslen(wide_name);
        if (name_length == 0)
        {
            continue;
        }
        if (destination_length + 1 + name_length >= SFX_PATH_MAX)
        {
            sfx_errorf(L"The embedded archive contains a path that is too long: %ls", wide_name);
            return FALSE;
        }
        wcscpy(path_buffer, destination);
        path_buffer[destination_length] = L'\\';
        wcscpy(path_buffer + destination_length + 1, wide_name);
        for (wchar_t *p = path_buffer + destination_length; *p != 0; p++)
        {
            if (*p == L'/')
            {
                *p = L'\\';
            }
        }

        if (stat.m_is_directory || wide_name[name_length - 1] == L'/' || wide_name[name_length - 1] == L'\\')
        {
            if (!ensure_directory(path_buffer))
            {
                sfx_errorf(L"Could not create directory:\n\n%ls\n\nError code: %lu", path_buffer, GetLastError());
                return FALSE;
            }
        }
        else
        {
            wchar_t *last_separator = wcsrchr(path_buffer, L'\\');
            if (last_separator != NULL)
            {
                *last_separator = 0;
                BOOL directory_ok = ensure_directory(path_buffer);
                *last_separator = L'\\';
                if (!directory_ok)
                {
                    sfx_errorf(L"Could not create directory:\n\n%ls\n\nError code: %lu", path_buffer, GetLastError());
                    return FALSE;
                }
            }
            HANDLE file = CreateFileW(to_api_path(path_buffer, api_buffer, SFX_PATH_MAX + 8),
                                      GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (file == INVALID_HANDLE_VALUE)
            {
                sfx_errorf(L"Could not create file:\n\n%ls\n\nError code: %lu", path_buffer, GetLastError());
                return FALSE;
            }
            SfxWriter writer;
            writer.file = file;
            mz_bool ok = mz_zip_reader_extract_to_callback(zip, i, sfx_write_callback, &writer, 0);
            CloseHandle(file);
            if (!ok)
            {
                sfx_errorf(L"Could not extract:\n\n%ls", path_buffer);
                return FALSE;
            }
        }
    }
    return TRUE;
}

/* Returns the part of the command line after the program name. */
static const wchar_t *command_line_tail(const wchar_t *command_line)
{
    const wchar_t *p = command_line;
    if (*p == L'"')
    {
        p++;
        while (*p != 0 && *p != L'"')
        {
            p++;
        }
        if (*p == L'"')
        {
            p++;
        }
    }
    else
    {
        while (*p != 0 && *p != L' ' && *p != L'\t')
        {
            p++;
        }
    }
    while (*p == L' ' || *p == L'\t')
    {
        p++;
    }
    return p;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous_instance, LPSTR command_line, int show_command)
{
    (void)instance;
    (void)previous_instance;
    (void)command_line;
    (void)show_command;

    static wchar_t self_path[SFX_PATH_MAX];
    static wchar_t api_buffer[SFX_PATH_MAX + 8];
    static wchar_t cache_root[SFX_PATH_MAX];
    static wchar_t cache_dir[SFX_PATH_MAX];
    static wchar_t marker_path[SFX_PATH_MAX];
    static wchar_t exe_path[SFX_PATH_MAX];
    static wchar_t command_buffer[SFX_PATH_MAX + 4096];
    static wchar_t mutex_name[128];
    char package_name[512];
    char main_exe[1024];
    wchar_t main_exe_wide[2048];

    if (!get_self_path(self_path, SFX_PATH_MAX))
    {
        sfx_error_message(L"Could not determine the location of this program.");
        return 1;
    }

    HANDLE file = CreateFileW(to_api_path(self_path, api_buffer, SFX_PATH_MAX + 8),
                              GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        sfx_errorf(L"Could not open:\n\n%ls\n\nError code: %lu", self_path, GetLastError());
        return 1;
    }

    LARGE_INTEGER file_size_li;
    if (!GetFileSizeEx(file, &file_size_li))
    {
        sfx_error_message(L"Could not determine the size of this program.");
        CloseHandle(file);
        return 1;
    }
    mz_uint64 file_size = (mz_uint64)file_size_li.QuadPart;

    unsigned char trailer[SFX_TRAILER_SIZE];
    LARGE_INTEGER trailer_position;
    if (file_size < SFX_TRAILER_SIZE + 4)
    {
        sfx_error_message(L"This file is not a valid single file application.");
        CloseHandle(file);
        return 1;
    }
    trailer_position.QuadPart = (LONGLONG)(file_size - SFX_TRAILER_SIZE);
    DWORD trailer_read = 0;
    if (!SetFilePointerEx(file, trailer_position, NULL, FILE_BEGIN) ||
        !ReadFile(file, trailer, SFX_TRAILER_SIZE, &trailer_read, NULL) ||
        trailer_read != SFX_TRAILER_SIZE)
    {
        sfx_error_message(L"Could not read this program.");
        CloseHandle(file);
        return 1;
    }
    if (memcmp(trailer, SFX_MAGIC, 4) != 0)
    {
        sfx_error_message(L"This file is not a valid single file application. It may be corrupted or truncated.");
        CloseHandle(file);
        return 1;
    }
    if (read_u32(trailer + 4) != SFX_VERSION)
    {
        sfx_errorf(L"This single file application was created by a newer version of the packager (format version %u).",
                   (unsigned int)read_u32(trailer + 4));
        CloseHandle(file);
        return 1;
    }
    mz_uint64 zip_offset = read_u64(trailer + 12);
    mz_uint64 zip_size = read_u64(trailer + 20);
    mz_uint32 zip_crc = read_u32(trailer + 28);
    if (zip_size == 0 || zip_offset + zip_size + SFX_TRAILER_SIZE != file_size)
    {
        sfx_error_message(L"This file is not a valid single file application. It may be corrupted or truncated.");
        CloseHandle(file);
        return 1;
    }

    SfxReader reader;
    reader.file = file;
    reader.base = zip_offset;
    reader.size = zip_size;
    reader.position = (mz_uint64)-1;

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    zip.m_pRead = sfx_read_callback;
    zip.m_pIO_opaque = &reader;
    if (!mz_zip_reader_init(&zip, zip_size, 0))
    {
        sfx_error_message(L"The embedded archive is not a valid zip archive.");
        CloseHandle(file);
        return 1;
    }

    /* Read __sfx_meta.txt, which tells us what to launch */
    mz_uint meta_index = MZ_UINT32_MAX;
    mz_uint file_count = mz_zip_reader_get_num_files(&zip);
    char entry_name[512];
    for (mz_uint i = 0; i < file_count; i++)
    {
        if (mz_zip_reader_get_filename(&zip, i, entry_name, sizeof(entry_name)) > 0 &&
            _stricmp(entry_name, SFX_META_FILE) == 0)
        {
            meta_index = i;
            break;
        }
    }
    if (meta_index == MZ_UINT32_MAX)
    {
        sfx_error_message(L"The embedded archive is missing its metadata.");
        mz_zip_reader_end(&zip);
        CloseHandle(file);
        return 1;
    }
    size_t meta_size = 0;
    void *meta_data = mz_zip_reader_extract_to_heap(&zip, meta_index, &meta_size, 0);
    if (meta_data == NULL)
    {
        sfx_error_message(L"Could not read the metadata of the embedded archive.");
        mz_zip_reader_end(&zip);
        CloseHandle(file);
        return 1;
    }
    package_name[0] = 0;
    main_exe[0] = 0;
    meta_lookup((const char *)meta_data, meta_size, "packageName", package_name, sizeof(package_name));
    meta_lookup((const char *)meta_data, meta_size, "mainExe", main_exe, sizeof(main_exe));
    mz_free(meta_data);
    if (package_name[0] == 0)
    {
        sfx_error_message(L"The embedded archive is missing its metadata.");
        mz_zip_reader_end(&zip);
        CloseHandle(file);
        return 1;
    }
    if (main_exe[0] == 0)
    {
        _snprintf(main_exe, sizeof(main_exe), "%s/%s.exe", package_name, package_name);
    }
    if (!utf8_to_wide(main_exe, main_exe_wide, 2048) || !is_safe_relative_name(main_exe_wide))
    {
        sfx_error_message(L"The embedded archive is missing its metadata.");
        mz_zip_reader_end(&zip);
        CloseHandle(file);
        return 1;
    }

    /* Build the cache directory path */
    wchar_t package_name_wide[512];
    if (!utf8_to_wide(package_name, package_name_wide, 512) || !is_safe_relative_name(package_name_wide))
    {
        sfx_error_message(L"The embedded archive is missing its metadata.");
        mz_zip_reader_end(&zip);
        CloseHandle(file);
        return 1;
    }
    cache_root[0] = 0;
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, cache_root)) || cache_root[0] == 0)
    {
        DWORD length = GetTempPathW(SFX_PATH_MAX, cache_root);
        if (length == 0 || length >= SFX_PATH_MAX)
        {
            sfx_error_message(L"Could not determine a directory to store the application in.");
            mz_zip_reader_end(&zip);
            CloseHandle(file);
            return 1;
        }
    }
    if (swprintf(cache_dir, SFX_PATH_MAX, L"%ls\\%ls\\%ls-%08x",
                 cache_root, SFX_CACHE_DIR_NAME, package_name_wide, (unsigned int)zip_crc) < 0)
    {
        sfx_error_message(L"The cache directory path is too long.");
        mz_zip_reader_end(&zip);
        CloseHandle(file);
        return 1;
    }
    if (swprintf(marker_path, SFX_PATH_MAX, L"%ls\\%ls", cache_dir, SFX_MARKER_FILE) < 0 ||
        swprintf(exe_path, SFX_PATH_MAX, L"%ls\\%ls", cache_dir, main_exe_wide) < 0)
    {
        sfx_error_message(L"The cache directory path is too long.");
        mz_zip_reader_end(&zip);
        CloseHandle(file);
        return 1;
    }

    /* Extract the archive if we haven't already. */
    if (!file_exists(marker_path) || !file_exists(exe_path))
    {
        mz_uint32 actual_crc = 0;
        if (!compute_crc(file, zip_offset, zip_size, &actual_crc))
        {
            sfx_error_message(L"Could not read this program.");
            mz_zip_reader_end(&zip);
            CloseHandle(file);
            return 1;
        }
        if (actual_crc != zip_crc)
        {
            sfx_error_message(L"This single file application is corrupted (checksum mismatch). Please download it again.");
            mz_zip_reader_end(&zip);
            CloseHandle(file);
            return 1;
        }

        /* Another instance of this application may be extracting the same files at the same time. */
        HANDLE mutex = NULL;
        if (swprintf(mutex_name, 128, L"Local\\TurboWarpPackager-SFX-%08x", (unsigned int)zip_crc) >= 0)
        {
            mutex = CreateMutexW(NULL, FALSE, mutex_name);
            if (mutex != NULL)
            {
                WaitForSingleObject(mutex, INFINITE);
            }
        }

        /* The other instance may have finished while we were waiting. */
        if (!file_exists(marker_path) || !file_exists(exe_path))
        {
            if (!extract_zip(&zip, cache_dir))
            {
                if (mutex != NULL)
                {
                    ReleaseMutex(mutex);
                    CloseHandle(mutex);
                }
                mz_zip_reader_end(&zip);
                CloseHandle(file);
                return 1;
            }
            HANDLE marker = CreateFileW(to_api_path(marker_path, api_buffer, SFX_PATH_MAX + 8),
                                        GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (marker != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile(marker, "ok\n", 3, &written, NULL);
                CloseHandle(marker);
            }
        }

        if (mutex != NULL)
        {
            ReleaseMutex(mutex);
            CloseHandle(mutex);
        }
    }

    mz_zip_reader_end(&zip);
    CloseHandle(file);

    if (wcslen(exe_path) >= SFX_LAUNCH_PATH_MAX)
    {
        sfx_errorf(L"The path to the application is too long:\n\n%ls", exe_path);
        return 1;
    }

    /* Launch the real application, passing along any command line arguments. */
    const wchar_t *arguments = command_line_tail(GetCommandLineW());
    if (swprintf(command_buffer, SFX_PATH_MAX + 4096, L"\"%ls\"", exe_path) < 0)
    {
        sfx_error_message(L"The path to the application is too long.");
        return 1;
    }
    if (arguments[0] != 0)
    {
        if (wcslen(command_buffer) + 1 + wcslen(arguments) >= SFX_PATH_MAX + 4096)
        {
            sfx_error_message(L"The command line is too long.");
            return 1;
        }
        wcscat(command_buffer, L" ");
        wcscat(command_buffer, arguments);
    }

    STARTUPINFOW startup_info;
    PROCESS_INFORMATION process_info;
    memset(&startup_info, 0, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    memset(&process_info, 0, sizeof(process_info));
    if (!CreateProcessW(exe_path, command_buffer, NULL, NULL, FALSE, 0, NULL, cache_dir, &startup_info, &process_info))
    {
        sfx_errorf(L"Could not start the application:\n\n%ls\n\nError code: %lu", exe_path, GetLastError());
        return 1;
    }
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return 0;
}
