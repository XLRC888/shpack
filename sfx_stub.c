#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#define EOCD_SIG 0x06054b50
#define CEN_SIG  0x02014b50
#define LOC_SIG  0x04034b50
#define COMPRESSION_FORMAT_DEFLATE 0x0002

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

static int has_dotdot(const char *path, WORD len)
{
    WORD dots = 0;
    for (WORD i = 0; i < len; i++) {
        if (path[i] == '.') { dots++; }
        else if (path[i] == '/' || path[i] == '\\') {
            if (dots == 2) return 1;
            dots = 0;
        }
        else { dots = 0; }
    }
    return dots == 2;
}

static int ensure_dirs(const char *full_path)
{
    char buf[MAX_PATH];
    buf[0] = 0;
    strncat(buf, full_path, sizeof(buf) - 1);
    char *p = buf;
    if (p[0] && p[1] == ':') p += 3;
    else if (p[0] == '\\' && p[1] == '\\') { p += 2; while (*p && *p != '\\') p++; if (*p) p++; }
    for (; *p; p++) {
        if (*p == '\\') {
            *p = 0;
            CreateDirectoryA(buf, NULL);
            *p = '\\';
        }
    }
    return 1;
}

static DWORD read_at(HANDLE hFile, DWORD offset, void *buf, DWORD size)
{
    OVERLAPPED ov = {0};
    ov.Offset = offset;
    DWORD read;
    if (!ReadFile(hFile, buf, size, &read, &ov)) return 0;
    return read;
}

static DWORD find_eocd(HANDLE hFile, DWORD filesize)
{
    DWORD search_start = filesize < 65557 ? 0 : filesize - 65557;
    DWORD buf_size = filesize - search_start;
    if (buf_size < 22) return 0;

    BYTE *buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, buf_size);
    if (!buf) return 0;

    if (read_at(hFile, search_start, buf, buf_size) != buf_size) {
        HeapFree(GetProcessHeap(), 0, buf);
        return 0;
    }

    DWORD eocd_pos = 0;
    for (DWORD i = buf_size - 22; i > 0; i--) {
        if (*(DWORD*)(buf + i) == EOCD_SIG) {
            WORD comment_len = *(WORD*)(buf + i + 20);
            if (search_start + i + 22 + comment_len == filesize) {
                eocd_pos = search_start + i;
                break;
            }
        }
    }
    if (eocd_pos == 0 && buf_size >= 22) {
        if (*(DWORD*)(buf) == EOCD_SIG) {
            WORD comment_len = *(WORD*)(buf + 20);
            if (search_start + 22 + comment_len == filesize)
                eocd_pos = search_start;
        }
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
    if (read_at(hFile, eocd_off, &eocd, sizeof(eocd)) != sizeof(eocd)) {
        CloseHandle(hFile); return 1;
    }
    if (eocd.sig != EOCD_SIG) { CloseHandle(hFile); return 1; }

    DWORD zip_start = eocd_off - eocd.cd_size - eocd.cd_offset;
    DWORD cd_abs = zip_start + eocd.cd_offset;
    WORD total_entries = eocd.cd_entries_total;

    load_decompressor();

    for (WORD i = 0; i < total_entries; i++) {
        CenEntry cen;
        if (read_at(hFile, cd_abs, &cen, sizeof(cen)) != sizeof(cen)) break;
        if (cen.sig != CEN_SIG) break;

        if (cen.name_len == 0) { cd_abs += sizeof(cen) + cen.extra_len + cen.comment_len; continue; }

        char *name = (char*)HeapAlloc(GetProcessHeap(), 0, cen.name_len + 1);
        if (!name) { CloseHandle(hFile); return 1; }
        if (read_at(hFile, cd_abs + sizeof(cen), name, cen.name_len) != cen.name_len) {
            HeapFree(GetProcessHeap(), 0, name); break;
        }
        name[cen.name_len] = 0;

        if (has_dotdot(name, cen.name_len)) {
            cd_abs += sizeof(cen) + cen.name_len + cen.extra_len + cen.comment_len;
            HeapFree(GetProcessHeap(), 0, name);
            continue;
        }

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

            DWORD loc_abs = zip_start + cen.local_offset;
            LocEntry loc;
            if (read_at(hFile, loc_abs, &loc, sizeof(loc)) == sizeof(loc) && loc.sig == LOC_SIG) {
                DWORD data_off = loc_abs + sizeof(loc) + loc.name_len + loc.extra_len;
                if (cen.comp_size > 0) {
                    BYTE *comp = (BYTE*)HeapAlloc(GetProcessHeap(), 0, cen.comp_size);
                    if (comp) {
                        if (read_at(hFile, data_off, comp, cen.comp_size) == cen.comp_size) {
                            if (cen.method == 0) {
                                write_file(full_path, comp, cen.uncomp_size);
                            } else if (cen.method == 8 && RtlDecompress && cen.uncomp_size > 0) {
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
                        }
                        HeapFree(GetProcessHeap(), 0, comp);
                    }
                }
            }
        }

        cd_abs += sizeof(cen) + cen.name_len + cen.extra_len + cen.comment_len;
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
    dest[0] = 0;
    strncat(dest, self, sizeof(dest) - 1);
    char *dot = strrchr(dest, '.');
    if (dot) *dot = 0;

    CreateDirectoryA(dest, NULL);

    int ret = extract_zip(self, dest);
    if (ret == 0) {
        ShellExecuteA(NULL, "open", dest, NULL, NULL, nShow);
    }

    return ret;
}
