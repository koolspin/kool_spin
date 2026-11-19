# Executables.
add_subdirectory(tone)
add_subdirectory(harness)
add_subdirectory(supervisor)
add_subdirectory(engine)
add_subdirectory(worker)
add_subdirectory(play)

# kool_spin refuses to start without ks-worker next to it
# (specs/track-loading/spec.md requirement 10), and ks-engine is its default
# engine: build them whenever kool_spin is built.
add_dependencies(kool_spin ks-engine ks-worker)
