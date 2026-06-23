#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#define EOCD_SIG 0x06054b50
#define LOC_SIG  0x04034b50
#define CEN_SIG  0x02014b50
#define COMPRESSION_FORMAT_DEFLATE 0x0002

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

typedef struct {
    DWORD sig;
    WORD  disk;
    WORD  cd_disk;
    WORD  cd_entries_disk;
    WORD  cd_entries_total;
    DWORD cd_size;
    DWORD cd_offset;
    WORD  comment_len;
} EocdRec;

typedef struct {
    DWORD sig;
    WORD  ver_made;
    WORD  ver_needed;
    WORD  flags;
    WORD  method;
    WORD  mod_time;
    WORD  mod_date;
    DWORD crc32;
    DWORD comp_size;
    DWORD uncomp_size;
    WORD  name_len;
    WORD  extra_len;
    WORD  comment_len;
    WORD  disk_start;
    WORD  int_attr;
    DWORD ext_attr;
    DWORD local_offset;
} CenEntry;

typedef struct {
    DWORD sig;
    WORD  ver_needed;
    WORD  flags;
    WORD  method;
    WORD  mod_time;
    WORD  mod_date;
    DWORD crc32;
    DWORD comp_size;
    DWORD uncomp_size;
    WORD  name_len;
    WORD  extra_len;
} LocEntry;

typedef NTSTATUS (WINAPI *RtlDecompress_t)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, PULONG);

static RtlDecompress_t RtlDecompress = NULL;

static int load_decompressor(void)
{
    if (RtlDecompress) return 1;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return 0;
    RtlDecompress = (RtlDecompress_t)GetProcAddress(ntdll, "RtlDecompressBuffer");
    return RtlDecompress != NULL;
}

static int write_file(const char *path, const BYTE *data, DWORD size)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD written;
    int ok = WriteFile(h, data, size, &written, NULL) && written == size;
    CloseHandle(h);
    return ok;
}

static int ensure_dirs(const char *full_path)
{
    char buf[MAX_PATH];
    strncpy(buf, full_path, sizeof(buf));
    for (char *p = buf + 3; *p; p++) {
        if (*p == '\\') {
            *p = 0;
            CreateDirectoryA(buf, NULL);
            *p = '\\';
        }
    }
    return 1;
}

static DWORD find_eocd(HANDLE hFile, DWORD filesize)
{
    DWORD pos = filesize < 65557 ? 0 : filesize - 65557;
    DWORD buf_size = filesize - pos;
    BYTE *buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, buf_size);
    if (!buf) return 0;

    OVERLAPPED ov = {0};
    ov.Offset = pos;
    DWORD read;
    if (!ReadFile(hFile, buf, buf_size, &read, NULL)) {
        HeapFree(GetProcessHeap(), 0, buf);
        return 0;
    }

    DWORD eocd_pos = 0;
    for (DWORD i = buf_size - 22; i > 0; i--) {
        if (*(DWORD*)(buf + i) == EOCD_SIG) {
            eocd_pos = pos + i;
            break;
        }
    }
    if (eocd_pos == 0 && buf_size >= 22) {
        if (*(DWORD*)(buf) == EOCD_SIG)
            eocd_pos = pos;
    }

    HeapFree(GetProcessHeap(), 0, buf);
    return eocd_pos;
}

static int extract_zip(const char *self_path, const char *dest_dir)
{
    HANDLE hFile = CreateFileA(self_path, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return 1;

    DWORD filesize = GetFileSize(hFile, NULL);
    if (filesize == INVALID_FILE_SIZE) { CloseHandle(hFile); return 1; }

    DWORD eocd_off = find_eocd(hFile, filesize);
    if (eocd_off == 0) { CloseHandle(hFile); return 1; }

    EocdRec eocd;
    OVERLAPPED ov = {0};
    ov.Offset = eocd_off;
    DWORD read;
    if (!ReadFile(hFile, &eocd, sizeof(eocd), &read, NULL)) {
        CloseHandle(hFile); return 1;
    }

    DWORD cd_pos = eocd.cd_offset;
    WORD total_entries = eocd.cd_entries_total;

    load_decompressor();

    for (WORD i = 0; i < total_entries; i++) {
        CenEntry cen;
        memset(&ov, 0, sizeof(ov));
        ov.Offset = cd_pos;
        if (!ReadFile(hFile, &cen, sizeof(cen), &read, NULL)) break;
        if (cen.sig != CEN_SIG) break;

        char *name = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cen.name_len + 1);
        if (!name) { CloseHandle(hFile); return 1; }
        memset(&ov, 0, sizeof(ov));
        ov.Offset = cd_pos + sizeof(cen);
        ReadFile(hFile, name, cen.name_len, &read, NULL);
        name[cen.name_len] = 0;

        int is_dir = name[cen.name_len - 1] == '/';

        if (is_dir) {
            char dir_path[MAX_PATH];
            snprintf(dir_path, sizeof(dir_path), "%s\\%s", dest_dir, name);
            for (char *p = dir_path + strlen(dest_dir) + 1; *p; p++)
                if (*p == '/') *p = '\\';
            CreateDirectoryA(dir_path, NULL);
        } else {
            char full_path[MAX_PATH];
            snprintf(full_path, sizeof(full_path), "%s\\%s", dest_dir, name);
            for (char *p = full_path + strlen(dest_dir) + 1; *p; p++)
                if (*p == '/') *p = '\\';

            ensure_dirs(full_path);

            LocEntry loc;
            memset(&ov, 0, sizeof(ov));
            ov.Offset = cen.local_offset;
            ReadFile(hFile, &loc, sizeof(loc), &read, NULL);

            DWORD data_off = cen.local_offset + sizeof(loc) + loc.name_len + loc.extra_len;
            BYTE *comp = (BYTE*)HeapAlloc(GetProcessHeap(), 0, cen.comp_size);
            if (!comp) { HeapFree(GetProcessHeap(), 0, name); break; }
            memset(&ov, 0, sizeof(ov));
            ov.Offset = data_off;
            ReadFile(hFile, comp, cen.comp_size, &read, NULL);

            if (cen.method == 0) {
                write_file(full_path, comp, cen.uncomp_size);
            } else if (cen.method == 8 && RtlDecompress) {
                BYTE *uncomp = (BYTE*)HeapAlloc(GetProcessHeap(), 0, cen.uncomp_size);
                if (uncomp) {
                    ULONG final_size = 0;
                    NTSTATUS status = RtlDecompress(COMPRESSION_FORMAT_DEFLATE,
                        uncomp, cen.uncomp_size, comp, cen.comp_size, &final_size);
                    if (status == 0)
                        write_file(full_path, uncomp, final_size);
                    HeapFree(GetProcessHeap(), 0, uncomp);
                }
            }

            HeapFree(GetProcessHeap(), 0, comp);
        }

        cd_pos += sizeof(cen) + cen.name_len + cen.extra_len + cen.comment_len;
        HeapFree(GetProcessHeap(), 0, name);
    }

    CloseHandle(hFile);
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, MAX_PATH);

    char dest[MAX_PATH];
    strncpy(dest, self, sizeof(dest));
    char *dot = strrchr(dest, '.');
    if (dot) *dot = 0;

    CreateDirectoryA(dest, NULL);

    int ret = extract_zip(self, dest);
    if (ret == 0) {
        ShellExecuteA(NULL, "open", dest, NULL, NULL, nShow);
    }

    return ret;
}
