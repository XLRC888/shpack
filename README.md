# shpack

folders as files. share a folder without compressing, extracting, or explaining anything to your grandma.

## the idea

`shpack pack myfolder` → `myfolder.shp` (a zip with metadata)
`shpack unpack myfolder.shp` → `myfolder/` (back to a folder)
`shpack mount myfolder.shp` → appears in your file manager as a folder (zero extraction)

on windows, `shpack pack --sfx myfolder` gives you `myfolder.shp.exe` — double-click it and it extracts itself. no software needed.

## what you need

**pack/unpack**: python 3.10+
**mount**: `pip install fusepy` (linux only, for FUSE)
**sfx builder**: `mingw-w64-gcc` (only if you want to rebuild the stub)

## quick start

```
git clone https://github.com/XLRC888/shpack.git
cd shpack
./shpack pack somefolder
```

or install it:

```
ln -s $PWD/shpack ~/.local/bin/shpack
```

## usage

**pack a folder**
```
shpack pack myfolder          → myfolder.shp
shpack pack myfolder -o out   → out.shp
shpack pack --sfx myfolder    → myfolder.shp.exe (self-extracting)
```

**extract**
```
shpack unpack myfolder.shp              → extracts to ./myfolder/
shpack unpack myfolder.shp -o somewhere
shpack unpack myfolder.shp --no-dir     → flat (no wrapper folder)
```

**mount (linux, needs fusepy)**
```
shpack mount myfolder.shp           → appears at ./myfolder/
shpack mount myfolder.shp /mnt/somewhere
shpack mount myfolder.shp --background
```

open `myfolder/` in your file manager. read files. copy stuff. hit ctrl+c when done.

**unmount**
```
shpack umount /mnt/somewhere
shpack umount myfolder.shp          → finds it if you used default naming
```

**browse without mounting**
```
shpack list myfolder.shp
shpack info myfolder.shp
```

## flags

`-f` or `--force` — overwrite without asking
`--sfx` — create a self-extracting `.shp.exe` for windows
`--stub <path>` — use a custom sfx stub
`--background` — mount in background (don't block the terminal)
`--no-dir` — unpack without wrapping in a folder

## the format

`.shp` files are standard zip archives with a `.shpmeta` metadata file inside. rename to `.zip` and any archive tool can open them.

`.shp.exe` files are the same zip glued to a 20kb windows executable stub. double-click extracts everything. on linux, `file` sees it as "Zip archive with extra data prepended" — still works with `shpack unpack`, `unzip`, and `shpack mount`.

## how it works

**pack**: walks the folder, writes a zip with deflate compression, embeds metadata as `.shpmeta`.

**mount**: runs a FUSE filesystem backed by the zip. reads happen on-demand, nothing gets extracted to disk.

**sfx stub** (`sfx_stub.c`): a 200-line windows program that finds the zip data at the end of itself and extracts it using `RtlDecompressBuffer` (part of windows since xp). compiles with mingw.

## project layout

```
shpack          — the cli (python)
sfx_stub.c      — windows sfx stub source
sfx_stub.exe    — pre-compiled (20kb pe64)
pyproject.toml  — for pip install
```

## why not just tar/zip?

because `.shp` is a file that acts like a folder. mount it in nautilus and it just looks like a directory. share it with a windows user and they double-click to extract. it's the same format underneath, but the experience is different.
