cmake_minimum_required(VERSION 3.16)

project(ScreenSubTranslator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type: Debug, Release, RelWithDebInfo, or MinSizeRel" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()


find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Network)

find_library(SENTENCEPIECE_LIBRARY
	sentencepiece
	PATHS /usr/lib /usr/local/lib
)
if(NOT SENTENCEPIECE_LIBRARY)
	message(FATAL_ERROR "Could not find libsentencepiece. Install with: sudo apt install libsentencepiece-dev")
endif()
message(STATUS "Using SentencePiece library: ${SENTENCEPIECE_LIBRARY}")
find_package(OpenCV REQUIRED)

# Find ONNX Runtime from environment variables (for example from ~/.zshrc).
set(ONNXRUNTIME_ROOT "$ENV{ONNXRUNTIME_ROOT}" CACHE PATH "Path to ONNX Runtime root")

find_path(ONNXRUNTIME_INCLUDE_DIR
	onnxruntime_cxx_api.h
	HINTS
		${ONNXRUNTIME_ROOT}
		$ENV{CMAKE_PREFIX_PATH}
	PATH_SUFFIXES
		include
		include/onnxruntime
)

find_library(ONNXRUNTIME_LIBRARY
	onnxruntime
	HINTS
		${ONNXRUNTIME_ROOT}
		$ENV{CMAKE_PREFIX_PATH}
	PATH_SUFFIXES
		lib
		lib64
)

if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
	message(FATAL_ERROR
		"Could not find ONNX Runtime. Check ONNXRUNTIME_ROOT/CMAKE_PREFIX_PATH in your shell env. "
		"Current ONNXRUNTIME_ROOT='${ONNXRUNTIME_ROOT}'")
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Using ONNX Runtime include: ${ONNXRUNTIME_INCLUDE_DIR}")
message(STATUS "Using ONNX Runtime library: ${ONNXRUNTIME_LIBRARY}")

add_executable(ScreenSubTranslator
	main.cpp
	src/capture_worker.cpp
	src/ocr_engine.cpp
	src/ocr_worker.cpp
	src/overlay_window.cpp
	src/subtitle_logger.cpp
	src/translate_client.cpp
	src/translation_backend_adapter.cpp
	src/translation_text_processor.cpp
)

add_executable(OcrBatchEval
	tools/ocr_batch_eval.cpp
	src/ocr_engine.cpp
)

target_include_directories(ScreenSubTranslator PRIVATE
	${OpenCV_INCLUDE_DIRS}
	${ONNXRUNTIME_INCLUDE_DIR}
	src
)

if(CMAKE_CONFIGURATION_TYPES)
	target_compile_definitions(ScreenSubTranslator PRIVATE
		$<$<CONFIG:Debug>:SST_DEBUG_BUILD>
	)
elseif(CMAKE_BUILD_TYPE)
	string(TOUPPER "${CMAKE_BUILD_TYPE}" SST_BUILD_TYPE_UPPER)
	if(SST_BUILD_TYPE_UPPER STREQUAL "DEBUG")
		target_compile_definitions(ScreenSubTranslator PRIVATE SST_DEBUG_BUILD)
	endif()
endif()

target_link_libraries(ScreenSubTranslator PRIVATE
	Qt6::Widgets
	Qt6::Concurrent
	Qt6::Network
	${OpenCV_LIBS}
	${ONNXRUNTIME_LIBRARY}
	${SENTENCEPIECE_LIBRARY}
)

target_include_directories(OcrBatchEval PRIVATE
	${OpenCV_INCLUDE_DIRS}
	${ONNXRUNTIME_INCLUDE_DIR}
	src
)

target_link_libraries(OcrBatchEval PRIVATE
	Qt6::Core
	${OpenCV_LIBS}
	${ONNXRUNTIME_LIBRARY}
	${SENTENCEPIECE_LIBRARY}
)
