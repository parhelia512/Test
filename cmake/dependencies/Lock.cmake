# Immutable package inputs. Archive contents are checked even on a warm cache.
include_guard(GLOBAL)
get_filename_component(CLASHFLUX_DEPENDENCY_REPO "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
macro(clashflux_lock_archive name version archive sha256 topdir)
    set(CLASHFLUX_DEP_${name}_VERSION "${version}")
    set(CLASHFLUX_DEP_${name}_ARCHIVE "${CLASHFLUX_DEPENDENCY_REPO}/third_party/tarballs/${archive}")
    set(CLASHFLUX_DEP_${name}_SHA256 "${sha256}")
    set(CLASHFLUX_DEP_${name}_TOPDIR "${topdir}")
endmacro()
clashflux_lock_archive(glaze 9.0.0 glaze-9.0.0-headers.tar.gz
    872764412db2ba94cf57e39cf6518f86c0e13728c1c668f7ed398801aadff812 glaze-9.0.0)
clashflux_lock_archive(zstd 1.5.7 zstd-1.5.7.tar.gz
    37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3 zstd-1.5.7)
clashflux_lock_archive(yamlcpp 0.8.0 yaml-cpp-0.8.0.tar.gz
    fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16 yaml-cpp-0.8.0)
clashflux_lock_archive(huxerui 0.3.0 huxerui-sdk-0.3.0-linux-x86_64.tar.gz
    5413ccd9d35d0ad50c08c67045d73cb9faff23c04005a554af0faa7a914eb916 huxerui-sdk-0.3.0-linux-x86_64)
set(CLASHFLUX_DEP_JSON_VERSION 3.12.0)
set(CLASHFLUX_DEP_QRCODE_VERSION 1.8.0)
set(CLASHFLUX_DEPENDENCY_ARCHIVES glaze zstd yamlcpp)
set(CLASHFLUX_DEPENDENCY_FILES
    "third_party/json/nlohmann/json.hpp|aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63"
    "third_party/qrcodegen/qrcodegen.hpp|b779c3b156cf7a57ce789d6fee4fc991ccc2913774d26c909d22bb8f26b2a793"
    "third_party/qrcodegen/qrcodegen.cpp|1f3b3fcdac6954c32cf583ccd02ec9b5901f756a38c461acedc70be4a77d3757")

# Immutable HTTPS repository URLs and full commit SHAs.
set(CLASHFLUX_HUXERUI_REPO "https://github.com/FarnaHerry/HuxerUI.git")
set(CLASHFLUX_HUXERUI_COMMIT "144aff6be5f8fc8ba09eaaa7189a375322aa01af")
set(CLASHFLUX_HUXERUI_CAMERA_URL "https://github.com/FarnaHerry/Lib-Camera.git")
set(CLASHFLUX_HUXERUI_CAMERA_COMMIT "5d6b8e814e3e5d8c22e63a4e470c2b6c1cd82cd0")
set(CLASHFLUX_HUXERUI_SQLITE_URL "https://github.com/FarnaHerry/Lib-SQLite.git")
set(CLASHFLUX_HUXERUI_SQLITE_COMMIT "882665a9772cc973b1cbfb7f491913703da1d636")
set(CLASHFLUX_HUXERUI_CHARTS_URL "https://github.com/FarnaHerry/Lib-Charts.git")
set(CLASHFLUX_HUXERUI_CHARTS_COMMIT "a879e96c64499b18f355b88648e1698fb5a25f72")
set(CLASHFLUX_HUXERUI_CODEEDITOR_URL "https://github.com/FarnaHerry/Lib-CodeEditor.git")
set(CLASHFLUX_HUXERUI_CODEEDITOR_COMMIT "a332e7c3158818a891bb8181756d439a62df6268")
set(CLASHFLUX_HUXERUI_MEDIA_URL "https://github.com/HuxerUI/Lib-Media.git")
set(CLASHFLUX_HUXERUI_MEDIA_COMMIT "01341304bdce46f109cc16d61b717add6dedc101")

function(clashflux_dependency_source name)
    clashflux_extract(${name} "${CLASHFLUX_DEP_${name}_ARCHIVE}"
        "${CLASHFLUX_DEP_${name}_SHA256}" "${CLASHFLUX_DEP_${name}_TOPDIR}")
    set(${name}_SOURCE_DIR "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

function(clashflux_verify_dependency_inputs)
    foreach(name IN LISTS CLASHFLUX_DEPENDENCY_ARCHIVES)
        file(SHA256 "${CLASHFLUX_DEP_${name}_ARCHIVE}" actual)
        if(NOT actual STREQUAL CLASHFLUX_DEP_${name}_SHA256)
            message(FATAL_ERROR "Dependency archive SHA256 mismatch: ${name}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CLASHFLUX_DEP_${name}_ARCHIVE}")
    endforeach()
    foreach(record IN LISTS CLASHFLUX_DEPENDENCY_FILES)
        string(REPLACE "|" ";" fields "${record}")
        list(GET fields 0 path)
        list(GET fields 1 expected)
        file(SHA256 "${CLASHFLUX_DEPENDENCY_REPO}/${path}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "Dependency file SHA256 mismatch: ${path}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CLASHFLUX_DEPENDENCY_REPO}/${path}")
    endforeach()
endfunction()
