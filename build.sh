set -e

DATOVIZ_FOLDER="../datoviz"
GLSLC_PATH=$DATOVIZ_FOLDER/bin/vulkan/linux/glslc
SHADER_DIR="shaders/"

# Compile GLSL shaders to SPIR-V.
for shader in "$SHADER_DIR"*.vert "$SHADER_DIR"*.frag; do
    [ -e "$shader" ] || continue

    output="${shader}.spv"

    # Only compile if source is newer than the output
    if [ ! -e "$output" ] || [ "$shader" -nt "$output" ]; then
        echo "Compiling $shader -> $output"
        "$GLSLC_PATH" "$shader" -o "$output"

        if [ $? -ne 0 ]; then
            echo "Error compiling $shader"
        fi
    # else
    #     echo "Skipping $shader (up to date)"
    fi
done

# Compile datostim.c
gcc -I$DATOVIZ_FOLDER/include \
    -I$DATOVIZ_FOLDER/build/_deps/cglm-src/include/ \
    -L$DATOVIZ_FOLDER/build \
    datostim.c -o datostim \
    -lm -ldatoviz \
    -Wl,-rpath,$DATOVIZ_FOLDER/build
