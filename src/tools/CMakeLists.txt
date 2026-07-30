add_executable(itch_count itch_count.cpp)
target_link_libraries(itch_count PRIVATE ob::lib ob::warnings)

add_executable(replay replay.cpp)
add_executable(bench_replay bench_replay.cpp)
target_link_libraries(replay PRIVATE ob::lib ob::warnings)
find_package(Threads REQUIRED)
target_link_libraries(replay PRIVATE Threads::Threads)
target_link_libraries(bench_replay PRIVATE ob::lib ob::warnings Threads::Threads)

add_executable(itch_slice itch_slice.cpp)
target_link_libraries(itch_slice PRIVATE ob::lib ob::warnings)

add_executable(differential differential.cpp)
target_link_libraries(differential PRIVATE ob::lib ob::warnings)
