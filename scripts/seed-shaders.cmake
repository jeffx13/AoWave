file(REMOVE_RECURSE "${OUT}")
execute_process(
    COMMAND "${APP}" --warm-shaders "${OUT}" "${CONFIG}" 640x480,720x480,1024x576,1280x720,1920x1080,1920x1080p10
            2560x1600 1920x1080 2560x1440 3840x2160
    RESULT_VARIABLE result
    TIMEOUT 1200)
if(result EQUAL 0 AND EXISTS "${OUT}/warmed.txt")
    file(GLOB shaders "${OUT}/shader_*")
    list(LENGTH shaders count)
    message(STATUS "Shader seed: ${count} compiled shaders in ${OUT}")
else()
    message(WARNING "Shader seed not made (${result}); the app will compile its upscalers in the background")
    file(REMOVE_RECURSE "${OUT}")
endif()
file(TOUCH "${STAMP}")
