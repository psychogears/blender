# turn everything OFF except for python which defaults to ON
# and is needed for the UI
#
# Example usage:
#   cmake -C../blender/build_files/cmake/config/blender_lite.cmake  ../blender
#

set(WITH_INSTALL_PORTABLE    ON  CACHE BOOL "" FORCE)
set(WITH_SYSTEM_GLEW         ON  CACHE BOOL "" FORCE)

set(WITH_ALEMBIC             OFF CACHE BOOL "" FORCE)
set(WITH_BOOST               OFF CACHE BOOL "" FORCE)
set(WITH_BUILDINFO           OFF CACHE BOOL "" FORCE)
set(WITH_BULLET              OFF CACHE BOOL "" FORCE)
set(WITH_CODEC_AVI           OFF CACHE BOOL "" FORCE)
set(WITH_CODEC_FFMPEG        OFF CACHE BOOL "" FORCE)
set(WITH_CODEC_SNDFILE       OFF CACHE BOOL "" FORCE)
set(WITH_CYCLES              OFF CACHE BOOL "" FORCE)
set(WITH_CYCLES_OSL          OFF CACHE BOOL "" FORCE)
set(WITH_DRACO               OFF CACHE BOOL "" FORCE)
set(WITH_FFTW3               OFF CACHE BOOL "" FORCE)
set(WITH_LIBMV               OFF CACHE BOOL "" FORCE)
set(WITH_LLVM                OFF CACHE BOOL "" FORCE)
set(WITH_COMPOSITOR          OFF CACHE BOOL "" FORCE)
set(WITH_FREESTYLE           OFF CACHE BOOL "" FORCE)
set(WITH_GHOST_XDND          OFF CACHE BOOL "" FORCE)
set(WITH_IK_SOLVER           OFF CACHE BOOL "" FORCE)
set(WITH_IK_ITASC            OFF CACHE BOOL "" FORCE)
set(WITH_IMAGE_CINEON        OFF CACHE BOOL "" FORCE)
set(WITH_IMAGE_DDS           OFF CACHE BOOL "" FORCE)
set(WITH_IMAGE_HDR           OFF CACHE BOOL "" FORCE)
set(WITH_IMAGE_OPENEXR       OFF CACHE BOOL "" FORCE)
set(WITH_IMAGE_OPENJPEG      OFF CACHE BOOL "" FORCE)
set(WITH_IMAGE_TIFF          OFF CACHE BOOL "" FORCE)
set(WITH_INPUT_NDOF          OFF CACHE BOOL "" FORCE)
set(WITH_INTERNATIONAL       OFF CACHE BOOL "" FORCE)
set(WITH_JACK                OFF CACHE BOOL "" FORCE)
set(WITH_LZMA                OFF CACHE BOOL "" FORCE)
set(WITH_LZO                 OFF CACHE BOOL "" FORCE)
set(WITH_MOD_REMESH          OFF CACHE BOOL "" FORCE)
set(WITH_MOD_MANTA           OFF CACHE BOOL "" FORCE)
set(WITH_MOD_OCEANSIM        OFF CACHE BOOL "" FORCE)
set(WITH_AUDASPACE           OFF CACHE BOOL "" FORCE)
set(WITH_OPENAL              OFF CACHE BOOL "" FORCE)
set(WITH_OPENCOLLADA         OFF CACHE BOOL "" FORCE)
set(WITH_OPENCOLORIO         OFF CACHE BOOL "" FORCE)
set(WITH_OPENIMAGEIO         OFF CACHE BOOL "" FORCE)
set(WITH_OPENMP              OFF CACHE BOOL "" FORCE)
set(WITH_OPENSUBDIV          OFF CACHE BOOL "" FORCE)
set(WITH_OPENVDB             OFF CACHE BOOL "" FORCE)
set(WITH_RAYOPTIMIZATION     OFF CACHE BOOL "" FORCE)
set(WITH_SDL                 OFF CACHE BOOL "" FORCE)
set(WITH_X11_XINPUT          OFF CACHE BOOL "" FORCE)
set(WITH_X11_XF86VMODE       OFF CACHE BOOL "" FORCE)
