add_executable(KhmerShaperTest
  KhmerShaperTest.cpp
  ${REPO_ROOT}/lib/Utf8/KhmerShaper.cpp
)

target_include_directories(KhmerShaperTest PRIVATE
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(KhmerShaperTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KhmerShaperTest)
