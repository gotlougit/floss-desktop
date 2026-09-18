{ pkgs }:
let
  fetch = name: repo: rev: subdir: hash: pkgs.fetchzip {
    inherit name hash;
    url = "https://android.googlesource.com/platform/${repo}/+archive/${rev}${subdir}.tar.gz";
    extension = "tar.gz";
    stripRoot = false;
  };
  sysprop = fetch "android-sysprop" "system/tools/sysprop" "aa6e5ff2053d88a326eebc1e1a1bcdc217633c5a" "" "sha256-bjYJclFrP/D7tQBWhxbidsPvH6CKY9Ssi1i1TW0T0x0=";
  libbase = fetch "android-libbase" "system/libbase" "6d19b5c690fa4220c10e42c2326150bbd4d4b7bb" "" "sha256-GDDGHGsnyfZu5NHnNQ4itGMGZYdrucoESAm/9vgttUA=";
  liblog = fetch "android-liblog" "system/logging" "bcac7c30d88a3773a7c0bc9f5617a23a886331fd" "/liblog" "sha256-ZIRsQr2hihnAQjLmh0nbJS46begmp44DMCtlZRWX4jU=";
in pkgs.llvmPackages.stdenv.mkDerivation {
  pname = "floss-sysprop-cpp";
  version = "0-unstable-2025-03-05";
  src = sysprop;
  nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.protobuf ];
  buildInputs = [ pkgs.protobuf pkgs.fmt ];
  postUnpack = ''
    cp -r ${libbase} "$sourceRoot/libbase"
    cp -r ${liblog} "$sourceRoot/liblog"
    chmod -R u+w "$sourceRoot"
  '';
  postPatch = ''
    # Android UID definitions are only used by the device-specific branch.
    substituteInPlace liblog/logger_write.cpp \
      --replace-fail '#include <private/android_filesystem_config.h>' \
      $'#ifdef __ANDROID__\n#include <private/android_filesystem_config.h>\n#endif'
    cat > CMakeLists.txt <<'CMAKE'
    cmake_minimum_required(VERSION 3.16)
    project(sysprop_cpp LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
    pkg_check_modules(FMT REQUIRED IMPORTED_TARGET fmt)
    add_custom_command(OUTPUT sysprop.pb.cc sysprop.pb.h
      COMMAND protoc --cpp_out=''${CMAKE_CURRENT_BINARY_DIR} -I ''${CMAKE_CURRENT_SOURCE_DIR} ''${CMAKE_CURRENT_SOURCE_DIR}/sysprop.proto
      DEPENDS sysprop.proto)
    add_executable(sysprop_cpp Common.cpp CodeWriter.cpp CppGen.cpp CppMain.cpp sysprop.pb.cc
      libbase/file.cpp libbase/logging.cpp libbase/stringprintf.cpp libbase/strings.cpp
      libbase/result.cpp libbase/errors_unix.cpp libbase/posix_strerror_r.cpp libbase/threads.cpp
      liblog/logger_write.cpp liblog/logger_name.cpp liblog/properties.cpp)
    target_include_directories(sysprop_cpp PRIVATE include libbase/include liblog/include ''${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(sysprop_cpp PRIVATE LIBLOG_LOG_TAG=1006 SNET_EVENT_LOG_TAG=1397638484 ANDROID_DEBUGGABLE=0)
    target_link_libraries(sysprop_cpp PRIVATE PkgConfig::PROTOBUF PkgConfig::FMT pthread)
    install(TARGETS sysprop_cpp DESTINATION bin)
    CMAKE
  '';
  doCheck = false;
  doInstallCheck = false;
  meta = { description = "Android system property C++ generator for Floss"; license = pkgs.lib.licenses.asl20; platforms = [ "x86_64-linux" ]; };
}
