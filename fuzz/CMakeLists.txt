# OB_LIBFUZZER=ON links the real coverage-guided runtime (-fsanitize=fuzzer;
# Linux clang / brew llvm). OFF builds the standalone random driver instead —
# same target code, still ASan/UBSan-instrumented via the asan preset.
option(OB_LIBFUZZER "Link the libFuzzer runtime" OFF)

foreach(tgt fuzz_parser fuzz_book)
  if(OB_LIBFUZZER)
    add_executable(${tgt} ${tgt}.cpp)
    target_compile_options(${tgt} PRIVATE -fsanitize=fuzzer)
    target_link_options(${tgt} PRIVATE -fsanitize=fuzzer)
  else()
    add_executable(${tgt} ${tgt}.cpp standalone_main.cpp)
  endif()
  target_link_libraries(${tgt} PRIVATE ob::lib ob::warnings)
endforeach()
