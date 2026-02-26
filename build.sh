#!/bin/bash
set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
RIVE="$PROJECT_DIR/deps/rive-runtime"
BUILD_DIR="$PROJECT_DIR/build"
OUT_LIB="$PROJECT_DIR/rive_love.dylib"

# ── Step 0: Check rive-runtime exists ──────────────────────────────
if [ ! -d "$RIVE" ]; then
    echo "ERROR: deps/rive-runtime not found."
    echo "Clone it:  git clone https://github.com/rive-app/rive-runtime deps/rive-runtime"
    exit 1
fi

# ── Step 1: Build rive core + tess via the tess premake ────────────
# The tess/premake5_tess.lua includes the core premake5_v2.lua and
# builds both rive + rive_tess_renderer. We symlink premake5.lua so
# build_rive.sh can find it.
echo "=== Building rive core + tess libraries ==="
export PATH="$RIVE/build:$PATH"

# Create symlink if not present
if [ ! -e "$RIVE/tess/premake5.lua" ]; then
    ln -s premake5_tess.lua "$RIVE/tess/premake5.lua"
fi

pushd "$RIVE/tess"
# Override RIVE_PREMAKE_ARGS to avoid unknown flags in this premake config.
RIVE_PREMAKE_ARGS="" bash ../build/build_rive.sh release
popd

# ── Step 2: Locate built artifacts ─────────────────────────────────
echo "=== Locating libraries ==="
TESS_OUT="$RIVE/tess/out/release"

# Find the static libraries — all built into tess/out/release/
RIVE_LIB_A=$(find "$TESS_OUT" -name "librive.a" 2>/dev/null | head -1)
TESS_LIB_A=$(find "$TESS_OUT" -name "librive_tess_renderer.a" 2>/dev/null | head -1)
HARFBUZZ_LIB_A=$(find "$TESS_OUT" -name "librive_harfbuzz.a" 2>/dev/null | head -1)
SHEENBIDI_LIB_A=$(find "$TESS_OUT" -name "librive_sheenbidi.a" 2>/dev/null | head -1)
YOGA_LIB_A=$(find "$TESS_OUT" -name "librive_yoga.a" 2>/dev/null | head -1)

# Tess dependencies (fetched by premake into tess/dependencies/)
for DEPS_ROOT in "$RIVE/tess/dependencies" "$RIVE/tess/build/dependencies" "$RIVE/build/dependencies"; do
    if [ -z "$EARCUT_DIR" ]; then
        EARCUT_DIR=$(find "$DEPS_ROOT" -maxdepth 1 -name "*earcut*" -type d 2>/dev/null | head -1)
    fi
    if [ -z "$LIBTESS2_DIR" ]; then
        LIBTESS2_DIR=$(find "$DEPS_ROOT" -maxdepth 1 -name "*libtess2*" -type d 2>/dev/null | head -1)
    fi
done

# Verify key files
for lib in "$TESS_LIB_A" "$RIVE_LIB_A"; do
    if [ -z "$lib" ] || [ ! -f "$lib" ]; then
        echo "ERROR: Expected static library not found — check build output above."
        echo "  Searched in: $TESS_OUT"
        ls -la "$TESS_OUT" 2>/dev/null || echo "  (directory does not exist)"
        exit 1
    fi
done

echo "  Core:  $RIVE_LIB_A"
echo "  Tess:  $TESS_LIB_A"
echo "  Earcut: $EARCUT_DIR"

# ── Step 4: Compile our bridge into a dylib ────────────────────────
echo "=== Compiling rive_love.cpp ==="
mkdir -p "$BUILD_DIR"

INCLUDES=(
    -I"$PROJECT_DIR/include"
    -I"$RIVE/include"
    -I"$RIVE/tess/include"
)
# earcut.hpp is needed by tess_render_path.hpp
if [ -n "$EARCUT_DIR" ]; then
    INCLUDES+=(-I"$EARCUT_DIR/include/mapbox")
fi
if [ -n "$LIBTESS2_DIR" ]; then
    INCLUDES+=(-I"$LIBTESS2_DIR/Include")
fi

CXXFLAGS=(
    -std=c++17
    -O2
    -fPIC
    -DNDEBUG
    -fvisibility=hidden
    # Rive uses its own lightweight RTTI, not C++ RTTI:
    -fno-rtti
    -fno-exceptions
)

LDFLAGS=(
    -dynamiclib
    -install_name "@rpath/rive_love.dylib"
    -Wl,-rpath,@loader_path
)

# Static libs to link (dependents before dependencies)
STATIC_LIBS=("$TESS_LIB_A" "$RIVE_LIB_A")
[ -n "$HARFBUZZ_LIB_A" ] && STATIC_LIBS+=("$HARFBUZZ_LIB_A")
[ -n "$SHEENBIDI_LIB_A" ] && STATIC_LIBS+=("$SHEENBIDI_LIB_A")
[ -n "$YOGA_LIB_A" ] && STATIC_LIBS+=("$YOGA_LIB_A")

# Build for the host architecture
ARCH=$(uname -m)  # arm64 or x86_64
echo "  Building for $ARCH..."

clang++ -arch $ARCH \
    "${CXXFLAGS[@]}" "${INCLUDES[@]}" \
    -c "$PROJECT_DIR/src/rive_love.cpp" \
    -o "$BUILD_DIR/rive_love.o"

clang++ -arch $ARCH \
    "${LDFLAGS[@]}" \
    "$BUILD_DIR/rive_love.o" \
    "${STATIC_LIBS[@]}" \
    -lc++ \
    -framework CoreText \
    -framework CoreGraphics \
    -framework CoreFoundation \
    -o "$OUT_LIB"

# ── Step 5: Code sign (required on macOS 14+) ─────────────────────
echo "=== Code signing ==="
codesign --force --sign - "$OUT_LIB"

# ── Step 6: Verify ─────────────────────────────────────────────────
echo "=== Verification ==="
file "$OUT_LIB"
otool -L "$OUT_LIB"
echo ""
echo "SUCCESS: $OUT_LIB ready."
echo "Copy it alongside main.lua and rive_love.lua to use."
