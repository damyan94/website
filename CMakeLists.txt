cmake_minimum_required(VERSION 3.25)

project(WebSiteBackend LANGUAGES CXX)

# Set this before Drogon's filesystem checks, which restore an empty standard
# when CMAKE_CXX_STANDARD was not provided by the consuming project.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Consume an installed Drogon package; dependency acquisition is managed separately.
find_package(Drogon CONFIG REQUIRED)
add_subdirectory(Dependencies/FelisFramework)

add_executable(WebSiteBackend
	Source/Main.cpp
	Source/PublicContent.cpp
	Source/PublicContent.h
	Source/ServerApplication.cpp
	Source/ServerApplication.h
	Source/stdafx.h
)

target_compile_features(WebSiteBackend PRIVATE cxx_std_23)
target_include_directories(WebSiteBackend PRIVATE Source)
target_precompile_headers(WebSiteBackend PRIVATE Source/stdafx.h)
target_link_libraries(WebSiteBackend PRIVATE Felis::Framework Drogon::Drogon)

if(MSVC)
	target_compile_options(WebSiteBackend PRIVATE /W4)
else()
	target_compile_options(WebSiteBackend PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(WebSiteBackend PROPERTIES CXX_EXTENSIONS OFF)

# Public-only sites can build without PostgreSQL, Argon2 or the accounts code.
option(WEBSITE_ENABLE_ACCOUNTS "Build optional PostgreSQL-backed accounts" OFF)
if(WEBSITE_ENABLE_ACCOUNTS)
	find_package(PostgreSQL REQUIRED)
	find_package(OpenSSL REQUIRED COMPONENTS Crypto)
	find_package(Threads REQUIRED)
	find_path(ARGON2_INCLUDE_DIR argon2.h REQUIRED)
	find_library(ARGON2_LIBRARY NAMES argon2 REQUIRED)

	target_sources(WebSiteBackend PRIVATE
		Source/Accounts/Crypto.cpp
		Source/Accounts/Database.cpp
		Source/Accounts/AccountStore.cpp
		Source/Accounts/AccountQueryService.cpp
		Source/Accounts/AccountWriteService.cpp
		Source/Accounts/SessionService.cpp
		Source/Accounts/PasswordService.cpp
		Source/Accounts/AccountRepository.cpp
		Source/Accounts/Module.cpp
		Source/Accounts/Controller.cpp
		Source/Accounts/Configuration.cpp
		Source/Accounts/Provisioning.cpp
		Source/Accounts/Email.cpp
		Source/Accounts/AccountStoreEmail.cpp
	)
	target_include_directories(WebSiteBackend PRIVATE ${ARGON2_INCLUDE_DIR})
	target_link_libraries(WebSiteBackend PRIVATE PostgreSQL::PostgreSQL
		OpenSSL::Crypto Threads::Threads ${ARGON2_LIBRARY})
endif()
target_compile_definitions(WebSiteBackend PRIVATE WEBSITE_ENABLE_ACCOUNTS=$<BOOL:${WEBSITE_ENABLE_ACCOUNTS}>)

option(WEBSITE_ENABLE_CONTENT_EDITOR "Build optional service-menu editor" OFF)
if(WEBSITE_ENABLE_CONTENT_EDITOR)
	if(NOT WEBSITE_ENABLE_ACCOUNTS OR NOT UNIX)
		message(FATAL_ERROR "Content editing requires WEBSITE_ENABLE_ACCOUNTS=ON and a POSIX host")
	endif()
	target_sources(WebSiteBackend PRIVATE Source/ContentEditor/ServiceEditor.cpp)
endif()
target_compile_definitions(WebSiteBackend PRIVATE WEBSITE_ENABLE_CONTENT_EDITOR=$<BOOL:${WEBSITE_ENABLE_CONTENT_EDITOR}>)

option(WEBSITE_ENABLE_RESERVATIONS "Build optional resource reservations" OFF)
if(WEBSITE_ENABLE_RESERVATIONS)
	if(NOT WEBSITE_ENABLE_ACCOUNTS)
		message(FATAL_ERROR "Reservations require WEBSITE_ENABLE_ACCOUNTS=ON")
	endif()
	target_sources(WebSiteBackend PRIVATE Source/Reservations/Module.cpp)
endif()
target_compile_definitions(WebSiteBackend PRIVATE WEBSITE_ENABLE_RESERVATIONS=$<BOOL:${WEBSITE_ENABLE_RESERVATIONS}>)
