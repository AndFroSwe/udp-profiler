include(FetchContent)

FetchContent_Declare(
  cli11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11
  GIT_TAG v2.6.2
  EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(cli11)
