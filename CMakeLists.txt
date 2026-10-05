cmake_minimum_required(VERSION 3.20)
project(xa_devhub VERSION 1.0.8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(devhub_warnings INTERFACE)
target_compile_options(devhub_warnings INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /w14242 /w14263 /w14265
                              /w14287 /w14296 /w14545 /w14546 /w14547
                              /w14549 /w14555 /w14640 /w14826 /w14928>
)
option(DEVHUB_WERROR "Treat first-party warnings as errors" OFF)
if(DEVHUB_WERROR)
    target_compile_options(devhub_warnings INTERFACE
        $<$<CXX_COMPILER_ID:MSVC>:/WX>)
endif()

option(DEVHUB_ASAN "Build first-party targets with AddressSanitizer" OFF)
if(DEVHUB_ASAN)
    add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/fsanitize=address>)
    add_link_options($<$<CXX_COMPILER_ID:MSVC>:/INCREMENTAL:NO>)
endif()

# When ON, builds a console subsystem app so startup/diagnostic output is
# visible. When OFF (default), builds a windowed native app.
option(DEVHUB_CONSOLE "Build with a console window for diagnostics" OFF)

find_package(Crow CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SQLiteCpp CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(dpp CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# devhub_core: pure, portable logic (no Windows API, no HTTP). Shared by the
# app and the test binary so the PTD parser / suggestion detector / version
# utilities can be unit-tested in isolation.
# ---------------------------------------------------------------------------
add_library(devhub_core STATIC
    src/SuggestionDetector.cpp
    src/TicketImage.cpp
    src/Util.cpp
    src/VersionUtil.cpp
)
target_include_directories(devhub_core PUBLIC include)
target_link_libraries(devhub_core PUBLIC nlohmann_json::nlohmann_json
                                     devhub_warnings)
target_compile_definitions(devhub_core PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX)

# ---------------------------------------------------------------------------
# devhub_app: native application logic shared by the product entry point and
# the dedicated app-level CTest executable.
# ---------------------------------------------------------------------------
# Explicit inventory: timestamped source backups can never become product
# inputs merely because they happen to end in .cpp.
add_library(devhub_app STATIC
    src/app/AppBootstrap.cpp
    src/app/AppSelftest.cpp
    src/app/BuildRunner.cpp
    src/app/DataMaintenance.cpp
    src/app/Db.cpp
    src/app/DiscordBot.cpp
    src/app/DiscordIntakeOps.cpp
    src/app/DiscordSettingsOps.cpp
    src/app/Gui.cpp
    src/app/GuiBranding.cpp
    src/app/GuiDiscord.cpp
    src/app/GuiKnowledge.cpp
    src/app/GuiModals.cpp
    src/app/KnowledgeOps.cpp
    src/app/Ops.cpp
    src/app/OpsCalendar.cpp
    src/app/OpsDiscordTickets.cpp
    src/app/OpsItems.cpp
    src/app/PacketData.cpp
    src/app/PacketExports.cpp
    src/app/PacketOps.cpp
    src/app/Server.cpp
    src/app/TicketImport.cpp
    src/app/TicketImageStore.cpp
    src/app/VersionChecker.cpp
)
target_include_directories(devhub_app PUBLIC src/app)
target_link_libraries(devhub_app PUBLIC
    devhub_core
    Crow::Crow
    nlohmann_json::nlohmann_json
    SQLiteCpp
    imgui::imgui
    dpp::dpp
    d3d11 dxgi dwmapi
    ws2_32
    shlwapi
    shell32
    winhttp
    bcrypt advapi32 crypt32
    ole32 oleaut32
    devhub_warnings
)
target_compile_definitions(devhub_app PUBLIC
    _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN
    CROW_HTTP_BODY_LIMIT=2097152
)
target_compile_options(devhub_app PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/guard:cf /sdl>)

add_executable(devhub
    src/app/main.cpp
    src/app/devhub.rc
)
target_link_libraries(devhub PRIVATE devhub_app devhub_warnings)
target_compile_definitions(devhub PRIVATE
    _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN
)
target_compile_options(devhub PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/guard:cf /sdl>)
target_link_options(devhub PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/guard:cf /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT>)

if(DEVHUB_CONSOLE)
    target_compile_definitions(devhub PRIVATE DEVHUB_CONSOLE=1)
else()
    # Windowed app, but keep the standard int main() entry point.
    target_link_options(devhub PRIVATE "/SUBSYSTEM:WINDOWS" "/ENTRY:mainCRTStartup")
endif()

# ---------------------------------------------------------------------------
# Local-only regression tests (CTest). The public source distribution omits
# tests/, so a fresh public checkout must still configure and build the app.
# ---------------------------------------------------------------------------
if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/tests")
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/tests/test_core.cpp" OR
   NOT EXISTS "${CMAKE_SOURCE_DIR}/tests/test_app.cpp")
    message(FATAL_ERROR "The local tests directory is incomplete")
endif()

enable_testing()
add_executable(devhub_tests tests/test_core.cpp)
target_link_libraries(devhub_tests PRIVATE devhub_core devhub_warnings)
add_test(NAME core_tests COMMAND devhub_tests)
set_tests_properties(core_tests PROPERTIES LABELS "behavior;core")
add_executable(devhub_app_tests tests/test_app.cpp)
target_link_libraries(devhub_app_tests PRIVATE devhub_app devhub_warnings)
add_test(NAME app_selftest COMMAND devhub_app_tests all)
set_tests_properties(app_selftest PROPERTIES
    TIMEOUT 300
    LABELS "behavior;application"
)
set(DEVHUB_SELFTEST_DOMAINS
    migration
    operations
    packets
    knowledge
    server
    discord
    buildrunner
)
foreach(domain IN LISTS DEVHUB_SELFTEST_DOMAINS)
    add_test(NAME "app_selftest_${domain}" COMMAND devhub_app_tests "${domain}")
    set_tests_properties("app_selftest_${domain}" PROPERTIES
        TIMEOUT 300
        LABELS "behavior;application;domain;${domain}"
    )
endforeach()

find_program(POWERSHELL_EXE NAMES powershell.exe powershell)
if(POWERSHELL_EXE)
    set(DEVHUB_CONTRACT_SCRIPTS
        tests/automatic_promotion_credit_contract.ps1
        tests/dashboard_completion_window_contract.ps1
        tests/discord_attachment_pending_edit_contract.ps1
        tests/discord_ticket_priority_contract.ps1
        tests/octo_p0_contract.ps1
        tests/octo_p1_contract.ps1
        tests/octo_p2_contract.ps1
        tests/review_reaction_cleanup_contract.ps1
        tests/ticket_merge_contract.ps1
        tests/ticket_menu_filter_contract.ps1
        tests/ticket_import_contract.ps1
    )
    foreach(script IN LISTS DEVHUB_CONTRACT_SCRIPTS)
        get_filename_component(contract_name "${script}" NAME_WE)
        add_test(
            NAME "source_contract_${contract_name}"
            COMMAND "${POWERSHELL_EXE}" -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_SOURCE_DIR}/${script}"
        )
        set_tests_properties("source_contract_${contract_name}"
            PROPERTIES LABELS "static;source-contract")
    endforeach()
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_test(NAME build_wrapper_pause
             COMMAND Python3::Interpreter
                     "${CMAKE_SOURCE_DIR}/tests/test_build_wrapper_pause.py")
    set_tests_properties(build_wrapper_pause
        PROPERTIES LABELS "static;wrapper-contract")
endif()
else()
    message(STATUS "Local-only tests are not included; building XA DevHub without CTest targets")
endif()
