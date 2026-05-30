# libretrocore

**libretocore** is a libretro core.

## Features


## Building

To build **libretrocore**, you must have [CMake](https://cmake.org) and a build system of your choice.

1. Download the repository.
2. Run `cmake -B build -G "Ninja"` (CMake supports many different build systems; [Ninja](https://ninja-build.org) is just one of them).
3. Run `cmake --build build`.
4. When finished, you will find `libretrocore.dll` or `libretrocore.so` in the `bin` folder.

## Running

Run the RetroArch executable with the following command, specifying the path to the **libretrocore** library after building, along with the game file:

```
retroarch -L path/to/libretrocore.so path/to/game_file
```

For example, if you are on a GNU/Linux system and located in the repository root, you can run:

```
retroarch -L bin/libretrocore.so bin/snake.cart
```

## Installing

### On Linux

Copy the shared object file to the cores directory of RetroArch:

```
cp bin/libretrocore.so ~/.config/retroarch/cores/
```

Copy the info file into the same directory:

```
cp bin/libretrocore.info ~/.config/retroarch/info/
```

### On macOS

Copy the dynamic library to the cores directory:

```
cp bin/libretrocore.dylib ~/Library/Application\ Support/RetroArch/cores/
```

Copy the info file:

```
cp bin/libretrocore.info ~/Library/Application\ Support/RetroArch/info/
```

### On Windows

Copy the DLL file to the cores directory of RetroArch:

```
copy bin\libretrocore.dll "C:\Program Files\RetroArch\cores\"
```

Copy the info file:

```
copy bin\libretrocore.info "C:\Program Files\RetroArch\info\"
```

## Credits

Made in 20XX by Author.