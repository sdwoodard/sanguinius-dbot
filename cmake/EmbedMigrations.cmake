function(sanguinius_embed_migrations output_file template_file)
    set(SANGUINIUS_MIGRATION_ENTRIES "")
    set(expected_version 1)

    foreach(input_file IN LISTS ARGN)
        if(NOT EXISTS "${input_file}")
            message(FATAL_ERROR "Migration source is missing: ${input_file}")
        endif()

        get_filename_component(migration_file "${input_file}" NAME)
        if(NOT migration_file MATCHES "^([0-9][0-9][0-9][0-9])_([a-z0-9_]+)\\.sql$")
            message(FATAL_ERROR "Migration filename is invalid: ${migration_file}")
        endif()
        math(EXPR migration_version "${CMAKE_MATCH_1} + 0")
        set(migration_name "${CMAKE_MATCH_2}")
        if(NOT migration_version EQUAL expected_version)
            message(FATAL_ERROR
                "Migration ${migration_file} is version ${migration_version}; expected ${expected_version}")
        endif()

        file(READ "${input_file}" migration_sql)
        file(SHA256 "${input_file}" migration_checksum)
        string(APPEND SANGUINIUS_MIGRATION_ENTRIES
            "      Migration{${migration_version}, \"${migration_name}\", \"${migration_checksum}\",\n"
            "                R\"SANGMIG${CMAKE_MATCH_1}(\n${migration_sql})SANGMIG${CMAKE_MATCH_1}\"},\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input_file}")
        math(EXPR expected_version "${expected_version} + 1")
    endforeach()

    configure_file("${template_file}" "${output_file}" @ONLY)
endfunction()
