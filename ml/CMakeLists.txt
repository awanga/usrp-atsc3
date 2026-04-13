# ml/ - Machine Learning backend (post-MVP)
# PyTorch models + ONNX Runtime inference

if(NOT ATSC3_ENABLE_ML)
    message(STATUS "ML backend disabled - set ATSC3_ENABLE_ML=ON to enable")
    return()
endif()

# Find ONNX Runtime
# find_package(onnxruntime)

# Note: Do not add ML dependencies to lib/ CMakeLists
# ML integration is via lib/channel/channel_estimator.h setBackend() slot

set(ML_SOURCES
    # onnx_backend.cc
    # channel_estimator_ml.cc
)

if(ML_SOURCES)
    add_library(atsc3_ml ${ML_SOURCES})

    target_include_directories(atsc3_ml
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(atsc3_ml
        PRIVATE
            atsc3_lib
            # onnxruntime
    )
endif()
