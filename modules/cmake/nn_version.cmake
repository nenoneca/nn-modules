# SPDX-License-Identifier: Apache-2.0
#
# nn_version.cmake — Auto-generate APPLICATION VERSION from git commit count.
#
# Include this BEFORE find_package(Zephyr) in your app CMakeLists.txt:
#
#   include(${CMAKE_CURRENT_SOURCE_DIR}/../../modules/cmake/nn_version.cmake)
#   find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
#
# Version mapping (each field 0-255):
#   commit_count  →  MAJOR.MINOR.PATCH+TWEAK
#   TWEAK  = count % 256
#   PATCH  = (count / 256)       % 256
#   MINOR  = (count / 65536)     % 256
#   MAJOR  = (count / 16777216)  % 256
#
# EXTRAVERSION = <git-short-hash>            (clean tree)
#              = <git-short-hash>-d.<dirty>  (uncommitted changes)
#
# The generated VERSION file is written into the app source directory
# so Zephyr's version.cmake picks it up automatically.  Add VERSION
# to the app's .gitignore.
#
# Outputs (after Zephyr processes VERSION):
#   APP_VERSION_MAJOR / APP_VERSION_MINOR / APP_PATCHLEVEL / APP_VERSION_TWEAK
#   APPVERSION (packed hex), APP_VERSION_STRING, APP_VERSION_TWEAK_STRING
#   CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION (auto, from APP_VERSION_TWEAK_STRING)

if(DEFINED NN_VERSION_DONE)
  return()
endif()
set(NN_VERSION_DONE TRUE)

# ── Locate app git root ──────────────────────────────────────────────────────

set(_nn_src "${CMAKE_CURRENT_SOURCE_DIR}")

execute_process(
  COMMAND git rev-parse --show-toplevel
  WORKING_DIRECTORY "${_nn_src}"
  OUTPUT_VARIABLE _nn_git_root
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
  RESULT_VARIABLE _nn_git_rc
)

# ── Commit count → version fields ────────────────────────────────────────────

if(_nn_git_rc EQUAL 0)
  execute_process(
    COMMAND git rev-list --count HEAD
    WORKING_DIRECTORY "${_nn_git_root}"
    OUTPUT_VARIABLE _nn_count
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _nn_rc
  )
  if(NOT _nn_rc EQUAL 0)
    set(_nn_count 0)
  endif()
else()
  set(_nn_count 0)
  set(_nn_git_root "${_nn_src}")
endif()

math(EXPR _nn_tweak  "${_nn_count} % 256")
math(EXPR _nn_patch  "(${_nn_count} / 256)      % 256")
math(EXPR _nn_minor  "(${_nn_count} / 65536)    % 256")
math(EXPR _nn_major  "(${_nn_count} / 16777216) % 256")

# ── Extra version string ─────────────────────────────────────────────────────

if(_nn_git_rc EQUAL 0)
  # Short commit hash
  execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${_nn_git_root}"
    OUTPUT_VARIABLE _nn_hash
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  # Check for uncommitted changes
  execute_process(
    COMMAND git status --porcelain
    WORKING_DIRECTORY "${_nn_git_root}"
    OUTPUT_VARIABLE _nn_status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  if(_nn_status)
    # Hash of uncommitted file contents (deterministic dirty marker)
    execute_process(
      COMMAND bash -c "git status --porcelain | awk '{print $2}' | xargs -I{} md5sum {} 2>/dev/null | md5sum | awk '{print substr($1,1,8)}'"
      WORKING_DIRECTORY "${_nn_git_root}"
      OUTPUT_VARIABLE _nn_dirty
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    # EXTRAVERSION format: <hash>-d.<dirty8>
    # Uses only [a-z0-9.\-] to satisfy Zephyr's regex constraint
    set(_nn_extra "${_nn_hash}-d.${_nn_dirty}")
  else()
    set(_nn_extra "${_nn_hash}")
  endif()
else()
  set(_nn_extra "nogit")
endif()

# ── Write VERSION file ────────────────────────────────────────────────────────

set(_nn_version_file "${_nn_src}/VERSION")

# If NN_APP_VERSION is set (env or cmake var), use it instead of
# git-derived version.  Format: MAJOR.MINOR.PATCH
if(NOT DEFINED NN_APP_VERSION AND DEFINED ENV{NN_APP_VERSION})
  set(NN_APP_VERSION "$ENV{NN_APP_VERSION}")
endif()
if(DEFINED NN_APP_VERSION)
  string(REPLACE "." ";" _nn_ver_parts "${NN_APP_VERSION}")
  list(LENGTH _nn_ver_parts _nn_ver_len)
  list(GET _nn_ver_parts 0 _nn_major)
  if(_nn_ver_len GREATER 1)
    list(GET _nn_ver_parts 1 _nn_minor)
  endif()
  if(_nn_ver_len GREATER 2)
    list(GET _nn_ver_parts 2 _nn_patch)
  endif()
  set(_nn_tweak 0)
  set(_nn_extra "")
  set(_nn_count "${NN_APP_VERSION}")
endif()

file(WRITE "${_nn_version_file}"
  "VERSION_MAJOR = ${_nn_major}\n"
  "VERSION_MINOR = ${_nn_minor}\n"
  "PATCHLEVEL = ${_nn_patch}\n"
  "VERSION_TWEAK = ${_nn_tweak}\n"
  "EXTRAVERSION = ${_nn_extra}\n"
)

message(STATUS "nn_version: ${_nn_major}.${_nn_minor}.${_nn_patch}+${_nn_tweak}-${_nn_extra} (${_nn_count} commits)")

# ── Cleanup ───────────────────────────────────────────────────────────────────

unset(_nn_src)
unset(_nn_git_root)
unset(_nn_git_rc)
unset(_nn_count)
unset(_nn_rc)
unset(_nn_major)
unset(_nn_minor)
unset(_nn_patch)
unset(_nn_tweak)
unset(_nn_hash)
unset(_nn_status)
unset(_nn_dirty)
unset(_nn_extra)
unset(_nn_version_file)
