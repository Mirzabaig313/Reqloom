# ipc/CMakeLists.txt
#
# Phase B scaffold (PRD §8.6 / Project Layout §6). Disabled by default
# (REQLOOM_BUILD_IPC=OFF). When the trigger to extract the engine fires,
# this directory will host the JSON-RPC-over-stdio façade that wraps
# reqloom::engine and is consumed by an IpcEngineClient on the desktop
# side.
#
# Intentionally empty in MVP. Adding sources here without flipping
# REQLOOM_BUILD_IPC=ON does nothing.

message(STATUS "[Reqloom] IPC scaffold present but no targets defined yet.")
