# Kiri Bridge — components/web build hook.
# Stage 1: run esbuild to bundle the Preact SPA into pages/app.{js,css}
# Stage 2: chunk + gzip + embed into ESP32 rodata via build_assets.py

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(WEB_PAGES_DIR "${CMAKE_CURRENT_LIST_DIR}/pages")
set(WEBUI_DIR "${CMAKE_CURRENT_LIST_DIR}/../../webui")
set(WEB_ASSET_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_assets")

# Track every source that should re-trigger a configure when changed.
file(GLOB_RECURSE WEBUI_SOURCES
    "${WEBUI_DIR}/src/*.ts"
    "${WEBUI_DIR}/src/*.tsx"
    "${WEBUI_DIR}/src/*.css")
set(WEB_ASSET_SOURCES
    "${WEB_PAGES_DIR}/loader.js"
    "${CMAKE_CURRENT_LIST_DIR}/tools/build_assets.py"
    "${WEBUI_DIR}/package.json"
    "${WEBUI_DIR}/tsconfig.json"
    "${WEBUI_DIR}/tools/bundle.mjs"
    ${WEBUI_SOURCES})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${WEB_ASSET_SOURCES})

# --- Stage 1: esbuild bundle. ---
# Require node_modules to already exist; let the developer run `npm install`
# explicitly so cmake reconfigures stay fast.
if(NOT EXISTS "${WEBUI_DIR}/node_modules/.package-lock.json")
    message(FATAL_ERROR
        "WebUI dependencies are not installed.\n"
        "Run:  cd webui && npm install\n"
        "Then re-run idf.py build.")
endif()

execute_process(
    COMMAND npm run build --silent
    WORKING_DIRECTORY "${WEBUI_DIR}"
    RESULT_VARIABLE WEBUI_BUNDLE_RESULT
    OUTPUT_VARIABLE WEBUI_BUNDLE_STDOUT
    ERROR_VARIABLE WEBUI_BUNDLE_STDERR)
if(NOT WEBUI_BUNDLE_RESULT EQUAL 0)
    message(FATAL_ERROR
        "WebUI esbuild bundle failed (exit ${WEBUI_BUNDLE_RESULT}).\n"
        "stdout:\n${WEBUI_BUNDLE_STDOUT}\n"
        "stderr:\n${WEBUI_BUNDLE_STDERR}")
endif()
message(STATUS "WebUI bundle:\n${WEBUI_BUNDLE_STDOUT}")

# Move bundle outputs into pages/ where build_assets.py looks for them.
file(COPY "${WEBUI_DIR}/dist/app.js" DESTINATION "${WEB_PAGES_DIR}")
file(COPY "${WEBUI_DIR}/dist/app.css" DESTINATION "${WEB_PAGES_DIR}")

# --- Stage 2: chunk, gzip, generate cpp. ---
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/build_assets.py"
            "--pages-dir" "${WEB_PAGES_DIR}"
            "--out-dir" "${WEB_ASSET_OUTPUT_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    RESULT_VARIABLE WEB_ASSET_BUILD_RESULT)
if(NOT WEB_ASSET_BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR "Web asset generation failed")
endif()

include("${WEB_ASSET_OUTPUT_DIR}/web_assets_manifest.cmake")

idf_component_register(SRCS "web_http.cpp"
                            "web_pages.cpp"
                            "web_routes.cpp"
                            "web_server.cpp"
                            "${WEB_ASSETS_CPP}"
                       INCLUDE_DIRS "include"
                       REQUIRES app_config app_update board_profile build_info cn105_transport core_cn105 device_settings esp_http_server esp_timer homekit_bridge jsmn lua_vm ota_handler platform_fs platform_log platform_maintenance platform_provisioning platform_uart platform_wifi log)

foreach(asset_file IN LISTS WEB_GZIP_ASSET_FILES)
    target_add_binary_data(${COMPONENT_LIB} "${asset_file}" BINARY)
endforeach()
