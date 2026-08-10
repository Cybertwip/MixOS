#!/usr/bin/env python3
#
# Teach EmulationStation-fcamod's CMake about a third renderer.
#
# Upstream has exactly two, selected by one boolean: GLSystem is either "Desktop
# OpenGL" -> -DUSE_OPENGL_21 or "Embedded OpenGL" -> -DUSE_OPENGLES_10, and every
# site that cares tests `MATCHES "Desktop OpenGL"` and lets the else() mean GLES1.
# That shape is why this is a patch and not a -D flag: adding a value to GLSystem
# without touching those else() branches would compile the GLES 2.0 renderer and
# then link EGL + the GLES1 library anyway, which is the pair that cannot make a
# context on this stack.
#
# Five sites in the top-level CMakeLists.txt (option, GLSystem chain, find_package,
# add_definitions, include dirs, link libraries) and one line in es-core's source
# list.  Each replacement is an exact string match against the pinned commit and
# raises if it is not found -- a rebuild against a moved upstream must fail loudly
# here rather than quietly produce a GLES1 binary that aborts on the panel.
#
# Usage: patch-gles20.py <es-source-dir>

import sys
import os

ANCHORS = []


def replace(path, old, new, count=1):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    found = text.count(old)
    if found != count:
        raise SystemExit(
            "patch-gles20: expected %d occurrence(s) of the anchor below in %s, "
            "found %d.  Upstream moved; re-read it and update this patch.\n"
            "--- anchor ---\n%s" % (count, path, found, old)
        )
    # No separate "already patched?" test: every anchor is consumed by its own
    # replacement, so a second run fails the count check above.
    with open(path, "w", encoding="utf-8") as f:
        f.write(text.replace(old, new, count))
    ANCHORS.append("%s: %s" % (os.path.basename(path), old.strip().splitlines()[0]))


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch-gles20.py <es-source-dir>")
    src = sys.argv[1]
    top = os.path.join(src, "CMakeLists.txt")
    core = os.path.join(src, "es-core", "CMakeLists.txt")
    for p in (top, core):
        if not os.path.isfile(p):
            raise SystemExit("patch-gles20: %s is not there" % p)

    # 1. The option.  Declared alongside GLES/GL so that -DGLES20=ON is discoverable
    #    in the cache like the other two rather than being an undocumented define.
    replace(
        top,
        'option(GLES "Set to ON if targeting Embedded OpenGL" ${GLES})\n',
        'option(GLES20 "Set to ON to target OpenGL ES 2.0 (J36 Ultra: lima + Mesa)" ${GLES20})\n'
        'option(GLES "Set to ON if targeting Embedded OpenGL" ${GLES})\n',
    )

    # 2. The GLSystem chain.  First, so an explicit -DGLES20=ON wins over the
    #    libMali.so sniffing below it -- which on this card finds the RK3326 blob's
    #    symlink and would otherwise pick GLES1.
    replace(
        top,
        'if(GLES)\n'
        '    set(GLSystem "Embedded OpenGL" CACHE STRING "The OpenGL system to be used")\n'
        'elseif(GL)\n',
        'if(GLES20)\n'
        '    set(GLSystem "Embedded OpenGL 2.0" CACHE STRING "The OpenGL system to be used")\n'
        'elseif(GLES)\n'
        '    set(GLSystem "Embedded OpenGL" CACHE STRING "The OpenGL system to be used")\n'
        'elseif(GL)\n',
    )

    # endif(GLES) no longer names the condition of its if().  CMake ignores the
    # argument, but a stale one is a lie to the next reader.
    replace(top, 'endif(GLES)\n', 'endif()\n')

    replace(
        top,
        'set_property(CACHE GLSystem PROPERTY STRINGS "Desktop OpenGL" "Embedded OpenGL")\n',
        'set_property(CACHE GLSystem PROPERTY STRINGS "Desktop OpenGL" "Embedded OpenGL" "Embedded OpenGL 2.0")\n',
    )

    # 3. find_package.  Nothing to find: Renderer_GLES20.cpp resolves all 43 entry
    #    points through SDL_GL_GetProcAddress and includes SDL2's own
    #    SDL_opengles2.h, so there is no GL library to link and no GL header to
    #    locate.  That is deliberate and not a convenience: every libGLESv2.so /
    #    libEGL.so name in this rootfs is a symlink to a SONAME-less ARMv8-A
    #    libMali.so, so any -l against those names records a DT_NEEDED that is
    #    SIGILL on a Cortex-A7.
    replace(
        top,
        'if(${GLSystem} MATCHES "Desktop OpenGL")\n'
        '    find_package(OpenGL REQUIRED)\n'
        'else()\n'
        '    find_package(OpenGLES REQUIRED)\n'
        'endif()\n',
        'if(${GLSystem} MATCHES "Embedded OpenGL 2.0")\n'
        '    # No GL package: every entry point is resolved with SDL_GL_GetProcAddress.\n'
        'elseif(${GLSystem} MATCHES "Desktop OpenGL")\n'
        '    find_package(OpenGL REQUIRED)\n'
        'else()\n'
        '    find_package(OpenGLES REQUIRED)\n'
        'endif()\n',
    )

    # 4. The define that selects the renderer .cpp.  All three files are always in
    #    the source list; each is wrapped in its own #if.
    replace(
        top,
        'if(${GLSystem} MATCHES "Desktop OpenGL")\n'
        '    add_definitions(-DUSE_OPENGL_21)\n'
        'else()\n'
        '    add_definitions(-DUSE_OPENGLES_10)\n'
        'endif()\n',
        'if(${GLSystem} MATCHES "Embedded OpenGL 2.0")\n'
        '    add_definitions(-DUSE_OPENGLES_20)\n'
        'elseif(${GLSystem} MATCHES "Desktop OpenGL")\n'
        '    add_definitions(-DUSE_OPENGL_21)\n'
        'else()\n'
        '    add_definitions(-DUSE_OPENGLES_10)\n'
        'endif()\n',
    )

    # 5. Include dirs.  The MATCHES tests upstream only ever ask for "Desktop
    #    OpenGL", so "Embedded OpenGL 2.0" would fall into the GLES1 else() at each
    #    of the two remaining sites and pull in ${OPENGLES_INCLUDE_DIR} and
    #    ${OPENGLES_LIBRARIES} -- both empty here, since find_package(OpenGLES) was
    #    skipped, but `EGL' on the link line is not empty and is the whole problem.
    replace(
        top,
        '    if(${GLSystem} MATCHES "Desktop OpenGL")\n'
        '        LIST(APPEND COMMON_INCLUDE_DIRS\n'
        '            ${OPENGL_INCLUDE_DIR}\n'
        '        )\n'
        '    else()\n'
        '        LIST(APPEND COMMON_INCLUDE_DIRS\n'
        '            ${OPENGLES_INCLUDE_DIR}\n'
        '        )\n'
        '    endif()\n',
        '    if(${GLSystem} MATCHES "Embedded OpenGL 2.0")\n'
        '        # SDL2 ships SDL_opengles2.h and the gl2/khrplatform headers it needs.\n'
        '    elseif(${GLSystem} MATCHES "Desktop OpenGL")\n'
        '        LIST(APPEND COMMON_INCLUDE_DIRS\n'
        '            ${OPENGL_INCLUDE_DIR}\n'
        '        )\n'
        '    else()\n'
        '        LIST(APPEND COMMON_INCLUDE_DIRS\n'
        '            ${OPENGLES_INCLUDE_DIR}\n'
        '        )\n'
        '    endif()\n',
    )

    # 6. Link libraries.  This is the site that matters: the else() here is what
    #    puts `EGL' in ES's DT_NEEDED as the bare `libEGL.so'.
    replace(
        top,
        '    if(${GLSystem} MATCHES "Desktop OpenGL")\n'
        '        LIST(APPEND COMMON_LIBRARIES\n'
        '            ${OPENGL_LIBRARIES}\n'
        '        )\n'
        '    else()\n'
        '        LIST(APPEND COMMON_LIBRARIES\n'
        '            EGL\n'
        '            ${OPENGLES_LIBRARIES}\n'
        '        )\n'
        '    endif()\n',
        '    if(${GLSystem} MATCHES "Embedded OpenGL 2.0")\n'
        '        # No GL library on the link line, on purpose.  SDL2 already carries\n'
        '        # the EGL/GLES loader, and every unversioned GL name in this rootfs\n'
        '        # points at an ARMv8-A libMali.so.\n'
        '    elseif(${GLSystem} MATCHES "Desktop OpenGL")\n'
        '        LIST(APPEND COMMON_LIBRARIES\n'
        '            ${OPENGL_LIBRARIES}\n'
        '        )\n'
        '    else()\n'
        '        LIST(APPEND COMMON_LIBRARIES\n'
        '            EGL\n'
        '            ${OPENGLES_LIBRARIES}\n'
        '        )\n'
        '    endif()\n',
    )

    # 7. The renderer itself.
    replace(
        core,
        '\t${CMAKE_CURRENT_SOURCE_DIR}/src/renderers/Renderer_GLES10.cpp\n',
        '\t${CMAKE_CURRENT_SOURCE_DIR}/src/renderers/Renderer_GLES10.cpp\n'
        '\t${CMAKE_CURRENT_SOURCE_DIR}/src/renderers/Renderer_GLES20.cpp\n',
    )

    print("patch-gles20: applied %d anchors" % len(ANCHORS))
    for a in ANCHORS:
        print("  " + a)


if __name__ == "__main__":
    main()
