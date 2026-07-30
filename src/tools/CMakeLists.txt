add_executable(itch_count itch_count.cpp)
target_link_libraries(itch_count PRIVATE ob::lib ob::warnings)

add_executable(replay replay.cpp)
target_link_libraries(replay PRIVATE ob::lib ob::warnings)
find_package(Threads REQUIRED)
target_link_libraries(replay PRIVATE Threads::Threads)
