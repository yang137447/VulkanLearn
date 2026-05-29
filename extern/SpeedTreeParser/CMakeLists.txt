add_library(SpeedTreeParserCore STATIC
    src/speedTreeParserCore.cpp
)

target_include_directories(SpeedTreeParserCore
PUBLIC
    include
)

target_link_libraries(SpeedTreeParserCore
PUBLIC
    Json
)
