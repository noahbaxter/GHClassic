# FetchContent's downloads, kept in one folder every checkout shares, so a new
# worktree configures without cloning everything again. tools/ghc.py passes this
# as CMAKE_PROJECT_TOP_LEVEL_INCLUDES, with the folder as GHC_DEP_CACHE.
#
# Each download is filed under its name and a hash of how it was declared, so a
# branch that moves a tag gets its own copy rather than a stale one. Only the
# sources are shared: every build still configures and builds them itself.

include(FetchContent)

# The provider hands a dependency over as FETCHCONTENT_SOURCE_DIR_<NAME>, which
# FetchContent then keeps in the cache, and a cached one is used without asking
# the provider again. Dropped each configure, so a moved tag is noticed.
get_cmake_property(_ghc_cached CACHE_VARIABLES)
foreach(_ghc_var IN LISTS _ghc_cached)
    if(GHC_DEP_CACHE AND _ghc_var MATCHES "^FETCHCONTENT_SOURCE_DIR_")
        string(FIND "${${_ghc_var}}" "${GHC_DEP_CACHE}/" _ghc_at)
        if(_ghc_at EQUAL 0)
            unset(${_ghc_var} CACHE)
        endif()
    endif()
endforeach()

macro(ghc_provide_dependency method name)
    # What the download depends on: the declared arguments, less the ones that
    # only say where this build puts it or how it joins the project.
    set(_ghc_fetch "")
    set(_ghc_skip FALSE)
    set(_ghc_args "${ARGN}")
    foreach(_ghc_arg IN LISTS _ghc_args)
        if(_ghc_skip)
            set(_ghc_skip FALSE)
        elseif(_ghc_arg MATCHES "^(SOURCE_DIR|BINARY_DIR|SOURCE_SUBDIR)$")
            set(_ghc_skip TRUE)
        elseif(NOT _ghc_arg MATCHES "^(SYSTEM|EXCLUDE_FROM_ALL)$")
            list(APPEND _ghc_fetch "${_ghc_arg}")
        endif()
    endforeach()
    string(TOLOWER "${name}" _ghc_name)
    string(SHA1 _ghc_key "${_ghc_fetch}")
    string(SUBSTRING "${_ghc_key}" 0 12 _ghc_key)
    set(_ghc_dir "${GHC_DEP_CACHE}/${_ghc_name}-${_ghc_key}")

    # Two checkouts configuring at once take turns. The marker is written last,
    # so a download cut short is made again.
    file(LOCK "${_ghc_dir}.lock")
    if(NOT EXISTS "${_ghc_dir}/.ghc-fetched")
        file(REMOVE_RECURSE "${_ghc_dir}" "${_ghc_dir}-subbuild")
        message(STATUS "Fetching ${name} into ${_ghc_dir}")
        FetchContent_Populate(${_ghc_name}-cache QUIET
            SOURCE_DIR "${_ghc_dir}" SUBBUILD_DIR "${_ghc_dir}-subbuild" ${_ghc_fetch})
        file(TOUCH "${_ghc_dir}/.ghc-fetched")
    endif()
    file(LOCK "${_ghc_dir}.lock" RELEASE)

    # Left unpopulated, FetchContent_MakeAvailable carries on as it always
    # would, taking the source from here instead of downloading it.
    string(TOUPPER "${name}" _ghc_upper)
    set(FETCHCONTENT_SOURCE_DIR_${_ghc_upper} "${_ghc_dir}")
    unset(_ghc_fetch)
    unset(_ghc_skip)
    unset(_ghc_args)
    unset(_ghc_arg)
    unset(_ghc_name)
    unset(_ghc_key)
    unset(_ghc_dir)
    unset(_ghc_upper)
endmacro()

if(GHC_DEP_CACHE)
    cmake_language(SET_DEPENDENCY_PROVIDER ghc_provide_dependency
        SUPPORTED_METHODS FETCHCONTENT_MAKEAVAILABLE_SERIAL)
endif()
