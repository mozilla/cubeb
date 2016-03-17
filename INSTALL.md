# Build instructions for libcubeb

Note Also:Cubeb does not currently build under Cygwin, but this is being worked on.

0. Change directory into the source directory.
1. Run |autoreconf --install| to generate configure.
2. Run |./configure| to configure the build.
3. Run |make| to build.
4. Run |make check| to run the test suite.

# Debugging

Debugging tests can be done like so:

```libtool --mode=execute gdb test/test_tone```

# Windows build prerequisite, using `msys2`

Cubeb for Windows uses win32 threads

- Download and install `msys2` 32-bits from <https://msys2.github.io>. Let it
  install in `C:\msys32`.
- Download and install `7z` from <http://www.7-zip.org/>.
- Run `msys2` (a shortcut has been added to the start menu, or use the `.bat`
  script: `C:\msys32\mingw32_shell.bat`), and issue the following commands to
  install the dependencies:
```pacman -S git automake autoconf libtool m4 make pkg-config gdb
```
- Download a `mingw` compiler with the WIN32 thread model [here](http://sourceforge.net/projects/mingw-w64/files/Toolchains%20targetting%20Win32/Personal%20Builds/mingw-builds/4.9.2/threads-win32/sjlj/i686-4.9.2-release-win32-sjlj-rt_v3-rev0.7z/download). `msys2` does not have `mingw` builds with win32 threads,
so we have to install another compiler.
- Unzip the compiler, and copy all folders in `C:\msys32\opt`
- Exit the `msys2` shell, and run `C:\msys32\autorebase.bat` (double clicking
  works).
- Open an `msys2` shell and follow the build instructions.

